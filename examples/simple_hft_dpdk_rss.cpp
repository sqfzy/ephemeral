/// @file simple_hft_dpdk_rss.cpp
///
/// Single-process DPDK example: one `Platform` with `enable_rss=true` and
/// `nb_rx_queues > 1`, multiple `DpdkUdpSocket`s spawned via the turnkey
/// `create_and_attach` factory and spread across the NIC's RX queues. The
/// point is the **RSS data plane in single-process mode** — the missing
/// piece between `simple_hft_dpdk.cpp` (single-queue skeleton) and
/// `simple_hft_dpdk_mp.cpp` (multi-process, where RSS is a side effect of
/// queue partitioning).
///
/// What's demonstrated:
///
///   * `Platform::create_primary` with `enable_rss=true` + `nb_rx_queues=N`,
///     hard-failing on RSS bring-up (no silent collapse to queue 0 — see
///     `eph-net-dpdk/CHANGELOG.md`).
///   * The four diagnostic getters that tell you what RSS path resolved:
///         `dispatch_mode()` /
///         `rss_using_probed_key()` / `effective_rx_queue_range()`.
///     The probed-key path lights up on ENA where the PMD rejects
///     `rte_eth_dev_rss_hash_update`; we read back the NIC's own key via
///     `rte_eth_dev_rss_hash_conf_get` and use that for `predict_rss_queue`.
///   * One `DpdkPoller` per owned RX queue (registered with
///     `Platform::register_poller`), driven sequentially from one lcore.
///     A real production deploy would pin one Poller per lcore.
///   * `DpdkUdpSocket::create_and_attach` with `pin_to_queue = q` for
///     each connection — the helper reverse-picks an ephemeral
///     `src_port` so the inbound 5-tuple Toeplitz-hashes back to `q`.
///     The library logs the picked port at INFO; that's what makes the
///     RSS pinning visible.
///   * Smoke-boot tolerance: `create_and_attach` may legitimately fail
///     (RSS port-search exhausted, ARP not resolved, MACs unset) — we
///     warn and continue so the cleanup branches still get exercised.
///
/// What's deliberately out of scope:
///
///   * No actual traffic. UDP has no handshake, so once the sockets are
///     created and the pollers attached, the demo just drives `poll()`
///     for `--seconds` and exits. Swap in a real `OnDatagram` callback +
///     send loop to drive production traffic.
///   * No ARP/DNS resolution. `dst_mac` is left zero — bytes won't egress
///     a real router, but that's not what this example exists to show.
///     For a real-server probe with full control plane, see
///     `examples/binance_latency.cpp`.
///   * Multi-process mode. See `examples/simple_hft_dpdk_mp.cpp` and
///     `eph-net-dpdk/docs/dpdk-multiprocess.md` for that surface.
///   * `RxDispatchMode::FlowDirector`. The same `create_and_attach` API
///     handles FD via rte_flow rule install when the NIC supports it;
///     selected automatically by `Platform::detect_rx_dispatch_mode()`.
///     This example forces `enable_rss=true` to make the RssPartitioned
///     path the focus.
///
/// Required environment: NIC bound to vfio-pci, ≥ 256 hugepages free.
/// See `eph-net-dpdk/scripts/dpdk-setup.sh`.
///
/// Usage:
///
///   sudo ./simple_hft_dpdk_rss --
///        --pci 0000:28:00.0
///        --pin 0=4 --pin 1=5 --pin 2=6 --pin 3=7
///        --nb-queues 4 --connections 4
///        --src-ip 10.0.0.16 --dst-ip 10.0.0.32 --dst-port 30000
///
/// Recommended log level for this demo is `info`: the library's
/// "create_and_attach: RSS pin → src_port=… hashes to queue=…" log line
/// is the only place the helper's reverse-pick is surfaced.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/codec/raw_datagram_codec.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/lcore_pin.hpp"   // LcorePin / EalGuard::init_with_pins
#include "eph/dpdk/platform.hpp"
#include "eph/net/dpdk/flow_steering.hpp"  // RxDispatchMode / rx_dispatch_mode_name
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
    std::string             pci          = "";   // -a passthrough; empty = all
    std::vector<ed::LcorePin> pins;
    std::string             lcores       = "";   // raw `--lcores` escape hatch
    uint16_t                port_id      = 0;
    uint16_t                nb_rx_queues = 4;
    uint16_t                connections  = 4;    // # of UDP sockets to spawn
    uint32_t                src_ip       = 0x0A000010;  // 10.0.0.16
    uint32_t                dst_ip       = 0x0A000020;  // 10.0.0.32
    uint16_t                dst_port     = 30000;
    std::chrono::seconds    run_seconds  = 5s;
    /// @brief Per-lcore / NUMA-aware mempools (T2.9). When > 0, Platform
    /// creates that many additional pools (one per lcore id, allocated on
    /// the lcore's local NUMA socket). `create_and_attach` then honours
    /// `UdpConfig::pool_lcore_hint = (i % per_lcore_pools)` per
    /// connection so each socket draws from a pool local to its lcore —
    /// the +50–100 ns cross-NUMA mbuf alloc penalty disappears on multi-
    /// socket hosts. `0` (default) preserves the byte-for-byte single-pool
    /// shape this example uses unless opted in.
    uint16_t                per_lcore_pools = 0;
};

