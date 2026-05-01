/// @file dpdk_mp_demo.cpp
///
/// DPDK single-NIC multi-process (primary + secondary) **declarative**
/// path skeleton. Each process declares its own `MpTopology` and
/// `Platform::create_primary` / `create_secondary` does the
/// bring-up. For the **autojoin** alternative — same scenario but
/// zero coordination between peers (no shared file_prefix, no
/// manual self_index, launch order is the only "agreement") — see
/// `Platform::join_dynamic` in `eph/dpdk/platform.hpp` and
/// `eph-net-dpdk/docs/dpdk-multiprocess.md`.
///
/// One binary that plays *both* roles, selected by `--role primary|secondary`
/// on the command line. Mirrors the spirit of `simple_hft.cpp` but
/// brings in the MP scaffolding shipped with `eph-net-dpdk`:
///
///   * `eph::dpdk::EalConfig` + `build_eal_argv` — typed assembly of
///     `--proc-type` / `--file-prefix` / `-l` / `-a`.
///   * `eph::dpdk::Platform::create_primary` / `create_secondary` — the
///     two factories that gate access to the shared NIC.
///   * `eph::dpdk::MpTopology::uniform(self_index, total_procs, nb_rx_queues)`
///     — the recommended auto-derive path. Both processes pass the same
///     `file_prefix` plus their own `self_index` (0 for primary, 1 for
///     secondary in this skeleton); the library carves RX queues +
///     src_port windows into disjoint sub-ranges and cross-validates
///     via the shared hugepage registry. Manual `rx_queue_range` +
///     hand-allocated src_port is still supported as the "Advanced
///     usage" escape hatch — see eph-net-dpdk/docs/dpdk-multiprocess.md.
///   * Cross-process ICMP MTU propagation is automatic when
///     `mp_topology` is set: ICMP Frag Needed messages that land on a
///     peer process's RX queue auto-forward via `rte_mp_*` IPC to the
///     owning stream's `effective_mss` — no caller wiring needed. See
///     "Cross-process ICMP MTU propagation" in the doc.
///   * FlowDir secondary install fallback is automatic too: if your
///     PMD rejects `rte_flow_create` from a secondary, the library
///     transparently routes the install through `eph_fd_install` IPC
///     to the primary; `Stream::create_and_attach` doesn't change.
///     PMD compatibility is no longer the caller's concern.
///
/// Scope:
///   * UDP, not TCP — no connect handshake clutter (RawDatagramCodec).
///   * No actual traffic. We bring up the per-process Platform, attach a
///     `DpdkUdpSocket`, drive `poll()` for a few seconds, then shut down.
///   * The point is the *control plane* (mempool create-vs-lookup, port
///     bring-up vs skip, cleanup branches) — once that's clear, swap in
///     a real codec / connect / loop and this scales to production.
///
/// For the single-process counterpart with full TLS+WS handshake via
/// `create_and_attach`, see `examples/simple_hft.cpp`. For a full
/// real-server probe with reconnect + latency histogram, see
/// `examples/binance_latency.cpp`.
///
/// Source-port partitioning across MP processes: with `mp_topology`
/// set (the path this skeleton drives) the library auto-narrows
/// `find_src_port_for_queue`'s search to `MpTopology::self().port_lo /
/// port_hi` so primary and secondary draw from disjoint segments
/// without the caller doing any manual range arithmetic. The legacy
/// "caller-side hand-partition" mode still works when `mp_topology` is
/// left empty — see `eph-net-dpdk/docs/dpdk-multiprocess.md` "Advanced
/// usage" for that flow.
///
/// Usage (two terminals, same host, same NIC):
///
///   # Terminal A — primary first; brings the port up
///   sudo ./dpdk_mp_demo --role primary --
///                             --file-prefix eph_mp_demo
///                             --pci 0000:28:00.0
///                             --pin 0=0:rx --pin 1=1:tx
///
///   # Terminal B — once primary logs "ready", secondary attaches
///   sudo ./dpdk_mp_demo --role secondary --
///                             --file-prefix eph_mp_demo
///                             --pci 0000:28:00.0
///                             --pin 0=2:rx --pin 1=3:tx
///
/// `--pin lcore_id=cpu_id[:role]` (repeatable) is the typed entry point —
/// goes through `EalGuard::init_with_pins`, which validates each pin
/// (cpu >= 0, no SMT/NUMA conflict per policy) and registers the cpus
/// into the process-wide pin registry BEFORE `rte_eal_init` fires. Any
/// later `eph::utils::pin_thread` on a colliding cpu is detected loudly
/// instead of silently fighting EAL.
///
/// Escape hatch: `--lcores '<raw EAL spec>'` (e.g. `--lcores '(0-1)@(4,5)'`
/// for set-of-sets) bypasses the typed path and goes through the legacy
/// `EalGuard::init` — useful when you need DPDK syntax beyond the 1:1
/// mapping `LcorePin` covers. Mutually exclusive with `--pin` in one call.
///
/// Required environment: NIC bound to vfio-pci, ≥ 256 hugepages free.
/// See `eph-net-dpdk/scripts/dpdk-setup.sh`. Both processes must use the
/// SAME `--file-prefix` so the secondary's `rte_mempool_lookup` can find
/// the primary's shared mempool.
///
/// Additional flags (all optional; full list at parse_args):
///   --port-id <id>    DPDK port enumeration index (default 0).
///   --nb-queues <n>   per-process RX/TX queue count (default 4 — see
///                     `AppArgs::nb_rx_queues` below). Both peers must
///                     agree — `MpTopology::uniform` uses this value to
///                     derive equal sub-ranges per `self_index`, so a
///                     mismatch makes one process's range overlap the
///                     other's.
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
#include "eph/dpdk/lcore_pin.hpp"   // LcorePin / EalGuard::init_with_pins
#include "eph/dpdk/mp_topology.hpp" // MpTopology::uniform
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
    ed::ProcType            role         = ed::ProcType::Primary;
    std::string             file_prefix  = "eph_mp_demo";
    /// EAL flags — --pci / --pin / --lcores / --port-id (or --dpdk-port).
    ed::cli::EalArgs        eal{};
    uint16_t                nb_rx_queues = 4;    // total queues primary configures
    std::chrono::seconds    run_seconds  = 5s;   // how long to drive poll()
};

