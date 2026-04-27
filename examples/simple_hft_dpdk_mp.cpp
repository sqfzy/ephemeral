/// @file simple_hft_dpdk_mp.cpp
///
/// DPDK single-NIC multi-process (primary + secondary) skeleton.
/// One binary that plays *both* roles, selected by `--role primary|secondary`
/// on the command line. Mirrors the spirit of `simple_hft_dpdk.cpp` but
/// brings in the MP scaffolding shipped with `eph-net-dpdk`:
///
///   * `eph::dpdk::EalConfig` + `build_eal_argv` — typed assembly of
///     `--proc-type` / `--file-prefix` / `-l` / `-a`.
///   * `eph::dpdk::Platform::create_primary` / `create_secondary` — the
///     two factories that gate access to the shared NIC.
///   * `PlatformConfig::file_prefix` + `rx_queue_range` — the only
///     fields callers must set differently between roles. Both processes
///     pass the same `file_prefix`; the queue range carves the NIC's
///     RX queues into disjoint sub-ranges (this skeleton uses a 50/50
///     partition over `nb_rx_queues = 4`).
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
/// Source-port partitioning across MP processes is the *caller*'s
/// responsibility — `eph-net-dpdk` does not auto-allocate src_port and
/// has no global view to enforce disjointness. This skeleton keeps the
/// demo tuple constant and would not actually share a real NIC; in a
/// production deploy each role must pick a src_port from a sub-range
/// disjoint from the other process. See `eph-net-dpdk/docs/dpdk-multiprocess.md`
/// for partitioning guidance.
///
/// Usage (two terminals, same host, same NIC):
///
///   # Terminal A — primary first; brings the port up
///   sudo ./simple_hft_dpdk_mp --role primary --
///                             --file-prefix eph_mp_demo
///                             --pci 0000:28:00.0
///                             --lcores 0,1
///
///   # Terminal B — once primary logs "ready", secondary attaches
///   sudo ./simple_hft_dpdk_mp --role secondary --
///                             --file-prefix eph_mp_demo
///                             --pci 0000:28:00.0
///                             --lcores 2,3
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
#include "eph/dpdk/platform.hpp"
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/dpdk/udp_socket.hpp"

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
    std::string             lcores       = "0,1";
    uint16_t                port_id      = 0;
    uint16_t                nb_rx_queues = 4;    // total queues primary configures
    std::chrono::seconds    run_seconds  = 5s;   // how long to drive poll()
};

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
        else if (a == "--port-id"     && i + 1 < argc) out.port_id      = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--nb-queues"   && i + 1 < argc) out.nb_rx_queues = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--seconds"     && i + 1 < argc) out.run_seconds  = std::chrono::seconds(std::atoi(argv[++i]));
    }
    return out;
}

// Build a primary or secondary `PlatformConfig` from `AppArgs`.
//
// The two roles share `port_id`, `nb_rx_queues`, `file_prefix` (those MUST
// match), and differ only on `proc_type` + the half of `rx_queue_range`
// they own.
ed::PlatformConfig make_platform_config(const AppArgs& a) {
    ed::PlatformConfig cfg{};
    cfg.port_id      = a.port_id;
    cfg.nb_rx_queues = a.nb_rx_queues;
    cfg.nb_tx_queues = a.nb_rx_queues;
    cfg.enable_rss   = (a.nb_rx_queues > 1);
    cfg.proc_type    = a.role;
    cfg.file_prefix  = a.file_prefix;

    // 50/50 partition. Primary owns the lower half, secondary the upper.
    // `validate_config` will reject anything outside [0, nb_rx_queues].
    const uint16_t mid = static_cast<uint16_t>(a.nb_rx_queues / 2);
    if (a.role == ed::ProcType::Primary)
        cfg.rx_queue_range = {uint16_t{0}, mid};
    else
        cfg.rx_queue_range = {mid, a.nb_rx_queues};
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

    // ── 1) EAL init via EalConfig ─────────────────────────────────────────
    ed::EalConfig eal_cfg{};
    eal_cfg.program_name  = (args.role == ed::ProcType::Primary)
                                ? "simple_hft_dpdk_mp.primary"
                                : "simple_hft_dpdk_mp.secondary";
    eal_cfg.proc_type     = args.role;
    eal_cfg.proc_type_set = true;
    eal_cfg.file_prefix   = args.file_prefix;
    eal_cfg.lcores        = {args.lcores};
    if (!args.pci.empty()) eal_cfg.allowed_devs = {args.pci};

    auto argv_owned = ed::build_eal_argv(eal_cfg);
    std::vector<char*> argv_ptrs;
    argv_ptrs.reserve(argv_owned.size());
    for (auto& s : argv_owned) argv_ptrs.push_back(s.data());

    auto eal = ed::EalGuard::init(static_cast<int>(argv_ptrs.size()),
                                  argv_ptrs.data());
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
                     "src_mac / dst_mac (e.g. via arp_resolve()) to drive "
                     "real traffic.");
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
