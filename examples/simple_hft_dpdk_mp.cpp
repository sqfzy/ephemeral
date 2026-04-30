/// @file simple_hft_dpdk_mp.cpp
///
/// DPDK single-NIC multi-process (primary + secondary) **declarative**
/// path skeleton. Each process declares its own `MpTopology` and
/// `Platform::create_primary` / `create_secondary` does the
/// bring-up. For the **autojoin** alternative — same scenario but
/// zero coordination between peers (no shared file_prefix, no
/// manual self_index, launch order is the only "agreement") — see
/// `examples/simple_hft_dpdk_mp_dynamic.cpp`.
///
/// One binary that plays *both* roles, selected by `--role primary|secondary`
/// on the command line. Mirrors the spirit of `simple_hft_dpdk.cpp` but
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
/// For the lower-level "raw create()" pattern without the MP machinery,
/// see `examples/simple_hft_dpdk.cpp`. For a full real-server probe (TLS
/// + WS + ARP + DNS + reconnect), see `examples/binance_latency.cpp`.
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
///   sudo ./simple_hft_dpdk_mp --role primary --
///                             --file-prefix eph_mp_demo
///                             --pci 0000:28:00.0
///                             --pin 0=0:rx --pin 1=1:tx
///
///   # Terminal B — once primary logs "ready", secondary attaches
///   sudo ./simple_hft_dpdk_mp --role secondary --
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
    std::string             pci          = "";   // -a passthrough; empty = all
    /// Typed lcore→cpu pin specs (preferred path, populated from --pin).
    /// Mutually exclusive with `lcores` raw escape hatch.
    std::vector<ed::LcorePin> pins;
    /// Raw `--lcores` string, passed verbatim through EalConfig::lcores
    /// to DPDK. Empty by default — set only when the demo needs DPDK
    /// syntax `LcorePin` cannot express (set-of-sets etc).
    std::string             lcores       = "";
    uint16_t                port_id      = 0;
    uint16_t                nb_rx_queues = 4;    // total queues primary configures
    std::chrono::seconds    run_seconds  = 5s;   // how long to drive poll()
};

/// Parse a single `--pin` token: `lcore=cpu` or `lcore=cpu:role`.
/// `lcore` and `cpu` must be non-negative integers; `role` is free-form
/// (empty allowed). Whitespace is not stripped — keep the spec compact.
std::expected<ed::LcorePin, std::string>
parse_pin_spec(std::string_view s) {
    auto eq = s.find('=');
    if (eq == std::string_view::npos || eq == 0 || eq + 1 == s.size()) {
        return std::unexpected(std::string{
            "--pin: expected 'lcore=cpu[:role]', got '"} + std::string{s} + "'");
    }
    auto col = s.find(':', eq + 1);
    auto cpu_end = (col == std::string_view::npos) ? s.size() : col;

    auto lcore_str = std::string(s.substr(0, eq));
    auto cpu_str   = std::string(s.substr(eq + 1, cpu_end - eq - 1));
    int  lcore     = std::atoi(lcore_str.c_str());
    int  cpu       = std::atoi(cpu_str.c_str());
    if (lcore < 0 || cpu < 0) {
        return std::unexpected(std::string{
            "--pin: lcore_id and cpu_id must be non-negative, got '"}
            + std::string{s} + "'");
    }
    std::string role = (col == std::string_view::npos)
                           ? std::string{}
                           : std::string(s.substr(col + 1));
    return ed::LcorePin{static_cast<uint16_t>(lcore), cpu, std::move(role)};
}