// Split argv at the first "--" — same convention as simple_hft.cpp.
// Anything before "--" is consumed by the wrapper that selects --role; the
// rest is application config.
int split_app_args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--") return i;
    }
    return argc;
}

AppArgs parse_args(int argc, char** argv) {
    AppArgs out;
    // Pass 1: pre-`--` selects role (so we can shape the EAL args).
    const int sep = split_app_args(argc, argv);
    for (int i = 1; i < sep; ++i) {
        std::string_view a = argv[i];
        if (a == "--role" && i + 1 < sep) {
            std::string_view v = argv[++i];
            out.role = (v == "secondary") ? ed::ProcType::Secondary
                                          : ed::ProcType::Primary;
        }
    }
    // Pass 2: post-`--` is the app-specific config.
    for (int i = sep + 1; i < argc; ++i) {
        std::string_view a = argv[i];
        char const* next = (i + 1 < argc) ? argv[i + 1] : nullptr;

        // EAL flags first.
        auto consumed = ed::cli::try_consume(out.eal, a, next);
        if (!consumed) {
            spdlog::error("dpdk_mp_demo: {}", consumed.error());
            std::exit(1);
        }
        if (*consumed > 0) { i += *consumed - 1; continue; }

        if      (a == "--file-prefix" && next) { out.file_prefix  = next; ++i; }
        else if (a == "--nb-queues"   && next) { out.nb_rx_queues = static_cast<uint16_t>(std::atoi(next)); ++i; }
        else if (a == "--seconds"     && next) { out.run_seconds  = std::chrono::seconds(std::atoi(next));  ++i; }
    }
    return out;
}

