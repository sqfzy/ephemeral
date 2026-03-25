/// @file simple_hft.cpp
/// Minimal HFT-style example: Binance market data + WebSocket ping/pong
/// latency measurement using the socket (kernel) backend.
///
/// Demonstrates:
///   - Transport with custom Probe for pipeline latency measurement
///   - EvictingQueue as RxQueue (market data is droppable)
///   - Manual WebSocket ping for simulated order round-trip
///   - Four latency metrics:
///       1) ping generation → tx_burst  (TX SPSC queue transit)
///       2) rx_burst → pong received    (RX pipeline latency)
///       3) rx_burst → market data      (same, for data frames)
///       4) RTT                         (end-to-end round-trip)
///   - TSC-based nanosecond-precision timing
///
/// Usage:
///   ./simple_hft
///   ./simple_hft --symbol btcusdt --count 200 --ping-interval 500
///   ./simple_hft --host stream.binance.com --port 9443 --no-verify
///
/// For the DPDK backend, see simple_hft_dpdk.cpp.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <spdlog/spdlog.h>

#include "eph/containers/evicting_queue.hpp"
#include "eph/net/proxy.hpp"
#include "eph/net/socket_transport.hpp"
#include "eph/utils/recorder.hpp"
#include "eph/utils/time.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// HFT Probe — collects pipeline latency timestamps
// ─────────────────────────────────────────────────────────────────────────────

/// Latency probe that records TSC timestamps at the four pipeline points.
///
/// Thread safety model:
///   - on_tx_enqueue: called from app thread (single writer)
///   - on_tx_flush:   called from TX thread  (single writer)
///   - on_rx_arrival: called from RX thread  (single writer)
///   - on_rx_deliver: called from RX thread  (single writer)
///
/// Each recorder is written by exactly one thread, read only at report time.
struct HftProbe {
    // Latency 1: app send() → TX thread flush (TX queue transit time)
    // tx_enqueue records are paired with tx_flush records.
    eph::utils::Recorder tx_queue_latency{"tx_queue_latency"};

    // Latency 2 & 3: RX arrival → RX deliver (decrypt + deframe overhead)
    eph::utils::Recorder rx_pipeline_latency{"rx_pipeline_latency"};

    // Latency 4: app send_ping() → on_pong (end-to-end RTT)
    // Measured via Transport's built-in rtt_histogram_ (ping_tsc → pong_tsc).

    // Shared state for pairing tx_enqueue with tx_flush
    std::atomic<uint64_t> last_tx_enqueue_tsc{0};

    // Shared state for pairing rx_arrival with rx_deliver
    std::atomic<uint64_t> last_rx_arrival_tsc{0};

    void on_tx_enqueue(uint64_t tsc) noexcept {
        last_tx_enqueue_tsc.store(tsc, std::memory_order_relaxed);
    }

    void on_tx_flush(uint64_t tsc) noexcept {
        uint64_t enqueue_tsc = last_tx_enqueue_tsc.load(std::memory_order_relaxed);
        if (enqueue_tsc > 0 && tsc > enqueue_tsc) {
            auto ns = eph::utils::TSC::to_ns(tsc - enqueue_tsc);
            if (ns) tx_queue_latency.record(static_cast<uint64_t>(*ns));
        }
    }

    void on_rx_arrival(uint64_t tsc) noexcept {
        last_rx_arrival_tsc.store(tsc, std::memory_order_relaxed);
    }

    void on_rx_deliver(uint64_t tsc) noexcept {
        uint64_t arrival_tsc = last_rx_arrival_tsc.load(std::memory_order_relaxed);
        if (arrival_tsc > 0 && tsc > arrival_tsc) {
            auto ns = eph::utils::TSC::to_ns(tsc - arrival_tsc);
            if (ns) rx_pipeline_latency.record(static_cast<uint64_t>(*ns));
        }
    }
};

static_assert(eph::net::TransportProbe<HftProbe>,
              "HftProbe must satisfy TransportProbe");

// ─────────────────────────────────────────────────────────────────────────────
// Transport type alias with HftProbe + EvictingQueue
// ─────────────────────────────────────────────────────────────────────────────

/// HFT transport: socket backend, WS framer, 512B payload, 1024-depth queue,
/// HftProbe instrumentation, EvictingQueue for RX (market data is droppable).
using HftTransport = eph::net::Transport<
    eph::net::SocketTransport,
    eph::net::WsFramer,
    512,   // MaxPayload — Binance bookTicker messages are ~200 bytes
    1024,  // QueueDepth
    HftProbe,
    eph::containers::EvictingQueue
>;

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

struct AppConfig {
    std::string host          = "stream.binance.com";
    uint16_t    port          = 9443;
    std::string symbol        = "btcusdt";
    std::string proxy_url{};     // e.g. "socks5://127.0.0.1:7890"
    int         count         = 100;     // 0 = infinite
    int         ping_interval = 1000;    // ms between manual pings
    bool        use_tls       = true;
    bool        verify        = false;
    int         tx_cpu        = -1;
    int         rx_cpu        = -1;
};

