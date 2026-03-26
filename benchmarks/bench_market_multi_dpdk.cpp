/// @file bench_market_multi_dpdk.cpp
/// Multi-symbol market data pipeline latency benchmark — DPDK (kernel-bypass) backend.
///
/// Connects to Binance combined bookTicker stream and measures:
///   RX pipeline: rx_burst → market data frame decoded
///
/// Combined stream delivers all symbols in one connection.
/// LastOnlyDeliver=false ensures every message reaches the app.
///
/// Usage (all threads on isolated, non-overlapping cores):
///   # Core allocation: lcore 4-7 (DPDK EAL), rx 8, tx 9, main 10
///
///   # Queue mode (default):
///   sudo ./bench_market_multi_dpdk -a 0000:28:00.0 -l 4-7 -- \
///       --local-ip 172.31.23.112 --gateway-ip 172.31.16.1 \
///       --rx-cpu 8 --tx-cpu 9 --main-cpu 10 --duration 30
///
///   # on_message mode (bypass queue):
///   sudo ./bench_market_multi_dpdk -a 0000:28:00.0 -l 4-7 -- \
///       --local-ip 172.31.23.112 --gateway-ip 172.31.16.1 \
///       --rx-cpu 8 --tx-cpu 9 --main-cpu 10 --on-message --duration 30
///
///   # Custom symbols:
///   sudo ./bench_market_multi_dpdk -a 0000:28:00.0 -l 4-7 -- \
///       --local-ip 172.31.23.112 --gateway-ip 172.31.16.1 \
///       --rx-cpu 8 --tx-cpu 9 --main-cpu 10 \
///       --symbols btcusdt,ethusdt,solusdt,bnbusdt --duration 60

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/containers/evicting_queue.hpp"
#include "eph/dpdk/connector.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/utils/time.hpp"
#include "eph/utils/cpu.hpp"

// Multi-symbol: larger payload for combined stream wrapper, deliver all frames.
using BenchTransport = eph::net::Transport<
    eph::dpdk::TcpSession<>,
    eph::net::WsFramer,
    16384, 1024,
    eph::containers::EvictingQueue,
    false  // LastOnlyDeliver — every symbol's update matters
>;

/// Extract symbol hash from Binance combined stream JSON payload.
/// Format: {"stream":"<symbol>@<channel>","data":{...}}
/// Scans the first ~64 bytes for "stream":" prefix, hashes until '@'.
/// Returns 0 on parse failure (payload delivered unconditionally).
static uint32_t binance_symbol_hash(const uint8_t* data, size_t len) {
    // {"stream":" is 11 bytes; shortest symbol is ~4 chars + '@'
    constexpr size_t kPrefixLen = 11;  // {"stream":"
    if (len < kPrefixLen + 2) return 0;

    // Verify prefix: quick check on key bytes
    if (data[0] != '{' || data[1] != '"' || data[9] != '"') return 0;

    const uint8_t* p = data + kPrefixLen;
    const uint8_t* end = data + std::min(len, size_t{64});
    uint32_t hash = 2166136261u;  // FNV-1a offset basis
    while (p < end && *p != '@' && *p != '"') {
        hash ^= *p;
        hash *= 16777619u;  // FNV-1a prime
        ++p;
    }
    return (p > data + kPrefixLen) ? hash : 0;
}

struct Config {
    std::string host       = "fstream.binance.com";
    uint16_t    port       = 443;
    std::vector<std::string> symbols = {"btcusdt", "ethusdt", "solusdt"};
    std::string local_ip{};
    std::string gateway_ip{};
    uint16_t    dpdk_port  = 0;
    uint16_t    local_port = 32768;
    int  duration          = 30;
    bool use_tls           = true;
    bool verify            = false;
    bool use_on_message    = false;  // bypass queue via on_message callback
    int  tx_cpu            = -1;
    int  rx_cpu            = -1;
    int  main_cpu          = -1;
    bool use_twophase      = false;
};

/// Per-second window record for traffic vs latency correlation.
struct WindowRecord {
    int    second;
    uint64_t msgs;
    double msg_rate;
    uint64_t p50_ns, p99_ns, p999_ns;
};

