/// @file multi_port_platform_demo.cpp
///
/// DPDK aggregator example: one process, *N* physical NIC ports, each
/// owned by an independent `eph::dpdk::Platform`, all wrapped under a
/// single `eph::dpdk::MultiPortPlatform`. The canonical use case is
/// "MD on NIC A, OE on NIC B" or "redundant feeds on NIC A and NIC B"
/// — two coarse-grained roles in one process, no fused state across
/// them.
///
/// What's demonstrated:
///
///   * `MultiPortPlatform::create(span<PlatformConfig>)` — atomic N-port
///     bringup with rollback. Pre-validates every config (validate_config
///     + distinct port_id check) before any DPDK state is touched, then
///     calls `Platform::create` per port. If port K fails, ports
///     `[0, K)` are reaped via the local vector's RAII before the error
///     surfaces.
///   * `num_ports()` / `port(i)` indexed access — the slot index is the
///     position in the input span, NOT the DPDK `port_id`. The example
///     prints both side-by-side so the distinction is unambiguous.
///   * `find_index_by_port_id(dpdk_port_id)` — cold linear scan that
///     translates an externally-known DPDK port id into an aggregator
///     slot. We exercise it once after bringup as a sanity check.
///   * One `DpdkPoller` per port (single queue each, queue 0), driven
///     sequentially from the main lcore. Production code would split
///     across lcores; this keeps the demo's main() short.
///
/// What's deliberately out of scope:
///
///   * RSS multi-queue per port — see `examples/dpdk_rss_demo.cpp`.
///     `MultiPortPlatform` is orthogonal to RSS: each owned Platform can
///     independently turn on `enable_rss + nb_rx_queues > 1`. We pin
///     `nb_rx_queues = 1` here to keep the focus on the aggregator.
///   * Cross-port ICMP routing or fused multi-port Poller — by design.
///     See the file header on `eph/dpdk/multi_port_platform.hpp` for the
///     full rationale.
///   * Real traffic. Each port gets a Poller and a brief poll() loop
///     just to exercise the per-port hot path; no streams are attached.
///     A real app would wire `DpdkTcpStream::create_and_attach` per
///     port (passing `mp.port(i)` as the Platform reference).
///
/// Required environment: every PCI address passed via `--pci` must be
/// bound to vfio-pci with hugepages backing it. This demo needs *at
/// least two* PCI args — a single port is the dominant case and goes
/// through `Platform::create` directly, no aggregator needed.
///
/// Usage (typed pin syntax — recommended):
///
///   sudo ./multi_port_platform_demo --
///        --pci 0000:28:00.0 --pci 0000:28:00.1
///        --pin 0=4 --pin 1=5
///        --seconds 3
///
/// Alternative escape-hatch syntax (for raw EAL `--lcores` specs that
/// `LcorePin` cannot express, e.g. `(0-3)@(4-7)`):
///
///   sudo ./multi_port_platform_demo --
///        --pci 0000:28:00.0 --pci 0000:28:00.1
///        --lcores '0@4,1@5'
///        --seconds 3
///
/// `--pin` and `--lcores` are mutually exclusive (the program rejects
/// the call with a diagnostic if both are given). `--pci <addr>` is
/// repeatable; the K-th occurrence becomes `port_id = K` in DPDK's
/// port enumeration (allowlist order). The example builds one
/// `PlatformConfig` per port, with `port_id` matching the slot index
/// 1:1.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/dpdk/cli.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/lcore_pin.hpp"
#include "eph/dpdk/multi_port_platform.hpp"
#include "eph/dpdk/platform.hpp"
#include "eph/net/dpdk/flow_steering.hpp"
#include "eph/net/dpdk/poller.hpp"
#include "eph/utils/cpu.hpp"

namespace edpdk = eph::net::dpdk;
namespace ed    = eph::dpdk;
using namespace std::chrono_literals;

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running.store(false, std::memory_order_release); }

namespace {

struct AppArgs {
    /// EAL flags. `eal.pci` is the repeatable `--pci` accumulator —
    /// MultiPortPlatform brings up one Platform per entry, in order.
    ed::cli::EalArgs          eal{};
    std::chrono::seconds      run_seconds = 3s;
};

int split_app_args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) == "--") return i;
    return argc;
}