static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running.store(false, std::memory_order_release);
}

static void print_usage(const char* prog) {
    std::cerr << std::format(
        "Usage: {} [options]\n"
        "\n"
        "Options:\n"
        "  --host <hostname>       Binance stream host (default: stream.binance.com)\n"
        "  --port <port>           Server port (default: 9443)\n"
        "  --symbol <symbol>       Trading pair (default: btcusdt)\n"
        "  --proxy <url>           Proxy URL: socks5://host:port or http://host:port\n"
        "  --count <n>             Number of pings to send, 0=infinite (default: 100)\n"
        "  --ping-interval <ms>    Milliseconds between pings (default: 1000)\n"
        "  --no-tls                Disable TLS\n"
        "  --no-verify             Disable certificate verification\n"
        "  --tx-cpu <id>           Pin TX thread to CPU\n"
        "  --rx-cpu <id>           Pin RX thread to CPU\n"
        "  --help                  Show this help\n", prog);
}

static AppConfig parse_args(int argc, char** argv) {
    AppConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        auto next = [&](std::string_view name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << std::format("Error: {} requires a value\n", name);
                std::exit(1);
            }
            return argv[++i];
        };
        if      (arg == "--host")          cfg.host          = next("--host");
        else if (arg == "--port")          cfg.port          = static_cast<uint16_t>(std::atoi(next("--port")));
        else if (arg == "--symbol")        cfg.symbol        = next("--symbol");
        else if (arg == "--proxy")         cfg.proxy_url     = next("--proxy");
        else if (arg == "--count")         cfg.count         = std::atoi(next("--count"));
        else if (arg == "--ping-interval") cfg.ping_interval = std::atoi(next("--ping-interval"));
        else if (arg == "--no-tls")        cfg.use_tls       = false;
        else if (arg == "--no-verify")     cfg.verify        = false;
        else if (arg == "--tx-cpu")        cfg.tx_cpu        = std::atoi(next("--tx-cpu"));
        else if (arg == "--rx-cpu")        cfg.rx_cpu        = std::atoi(next("--rx-cpu"));
        else if (arg == "--help")        { print_usage(argv[0]); std::exit(0); }
        else {
            std::cerr << std::format("Unknown argument: {}\n", arg);
            print_usage(argv[0]);
            std::exit(1);
        }
    }
    return cfg;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    spdlog::set_level(spdlog::level::info);

    auto cfg = parse_args(argc, argv);

    // ── Step 1: Calibrate TSC ─────────────────────────────────────────────
    spdlog::info("Calibrating TSC...");
    if (!eph::utils::TSC::init(std::chrono::milliseconds{200})) {
        spdlog::error("TSC calibration failed");
        return 1;
    }
    spdlog::info("TSC calibrated: {:.4f} ns/cycle",
                 eph::utils::TSC::get_ns_per_cycle().value());

    // ── Step 2: Build WebSocket path for Binance stream ───────────────────
    // Binance bookTicker: high-frequency best bid/ask updates
    auto ws_path = std::format("/ws/{}@bookTicker", cfg.symbol);
    spdlog::info("Subscribing to {}", ws_path);

    // ── Step 3: Configure Transport ───────────────────────────────────────
    eph::net::TransportConfig transport_cfg{
        .remote_host = cfg.host,
        .remote_port = cfg.port,
        .ws_path     = ws_path,
        .use_tls     = cfg.use_tls,
        .verify_peer = cfg.verify,
        .max_reconnect_attempts = 3,
        // Disable auto-ping — we send manual pings for latency measurement
        .ping_interval = std::chrono::seconds{0},
        .skip_utf8_validation = true,  // Binance sends valid UTF-8 JSON
        .tx_cpu = cfg.tx_cpu,
        .rx_cpu = cfg.rx_cpu,

        .on_state_change = [](eph::net::TransportEvent event,
                              std::string_view detail) {
            spdlog::info("[STATE] {} — {}",
                         eph::net::transport_event_name(event), detail);
        },

        .on_pong = [](const uint8_t*, uint16_t) {
            SPDLOG_DEBUG("Pong received");
        },
    };

    eph::net::SocketConfig sock_cfg{
        .host        = cfg.host,
        .port        = cfg.port,
        .tcp_nodelay = true,
        .tcp_keepalive = true,
    };

    // ── Step 4: Build TcpFactory (with or without proxy) ──────────────────
    HftTransport::TcpFactory tcp_factory;

    if (!cfg.proxy_url.empty()) {
        auto proxy_cfg = eph::net::proxy::parse_proxy_url(cfg.proxy_url);
        if (!proxy_cfg) {
            spdlog::error("Invalid proxy URL: {}", proxy_cfg.error());
            return 1;
        }
        spdlog::info("Using proxy: {}:{} ({})", proxy_cfg->host, proxy_cfg->port,
                     proxy_cfg->type == eph::net::proxy::ProxyType::kSocks5
                         ? "SOCKS5" : "HTTP CONNECT");
        tcp_factory = eph::net::proxy::make_proxied_factory(
            sock_cfg, *proxy_cfg, cfg.host, cfg.port);
    } else {
        tcp_factory = [sock_cfg]()
            -> std::expected<std::unique_ptr<eph::net::SocketTransport>, std::string> {
            auto tcp = std::make_unique<eph::net::SocketTransport>(sock_cfg);
            auto result = tcp->connect(std::chrono::milliseconds{5000});
            if (!result) return std::unexpected(result.error());
            return tcp;
        };
    }

    // ── Step 5: Connect ───────────────────────────────────────────────────
    auto scheme = cfg.use_tls ? "wss" : "ws";
    spdlog::info("Connecting to {}://{}:{}{}",
                 scheme, cfg.host, cfg.port, ws_path);

    auto result = HftTransport::create(std::move(tcp_factory), transport_cfg);
    if (!result) {
        spdlog::error("Connection failed: {}", result.error().message());
        return 1;
    }
    auto& tp = **result;
    spdlog::info("Connected (handshake {:.2f} ms)", tp.stats().handshake_ms());

    // ── Step 5: Main loop — receive market data + periodic pings ──────────
    int pings_sent = 0;
    uint64_t market_msgs = 0;
    auto last_ping = std::chrono::steady_clock::now();
    auto ping_interval = std::chrono::milliseconds(cfg.ping_interval);
    auto start_time = std::chrono::steady_clock::now();

    while (g_running.load(std::memory_order_acquire) && tp.is_running()) {
        // Send periodic pings (simulated order submission)
        auto now = std::chrono::steady_clock::now();
        if (now - last_ping >= ping_interval) {
            if (cfg.count == 0 || pings_sent < cfg.count) {
                auto rc = tp.send_ping();
                if (rc == eph::net::SendError::kOk) {
                    ++pings_sent;
                    SPDLOG_DEBUG("Ping #{} sent", pings_sent);
                } else {
                    spdlog::warn("Ping send failed: {}",
                                 eph::net::send_error_name(rc));
                }
                last_ping = now;
            } else if (pings_sent >= cfg.count) {
                spdlog::info("All {} pings sent, collecting remaining data...",
                             cfg.count);
                // Wait a bit for final pong responses
                std::this_thread::sleep_for(std::chrono::milliseconds{500});
                break;
            }
        }

        // Drain market data (EvictingQueue: get the latest available)
        bool got = tp.recv([&](const uint8_t* data, size_t len) {
            ++market_msgs;
            // Log periodically to avoid flooding
            if ((market_msgs & 0xFF) == 1) {
                std::string_view json(reinterpret_cast<const char*>(data), len);
                spdlog::info("[MKT #{:>6}] {:.80}", market_msgs, json);
            }
        });

        if (!got) {
            eph::utils::cpu_relax();
        }
    }

    // ── Step 6: Report ────────────────────────────────────────────────────
    tp.stop();
    auto elapsed = std::chrono::steady_clock::now() - start_time;
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    spdlog::info("=== Session Summary ===");
    spdlog::info("Duration: {:.1f}s", static_cast<double>(elapsed_ms) / 1000.0);
    spdlog::info("Market data messages: {}", market_msgs);
    spdlog::info("Pings sent: {}", pings_sent);

    // Transport-level stats
    auto stats = tp.stats();
    spdlog::info("Transport stats:\n{}", stats.dump());

    // Pipeline latency reports
    auto& probe = tp.probe();

    spdlog::info("--- Latency 1: TX Queue (send → tx_burst) ---");
    if (probe.tx_queue_latency.has_data()) {
        probe.tx_queue_latency.print_report();
    } else {
        spdlog::info("  (no samples)");
    }

    spdlog::info("--- Latency 2/3: RX Pipeline (rx_burst → deliver) ---");
    if (probe.rx_pipeline_latency.has_data()) {
        probe.rx_pipeline_latency.print_report();
    } else {
        spdlog::info("  (no samples)");
    }

    spdlog::info("--- Latency 4: Ping/Pong RTT (end-to-end) ---");
    auto rtt = tp.rtt_stats();
    if (rtt.count > 0) {
        spdlog::info("  samples: {}", rtt.count);
        spdlog::info("  min:     {:.0f} ns", static_cast<double>(rtt.min_ns));
        spdlog::info("  p50:     {:.0f} ns", static_cast<double>(rtt.p50_ns));
        spdlog::info("  p99:     {:.0f} ns", static_cast<double>(rtt.p99_ns));
        spdlog::info("  p99.9:   {:.0f} ns", static_cast<double>(rtt.p999_ns));
        spdlog::info("  max:     {:.0f} ns", static_cast<double>(rtt.max_ns));
    } else {
        spdlog::info("  (no RTT samples — pong not received?)");
    }

    return 0;
}
