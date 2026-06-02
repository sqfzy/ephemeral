/// @file dpdk_mp_demo.cpp
///
/// DPDK single-NIC multi-process demo under the **daemon-led model**.
/// The pre-2026-05-02 autojoin path
/// (`Platform::create_or_join` / `CreateOrJoinConfig`) was deleted —
/// see `.claude/plans/calm-roaming-sedgewick.md`. The new shape:
///
///   1. `eph-nicd@<pci>.service` runs as the NIC primary (operator
///      starts it via systemd, or ad-hoc via
///      `sudo /usr/local/bin/eph-nicd --pci <bdf>`). It owns the
///      port bring-up, mempool, RSS key, and queue pool.
///   2. Every application process — including multiple instances of
///      THIS BINARY — calls `Platform::create(PlatformConfig{...})`
///      and attaches as a secondary. Each ask claims a sub-range of
///      the daemon's queue pool. Different binaries (strategy A, MD B,
///      risk C) coexist with zero point-to-point coordination — the
///      daemon is the single arbiter.
///
/// What the library still does for you (cross-process control plane):
///   * RX-queue partitioning: the daemon's QueueAllocator (S5, live —
///     hugepage-backed greedy first-fit, see
///     `eph/dpdk/detail/queue_allocator.hpp`) hands each secondary a
///     disjoint sub-range; pool-exhausted asks fail with
///     "QueuePoolExhausted" rather than racing other tenants for
///     queue 0.
///   * Source-port partitioning: secondaries draw from disjoint
///     segments of the ephemeral range based on their owned queue ids
///     so two apps do not collide on `src_port`.
///   * ICMP MTU propagation: ICMP Frag Needed messages auto-forward
///     via `rte_mp_*` IPC to the owning stream's `effective_mss`.
///   * FlowDir secondary install fallback: if the PMD rejects
///     `rte_flow_create` from a secondary, the library transparently
///     routes the install through `eph_fd_install` IPC to the primary
///     (= daemon).
///
/// Scope:
///   * UDP, not TCP — no connect handshake clutter (RawDatagramCodec).
///   * No actual traffic. Each invocation brings up a per-process
///     Platform, attaches a `DpdkUdpSocket`, drives `poll()` for a few
///     seconds, then shuts down.
///   * The point is the *control plane* (mempool lookup, queue pool
///     claim/release, cleanup branches) — once that's clear, swap in a
///     real codec / connect / loop and this scales to production.
///
/// Usage (three terminals: daemon + two app peers):
///
///   # Terminal A — daemon (NIC primary)
///   sudo ./eph-nicd --pci 0000:28:00.0 --total-queues 8
///
///   # Terminal B — first application secondary
///   sudo ./dpdk_mp_demo --pci 0000:28:00.0 --pin 0=0:rx --pin 1=1:tx
///                       --queues 4
///
///   # Terminal C — second application secondary (different lcores
///   #              + different queue range; the daemon arbitrates)
///   sudo ./dpdk_mp_demo --pci 0000:28:00.0 --pin 0=2:rx --pin 1=3:tx
///                       --queues 4
///
/// `--pin lcore_id=cpu_id[:role]` (repeatable) is the typed entry
/// point — goes through `Platform::create`'s typed-pin path,
/// which validates each pin (cpu >= 0, no SMT/NUMA conflict per
/// policy) and registers the cpus into the process-wide pin registry
/// BEFORE `rte_eal_init` fires. Any later `eph::utils::pin_thread`
/// on a colliding cpu is detected loudly instead of silently fighting
/// EAL.
///
/// Escape hatch: `--lcores '<raw EAL spec>'` (e.g.
/// `--lcores '(0-1)@(4,5)'` for set-of-sets) bypasses the typed path
/// and goes through the raw-lcores path. Mutually exclusive with
/// `--pin` in one call.
///
/// Required environment: NIC bound to vfio-pci, ≥ 256 hugepages free.
/// See `eph-net-dpdk/tools/dpdk-setup.sh`. The daemon and every app
/// peer must use the SAME `--pci` — file_prefix is auto-derived from
/// the BDF, so identical pci → identical hugepage namespace →
/// secondary's `rte_mempool_lookup` finds the daemon's mempool.
///
/// Additional flags (all optional; full list at parse_args):
///   --queues <n>      bidirectional queue pairs to claim from the
///                     daemon's pool (default 1; the daemon's S5
///                     QueueAllocator hands back a disjoint sub-range
///                     and pool-exhausted asks fail fast).
///   --seconds <n>     demo run duration (default 5).

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/codec/raw_datagram_codec.hpp"
#include "eph/dpdk/cli.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/lcore_pin.hpp"   // LcorePin
#include "eph/dpdk/platform.hpp"
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/dpdk/udp_socket.hpp"
#include "eph/utils/cpu.hpp"        // CpuPinPolicy