/// Convert HdrHistogram to RttStats (standalone version of Transport's private helper).
static eph::net::RttStats hdr_to_stats(const eph::utils::HdrHistogram& h) noexcept {
    if (h.get_total_count() == 0) return {};
    return {
        .count   = h.get_total_count(),
        .min_ns  = h.get_min_value(),
        .max_ns  = h.get_max_value(),
        .mean_ns = h.get_mean(),
        .p50_ns  = h.get_value_at_percentile(50.0),
        .p99_ns  = h.get_value_at_percentile(99.0),
        .p999_ns = h.get_value_at_percentile(99.9),
    };
}

/// Print bucketed traffic-vs-latency summary (windows sorted by msg/s, split into terciles).
static void print_traffic_summary(std::vector<WindowRecord>& windows) {
    if (windows.size() < 3) return;

    std::sort(windows.begin(), windows.end(),
              [](auto& a, auto& b) { return a.msg_rate < b.msg_rate; });

    size_t n = windows.size();
    size_t t1 = n / 3, t2 = 2 * n / 3;

    auto bucket = [&](size_t from, size_t to) {
        double min_r = 1e9, max_r = 0;
        uint64_t sum_p50 = 0, sum_p99 = 0, sum_p999 = 0;
        for (size_t i = from; i < to; ++i) {
            min_r = std::min(min_r, windows[i].msg_rate);
            max_r = std::max(max_r, windows[i].msg_rate);
            sum_p50 += windows[i].p50_ns;
            sum_p99 += windows[i].p99_ns;
            sum_p999 += windows[i].p999_ns;
        }
        size_t cnt = to - from;
        return std::tuple{min_r, max_r, sum_p50 / cnt, sum_p99 / cnt, sum_p999 / cnt, cnt};
    };

    spdlog::info("=== Traffic vs Latency (by msg/s tercile) ===");
    auto [lo_min, lo_max, lo_p50, lo_p99, lo_p999, lo_n] = bucket(0, t1);
    auto [md_min, md_max, md_p50, md_p99, md_p999, md_n] = bucket(t1, t2);
    auto [hi_min, hi_max, hi_p50, hi_p99, hi_p999, hi_n] = bucket(t2, n);

    spdlog::info("  Low  ({:>4.0f}-{:>4.0f} msg/s, {:>2} wins): p50={:>6} p99={:>6} p99.9={:>6} ns",
                 lo_min, lo_max, lo_n, lo_p50, lo_p99, lo_p999);
    spdlog::info("  Mid  ({:>4.0f}-{:>4.0f} msg/s, {:>2} wins): p50={:>6} p99={:>6} p99.9={:>6} ns",
                 md_min, md_max, md_n, md_p50, md_p99, md_p999);
    spdlog::info("  High ({:>4.0f}-{:>4.0f} msg/s, {:>2} wins): p50={:>6} p99={:>6} p99.9={:>6} ns",
                 hi_min, hi_max, hi_n, hi_p50, hi_p99, hi_p999);
}

static std::atomic<bool> g_running{true};
static void sig(int) { g_running.store(false, std::memory_order_release); }

static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> tokens;
    std::istringstream ss(s);
    std::string token;
    while (std::getline(ss, token, delim))
        if (!token.empty()) tokens.push_back(token);
    return tokens;
}

static Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 0; i < argc; ++i) {
        std::string_view a = argv[i];
        auto next = [&](std::string_view n) -> const char* {
            if (i + 1 >= argc) { std::cerr << std::format("{} requires a value\n", n); std::exit(1); }
            return argv[++i];
        };
        if      (a == "--host")       c.host       = next(a);
        else if (a == "--port")       c.port       = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--symbols")    c.symbols    = split(next(a), ',');
        else if (a == "--local-ip")   c.local_ip   = next(a);
        else if (a == "--gateway-ip") c.gateway_ip = next(a);
        else if (a == "--dpdk-port")  c.dpdk_port  = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--local-port") c.local_port = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--duration")   c.duration   = std::atoi(next(a));
        else if (a == "--tx-cpu")     c.tx_cpu     = std::atoi(next(a));
        else if (a == "--rx-cpu")     c.rx_cpu     = std::atoi(next(a));
        else if (a == "--main-cpu") c.main_cpu = std::atoi(next(a));
        else if (a == "--no-tls")     c.use_tls    = false;
        else if (a == "--on-message") c.use_on_message = true;
        else if (a == "--mode") {
            std::string_view m = next(a);
            if      (m == "all")      c.use_twophase = false;
            else if (m == "twophase") c.use_twophase = true;
            else { std::cerr << std::format("Unknown mode: {} (use all|twophase)\n", m); std::exit(1); }
        }
        else if (a == "--help") {
            std::cerr << std::format(
                "Usage: {} [EAL args] -- [--host H] [--port P] [--symbols S1,S2,S3]\n"
                "       [--local-ip IP] [--gateway-ip IP] [--dpdk-port N] [--local-port N]\n"
                "       [--duration SEC] [--tx-cpu N] [--rx-cpu N] [--no-tls]\n"
                "       [--mode all|twophase]\n",
                "bench_market_multi_dpdk");
            std::exit(0);
        }
        else { std::cerr << std::format("Unknown: {}\n", a); std::exit(1); }
    }
    return c;
}

