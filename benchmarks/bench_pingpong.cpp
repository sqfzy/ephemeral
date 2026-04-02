/// @file bench_pingpong.cpp
/// Ping/pong latency benchmark — socket (kernel) backend.
///
/// Connects to Binance WITHOUT subscribing to any stream, then sends
/// periodic WebSocket pings. Measures three metrics:
///   1) TX Pipeline:  ping enqueue → tx_burst (SPSC transit + encode + encrypt)
///   2) RX Pipeline:  rx_burst → pong decoded (TLS decrypt + WS decode)
///   3) RTT:          ping tx_burst → pong rx_burst (end-to-end round-trip)
///
/// Usage (all threads on isolated, non-overlapping cores):
///   ./bench_pingpong --rx-cpu 0 --tx-cpu 1 --main-cpu 2
///   ./bench_pingpong --rx-cpu 0 --tx-cpu 1 --main-cpu 2 --count 500 --ping-interval 200
///   ./bench_pingpong --rx-cpu 0 --tx-cpu 1 --main-cpu 2 --proxy socks5://127.0.0.1:7890

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <iostream>
#include <string_view>
#include <thread>

#include <spdlog/spdlog.h>

#include "eph/containers/bounded_queue.hpp"
#include "eph/net/proxy.hpp"
#include "eph/net/socket_transport.hpp"
#include "eph/transport/transport.hpp"
#include "eph/transport/ws_framer.hpp"
#include "eph/utils/time.hpp"
#include "eph/utils/cpu.hpp"

#include "bench_common.hpp"

using BenchTransport = eph::net::Transport<
    eph::net::SocketTransport,
    eph::net::WsFramer,
    512, 1024,
    eph::containers::BoundedQueue,
    true  // LastOnlyDeliver
>;

struct Config {
    std::string host          = "fstream.binance.com";
    uint16_t    port          = 443;
    std::string proxy_url{};
    int  count                = 200;    // pings to send, 0 = infinite
    int  ping_interval        = 500;    // ms
    int  payload_size         = 0;      // ping payload bytes (0-125, simulates order data)
    bool use_tls              = true;
    bool verify               = false;
    int  tx_cpu               = -1;
    int  rx_cpu               = -1;
    int  main_cpu        = -1;
};

static Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        auto next = [&](std::string_view n) -> const char* {
            if (i + 1 >= argc) { std::cerr << std::format("{} requires a value\n", n); std::exit(1); }
            return argv[++i];
        };
        if      (a == "--host")          c.host          = next(a);
        else if (a == "--port")          c.port          = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--proxy")         c.proxy_url     = next(a);
        else if (a == "--count")         c.count         = std::atoi(next(a));
        else if (a == "--ping-interval") c.ping_interval = std::atoi(next(a));
        else if (a == "--payload-size")  c.payload_size  = std::clamp(std::atoi(next(a)), 0, 125);
        else if (a == "--no-tls")        c.use_tls       = false;
        else if (a == "--verify")        c.verify        = true;
        else if (a == "--tx-cpu")        c.tx_cpu        = std::atoi(next(a));
        else if (a == "--rx-cpu")        c.rx_cpu        = std::atoi(next(a));
        else if (a == "--main-cpu") c.main_cpu = std::atoi(next(a));
        else if (a == "--help") {
            std::cerr << std::format(
                "Usage: {} [--host H] [--port P] [--proxy URL] [--count N]\n"
                "       [--ping-interval MS] [--payload-size BYTES] [--no-tls] [--verify]\n"
                "       [--tx-cpu N] [--rx-cpu N] [--main-cpu N]\n", argv[0]);
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

    // Connect to bare WebSocket endpoint — no stream subscription.
    // Binance keeps the connection alive; we only send/receive pings/pongs.
    eph::net::TransportConfig tc{
        .remote_host = cfg.host, .remote_port = cfg.port,
        .ws_path = "/ws",  // bare endpoint, no subscription
        .use_tls = cfg.use_tls, .verify_peer = cfg.verify,
        .max_reconnect_attempts = 3,
        .ping_interval = std::chrono::seconds{0},  // manual pings only
        .skip_utf8_validation = true,
        .tx_cpu = cfg.tx_cpu, .rx_cpu = cfg.rx_cpu,
        .on_state_change = [](eph::net::TransportEvent e, std::string_view d) {
            spdlog::info("[STATE] {} — {}", eph::net::transport_event_name(e), d);
        },
        .on_pong = [](const uint8_t*, uint16_t) {
            SPDLOG_DEBUG("Pong received");
        },
    };

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

    spdlog::info("Connecting to wss://{}:{}/ws (ping-only, no subscription)", cfg.host, cfg.port);
    auto result = BenchTransport::create(std::move(factory), tc);
    if (!result) { spdlog::error("Connect failed: {}", result.error().message()); return 1; }
    auto& tp = **result;
    spdlog::info("Connected (handshake {:.2f} ms)", tp.stats().handshake_ms());

    // ── Prepare ping payload (simulates order-sized data) ──────────────────
    std::array<uint8_t, 125> ping_payload{};
    if (cfg.payload_size > 0) {
        // Fill with pseudo-realistic data (JSON-like byte distribution)
        for (int i = 0; i < cfg.payload_size; ++i)
            ping_payload[static_cast<size_t>(i)] = static_cast<uint8_t>('A' + (i % 26));
        spdlog::info("Ping payload: {} bytes", cfg.payload_size);
    }

    // ── Main loop: send pings, drain any data (shouldn't arrive) ────────────
    int pings_sent = 0;
    auto start = std::chrono::steady_clock::now();
    auto last_ping = start;
    auto interval = std::chrono::milliseconds(cfg.ping_interval);

    while (g_running.load(std::memory_order_acquire) && tp.is_running()) {
        auto now = std::chrono::steady_clock::now();
        if (now - last_ping >= interval) {
            if (cfg.count == 0 || pings_sent < cfg.count) {
                auto rc = cfg.payload_size > 0
                    ? tp.send_ping(ping_payload.data(), static_cast<size_t>(cfg.payload_size))
                    : tp.send_ping();
                if (rc == eph::net::SendError::kOk) {
                    ++pings_sent;
                    SPDLOG_DEBUG("Ping #{} sent", pings_sent);
                } else {
                    spdlog::warn("Ping failed: {}", eph::net::send_error_name(rc));
                }
                last_ping = now;
            } else {
                spdlog::info("All {} pings sent, waiting for final pongs...", cfg.count);
                std::this_thread::sleep_for(std::chrono::milliseconds{500});
                break;
            }
        }

        // Drain any unexpected data (shouldn't arrive without subscription)
        tp.recv([](const uint8_t*, size_t) {});

        eph::utils::cpu_relax();
    }

    // ── Report ──────────────────────────────────────────────────────────────
    tp.stop();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    auto stats = tp.stats();
    spdlog::info("=== Ping/Pong Benchmark (Socket) ===");
    spdlog::info("Duration: {:.1f}s | Pings sent: {}", elapsed_ms / 1000.0, pings_sent);
    spdlog::info("Transport stats:\n{}", stats.dump());

    bench::print_latency("TX Pipeline (enqueue → tx_burst)", stats.tx_latency);
    bench::print_latency("RX Pipeline (rx_burst → pong decoded)", stats.rx_latency);
    bench::print_latency("RTT (tx_burst → rx_burst)", stats.rtt);

    return 0;
}
