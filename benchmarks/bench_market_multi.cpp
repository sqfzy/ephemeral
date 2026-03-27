/// @file bench_market_multi.cpp
/// Multi-symbol market data pipeline latency benchmark — socket (kernel) backend.
///
/// Connects to Binance combined bookTicker stream and measures:
///   RX pipeline: rx_burst → market data frame decoded
///
/// Combined stream delivers all symbols in one connection.
/// LastOnlyDeliver=false ensures every message reaches the app.
///
/// Usage (all threads on isolated, non-overlapping cores):
///   # Queue mode (default):
///   ./bench_market_multi --rx-cpu 0 --tx-cpu 1 --main-cpu 2 --duration 30
///
///   # on_message mode (bypass queue):
///   ./bench_market_multi --rx-cpu 0 --tx-cpu 1 --main-cpu 2 --on-message --duration 30
///
///   # Custom symbols:
///   ./bench_market_multi --rx-cpu 0 --tx-cpu 1 --main-cpu 2
///       --symbols btcusdt,ethusdt,solusdt,bnbusdt --duration 60
///
///   # With proxy:
///   ./bench_market_multi --rx-cpu 0 --tx-cpu 1 --main-cpu 2
///       --proxy socks5://127.0.0.1:7890

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <format>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/containers/evicting_queue.hpp"
#include "eph/net/proxy.hpp"
#include "eph/net/socket_transport.hpp"
#include "eph/utils/hdr_histogram.hpp"
#include "eph/utils/time.hpp"
#include "eph/utils/cpu.hpp"

// Multi-symbol: larger payload for combined stream wrapper, deliver all frames.
using BenchTransport = eph::net::Transport<
    eph::net::SocketTransport,
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
    constexpr size_t kPrefixLen = 11;  // {"stream":"
    if (len < kPrefixLen + 2) return 0;

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

/// Extract Binance event timestamp (ms) from combined stream JSON.
/// Scans for "E": in the outer data object and parses the integer.
/// Returns 0 on parse failure.
static int64_t binance_event_time_ms(const uint8_t* data, size_t len) {
    // Combined stream: {"stream":"...","data":{"e":"bookTicker","E":1234567890123,...}}
    // Scan for "E": pattern — the event time field.
    std::string_view json(reinterpret_cast<const char*>(data), len);
    auto pos = json.find("\"E\":");
    if (pos == std::string_view::npos) return 0;
    pos += 4;  // skip "E":
    while (pos < len && json[pos] == ' ') ++pos;
    int64_t val = 0;
    while (pos < len && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + (json[pos] - '0');
        ++pos;
    }
    return val;
}