namespace edpdk = eph::net::dpdk;
namespace ed    = eph::dpdk;
namespace ec    = eph::codec;
using namespace std::chrono_literals;

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running.store(false, std::memory_order_release); }

namespace {

struct AppArgs {
    /// EAL flags — --pci / --pin / --lcores / --port-id (or --dpdk-port).
    ed::cli::EalCliConfig        eal{};
    /// `cfg.queues` for `Platform::create` — bidirectional queue pairs
    /// to claim from the daemon's pool. Must be <= the daemon's
    /// `total_queues` minus what other secondaries already hold.
    uint16_t                queues       = 1;
    std::chrono::seconds    run_seconds  = 5s;   // how long to drive poll()
};

AppArgs parse_args(int argc, char** argv) {
    AppArgs out;
    // Optional `--` separator is accepted but no longer required —
    // every flag is consumed by the EAL CLI helper or the post-EAL
    // application config.
    int start = 1;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--") { start = i + 1; break; }
    }
    for (int i = start; i < argc; ++i) {
        std::string_view a = argv[i];
        char const* next = (i + 1 < argc) ? argv[i + 1] : nullptr;

        // EAL flags first.
        auto consumed = ed::cli::consume_one(out.eal, a, next);
        if (!consumed) {
            spdlog::error("dpdk_mp_demo: {}", consumed.error());
            std::exit(1);
        }
        if (*consumed > 0) { i += *consumed - 1; continue; }

        if      (a == "--queues"    && next) { out.queues      = static_cast<uint16_t>(std::atoi(next)); ++i; }
        else if (a == "--nb-queues" && next) { out.queues      = static_cast<uint16_t>(std::atoi(next)); ++i; }  // legacy alias
        else if (a == "--seconds"   && next) { out.run_seconds = std::chrono::seconds(std::atoi(next));  ++i; }
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    spdlog::set_level(spdlog::level::info);

    const AppArgs args = parse_args(argc, argv);
    if (args.queues == 0) {
        spdlog::error("dpdk_mp_demo: --queues must be >= 1");
        return 1;
    }
    if (args.eal.pci.empty()) {
        spdlog::error("dpdk_mp_demo: --pci <BDF> is required "
                      "(file_prefix is auto-derived from the BDF and must "
                      "match the daemon's `eph-nicd@<pci>.service` instance)");
        return 1;
    }
    if (args.eal.pci.size() > 1) {
        spdlog::error("dpdk_mp_demo: --pci passed {} times; one app peer "
                      "attaches to one NIC (one daemon = one BDF)",
                      args.eal.pci.size());
        return 1;
    }

    // ── Validate --pin / --lcores exclusivity (require at least one) ─────
    if (auto v = ed::cli::validate(args.eal); !v) {
        spdlog::error("dpdk_mp_demo: {}", v.error());
        return 1;
    }
    if (args.eal.pins.empty() && args.eal.lcores_raw.empty()) {
        spdlog::error("dpdk_mp_demo: provide either --pin lcore=cpu[:role] "
                      "(preferred typed path) or --lcores '<raw EAL spec>' "
                      "(escape hatch for set-of-sets / coremask syntax)");
        return 1;
    }

    // ── Platform::create — application secondary attach ───────────────
    // Every app peer (this binary, strategy A, MD B, …) takes the same
    // path: claim N queue pairs from the daemon's pool. The daemon's
    // QueueAllocator (S5, live) arbitrates so two peers never share
    // a queue, and pool-exhausted asks fail fast.
    //
    // Pre-flight: the daemon must already be running on this NIC, e.g.
    //
    //   sudo systemctl start eph-nicd@<bdf>.service     # production
    //   sudo /usr/local/bin/eph-nicd --pci <bdf>        # ad-hoc
    //
    // EAL forbids primary+secondary in one process, so the daemon
    // MUST be a separate process.
    const std::string& pci_bdf = args.eal.pci.front();
    ed::PlatformConfig pcfg{};
    pcfg.pci          = pci_bdf;
    pcfg.queues       = args.queues;
    pcfg.pins         = std::span<ed::LcorePin const>{args.eal.pins};
    pcfg.pin_policy   = eph::utils::CpuPinPolicy{};
    pcfg.lcores       = args.eal.lcores_raw.empty()
                            ? std::vector<std::string>{}
                            : std::vector<std::string>{args.eal.lcores_raw};
    pcfg.program_name = "dpdk_mp_demo";

    if (!args.eal.pins.empty()) {
        spdlog::info("dpdk_mp_demo: Platform::create "
                     "(typed pins, {} pin(s); pci={}, queues={})",
                     args.eal.pins.size(), pci_bdf, args.queues);
    } else {
        spdlog::info("dpdk_mp_demo: Platform::create "
                     "(raw lcores='{}'; pci={}, queues={})",
                     args.eal.lcores_raw, pci_bdf, args.queues);
    }

    auto plat_r = ed::Platform::create(std::move(pcfg));
    if (!plat_r) {
        spdlog::error("dpdk_mp_demo: Platform::create failed: {} "
                      "(is `eph-nicd@{}.service` running with "
                      "total_queues >= what other peers have already "
                      "claimed + {}?)",
                      plat_r.error(), pci_bdf, args.queues);
        return 2;
    }
    auto platform = std::move(*plat_r);

    // Every peer is a secondary under the daemon-led model; no more
    // primary/secondary role distinction at the app layer.
    const auto qr = platform.effective_rx_queue_range();
    spdlog::info("dpdk_mp_demo: secondary ready — port={}, "
                 "rx_queue_range=[{},{}), mempool={:p}",
                 platform.port_id(), qr.first, qr.second,
                 static_cast<void*>(platform.mempool()));

    // ── Per-process Poller bound to one of the queues we own ────────────
    // `effective_rx_queue_range` is a cold getter. Real production code
    // would create one Poller per owned RX queue and pin each to its own
    // lcore. This skeleton drives just the first queue in our range to
    // keep main() short.
    edpdk::PollerConfig poller_cfg{};
    poller_cfg.port_id     = platform.port_id();
    poller_cfg.rx_queue_id = qr.first;

    auto poller_r = edpdk::DpdkPoller<>::create(poller_cfg);
    if (!poller_r) {
        spdlog::error("DpdkPoller::create failed: {}", poller_r.error().detail);
        return 4;
    }
    auto poller = std::move(*poller_r);

    if (auto r = platform.register_poller(qr.first, poller.get()); !r) {
        spdlog::error("register_poller failed: {}", r.error().detail);
        return 5;
    }

    // ── DpdkUdpSocket via create_and_attach (turnkey factory) ───────────
    // Hardcoded demo tuple. Each role draws src_port from a disjoint
    // `port_lo / port_hi` window; the operator assigns these windows (and
    // measures which src_port lands on which queue via dpdk_rsskey_probe
    // --finder, since ENA RSS landing cannot be predicted). In the
    // post-2026-05-02 daemon-led model that assignment is operator config
    // (toml consumed by `Platform::serve_nic` / `Platform::create`, see
    // eph-net-dpdk/docs/dpdk-daemon-deployment.md); the eph library does
    // not auto-coordinate across tenants. This skeleton does not actually
    // drive traffic, so the tuple values here only need to pass validation.
    using UdpSock = edpdk::DpdkUdpSocket<ec::RawDatagramCodec>;

    edpdk::UdpConfig ucfg{};
    ucfg.dpdk.wire.src_ip   = 0x0A000010;   // 10.0.0.16
    ucfg.dpdk.wire.dst_ip   = 0x0A000020;   // 10.0.0.32
    // Pick a src_port that lands in our owned queue range — production
    // code measures this empirically with dpdk_rsskey_probe --finder;
    // here we just hardcode something inside the ephemeral range.
    ucfg.dpdk.wire.src_port = static_cast<uint16_t>(32768 + qr.first);
    ucfg.dpdk.wire.dst_port = 30000;
    ucfg.dpdk.wire.port_id  = platform.port_id();
    ucfg.dpdk.wire.pool     = platform.mempool();

    auto sock_r = UdpSock::create_and_attach(std::move(ucfg), platform);
    if (!sock_r) {
        spdlog::warn("dpdk_mp_demo: create_and_attach failed "
                     "(expected on a smoke-boot without ARP-resolved peer): {}",
                     sock_r.error().detail);
        spdlog::info("dpdk_mp_demo: skeleton path — populate "
                     "src_mac / dst_mac (e.g. via eph::dpdk::arp::resolve) "
                     "to drive real traffic.");
        // Fall through to the poll loop anyway so the cleanup branches
        // are exercised end-to-end on every run.
    }

    // ── Drive the burst loop for `--seconds` (default 5s) ───────────────
    spdlog::info("dpdk_mp_demo: entering poll loop for {}s",
                 args.run_seconds.count());
    const auto deadline = std::chrono::steady_clock::now() + args.run_seconds;
    while (g_running.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        (void)poller->poll();
    }

    spdlog::info("dpdk_mp_demo: shutting down — Platform RAII releases "
                 "the secondary attach (queue range returns to the "
                 "daemon's pool).");
    // RAII: ~UdpSock → ~DpdkPoller → ~Platform (which owns EAL).
    // Secondary's ~Platform leaves the port + mempool untouched
    // (owned by primary); primary's ~Platform does dev_stop / dev_close /
    // mempool_free — see Platform::Impl::cleanup.
    return 0;
}