int main(int argc, char** argv) {
    std::signal(SIGINT, sig);
    std::signal(SIGTERM, sig);
    spdlog::set_level(spdlog::level::info);

    int eal_argc = argc; char** eal_argv = argv;
    int app_argc = 0;    char** app_argv = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--") == 0) {
            eal_argc = i; app_argc = argc - i - 1; app_argv = argv + i + 1; break;
        }
    }
    auto cfg = parse_args(app_argc, app_argv);
    if (cfg.main_cpu >= 0) eph::utils::set_thread_affinity(cfg.main_cpu, "main");
    if (cfg.local_ip.empty() || cfg.gateway_ip.empty()) {
        spdlog::error("--local-ip and --gateway-ip are required"); return 1;
    }

    spdlog::info("Calibrating TSC...");
    if (!eph::utils::TSC::init(std::chrono::milliseconds{200})) {
        spdlog::error("TSC calibration failed"); return 1;
    }
    spdlog::info("TSC: {:.4f} ns/cycle", eph::utils::TSC::get_ns_per_cycle().value());

    spdlog::info("Initializing DPDK EAL...");
    auto eal = eph::dpdk::EalGuard::init(eal_argc, eal_argv);
    if (!eal) { spdlog::error("EAL init failed: {}", eal.error()); return 1; }

    // Build combined stream path
    std::string streams;
    for (size_t i = 0; i < cfg.symbols.size(); ++i) {
        if (i > 0) streams += '/';
        streams += cfg.symbols[i] + "@bookTicker";
    }
    auto ws_path = "/stream?streams=" + streams;

    const char* mode_name = cfg.use_twophase ? "twophase" : "all";

    eph::net::TransportConfig tc{
        .remote_host = cfg.host, .remote_port = cfg.port,
        .ws_path = ws_path, .use_tls = cfg.use_tls, .verify_peer = cfg.verify,
        .max_reconnect_attempts = 3,
        .ping_interval = std::chrono::seconds{0},
        .skip_utf8_validation = true,
        .tx_cpu = cfg.tx_cpu, .rx_cpu = cfg.rx_cpu,
        .on_state_change = [](eph::net::TransportEvent e, std::string_view d) {
            spdlog::info("[STATE] {} — {}", eph::net::transport_event_name(e), d);
        },
        .on_frame_filter = cfg.use_twophase
            ? eph::net::make_twophase_filter(binance_symbol_hash)
            : eph::net::FrameFilterFn{},
    };

    // on_message bypasses EvictingQueue — callback runs in RX thread
    static volatile uint64_t on_msg_sink = 0;
    if (cfg.use_on_message) {
        tc.on_message = [](const uint8_t* data, uint16_t len, uint8_t) {
            on_msg_sink = *reinterpret_cast<const uint64_t*>(data);
        };
    }

    spdlog::info("Connecting via DPDK to wss://{}:{}{} ({} symbols, on_message={}, mode={})",
                 cfg.host, cfg.port, ws_path, cfg.symbols.size(), cfg.use_on_message,
                 mode_name);
    auto conn = eph::dpdk::connect<BenchTransport>(
        eph::dpdk::DpdkEndpoint{.local_ip = cfg.local_ip, .gateway_ip = cfg.gateway_ip},
        tc, eph::dpdk::ConnectorOptions{.platform = {.port_id = cfg.dpdk_port}, .local_port = cfg.local_port});
    if (!conn) { spdlog::error("DPDK connect failed: {}", conn.error()); return 1; }
    auto& tp = *conn->transport;
    spdlog::info("Connected via DPDK ({} symbols)!", cfg.symbols.size());

    // ── Main loop: drain market data ────────────────────────────────────────
    uint64_t msgs = 0;
    auto start = std::chrono::steady_clock::now();
    auto deadline = cfg.duration > 0
        ? start + std::chrono::seconds(cfg.duration)
        : std::chrono::steady_clock::time_point::max();

    // Per-second window tracking for traffic-vs-latency correlation
    std::vector<WindowRecord> windows;
    auto prev_hist = tp.rx_latency_histogram_snapshot();
    uint64_t prev_msgs = 0;
    auto window_start = start;
    int window_idx = 0;

    spdlog::info("{:>8} {:>10} {:>10} {:>10} {:>12}",
                 "time", "msg/s", "p50(ns)", "p99(ns)", "p99.9(ns)");

    while (g_running.load(std::memory_order_acquire) && tp.is_running()) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

        if (cfg.use_on_message) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        } else {
            bool got = tp.recv([&](const uint8_t* data, size_t len) {
                ++msgs;
                if ((msgs & 0xFF) == 1) {
                    std::string_view json(reinterpret_cast<const char*>(data), len);
                    spdlog::debug("[MKT #{:>6}] {:.80}", msgs, json);
                }
            });
            if (!got) eph::utils::cpu_relax();
        }

        // Per-second window snapshot
        if (std::chrono::steady_clock::now() - window_start >= std::chrono::seconds(1)) {
            auto snap_time = std::chrono::steady_clock::now();
            ++window_idx;
            auto curr_hist = tp.rx_latency_histogram_snapshot();
            auto delta = curr_hist;
            (void)delta.subtract(prev_hist);
            auto ws = hdr_to_stats(delta);

            uint64_t delta_msgs = msgs - prev_msgs;
            double elapsed_s = std::chrono::duration<double>(snap_time - window_start).count();
            double rate = delta_msgs / elapsed_s;

            spdlog::info("[T+{:>3}s] {:>8.0f} {:>10} {:>10} {:>12}",
                         window_idx, rate, ws.p50_ns, ws.p99_ns, ws.p999_ns);

            windows.push_back({window_idx, delta_msgs, rate, ws.p50_ns, ws.p99_ns, ws.p999_ns});
            prev_hist = std::move(curr_hist);
            prev_msgs = msgs;
            window_start = snap_time;
        }
    }

    // ── Report ──────────────────────────────────────────────────────────────
    tp.stop();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    // Traffic vs latency bucketed summary
    print_traffic_summary(windows);

    auto stats = tp.stats();
    spdlog::info("=== Multi-Symbol Market Data Benchmark (DPDK) ===");
    spdlog::info("Symbols: {} | Duration: {:.1f}s | Messages: {} | Mode: {}",
                 cfg.symbols.size(), elapsed_ms / 1000.0, msgs,
                 mode_name);
    spdlog::info("Transport stats:\n{}", stats.dump());

    auto& rx = stats.rx_latency;
    spdlog::info("--- RX Pipeline (rx_burst → data decoded) ---");
    if (rx.count > 0) {
        spdlog::info("  samples: {}", rx.count);
        spdlog::info("  min:     {:.0f} ns", static_cast<double>(rx.min_ns));
        spdlog::info("  p50:     {:.0f} ns", static_cast<double>(rx.p50_ns));
        spdlog::info("  p99:     {:.0f} ns", static_cast<double>(rx.p99_ns));
        spdlog::info("  p99.9:   {:.0f} ns", static_cast<double>(rx.p999_ns));
        spdlog::info("  max:     {:.0f} ns", static_cast<double>(rx.max_ns));
    } else {
        spdlog::info("  (no samples)");
    }

    return 0;
}