struct Config {
    std::string host   = "fstream.binance.com";
    uint16_t    port   = 443;
    std::vector<std::string> symbols = {"btcusdt", "ethusdt", "solusdt"};
    std::string proxy_url{};
    int  duration      = 30;
    bool use_tls       = true;
    bool verify        = false;
    bool use_on_message = false;
    int  tx_cpu        = -1;
    int  rx_cpu        = -1;
    int  main_cpu      = -1;
    bool use_twophase   = false;
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
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        auto next = [&](std::string_view n) -> const char* {
            if (i + 1 >= argc) { std::cerr << std::format("{} requires a value\n", n); std::exit(1); }
            return argv[++i];
        };
        if      (a == "--host")      c.host      = next(a);
        else if (a == "--port")      c.port      = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--symbols")   c.symbols   = split(next(a), ',');
        else if (a == "--proxy")     c.proxy_url = next(a);
        else if (a == "--duration")  c.duration  = std::atoi(next(a));
        else if (a == "--no-tls")    c.use_tls   = false;
        else if (a == "--on-message") c.use_on_message = true;
        else if (a == "--no-verify") c.verify    = false;
        else if (a == "--tx-cpu")    c.tx_cpu    = std::atoi(next(a));
        else if (a == "--rx-cpu")    c.rx_cpu    = std::atoi(next(a));
        else if (a == "--main-cpu") c.main_cpu = std::atoi(next(a));
        else if (a == "--mode") {
            std::string_view m = next(a);
            if      (m == "all")      c.use_twophase = false;
            else if (m == "twophase") c.use_twophase = true;
            else { std::cerr << std::format("Unknown mode: {} (use all|twophase)\n", m); std::exit(1); }
        }
        else if (a == "--help") {
            std::cerr << std::format(
                "Usage: {} [--host H] [--port P] [--symbols S1,S2,S3] [--proxy URL]\n"
                "       [--duration SEC] [--no-tls] [--no-verify] [--tx-cpu N] [--rx-cpu N]\n"
                "       [--mode all|twophase]\n", argv[0]);
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

    auto cfg = parse_args(argc, argv);
    if (cfg.main_cpu >= 0) (void)eph::utils::set_thread_affinity(cfg.main_cpu, "main");

    spdlog::info("Calibrating TSC...");
    if (!eph::utils::TSC::init(std::chrono::milliseconds{200})) {
        spdlog::error("TSC calibration failed");
        return 1;
    }
    spdlog::info("TSC: {:.4f} ns/cycle", eph::utils::TSC::get_ns_per_cycle().value());

    // Build combined stream path
    std::string streams;
    for (size_t i = 0; i < cfg.symbols.size(); ++i) {
        if (i > 0) streams += '/';
        streams += cfg.symbols[i] + "@bookTicker";
    }
    auto ws_path = "/stream?streams=" + streams;

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

    static volatile uint64_t on_msg_sink = 0;
    if (cfg.use_on_message) {
        tc.on_message = [](const uint8_t* data, [[maybe_unused]] uint16_t len, uint8_t) {
            on_msg_sink = *reinterpret_cast<const uint64_t*>(data);
        };
    }
    (void)on_msg_sink;

    eph::net::SocketConfig sc{
        .host = cfg.host, .port = cfg.port,
        .tcp_nodelay = true, .tcp_keepalive = true,
    };

    BenchTransport::TcpFactory factory;
    if (!cfg.proxy_url.empty()) {
        auto pc = eph::net::proxy::parse_proxy_url(cfg.proxy_url);
        if (!pc) { spdlog::error("Invalid proxy: {}", pc.error()); return 1; }
        factory = eph::net::proxy::make_proxied_factory(sc, *pc, cfg.host, cfg.port);
    } else {
        factory = [sc]() -> std::expected<std::unique_ptr<eph::net::SocketTransport>, std::string> {
            auto tcp = std::make_unique<eph::net::SocketTransport>(sc);
            if (auto r = tcp->connect(std::chrono::milliseconds{5000}); !r) return std::unexpected(r.error());
            return tcp;
        };
    }

    const char* mode_name = cfg.use_twophase ? "twophase" : "all";

    spdlog::info("Connecting to wss://{}:{}{} ({} symbols, mode={})",
                 cfg.host, cfg.port, ws_path, cfg.symbols.size(), mode_name);
    auto result = BenchTransport::create(std::move(factory), tc);
    if (!result) { spdlog::error("Connect failed: {}", result.error().message()); return 1; }
    auto& tp = **result;
    spdlog::info("Connected (handshake {:.2f} ms)", tp.stats().handshake_ms());

    // ── Feed latency tracking ─────────────────────────────────────────────
    // Range: 1us to 60s (in nanoseconds) — covers all practical feed latencies.
    eph::utils::HdrHistogram feed_hist{1'000, 60'000'000'000ULL, 3};

    // ── Main loop: drain market data ────────────────────────────────────────
    uint64_t msgs = 0;
    auto start = std::chrono::steady_clock::now();
    auto deadline = cfg.duration > 0
        ? start + std::chrono::seconds(cfg.duration)
        : std::chrono::steady_clock::time_point::max();

    // Per-second status line
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
                // Feed latency: Binance E timestamp vs local wall clock
                auto event_ms = binance_event_time_ms(data, len);
                if (event_ms > 0) {
                    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    auto delta_ms = now_ms - event_ms;
                    if (delta_ms > 0 && delta_ms < 60000) {
                        feed_hist.record(static_cast<uint64_t>(delta_ms) * 1'000'000);  // ms → ns
                    }
                }
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

            prev_hist = std::move(curr_hist);
            prev_msgs = msgs;
            window_start = snap_time;
        }
    }

    // ── Report ──────────────────────────────────────────────────────────────
    tp.stop();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    auto stats = tp.stats();
    auto& rx = stats.rx_latency;
    double avg_bytes_per_burst = rx.count > 0
        ? static_cast<double>(stats.rx_bytes) / rx.count : 0.0;

    spdlog::info("=== Multi-Symbol Market Data Benchmark (Socket) ===");
    spdlog::info("Symbols: {} | Duration: {:.1f}s | Messages: {} | Mode: {}",
                 cfg.symbols.size(), elapsed_ms / 1000.0, msgs, mode_name);
    double avg_frames = rx.count > 0
        ? static_cast<double>(stats.rx_packets) / rx.count : 0.0;
    spdlog::info("RX totals: {} bytes, {} WS frames, {} TLS records",
                 stats.rx_bytes, stats.rx_packets, rx.count);
    spdlog::info("Per TLS record: {:.0f} bytes, {:.1f} WS frames",
                 avg_bytes_per_burst, avg_frames);
    spdlog::info("Transport stats:\n{}", stats.dump());
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

    // Feed latency report
    auto fl = hdr_to_stats(feed_hist);
    spdlog::info("--- Feed Latency (Binance E → App recv) ---");
    if (fl.count > 0) {
        spdlog::info("  samples: {}", fl.count);
        spdlog::info("  min:     {:.1f} ms", fl.min_ns / 1e6);
        spdlog::info("  p50:     {:.1f} ms", fl.p50_ns / 1e6);
        spdlog::info("  p99:     {:.1f} ms", fl.p99_ns / 1e6);
        spdlog::info("  p99.9:   {:.1f} ms", fl.p999_ns / 1e6);
        spdlog::info("  max:     {:.1f} ms", fl.max_ns / 1e6);
    } else {
        spdlog::info("  (no samples — check NTP sync)");
    }

    return 0;
}
