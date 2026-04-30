/// @file dpdk_multicast_md.cpp
///
/// DPDK multicast market-data receiver example. Demonstrates the
/// `eph::dpdk::MulticastReceiver` class — the supported path for ITCH /
/// MoldUDP64 / CME MDP3 style equity feeds over user-space DPDK.
///
/// What's demonstrated:
///
///   * `Platform::create_primary` with single queue (`nb_rx_queues=1`,
///     `enable_rss=false`) — the canonical safe shape for multicast RX.
///     For RSS multi-queue + multicast, see the FlowDirector caveat
///     in the second config block.
///   * `MulticastReceiver::join_group` — RFC 1112 multicast MAC
///     derivation (`01:00:5e:XX:XX:XX`) is internal; caller passes the
///     IPv4 group + UDP port. Up to `kMaxMulticastGroups` (8) joins per
///     receiver.
///   * `--rss-fail-test` flag — opts into a deliberately broken config
///     (`rss_active_multi_queue=true`) and proves `start()` fail-fasts.
///     This is the safety gate added in stage 5 of the RSS blind-spot
///     fix (commit `6dcdf225`); the previous behaviour silently dropped
///     packets that hashed to other queues.
///   * `on_packet(cb)` — UDP payload delivery from the receiver's own
///     RX thread. The callback runs on the RX thread, NOT main —
///     callers must synchronize any cross-thread state themselves.
///   * `total_rx_packets()` / `rx_unmatched_packets()` — the two
///     diagnostic counters. A non-zero `rx_unmatched` after a clean
///     run is the single most useful "is my filter wrong?" indicator.
///
/// What's deliberately out of scope:
///
///   * No actual market-data parsing. The bytes are counted and the
///     first 32 bytes of the first 3 packets are hex-dumped, no more.
///     For a full ITCH parse + book apply, integrate
///     `eph::itch::IttchParser` (per-frame) into the on_packet body.
///   * No MoldUDP64 demuxing — that's `eph::codec::MoldUdp64Codec`
///     applied on the application side after the multicast receiver
///     hands you the UDP payload. The receiver itself is codec-agnostic.
///   * No SSM (source-specific multicast) — `MulticastGroup::source_ip`
///     is left zero (any-source). Fill it for SSM filtering.
///
/// Required environment: a NIC bound to vfio-pci, hugepages reserved,
/// and either a real exchange feed reachable on the multicast group
/// you join, or no traffic at all (in which case the demo runs for
/// `--seconds` and reports zero received packets — still proves the
/// path links and starts).
///
/// Usage:
///
///   # Default: join 233.54.12.111:26477 for 5s on port 0
///   sudo ./dpdk_multicast_md --pci 0000:28:00.0 --pin 0=4
///
///   # Demonstrate the RSS multi-queue safety gate (start() fail-fast):
///   sudo ./dpdk_multicast_md --pci 0000:28:00.0 --pin 0=4 --rss-fail-test
///
///   # Custom group + port:
///   sudo ./dpdk_multicast_md --pci 0000:28:00.0 --pin 0=4
///        --group 224.0.50.50 --group-port 30000 --seconds 8

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/lcore_pin.hpp"
#include "eph/dpdk/multicast.hpp"
#include "eph/dpdk/platform.hpp"
#include "eph/utils/cpu.hpp"

namespace ed = eph::dpdk;
using namespace std::chrono_literals;

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running.store(false, std::memory_order_release); }

namespace {

struct AppArgs {
    std::string               pci;
    std::vector<ed::LcorePin> pins;
    std::string               lcores;
    uint16_t                  port_id    = 0;
    std::string               group_str  = "233.54.12.111";  // Nasdaq TVITCH-like
    uint16_t                  group_port = 26477;
    int                       seconds    = 5;
    bool                      rss_fail_test = false;
};

std::expected<ed::LcorePin, std::string>
parse_pin_spec(std::string_view s) {
    auto eq = s.find('=');
    if (eq == std::string_view::npos || eq == 0 || eq + 1 == s.size())
        return std::unexpected(std::string{"--pin: expected 'lcore=cpu[:role]'"});
    auto col = s.find(':', eq + 1);
    auto cpu_end = (col == std::string_view::npos) ? s.size() : col;
    int lcore = std::atoi(std::string(s.substr(0, eq)).c_str());
    int cpu   = std::atoi(std::string(s.substr(eq + 1, cpu_end - eq - 1)).c_str());
    if (lcore < 0 || cpu < 0)
        return std::unexpected("--pin: lcore/cpu must be non-negative");
    std::string role = (col == std::string_view::npos) ? "" : std::string(s.substr(col + 1));
    return ed::LcorePin{static_cast<uint16_t>(lcore), cpu, std::move(role)};
}

int split_app_args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) == "--") return i;
    return argc;
}

