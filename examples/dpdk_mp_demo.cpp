/// @file dpdk_mp_demo.cpp
///
/// DPDK single-NIC multi-process (primary + secondary) **autojoin**
/// path skeleton. Both peers run THIS SAME BINARY with the same
/// arguments — `Platform::create_or_join` races on `eal_init` to decide
/// which process becomes primary and which becomes secondary, then
/// CAS-claims a registry slot for each peer. There is no `--role`
/// flag and no shared `--file-prefix` (it is auto-derived from the
/// PCI BDF inside the library).
///
/// The cooperative MP path (`Platform::create_with_eal` /
/// `attach_with_eal` with explicit role + shared file_prefix) was
/// deleted in favor of this single autojoin entry point. See
/// `eph-net-dpdk/docs/dpdk-multiprocess.md` for the full ordering /
/// teardown protocol; for the single-process counterpart with full
/// TLS+WS handshake via `create_and_attach`, see
/// `examples/simple_hft.cpp`.
///
/// What the library does for you (cross-process control plane):
///   * RX-queue partitioning: each peer's `effective_rx_queue_range`
///     resolves to a disjoint sub-range of `[0, nb_rx_queues)` so two
///     processes never burst the same queue.
///   * Source-port partitioning: each peer's
///     `find_src_port_for_queue` window is narrowed to its own
///     `[port_lo, port_hi)` segment so primary and secondary do not
///     collide on `src_port` selection.
///   * ICMP MTU propagation: ICMP Frag Needed messages that land on a
///     peer process's RX queue auto-forward via `rte_mp_*` IPC to the
///     owning stream's `effective_mss`.
///   * FlowDir secondary install fallback: if the PMD rejects
///     `rte_flow_create` from a secondary, the library transparently
///     routes the install through `eph_fd_install` IPC to the primary.
///
/// Scope:
///   * UDP, not TCP — no connect handshake clutter (RawDatagramCodec).
///   * No actual traffic. We bring up the per-process Platform, attach a
///     `DpdkUdpSocket`, drive `poll()` for a few seconds, then shut down.
///   * The point is the *control plane* (mempool create-vs-lookup, port
///     bring-up vs skip, cleanup branches) — once that's clear, swap in
///     a real codec / connect / loop and this scales to production.
///
/// Usage (two terminals, same host, same NIC, same args):
///
///   # Terminal A — first peer; auto-resolves as primary
///   sudo ./dpdk_mp_demo --pci 0000:28:00.0 --pin 0=0:rx --pin 1=1:tx
///
///   # Terminal B — second peer; auto-resolves as secondary
///   sudo ./dpdk_mp_demo --pci 0000:28:00.0 --pin 0=2:rx --pin 1=3:tx
///
/// `--pin lcore_id=cpu_id[:role]` (repeatable) is the typed entry
/// point — goes through `Platform::create_or_join`'s typed-pin path,
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
/// See `eph-net-dpdk/scripts/dpdk-setup.sh`. Both peers must use the
/// SAME `--pci` (file_prefix is auto-derived from BDF, so identical
/// pci → identical hugepage namespace → secondary's
/// `rte_mempool_lookup` finds primary's mempool).
///
/// Additional flags (all optional; full list at parse_args):
///   --port-id <id>    DPDK port enumeration index (default 0).
///   --nb-queues <n>   total RX/TX queue count primary configures
///                     (default 4 — see AppArgs::nb_rx_queues below).
///                     Both peers must agree; the library partitions
///                     these queues into disjoint per-peer sub-ranges.
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
    uint16_t                nb_rx_queues = 4;    // total queues primary configures
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

        if      (a == "--nb-queues" && next) { out.nb_rx_queues = static_cast<uint16_t>(std::atoi(next)); ++i; }
        else if (a == "--seconds"   && next) { out.run_seconds  = std::chrono::seconds(std::atoi(next));  ++i; }
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    spdlog::set_level(spdlog::level::info);

    const AppArgs args = parse_args(argc, argv);
    if (args.nb_rx_queues < 2) {
        spdlog::error("dpdk_mp_demo: --nb-queues must be >= 2 "
                      "(per-peer ranges need to be disjoint)");
        return 1;
    }
    if (args.eal.pci.empty()) {
        spdlog::error("dpdk_mp_demo: --pci <BDF> is required "
                      "(autojoin auto-derives file_prefix from the PCI BDF)");
        return 1;
    }
    if (args.eal.pci.size() > 1) {
        spdlog::error("dpdk_mp_demo: --pci passed {} times; autojoin "
                      "is single-NIC (file_prefix is derived from one BDF)",
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

    // ── Single autojoin entry point — no role flag ─────────────────────
    // Whoever wins the EAL race becomes primary; the other becomes
    // secondary. Both peers populate nic.nb_rx_queues so the
    // primary peer (whichever it ends up being) has the value it needs;
    // secondary peers ignore it post-resolution.
    const std::string& pci_bdf = args.eal.pci.front();
    ed::CreateOrJoinConfig cfg{};
    cfg.pci                          = pci_bdf;
    cfg.nic.port_id       = args.eal.port_id;
    cfg.nic.nb_rx_queues  = args.nb_rx_queues;
    cfg.nic.nb_tx_queues  = args.nb_rx_queues;
    cfg.pins                         = std::span<ed::LcorePin const>{args.eal.pins};
    cfg.lcores                       = args.eal.lcores_raw.empty()
                                           ? std::vector<std::string>{}
                                           : std::vector<std::string>{args.eal.lcores_raw};
    cfg.pin_policy                   = eph::utils::CpuPinPolicy{};

    if (!args.eal.pins.empty()) {
        spdlog::info("dpdk_mp_demo: bring-up via Platform::create_or_join "
                     "(typed pins, {} pin(s); pci={})",
                     args.eal.pins.size(), pci_bdf);
    } else {
        spdlog::info("dpdk_mp_demo: bring-up via Platform::create_or_join "
                     "(raw lcores='{}'; pci={})",
                     args.eal.lcores_raw, pci_bdf);
    }

    auto plat_r = ed::Platform::create_or_join(std::move(cfg));
    if (!plat_r) {
        spdlog::error("dpdk_mp_demo: Platform::create_or_join failed: {}",
                      plat_r.error());
        return 2;
    }
    auto platform = std::move(*plat_r);

    const bool is_secondary = platform.is_secondary();
    const char* role_str = is_secondary ? "secondary" : "primary";
    const auto qr = platform.effective_rx_queue_range();
    spdlog::info("dpdk_mp_demo[{}]: ready — port={}, "
                 "rx_queue_range=[{},{}), mempool={:p}",
                 role_str, platform.port_id(), qr.first, qr.second,
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
    // Hardcoded demo tuple. With autojoin's mp_topology in place, the
    // library auto-narrows src_port selection to each role's
    // `port_lo / port_hi` window inside `find_src_port_for_queue` —
    // primary and secondary draw from disjoint segments without manual
    // coordination. This skeleton does not actually drive traffic, so
    // the tuple values here only need to pass validation.
    using UdpSock = edpdk::DpdkUdpSocket<ec::RawDatagramCodec>;

    edpdk::UdpConfig ucfg{};
    ucfg.dpdk.wire.src_ip   = 0x0A000010;   // 10.0.0.16
    ucfg.dpdk.wire.dst_ip   = 0x0A000020;   // 10.0.0.32
    ucfg.dpdk.wire.src_port = is_secondary ? 49152 : 32768;
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
    spdlog::info("dpdk_mp_demo[{}]: entering poll loop for {}s",
                 role_str, args.run_seconds.count());
    const auto deadline = std::chrono::steady_clock::now() + args.run_seconds;
    while (g_running.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        (void)poller->poll();
    }

    spdlog::info("dpdk_mp_demo[{}]: shutting down — Platform RAII "
                 "will run the {}-mode cleanup branch",
                 role_str, role_str);
    // RAII: ~UdpSock → ~DpdkPoller → ~Platform (which owns EAL).
    // Secondary's ~Platform leaves the port + mempool untouched
    // (owned by primary); primary's ~Platform does dev_stop / dev_close /
    // mempool_free — see Platform::Impl::cleanup.
    return 0;
}