AppArgs parse_args(int argc, char** argv) {
    AppArgs out;
    const int sep = split_app_args(argc, argv);
    for (int i = sep + 1; i < argc; ++i) {
        std::string_view a = argv[i];
        char const* next = (i + 1 < argc) ? argv[i + 1] : nullptr;

        // EAL flags first. --pci accumulates because the helper always pushes
        // back; this is the multi-NIC entry point.
        auto consumed = ed::cli::try_consume(out.eal, a, next);
        if (!consumed) { spdlog::error("multi_port_platform_demo: {}", consumed.error()); std::exit(1); }
        if (*consumed > 0) { i += *consumed - 1; continue; }

        if (a == "--seconds" && next) { out.run_seconds = std::chrono::seconds(std::atoi(next)); ++i; }
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    spdlog::set_level(spdlog::level::info);

    AppArgs args = parse_args(argc, argv);
    if (args.eal.pci.size() < 2) {
        spdlog::error("multi_port_platform_demo: need at least 2 --pci entries "
                      "(single-port case goes through Platform::create "
                      "directly — see simple_hft.cpp)");
        return 1;
    }
    if (auto v = ed::cli::validate(args.eal); !v) {
        spdlog::error("multi_port_platform_demo: {}", v.error());
        return 1;
    }
    if (args.eal.pins.empty() && args.eal.lcores_raw.empty()) {
        spdlog::error("multi_port_platform_demo: provide --pin lcore=cpu[:role] "
                      "(typed) or --lcores '<raw EAL spec>' (escape hatch)");
        return 1;
    }

    // ── 1) EAL init ───────────────────────────────────────────────────────
    // Every --pci entry is forwarded as an EAL allowlist (-a) entry. DPDK
    // enumerates the allowed devs in the order they appear, so the first
    // --pci becomes port_id=0, the second port_id=1, etc.
    const std::size_t n_ports = args.eal.pci.size();
    const bool typed_pins = !args.eal.pins.empty();
    auto pins_for_init    = args.eal.pins;  // copy — to_eal_config consumes args.eal
    ed::EalConfig eal_cfg = ed::cli::to_eal_config(std::move(args.eal),
                                                   "multi_port_platform_demo");

    std::expected<ed::EalGuard, std::string> eal = std::unexpected(std::string{});
    if (typed_pins) {
        spdlog::info("multi_port_platform_demo: EAL init via init_with_pins ({} pin(s))",
                     pins_for_init.size());
        eal = ed::EalGuard::init_with_pins(eal_cfg, pins_for_init,
                                           eph::utils::CpuPinPolicy{});
    } else {
        spdlog::info("multi_port_platform_demo: EAL init via raw lcores");
        auto argv_owned = ed::build_eal_argv(eal_cfg);
        std::vector<char*> argv_ptrs;
        argv_ptrs.reserve(argv_owned.size());
        for (auto& s : argv_owned) argv_ptrs.push_back(s.data());
        eal = ed::EalGuard::init(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());
    }
    if (!eal) {
        spdlog::error("multi_port_platform_demo: EAL init failed: {}", eal.error());
        return 2;
    }

    // ── 2) One PlatformConfig per --pci, port_id = slot index ─────────────
    // `nb_rx_queues = 1` keeps the demo's focus on the aggregator. To
    // overlap with RSS, set `nb_rx_queues > 1` and `enable_rss = true`
    // per port — the aggregator imposes no policy.
    std::vector<ed::PlatformConfig> port_cfgs;
    port_cfgs.reserve(n_ports);
    for (std::size_t i = 0; i < n_ports; ++i) {
        ed::PlatformConfig pcfg{};
        pcfg.port_id      = static_cast<uint16_t>(i);
        pcfg.nb_rx_queues = 1;
        pcfg.nb_tx_queues = 1;
        port_cfgs.push_back(pcfg);
    }

    // ── 3) Atomic N-port bringup with rollback ────────────────────────────
    auto mp_r = ed::MultiPortPlatform::create(
        std::span<const ed::PlatformConfig>(port_cfgs));
    if (!mp_r) {
        spdlog::error("multi_port_platform_demo: MultiPortPlatform::create failed: {}",
                      mp_r.error().detail);
        return 3;
    }
    auto mp = std::move(*mp_r);
    spdlog::info("multi_port_platform_demo: aggregator ready, num_ports={}",
                 mp->num_ports());

    // ── 4) Per-port summary ───────────────────────────────────────────────
    // Slot index ≠ DPDK port_id in the general case (the user may pass
    // `--pci` in any order, and DPDK's eth dev enumeration may differ
    // from PCI bus order on some firmware). Print both columns so the
    // mapping is transparent.
    for (std::size_t i = 0; i < mp->num_ports(); ++i) {
        const auto& p = mp->port(i);
        const auto qr = p.effective_rx_queue_range();
        spdlog::info("  port[slot={}]: dpdk_port_id={}, nb_rx_queues={}, "
                     "dispatch_mode={}, is_rss_active={}, "
                     "rx_queue_range=[{},{})",
                     i, p.port_id(), p.nb_rx_queues(),
                     edpdk::rx_dispatch_mode_name(p.dispatch_mode()),
                     p.dispatch_mode() == edpdk::RxDispatchMode::RssPartitioned,
                     qr.first, qr.second);
    }

    // Sanity-check the reverse lookup helper. Picks slot 0's DPDK port_id
    // and asks the aggregator to find it; result must equal slot 0.
    if (mp->num_ports() > 0) {
        const uint16_t pid = mp->port(0).port_id();
        const std::size_t idx = mp->find_index_by_port_id(pid);
        spdlog::info("multi_port_platform_demo: find_index_by_port_id({}) → {} "
                     "(expected 0)", pid, idx);
    }

    // ── 5) One Poller per port; sequential round-robin poll loop ─────────
    std::vector<std::unique_ptr<edpdk::DpdkPoller<>>> pollers;
    pollers.reserve(mp->num_ports());
    for (std::size_t i = 0; i < mp->num_ports(); ++i) {
        edpdk::PollerConfig pcfg{};
        pcfg.port_id     = mp->port(i).port_id();
        pcfg.rx_queue_id = 0;
        auto pr = edpdk::DpdkPoller<>::create(pcfg);
        if (!pr) {
            spdlog::error("multi_port_platform_demo: DpdkPoller::create(port={}) "
                          "failed: {}", pcfg.port_id, pr.error().detail);
            return 4;
        }
        if (auto r = mp->port(i).register_poller(0, pr->get()); !r) {
            spdlog::error("multi_port_platform_demo: register_poller(slot={}, "
                          "queue=0) failed: {}", i, r.error().detail);
            return 5;
        }
        pollers.emplace_back(std::move(*pr));
    }

    spdlog::info("multi_port_platform_demo: entering poll loop for {}s "
                 "(no streams attached — this just exercises each port's "
                 "burst RX path)", args.run_seconds.count());
    const auto deadline = std::chrono::steady_clock::now() + args.run_seconds;
    while (g_running.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        for (auto& p : pollers) (void)p->poll();
    }

    spdlog::info("multi_port_platform_demo: shutting down — RAII teardown:"
                 " pollers → MultiPortPlatform (each owned Platform "
                 "destroyed in reverse order) → eal");
    return 0;
}
