/// @file bench_market_tx.cpp
/// Socket backend: market data TX (client → server) latency benchmark.
///
/// Simulates a market maker pushing quotes to an exchange. Sends WS messages
/// at tick_interval, mock server echoes with timestamps for TX/RX breakdown.
///
/// Usage: bench_market_tx --server-ip IP [--port PORT] [--duration SEC]
///            [--tick-us US] [--poll-cpu N]

#include <csignal>
#include <string>

#include <spdlog/spdlog.h>

#include "bench_impl.hpp"
#include "eph/net/socket_transport.hpp"
#include "eph/net/socket_config.hpp"

int main(int argc, char** argv) {
    signal(SIGINT, sig);
    signal(SIGTERM, sig);

    bench::BenchConfig cfg;
    cfg.tick_interval = std::chrono::microseconds{100};

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--server-ip" && i + 1 < argc) cfg.server_ip = argv[++i];
        else if (arg == "--port" && i + 1 < argc) cfg.server_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--duration" && i + 1 < argc) cfg.duration = std::chrono::seconds{std::stoi(argv[++i])};
        else if (arg == "--tick-us" && i + 1 < argc) cfg.tick_interval = std::chrono::microseconds{std::stoi(argv[++i])};
        else if (arg == "--poll-cpu" && i + 1 < argc) cfg.poll_cpu = std::stoi(argv[++i]);
        else if (arg == "--help" || arg == "-h") {
            spdlog::info("Usage: bench_market_tx --server-ip IP [--port PORT] [--duration SEC] [--tick-us US] [--poll-cpu N]");
            return 0;
        }
    }

    if (cfg.server_ip.empty()) {
        spdlog::error("--server-ip is required");
        return 1;
    }

    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC calibration failed");
        return 1;
    }

    spdlog::info("bench_market_tx (socket): server={}:{}, tick={}us, duration={}s",
                 cfg.server_ip, cfg.server_port, cfg.tick_interval.count(), cfg.duration.count());

    auto tcp_factory = [&]() -> std::expected<std::unique_ptr<eph::net::SocketTransport>, std::string> {
        eph::net::SocketConfig sc{.host = cfg.server_ip, .port = cfg.server_port, .tcp_nodelay = true};
        auto tcp = std::make_unique<eph::net::SocketTransport>(sc);
        auto r = tcp->connect(std::chrono::milliseconds{3000});
        if (!r) return std::unexpected(r.error());
        return tcp;
    };

    bench::run_market_tx_bench<eph::net::SocketTransport>(std::move(tcp_factory), cfg);
    return 0;
}
