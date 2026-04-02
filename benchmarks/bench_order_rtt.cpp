/// @file bench_order_rtt.cpp
/// Socket backend: order send + ExecutionReport RTT benchmark.
///
/// Uses DirectTransport + SO_BINDTODEVICE to force traffic through NIC-B.
/// Mock server runs in-process on NIC-A.
///
/// Usage: bench_order_rtt --server-ip IP --bind-dev IFACE
///            [--port PORT] [--duration SEC] [--order-interval-us US]
///            [--poll-cpu N] [--mock-cpu N]

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
    std::string bind_dev;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--server-ip" && i + 1 < argc) cfg.server_ip = argv[++i];
        else if (arg == "--port" && i + 1 < argc) cfg.server_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--bind-dev" && i + 1 < argc) bind_dev = argv[++i];
        else if (arg == "--symbols" && i + 1 < argc) cfg.symbols = bench::split(argv[++i], ',');
        else if (arg == "--duration" && i + 1 < argc) cfg.duration = std::chrono::seconds{std::stoi(argv[++i])};
        else if (arg == "--order-interval-us" && i + 1 < argc) cfg.order_interval = std::chrono::microseconds{std::stoi(argv[++i])};
        else if (arg == "--tick-us" && i + 1 < argc) cfg.tick_interval = std::chrono::microseconds{std::stoi(argv[++i])};
        else if (arg == "--poll-cpu" && i + 1 < argc) cfg.poll_cpu = std::stoi(argv[++i]);
        else if (arg == "--mock-cpu" && i + 1 < argc) cfg.mock_cpu = std::stoi(argv[++i]);
    }

    if (bind_dev.empty()) {
        spdlog::error("--bind-dev is required (e.g. --bind-dev ens35) to avoid loopback");
        return 1;
    }

    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC calibration failed");
        return 1;
    }

    spdlog::info("bench_order_rtt (socket): server={}:{}, bind_dev={}, interval={}us",
                 cfg.server_ip, cfg.server_port, bind_dev, cfg.order_interval.count());

    auto tcp_factory = [&]() -> std::expected<std::unique_ptr<eph::net::SocketTransport>, std::string> {
        eph::net::SocketConfig sc{
            .host = cfg.server_ip,
            .port = cfg.server_port,
            .tcp_nodelay = true,
            .bind_device = bind_dev,
        };
        auto tcp = std::make_unique<eph::net::SocketTransport>(sc);
        auto r = tcp->connect(std::chrono::milliseconds{3000});
        if (!r) return std::unexpected(r.error());
        return tcp;
    };

    bench::run_order_rtt_bench<eph::net::SocketTransport>(std::move(tcp_factory), cfg);
    return 0;
}