// Build a primary or secondary `PlatformConfig` from `AppArgs`.
//
// Recommended path (the one we drive here): the two roles share
// `port_id`, `nb_rx_queues`, `file_prefix` (those MUST match), and
// each declares the same `MpTopology::uniform(self_index, total_procs,
// nb_rx_queues)`. The library auto-derives `rx_queue_range` and the
// per-process src_port window from the topology and cross-validates
// disjointness via the shared hugepage registry. Two numbers per
// process (`self_index`, `total_procs`) — no other coordination.
//
// Legacy path (manual partitioning) — for context, equivalent of this
// would have been:
//
//   const uint16_t mid = a.nb_rx_queues / 2;
//   if (primary)  cfg.rx_queue_range = {0, mid};
//   else          cfg.rx_queue_range = {mid, a.nb_rx_queues};
//   // and then the caller had to allocate src_port from
//   //   primary:   [32768, 49151]
//   //   secondary: [49152, 65535]
//   // by hand on every stream attach.
//
// That path still works (rx_queue_range remains a public field — see
// docs/dpdk-multiprocess.md "Advanced usage: manual partitioning"),
// but the recommended path below is shorter and lets the library
// detect cross-process self_index collisions instead of trusting the
// operator to keep them disjoint.
ed::PlatformConfig make_platform_config(const AppArgs& a) {
    ed::PlatformConfig cfg{};
    cfg.port_id      = a.eal.port_id;
    cfg.nb_rx_queues = a.nb_rx_queues;
    cfg.nb_tx_queues = a.nb_rx_queues;
    cfg.proc_type    = a.role;
    cfg.file_prefix  = a.file_prefix;

    const uint8_t self_index = (a.role == ed::ProcType::Primary) ? 0 : 1;
    cfg.mp_topology = ed::MpTopology::uniform(
        self_index, /*total_procs=*/2, a.nb_rx_queues);
    return cfg;
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    spdlog::set_level(spdlog::level::info);

    const AppArgs args = parse_args(argc, argv);
    if (args.nb_rx_queues < 2) {
        spdlog::error("dpdk_mp_demo: nb_rx_queues must be >= 2 "
                      "(50/50 partition needs disjoint sub-ranges)");
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

    // ── 1) EAL + Platform via the unified create_with_eal factory ────────
    // create_with_eal dispatches by pcfg.proc_type to create_primary
    // (mp_topology auto-derive + registry reserve) or create_secondary
    // (registry attach), and the returned Platform owns the EAL session.
    // Single one-liner replaces the EalGuard::init_with_pins +
    // Platform::create_primary/secondary two-step pattern.
    ed::PlatformConfig pcfg = make_platform_config(args);

    ed::EalConfig eal_cfg = ed::cli::to_eal_config(
        args.eal,
        (args.role == ed::ProcType::Primary) ? "dpdk_mp_demo.primary"
                                             : "dpdk_mp_demo.secondary");
    eal_cfg.proc_type     = args.role;
    eal_cfg.proc_type_set = true;
    eal_cfg.file_prefix   = args.file_prefix;

    const char* role_str = args.role == ed::ProcType::Primary ? "primary" : "secondary";
    if (!args.eal.pins.empty()) {
        spdlog::info("dpdk_mp_demo[{}]: bring-up via create_with_eal "
                     "(typed pins, {} pin(s))", role_str, args.eal.pins.size());
    } else {
        spdlog::info("dpdk_mp_demo[{}]: bring-up via create_with_eal "
                     "(raw lcores='{}')", role_str, args.eal.lcores_raw);
    }

    auto plat_r = ed::Platform::create_with_eal(
        std::move(pcfg), std::move(eal_cfg),
        std::span<ed::LcorePin const>{args.eal.pins},
        eph::utils::CpuPinPolicy{});
    if (!plat_r) {
        spdlog::error("dpdk_mp_demo: Platform::create_with_eal failed: {}",
                      plat_r.error());
        return 2;
    }
    auto platform = std::move(*plat_r);

    const auto qr = platform.effective_rx_queue_range();
    spdlog::info("dpdk_mp_demo[{}]: ready — port={}, "
                 "rx_queue_range=[{},{}), mempool={:p}, file_prefix='{}'",
                 args.role == ed::ProcType::Primary ? "primary" : "secondary",
                 platform.port_id(), qr.first, qr.second,
                 static_cast<void*>(platform.mempool()),
                 args.file_prefix);

    // ── 3) Per-process Poller bound to one of the queues we own ──────────
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

    // ── 4) DpdkUdpSocket via create_and_attach (turnkey factory) ─────────
    // Hardcoded demo tuple. With `mp_topology` set in PlatformConfig (the
    // path this skeleton drives), the library auto-narrows src_port
    // selection to each role's `port_lo / port_hi` window inside
    // `find_src_port_for_queue` — primary and secondary draw from
    // disjoint segments without manual coordination. The legacy hand-
    // partitioned path (`mp_topology` empty + caller picks ports from
    // `[32768, 49151]` / `[49152, 65535]`) still works; see file header
    // and `eph-net-dpdk/docs/dpdk-multiprocess.md` "Advanced usage".
    // This skeleton does not actually drive traffic, so the tuple values
    // here only need to pass validation.
    using UdpSock = edpdk::DpdkUdpSocket<ec::RawDatagramCodec>;

    edpdk::UdpConfig ucfg{};
    ucfg.legacy.src_ip   = 0x0A000010;   // 10.0.0.16
    ucfg.legacy.dst_ip   = 0x0A000020;   // 10.0.0.32
    ucfg.legacy.src_port = (args.role == ed::ProcType::Primary) ? 32768 : 49152;
    ucfg.legacy.dst_port = 30000;
    ucfg.legacy.port_id  = platform.port_id();
    ucfg.legacy.pool     = platform.mempool();

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

    // ── 5) Drive the burst loop for `--seconds` (default 5s) ─────────────
    spdlog::info("dpdk_mp_demo[{}]: entering poll loop for {}s",
                 args.role == ed::ProcType::Primary ? "primary" : "secondary",
                 args.run_seconds.count());
    const auto deadline = std::chrono::steady_clock::now() + args.run_seconds;
    while (g_running.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        (void)poller->poll();
    }

    spdlog::info("dpdk_mp_demo[{}]: shutting down — Platform RAII "
                 "will run the {}-mode cleanup branch",
                 args.role == ed::ProcType::Primary ? "primary" : "secondary",
                 args.role == ed::ProcType::Primary ? "primary" : "secondary");
    // RAII: ~UdpSock → ~DpdkPoller → ~Platform → ~EalGuard.
    // Secondary's ~Platform leaves the port + mempool untouched
    // (owned by primary); primary's ~Platform does dev_stop / dev_close /
    // mempool_free — see Platform::Impl::cleanup.
    return 0;
}