uint32_t parse_ipv4_dotted(std::string_view s) noexcept {
    // Tolerant dotted-quad parser. Returns 0 on malformed input — caller
    // checks for that.
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(std::string(s).c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    return (a << 24) | (b << 16) | (c << 8) | d;
}

AppArgs parse_args(int argc, char** argv) {
    AppArgs out;
    const int sep = split_app_args(argc, argv);
    for (int i = sep + 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if      (a == "--pci"            && i + 1 < argc) out.pci        = argv[++i];
        else if (a == "--lcores"         && i + 1 < argc) out.lcores     = argv[++i];
        else if (a == "--pin"            && i + 1 < argc) {
            auto p = parse_pin_spec(argv[++i]);
            if (!p) { spdlog::error("dpdk_multicast_md: {}", p.error()); std::exit(1); }
            out.pins.push_back(std::move(*p));
        }
        else if (a == "--port-id"        && i + 1 < argc) out.port_id    = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--group"          && i + 1 < argc) out.group_str  = argv[++i];
        else if (a == "--group-port"     && i + 1 < argc) out.group_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--seconds"        && i + 1 < argc) out.seconds    = std::atoi(argv[++i]);
        else if (a == "--rss-fail-test")                  out.rss_fail_test = true;
    }
    return out;
}

void hex_dump(std::span<const uint8_t> data, std::string& out) {
    out.clear();
    static constexpr char kHex[] = "0123456789abcdef";
    for (auto b : data) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    spdlog::set_level(spdlog::level::info);

    const AppArgs args = parse_args(argc, argv);
    if (args.pci.empty()) {
        spdlog::error("dpdk_multicast_md: --pci <addr> required");
        return 1;
    }
    const bool typed_pins = !args.pins.empty();
    const bool raw_lcores = !args.lcores.empty();
    if (!typed_pins && !raw_lcores) {
        spdlog::error("dpdk_multicast_md: provide --pin lcore=cpu[:role] or "
                      "--lcores '<raw>'");
        return 1;
    }

    const uint32_t group_ip = parse_ipv4_dotted(args.group_str);
    if (group_ip == 0 || !ed::is_multicast_ip(group_ip)) {
        spdlog::error("dpdk_multicast_md: --group must be a multicast IPv4 "
                      "address (224.0.0.0/4); got '{}'", args.group_str);
        return 1;
    }

    // ── 1) EAL + Platform via the unified create_with_eal factory ────────
    // Single queue, RSS off (the safe shape for multicast). Platform owns
    // EAL — ~Platform handles eal_cleanup atomically.
    //
    // For RSS+multicast: bring the Platform up with nb_rx_queues > 1 AND
    // install a FlowDirector rule pinning each multicast 5-tuple to one
    // queue, THEN clear `MulticastConfig::rss_active_multi_queue` to
    // acknowledge the explicit pin. The receiver cannot reverse-pick a
    // src_port (it doesn't control the sender) so RSS without FD is unsafe.
    ed::PlatformConfig pcfg{};
    pcfg.port_id            = args.port_id;
    pcfg.nb_rx_queues       = 1;
    pcfg.nb_tx_queues       = 1;
    pcfg.enable_promiscuous = false;
    pcfg.proc_type          = ed::ProcType::Primary;

    ed::EalConfig eal_cfg{};
    eal_cfg.program_name = "dpdk_multicast_md";
    if (raw_lcores) eal_cfg.lcores = {args.lcores};
    eal_cfg.allowed_devs = {args.pci};

    auto plat_r = ed::Platform::create_with_eal(
        std::move(pcfg), std::move(eal_cfg),
        std::span<ed::LcorePin const>{args.pins},
        eph::utils::CpuPinPolicy{});
    if (!plat_r) {
        spdlog::error("dpdk_multicast_md: Platform::create_with_eal failed: {}",
                      plat_r.error());
        return 2;
    }
    auto platform = std::move(*plat_r);
    const bool rss_active_diag =
        platform.dispatch_mode() == ::eph::net::dpdk::RxDispatchMode::RssPartitioned;
    spdlog::info("dpdk_multicast_md: Platform up — port={}, is_rss_active={}",
                 platform.port_id(), rss_active_diag);

    // ── 2) Build MulticastConfig + receiver ──────────────────────────────
    ed::MulticastConfig mc_cfg{};
    mc_cfg.port_id     = platform.port_id();
    mc_cfg.rx_queue_id = 0;
    mc_cfg.rx_cpu      = -1;            // user pre-pinned via --pin
    mc_cfg.rx_burst    = 32;
    mc_cfg.rss_active_multi_queue = args.rss_fail_test
                                      ? true   // demonstrate the gate
                                      : rss_active_diag;
    spdlog::info("dpdk_multicast_md: MulticastConfig:\n{}", mc_cfg.dump());

    ed::MulticastReceiver receiver{mc_cfg};

    // Atomic counters updated from the RX thread, read from main.
    std::atomic<uint64_t> rx_count{0};
    std::atomic<uint64_t> rx_bytes{0};
    std::atomic<uint64_t> sample_dumped{0};
    std::string hex_buf;
    hex_buf.reserve(80);

    receiver.on_packet([&](const uint8_t* data, std::size_t len) {
        const auto idx = rx_count.fetch_add(1, std::memory_order_relaxed);
        rx_bytes.fetch_add(len, std::memory_order_relaxed);
        // Dump the first 3 packets' first 32 bytes for sanity-eyeballing.
        // SAMPLE-COUNT is bounded so the demo doesn't flood stdout.
        if (idx < 3) {
            const std::size_t take = std::min<std::size_t>(len, 32);
            std::span<const uint8_t> s{data, take};
            hex_dump(s, hex_buf);
            spdlog::info("[mcast pkt #{}] {} bytes; first {}: {}",
                         idx, len, take, hex_buf);
            sample_dumped.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // ── 3) Join group ────────────────────────────────────────────────────
    ed::MulticastGroup grp{};
    grp.group_ip   = group_ip;
    grp.group_port = args.group_port;
    if (auto j = receiver.join_group(grp); !j) {
        spdlog::error("dpdk_multicast_md: join_group({}.{}.{}.{}:{}) failed: {}",
                      (group_ip >> 24) & 0xFF, (group_ip >> 16) & 0xFF,
                      (group_ip >> 8)  & 0xFF, group_ip        & 0xFF,
                      args.group_port, j.error());
        return 4;
    }
    spdlog::info("dpdk_multicast_md: joined {}:{}",
                 args.group_str, args.group_port);

    // ── 4) start() — exercise the RSS safety gate when --rss-fail-test ───
    auto sr = receiver.start();
    if (!sr) {
        if (args.rss_fail_test) {
            spdlog::info("dpdk_multicast_md: --rss-fail-test confirmed: "
                         "start() refused to run with rss_active_multi_queue"
                         "=true. Error was: {}", sr.error());
            return 0;  // demonstration successful
        }
        spdlog::error("dpdk_multicast_md: start() failed: {}", sr.error());
        return 5;
    }
    if (args.rss_fail_test) {
        spdlog::error("dpdk_multicast_md: --rss-fail-test FAILED — start() "
                      "should have refused with rss_active_multi_queue=true");
        receiver.stop();
        return 6;
    }

    // ── 5) Run for `--seconds` (RX thread is internal) ───────────────────
    spdlog::info("dpdk_multicast_md: receiver running for {}s "
                 "(no traffic on the wire is fine — final rx=0 still "
                 "demonstrates the path)", args.seconds);
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds{args.seconds};
    while (g_running.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(200ms);
    }

    // ── 6) stop() + diagnostics ───────────────────────────────────────────
    receiver.stop();
    const auto total_matched = receiver.total_rx_packets();
    const auto total_unm     = receiver.rx_unmatched_packets();
    spdlog::info("dpdk_multicast_md: rx={}, bytes={}, "
                 "total_matched={}, rx_unmatched={} "
                 "(non-zero unmatched = wrong filter, MAC collision, or "
                 "stale group state — see eph::dpdk::MulticastReceiver docs)",
                 rx_count.load(std::memory_order_relaxed),
                 rx_bytes.load(std::memory_order_relaxed),
                 total_matched, total_unm);
    return 0;
}
