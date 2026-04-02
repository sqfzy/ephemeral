/// @file bench_order_rtt.cpp
/// Socket backend: order send + execution report RTT benchmark.
///
/// Connects to the in-process mock WS server via kernel TCP, periodically
/// sends order JSON with embedded TSC timestamps, and measures:
///   - Order RTT:        app send() TSC -> app recv(executionReport) TSC
///   - Response Latency: mock sendmsg() TSC -> app on_message TSC
///
/// Usage:
///   bench_order_rtt [--server-ip IP] [--port PORT] [--duration SEC]
///                   [--order-interval-us US] [--tick-us US]
///                   [--symbols SYM1,SYM2]
///                   [--rx-cpu N] [--tx-cpu N]

#include "bench_impl.hpp"
#include "eph/net/socket_transport.hpp"
#include "eph/net/socket_config.hpp"

int main(int argc, char** argv) {
    signal(SIGINT, sig);
    signal(SIGTERM, sig);

    bench::BenchConfig cfg;

    // Parse command line args
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--server-ip" && i + 1 < argc) {
            cfg.server_ip = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            cfg.server_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--symbols" && i + 1 < argc) {
            cfg.symbols = bench::split(argv[++i], ',');
        } else if (arg == "--duration" && i + 1 < argc) {
            cfg.duration = std::chrono::seconds{std::stoi(argv[++i])};
        } else if (arg == "--order-interval-us" && i + 1 < argc) {
            cfg.order_interval = std::chrono::microseconds{std::stoi(argv[++i])};
        } else if (arg == "--tick-us" && i + 1 < argc) {
            cfg.tick_interval = std::chrono::microseconds{std::stoi(argv[++i])};
        } else if (arg == "--rx-cpu" && i + 1 < argc) {
            cfg.rx_cpu = std::stoi(argv[++i]);
        } else if (arg == "--no-mock") {
            cfg.start_mock = false;
        } else if (arg == "--tx-cpu" && i + 1 < argc) {
            cfg.tx_cpu = std::stoi(argv[++i]);
        }
    }

    // Initialize TSC (calibrate CPU frequency)
    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC initialization failed — cannot measure latency");
        return 1;
    }

    spdlog::info("bench_order_rtt (socket): {}:{}, order_interval={}us, duration={}s",
                 cfg.server_ip, cfg.server_port, cfg.order_interval.count(),
                 cfg.duration.count());

    auto tcp_factory = [&]()
        -> std::expected<std::unique_ptr<eph::net::SocketTransport>, std::string> {
        eph::net::SocketConfig sc{
            .host = cfg.server_ip,
            .port = cfg.server_port,
            .tcp_nodelay = true,
        };
        auto tcp = std::make_unique<eph::net::SocketTransport>(sc);
        auto r = tcp->connect(std::chrono::milliseconds{3000});
        if (!r) return std::unexpected(r.error());
        return tcp;
    };

    bench::run_order_rtt_bench<eph::net::SocketTransport>(std::move(tcp_factory), cfg);
    return 0;
}