// Split argv at the first "--" — same convention as simple_hft_dpdk.cpp.
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
        if      (a == "--file-prefix" && i + 1 < argc) out.file_prefix  = argv[++i];
        else if (a == "--pci"         && i + 1 < argc) out.pci          = argv[++i];
        else if (a == "--lcores"      && i + 1 < argc) out.lcores       = argv[++i];
        else if (a == "--pin"         && i + 1 < argc) {
            auto p = parse_pin_spec(argv[++i]);
            if (!p) {
                spdlog::error("simple_hft_dpdk_mp: {}", p.error());
                std::exit(1);
            }
            out.pins.push_back(std::move(*p));
        }
        else if (a == "--port-id"     && i + 1 < argc) out.port_id      = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--nb-queues"   && i + 1 < argc) out.nb_rx_queues = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--seconds"     && i + 1 < argc) out.run_seconds  = std::chrono::seconds(std::atoi(argv[++i]));
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
    cfg.port_id      = a.port_id;
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
        spdlog::error("simple_hft_dpdk_mp: nb_rx_queues must be >= 2 "
                      "(50/50 partition needs disjoint sub-ranges)");
        return 1;
    }

    // ── Validate --pin / --lcores exclusivity ─────────────────────────────
    // The two paths are mutually exclusive in one EalGuard call — see
    // init_with_pins's contract. We surface the choice here so the
    // diagnostic is application-level (file/flag-aware), not deep in
    // eph-net-dpdk's expected-error string.
    const bool typed_pins = !args.pins.empty();
    const bool raw_lcores = !args.lcores.empty();
    if (typed_pins && raw_lcores) {
        spdlog::error("simple_hft_dpdk_mp: --pin and --lcores are mutually "
                      "exclusive (typed and raw EAL paths); pick one");
        return 1;
    }
    if (!typed_pins && !raw_lcores) {
        spdlog::error("simple_hft_dpdk_mp: provide either --pin lcore=cpu[:role] "
                      "(preferred typed path) or --lcores '<raw EAL spec>' "
                      "(escape hatch for set-of-sets / coremask syntax)");
        return 1;
    }

    // ── 1) EAL init ───────────────────────────────────────────────────────
    // Two paths share the same EalConfig scaffolding; the difference is
    // whether we go through `init_with_pins` (typed) or the legacy
    // `init` after `build_eal_argv` (raw).
    ed::EalConfig eal_cfg{};
    eal_cfg.program_name  = (args.role == ed::ProcType::Primary)
                                ? "simple_hft_dpdk_mp.primary"
                                : "simple_hft_dpdk_mp.secondary";
    eal_cfg.proc_type     = args.role;
    eal_cfg.proc_type_set = true;
    eal_cfg.file_prefix   = args.file_prefix;
    if (raw_lcores) eal_cfg.lcores = {args.lcores};
    if (!args.pci.empty()) eal_cfg.allowed_devs = {args.pci};

    std::expected<ed::EalGuard, std::string> eal = std::unexpected(std::string{});
    if (typed_pins) {
        // Typed path: pins go through pre-EAL validation + registry write
        // before rte_eal_init runs. SMT/NUMA/IRQ checks default to relaxed
        // (CpuPinPolicy{}) — production deployments may want to flip
        // require_no_sibling_conflict / require_same_numa to true after
        // confirming their cpu layout.
        spdlog::info("simple_hft_dpdk_mp[{}]: EAL init via init_with_pins "
                     "({} pin(s))",
                     args.role == ed::ProcType::Primary ? "primary" : "secondary",
                     args.pins.size());
        eal = ed::EalGuard::init_with_pins(eal_cfg, args.pins,
                                           eph::utils::CpuPinPolicy{});
    } else {
        // Raw path: legacy EalConfig::lcores → build_eal_argv → init.
        // No pin registry interaction here; downstream pin_thread calls
        // will not see EAL's lcore cpus and may collide silently.
        spdlog::info("simple_hft_dpdk_mp[{}]: EAL init via raw lcores='{}' "
                     "(legacy path; consider --pin for typed validation)",
                     args.role == ed::ProcType::Primary ? "primary" : "secondary",
                     args.lcores);
        auto argv_owned = ed::build_eal_argv(eal_cfg);
        std::vector<char*> argv_ptrs;
        argv_ptrs.reserve(argv_owned.size());
        for (auto& s : argv_owned) argv_ptrs.push_back(s.data());
        eal = ed::EalGuard::init(static_cast<int>(argv_ptrs.size()),
                                 argv_ptrs.data());
    }
    if (!eal) {
        spdlog::error("simple_hft_dpdk_mp: EAL init failed: {}", eal.error());
        return 2;
    }

    // ── 2) Platform — primary brings the port up; secondary attaches ──────
    ed::PlatformConfig pcfg = make_platform_config(args);
    auto plat_r = (args.role == ed::ProcType::Primary)
                      ? ed::Platform::create_primary(std::move(pcfg))
                      : ed::Platform::create_secondary(std::move(pcfg));
    if (!plat_r) {
        spdlog::error("simple_hft_dpdk_mp: Platform::create_{} failed: {}",
                      args.role == ed::ProcType::Primary ? "primary"
                                                         : "secondary",
                      plat_r.error());
        return 3;
    }
    auto platform = std::move(*plat_r);

    const auto qr = platform.effective_rx_queue_range();
    spdlog::info("simple_hft_dpdk_mp[{}]: ready — port={}, "
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
    // Hardcoded demo tuple. In production each role MUST allocate
    // src_port from a sub-range disjoint from the other process — see
    // file header. This skeleton does not actually drive traffic, so the
    // tuple values don't matter beyond passing validation.
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
        spdlog::warn("simple_hft_dpdk_mp: create_and_attach failed "
                     "(expected on a smoke-boot without ARP-resolved peer): {}",
                     sock_r.error().detail);
        spdlog::info("simple_hft_dpdk_mp: skeleton path — populate "
                     "src_mac / dst_mac (e.g. via eph::dpdk::arp::resolve) "
                     "to drive real traffic.");
        // Fall through to the poll loop anyway so the cleanup branches
        // are exercised end-to-end on every run.
    }

    // ── 5) Drive the burst loop for `--seconds` (default 5s) ─────────────
    spdlog::info("simple_hft_dpdk_mp[{}]: entering poll loop for {}s",
                 args.role == ed::ProcType::Primary ? "primary" : "secondary",
                 args.run_seconds.count());
    const auto deadline = std::chrono::steady_clock::now() + args.run_seconds;
    while (g_running.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        (void)poller->poll();
    }

    spdlog::info("simple_hft_dpdk_mp[{}]: shutting down — Platform RAII "
                 "will run the {}-mode cleanup branch",
                 args.role == ed::ProcType::Primary ? "primary" : "secondary",
                 args.role == ed::ProcType::Primary ? "primary" : "secondary");
    // RAII: ~UdpSock → ~DpdkPoller → ~Platform → ~EalGuard.
    // Secondary's ~Platform leaves the port + mempool untouched
    // (owned by primary); primary's ~Platform does dev_stop / dev_close /
    // mempool_free — see Platform::Impl::cleanup.
    return 0;
}
