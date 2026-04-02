/// @file bench_order_rtt_dpdk.cpp
/// DPDK backend: order send + ExecutionReport RTT benchmark.
///
/// Uses DirectTransport (no threads) — app thread polls directly.
/// Requires external bench_mock_server --order-mode running on server-ip.
///
/// Usage: bench_order_rtt_dpdk [EAL args] -- --server-ip IP --local-ip IP
///            --gateway-ip IP [--port PORT] [--duration SEC]
///            [--order-interval-us US] [--poll-cpu N]

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

    int app_argc = 0;
    char** app_argv = nullptr;
    for (int i = 0; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--") {
            app_argc = argc - i - 1;
            app_argv = argv + i + 1;
            break;
        }
    }

    spdlog::info("Initializing DPDK EAL...");
    auto eal = eph::dpdk::EalGuard::init(argc, argv);
    if (!eal) {
        spdlog::error("EAL init failed: {}", eal.error());
        return 1;
    }

    bench::BenchConfig cfg;
    std::string local_ip, gateway_ip;
    uint16_t dpdk_port = 0, local_port = 0;

    for (int i = 0; i < app_argc; ++i) {
        std::string arg = app_argv[i];
        if (arg == "--server-ip" && i + 1 < app_argc) cfg.server_ip = app_argv[++i];
        else if (arg == "--port" && i + 1 < app_argc) cfg.server_port = static_cast<uint16_t>(std::stoi(app_argv[++i]));
        else if (arg == "--local-ip" && i + 1 < app_argc) local_ip = app_argv[++i];
        else if (arg == "--gateway-ip" && i + 1 < app_argc) gateway_ip = app_argv[++i];
        else if (arg == "--dpdk-port" && i + 1 < app_argc) dpdk_port = static_cast<uint16_t>(std::stoi(app_argv[++i]));
        else if (arg == "--local-port" && i + 1 < app_argc) local_port = static_cast<uint16_t>(std::stoi(app_argv[++i]));
        else if (arg == "--duration" && i + 1 < app_argc) cfg.duration = std::chrono::seconds{std::stoi(app_argv[++i])};
        else if (arg == "--order-interval-us" && i + 1 < app_argc) cfg.order_interval = std::chrono::microseconds{std::stoi(app_argv[++i])};
        else if (arg == "--poll-cpu" && i + 1 < app_argc) cfg.poll_cpu = std::stoi(app_argv[++i]);
    }

    if (local_ip.empty() || gateway_ip.empty()) {
        spdlog::error("--local-ip and --gateway-ip are required");
        return 1;
    }

    spdlog::info("bench_order_rtt_dpdk: server={}:{}, local={}, gateway={}, interval={}us",
                 cfg.server_ip, cfg.server_port, local_ip, gateway_ip, cfg.order_interval.count());

    // TransportConfig + latency measurement (identical to socket bench)
    eph::net::TransportConfig tc;
    tc.remote_host = cfg.server_ip;
    tc.remote_port = cfg.server_port;
    tc.ws_path = "/ws";
    tc.use_tls = false;
    tc.ping_interval = std::chrono::seconds{0};
    tc.max_reconnect_attempts = 0;
    tc.skip_utf8_validation = true;

    eph::utils::HdrHistogram rtt_hist{10, 1'000'000'000ULL, 3};
    eph::utils::HdrHistogram resp_latency_hist{10, 1'000'000'000ULL, 3};
    uint64_t last_order_tsc = 0;
    uint64_t order_count = 0, response_count = 0;

    tc.on_message = [&](const uint8_t* data, uint16_t len, uint8_t) {
        std::string_view json(reinterpret_cast<const char*>(data), len);
        if (json.find("\"e\":\"executionReport\"") != std::string_view::npos) {
            if (last_order_tsc > 0) {
                uint64_t now = eph::utils::TSC::now();
                if (now > last_order_tsc) {
                    auto ns = eph::utils::TSC::to_ns(now - last_order_tsc);
                    if (ns) rtt_hist.record(static_cast<uint64_t>(*ns));
                }
            }
            uint64_t t_mock = bench::parse_tsc_field(data, len);
            if (t_mock > 0) {
                uint64_t now = eph::utils::TSC::now();
                if (now > t_mock) {
                    auto ns = eph::utils::TSC::to_ns(now - t_mock);
                    if (ns) resp_latency_hist.record(static_cast<uint64_t>(*ns));
                }
            }
            ++response_count;
        }
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

    auto& transport = *conn->transport;

    if (cfg.poll_cpu >= 0) {
        (void)eph::utils::set_thread_affinity(cfg.poll_cpu, "poll");
    }

    spdlog::info("Order RTT bench (DPDK) started: interval={}us, duration={}s",
                 cfg.order_interval.count(), cfg.duration.count());

    // Poll + send loop (identical logic to socket bench)
    auto start = std::chrono::steady_clock::now();
    auto next_order = start;

    while (g_running.load(std::memory_order_acquire) && transport.is_running()) {
        auto now_tp = std::chrono::steady_clock::now();
        if (now_tp - start >= cfg.duration) break;

        if (now_tp >= next_order) {
            last_order_tsc = eph::utils::TSC::now();
            char buf[256];
            int n = std::snprintf(buf, sizeof(buf),
                R"({"method":"order.place","symbol":"BTCUSDT",)"
                R"("side":"BUY","price":"50000.00",)"
                R"("quantity":"0.001","T_send":%llu})",
                static_cast<unsigned long long>(last_order_tsc));
            transport.send_text(std::string_view(buf, static_cast<size_t>(n)));
            ++order_count;
            next_order += cfg.order_interval;
        }

        transport.poll();
    }

    transport.stop();

    auto duration_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();

    spdlog::info("=== Order RTT Benchmark Results (DPDK) ===");
    spdlog::info("Duration: {}s, Orders: {}, Responses: {}, Rate: {} order/s",
                 duration_s, order_count, response_count,
                 duration_s > 0 ? order_count / static_cast<uint64_t>(duration_s) : 0);
    bench::print_latency("Order RTT (send -> response recv)", bench::hdr_to_stats(rtt_hist));
    bench::print_latency("Response Latency (mock send -> app recv)", bench::hdr_to_stats(resp_latency_hist));
    return 0;
}