/// Parse `--pin lcore=cpu[:role]`. Mirrors simple_hft_dpdk_mp.cpp.
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

int split_app_args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--") return i;
    }
    return argc;
}

AppArgs parse_args(int argc, char** argv) {
    AppArgs out;
    const int sep = split_app_args(argc, argv);
    for (int i = sep + 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if      (a == "--pci"         && i + 1 < argc) out.pci          = argv[++i];
        else if (a == "--lcores"      && i + 1 < argc) out.lcores       = argv[++i];
        else if (a == "--pin"         && i + 1 < argc) {
            auto p = parse_pin_spec(argv[++i]);
            if (!p) {
                spdlog::error("simple_hft_dpdk_rss: {}", p.error());
                std::exit(1);
            }
            out.pins.push_back(std::move(*p));
        }
        else if (a == "--port-id"     && i + 1 < argc) out.port_id      = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--nb-queues"   && i + 1 < argc) out.nb_rx_queues = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--connections" && i + 1 < argc) out.connections  = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--src-ip"      && i + 1 < argc) out.src_ip       = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 16));
        else if (a == "--dst-ip"      && i + 1 < argc) out.dst_ip       = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 16));
        else if (a == "--dst-port"    && i + 1 < argc) out.dst_port     = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--seconds"     && i + 1 < argc) out.run_seconds  = std::chrono::seconds(std::atoi(argv[++i]));
        else if (a == "--per-lcore-pools" && i + 1 < argc) out.per_lcore_pools = static_cast<uint16_t>(std::atoi(argv[++i]));
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
        spdlog::error("simple_hft_dpdk_rss: --nb-queues must be >= 2 "
                      "(RSS demo needs multi-queue dispatch)");
        return 1;
    }
    if (args.connections == 0) {
        spdlog::error("simple_hft_dpdk_rss: --connections must be >= 1");
        return 1;
    }

    // ── Validate --pin / --lcores exclusivity (same shape as mp.cpp) ──────
    const bool typed_pins = !args.pins.empty();
    const bool raw_lcores = !args.lcores.empty();
    if (typed_pins && raw_lcores) {
        spdlog::error("simple_hft_dpdk_rss: --pin and --lcores are mutually "
                      "exclusive; pick one");
        return 1;
    }
    if (!typed_pins && !raw_lcores) {
        spdlog::error("simple_hft_dpdk_rss: provide either --pin lcore=cpu[:role] "
                      "(typed) or --lcores '<raw EAL spec>' (escape hatch)");
        return 1;
    }

    // ── 1) EAL init ───────────────────────────────────────────────────────
    ed::EalConfig eal_cfg{};
    eal_cfg.program_name = "simple_hft_dpdk_rss";
    if (raw_lcores) eal_cfg.lcores = {args.lcores};
    if (!args.pci.empty()) eal_cfg.allowed_devs = {args.pci};

    std::expected<ed::EalGuard, std::string> eal = std::unexpected(std::string{});
    if (typed_pins) {
        spdlog::info("simple_hft_dpdk_rss: EAL init via init_with_pins ({} pin(s))",
                     args.pins.size());
        eal = ed::EalGuard::init_with_pins(eal_cfg, args.pins,
                                           eph::utils::CpuPinPolicy{});
    } else {
        spdlog::info("simple_hft_dpdk_rss: EAL init via raw lcores='{}'",
                     args.lcores);
        auto argv_owned = ed::build_eal_argv(eal_cfg);
        std::vector<char*> argv_ptrs;
        argv_ptrs.reserve(argv_owned.size());
        for (auto& s : argv_owned) argv_ptrs.push_back(s.data());
        eal = ed::EalGuard::init(static_cast<int>(argv_ptrs.size()),
                                 argv_ptrs.data());
    }
    if (!eal) {
        spdlog::error("simple_hft_dpdk_rss: EAL init failed: {}", eal.error());
        return 2;
    }

    // ── 2) Platform with RSS enabled ──────────────────────────────────────
    // Single-process: no `proc_type` / `file_prefix` / `rx_queue_range`
    // overrides. `effective_rx_queue_range()` below resolves the sentinel
    // `{0, 0}` to `[0, nb_rx_queues)`.
    ed::PlatformConfig pcfg{};
    pcfg.port_id          = args.port_id;
    pcfg.nb_rx_queues     = args.nb_rx_queues;
    pcfg.nb_tx_queues     = args.nb_rx_queues;
    pcfg.per_lcore_pools  = args.per_lcore_pools;  // 0 = single shared pool

    auto plat_r = ed::Platform::create_primary(std::move(pcfg));
    if (!plat_r) {
        spdlog::error("simple_hft_dpdk_rss: Platform::create_primary failed: {}",
                      plat_r.error());
        return 3;
    }
    auto platform = std::move(*plat_r);

    // ── 3) RSS diagnostics — what path actually resolved? ────────────────
    // `rss_using_probed_key()` lights up when `configure_rss` is rejected
    // by the PMD (notably ENA on AWS) and we fell back to reading the
    // NIC's own hash key for `predict_rss_queue`.
    const auto qr = platform.effective_rx_queue_range();
    spdlog::info(
        "simple_hft_dpdk_rss: Platform up — port={}, nb_rx_queues={}, "
        "dispatch_mode={}, is_rss_active={}, rss_using_probed_key={}, "
        "rx_queue_range=[{},{})",
        platform.port_id(), platform.nb_rx_queues(),
        edpdk::rx_dispatch_mode_name(platform.dispatch_mode()),
        platform.dispatch_mode() == edpdk::RxDispatchMode::RssPartitioned,
        platform.rss_using_probed_key(),
        qr.first, qr.second);

    if (platform.dispatch_mode() != edpdk::RxDispatchMode::RssPartitioned) {
        spdlog::warn("simple_hft_dpdk_rss: NIC did not resolve to "
                     "RssPartitioned mode — RSS-pinned src_port path will "
                     "not engage. The example continues so cleanup branches "
                     "are exercised, but this is not what the demo is for.");
    }

    // ── 4) One Poller per owned RX queue, registered with Platform ───────
    // Single-lcore demo: we drive every Poller from this thread's loop in
    // round-robin. A production deploy would create one std::thread per
    // queue, pin it to its own cpu (via eph::utils::pin_thread), and run
    // poll() in a tight burst loop.
    std::vector<std::unique_ptr<edpdk::DpdkPoller<>>> pollers;
    pollers.reserve(qr.second - qr.first);
    for (uint16_t q = qr.first; q < qr.second; ++q) {
        edpdk::PollerConfig poller_cfg{};
        poller_cfg.port_id     = platform.port_id();
        poller_cfg.rx_queue_id = q;
        auto pr = edpdk::DpdkPoller<>::create(poller_cfg);
        if (!pr) {
            spdlog::error("simple_hft_dpdk_rss: DpdkPoller::create(queue={}) "
                          "failed: {}", q, pr.error().detail);
            return 4;
        }
        if (auto r = platform.register_poller(q, pr->get()); !r) {
            spdlog::error("simple_hft_dpdk_rss: register_poller(queue={}) "
                          "failed: {}", q, r.error().detail);
            return 5;
        }
        pollers.emplace_back(std::move(*pr));
        spdlog::info("simple_hft_dpdk_rss: poller registered for queue={}", q);
    }

    // ── 5) Spawn UDP sockets, each pinned to a distinct RX queue ─────────
    // For each connection i: ask `create_and_attach` to land the inbound
    // 5-tuple on `queue = qr.first + (i % nb_owned_queues)`. The helper
    // walks `[32768, 60999]` looking for an ephemeral src_port whose
    // Toeplitz hash matches that queue under the NIC's RSS key, rebinds
    // `cfg.legacy.src_port` to the picked value, and constructs the
    // sender. The library logs the picked port at INFO — watch for
    // "RSS pin → src_port=… hashes to queue=…".
    using UdpSock = edpdk::DpdkUdpSocket<ec::RawDatagramCodec>;
    std::vector<std::unique_ptr<UdpSock>> sockets;
    sockets.reserve(args.connections);

    const uint16_t nb_owned = static_cast<uint16_t>(qr.second - qr.first);
    for (uint16_t i = 0; i < args.connections; ++i) {
        const uint16_t want_q = qr.first + (i % nb_owned);

        edpdk::UdpConfig ucfg{};
        ucfg.legacy.src_ip   = args.src_ip;
        ucfg.legacy.dst_ip   = args.dst_ip;
        // src_port populated as a placeholder: validate() requires non-zero.
        // The RSS-pin path overwrites it with the reverse-picked ephemeral.
        ucfg.legacy.src_port = static_cast<uint16_t>(32768 + i);
        ucfg.legacy.dst_port = args.dst_port;
        ucfg.legacy.port_id  = platform.port_id();
        ucfg.legacy.pool     = platform.mempool();
        // dst_mac left zero — would need ARP resolution to drive real
        // traffic. See eph::dpdk::arp::resolve / examples/binance_latency.cpp.

        ucfg.pin_to_queue = want_q;

        // Per-lcore mempool hint (T2.9, NUMA-aware alloc). When
        // `per_lcore_pools > 0`, each socket gets a pool local to its
        // own lcore — Platform clamps `hint` against the pool count, so
        // `i % per_lcore_pools` is safe.  When `per_lcore_pools == 0`,
        // leave the hint at -1 so the helper uses `legacy.pool` as set
        // (the single shared pool, byte-for-byte the pre-T2.9 path).
        if (args.per_lcore_pools > 0) {
            ucfg.pool_lcore_hint =
                static_cast<int>(i % args.per_lcore_pools);
        }

        auto sr = UdpSock::create_and_attach(std::move(ucfg), platform);
        if (!sr) {
            spdlog::warn(
                "simple_hft_dpdk_rss: create_and_attach[{}] (pin queue={}) "
                "failed: {} -- continuing so RAII cleanup is exercised",
                i, want_q, sr.error().detail);
            continue;
        }
        spdlog::info("simple_hft_dpdk_rss: socket[{}] attached, pinned to queue={}",
                     i, want_q);
        sockets.emplace_back(std::move(*sr));
    }
    spdlog::info("simple_hft_dpdk_rss: {}/{} sockets attached",
                 sockets.size(), args.connections);

    // ── 6) Drive every Poller in a single-lcore round-robin for N seconds ─
    // Production code would split this across lcores (one thread per
    // queue, each pinned to its own cpu). Round-robin on one lcore keeps
    // the example small while still exercising every queue's burst path.
    spdlog::info("simple_hft_dpdk_rss: entering poll loop for {}s",
                 args.run_seconds.count());
    const auto deadline = std::chrono::steady_clock::now() + args.run_seconds;
    while (g_running.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        for (auto& p : pollers) (void)p->poll();
    }

    spdlog::info("simple_hft_dpdk_rss: shutting down — RAII teardown order: "
                 "sockets → pollers → platform → eal");
    // RAII: ~UdpSock (each socket unregisters from its Poller) →
    //       ~DpdkPoller (each poller unregisters from Platform) →
    //       ~Platform (dev_stop / dev_close / mempool_free in primary mode) →
    //       ~EalGuard (rte_eal_cleanup).
    return 0;
}
