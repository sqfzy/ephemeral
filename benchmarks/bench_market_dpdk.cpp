/// @file bench_market_dpdk.cpp
/// DPDK backend: single-connection multi-symbol market data benchmark.
///
/// Uses DirectTransport (no threads) — app thread polls directly.
/// Requires external bench_mock_server running on server-ip (NIC-A).
///
/// Usage: bench_market_dpdk [EAL args] -- --server-ip IP --local-ip IP
///            --gateway-ip IP [--port PORT] [--duration SEC] [--poll-cpu N]

#include <csignal>
#include <string>

#include <spdlog/spdlog.h>

#include "bench_impl.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/connector.hpp"
#include "eph/dpdk/tcp.hpp"

int main(int argc, char** argv) {
    signal(SIGINT, sig);
    signal(SIGTERM, sig);

    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC calibration failed");
        return 1;
    }

    // Split args at "--"
    int app_argc = 0;
    char** app_argv = nullptr;
    for (int i = 0; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--") {
            app_argc = argc - i - 1;
            app_argv = argv + i + 1;
            break;
        }
    }

    // Initialize DPDK EAL
    spdlog::info("Initializing DPDK EAL...");
    auto eal = eph::dpdk::EalGuard::init(argc, argv);
    if (!eal) {
        spdlog::error("EAL init failed: {}", eal.error());
        return 1;
    }

    // Parse app args
    bench::BenchConfig cfg;
    std::string local_ip, gateway_ip;
    uint16_t dpdk_port = 0;
    uint16_t local_port = 0;

    for (int i = 0; i < app_argc; ++i) {
        std::string arg = app_argv[i];
        if (arg == "--server-ip" && i + 1 < app_argc) cfg.server_ip = app_argv[++i];
        else if (arg == "--port" && i + 1 < app_argc) cfg.server_port = static_cast<uint16_t>(std::stoi(app_argv[++i]));
        else if (arg == "--local-ip" && i + 1 < app_argc) local_ip = app_argv[++i];
        else if (arg == "--gateway-ip" && i + 1 < app_argc) gateway_ip = app_argv[++i];
        else if (arg == "--dpdk-port" && i + 1 < app_argc) dpdk_port = static_cast<uint16_t>(std::stoi(app_argv[++i]));
        else if (arg == "--local-port" && i + 1 < app_argc) local_port = static_cast<uint16_t>(std::stoi(app_argv[++i]));
        else if (arg == "--symbols" && i + 1 < app_argc) cfg.symbols = bench::split(app_argv[++i], ',');
        else if (arg == "--duration" && i + 1 < app_argc) cfg.duration = std::chrono::seconds{std::stoi(app_argv[++i])};
        else if (arg == "--poll-cpu" && i + 1 < app_argc) cfg.poll_cpu = std::stoi(app_argv[++i]);
    }

    if (local_ip.empty() || gateway_ip.empty()) {
        spdlog::error("--local-ip and --gateway-ip are required");
        return 1;
    }

    spdlog::info("bench_market_dpdk: server={}:{}, local={}, gateway={}, duration={}s",
                 cfg.server_ip, cfg.server_port, local_ip, gateway_ip, cfg.duration.count());

    // TransportConfig (same as socket bench)
    eph::net::TransportConfig tc;
    tc.remote_host = cfg.server_ip;
    tc.remote_port = cfg.server_port;
    tc.ws_path = "/ws";
    tc.use_tls = false;
    tc.ping_interval = std::chrono::seconds{0};
    tc.max_reconnect_attempts = 0;
    tc.skip_utf8_validation = true;

    // Latency measurement via on_message (same as socket bench)
    eph::utils::HdrHistogram latency_hist{10, 1'000'000'000ULL, 3};
    uint64_t msg_count = 0;

    tc.on_message = [&](const uint8_t* data, uint16_t len, uint8_t) {
        uint64_t t_send = bench::parse_tsc_field(data, len);
        if (t_send > 0) {
            uint64_t now = eph::utils::TSC::now();
            if (now > t_send) {
                auto ns = eph::utils::TSC::to_ns(now - t_send);
                if (ns) latency_hist.record(static_cast<uint64_t>(*ns));
            }
        }
        ++msg_count;
    };

    // DPDK connect
    using BenchTransport = eph::net::DirectTransport<eph::dpdk::TcpSession<>, eph::net::WsFramer, 4096>;

    eph::dpdk::DpdkEndpoint ep{.local_ip = local_ip, .gateway_ip = gateway_ip};
    eph::dpdk::ConnectorOptions opts{
        .platform = {.port_id = dpdk_port},
        .local_port = local_port,
    };

    auto conn = eph::dpdk::connect<BenchTransport>(ep, tc, opts);
    if (!conn) {
        spdlog::error("DPDK connect failed: {}", conn.error());
        return 1;
    }
    spdlog::info("Connected via DPDK to {}:{}", cfg.server_ip, cfg.server_port);

    auto& transport = *conn->transport;

    if (cfg.poll_cpu >= 0) {
        (void)eph::utils::set_thread_affinity(cfg.poll_cpu, "poll");
    }

    spdlog::info("Market bench (DPDK) started: {} symbols, duration={}s",
                 cfg.symbols.size(), cfg.duration.count());

    // Poll loop — identical to socket bench logic
    auto start = std::chrono::steady_clock::now();
    while (g_running.load(std::memory_order_acquire) && transport.is_running()) {
        if (std::chrono::steady_clock::now() - start >= cfg.duration) break;
        transport.poll();
    }

    transport.stop();

    auto duration_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();

    spdlog::info("=== Market Data Benchmark Results (DPDK) ===");
    spdlog::info("Duration: {}s, Messages: {}, Rate: {} msg/s",
                 duration_s, msg_count,
                 duration_s > 0 ? msg_count / static_cast<uint64_t>(duration_s) : 0);
    bench::print_latency("Pipeline Latency (mock send -> app recv)", bench::hdr_to_stats(latency_hist));
    return 0;
}
