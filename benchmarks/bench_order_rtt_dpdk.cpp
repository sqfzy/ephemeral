/// @file bench_order_rtt_dpdk.cpp
/// DPDK backend: order send + execution report RTT benchmark.
///
/// Connects to the in-process mock WS server via DPDK kernel-bypass TCP,
/// periodically sends order JSON with embedded TSC timestamps, and measures:
///   - Order RTT:        app send() TSC -> app recv(executionReport) TSC
///   - Response Latency: mock sendmsg() TSC -> app on_message TSC
///
/// Usage:
///   bench_order_rtt_dpdk [EAL args] -- [app args]
///
/// App args:
///   --server-ip IP          Mock server IP (default: 10.0.0.1)
///   --port PORT             Mock server port (default: 9999)
///   --local-ip IP           NIC-B IP for DPDK (required, e.g. 10.0.0.2)
///   --gateway-ip IP         NIC-A IP / gateway (required, e.g. 10.0.0.1)
///   --dpdk-port N           DPDK port id (default: 0)
///   --local-port N          Local TCP port (0 = ephemeral)
///   --symbols SYM1,SYM2    Symbols (default: BTCUSDT,ETHUSDT,SOLUSDT)
///   --duration SEC          Benchmark duration (default: 10)
///   --order-interval-us US  Order send interval (default: 1000)
///   --tick-us US            Mock server tick interval (default: 100)
///   --rx-cpu N              RX thread CPU affinity
///   --tx-cpu N              TX thread CPU affinity
///   --main-cpu N            Main thread CPU affinity

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <spdlog/spdlog.h>

#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/connector.hpp"
#include "eph/dpdk/tcp.hpp"
#include "eph/utils/cpu.hpp"

#include "bench_impl.hpp"

int main(int argc, char** argv) {
    std::signal(SIGINT, sig);
    std::signal(SIGTERM, sig);

    // ── Split EAL args from app args at "--" ────────────────────────────────
    int eal_argc = argc;
    int app_argc = 0;
    char** app_argv = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--") == 0) {
            eal_argc = i;
            app_argc = argc - i - 1;
            app_argv = argv + i + 1;
            break;
        }
    }

    // ── Parse app-specific args ─────────────────────────────────────────────
    bench::BenchConfig cfg;
    std::string local_ip;
    std::string gateway_ip;
    uint16_t dpdk_port = 0;
    uint16_t local_port = 0;

    for (int i = 0; i < app_argc; ++i) {
        std::string_view a = app_argv[i];
        auto next = [&](std::string_view name) -> const char* {
            if (i + 1 >= app_argc) {
                spdlog::error("{} requires a value", name);
                std::exit(1);
            }
            return app_argv[++i];
        };
        if      (a == "--server-ip")         cfg.server_ip = next(a);
        else if (a == "--port")              cfg.server_port = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--local-ip")          local_ip = next(a);
        else if (a == "--gateway-ip")        gateway_ip = next(a);
        else if (a == "--dpdk-port")         dpdk_port = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--local-port")        local_port = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--symbols")           cfg.symbols = bench::split(std::string(next(a)), ',');
        else if (a == "--duration")          cfg.duration = std::chrono::seconds{std::atoi(next(a))};
        else if (a == "--order-interval-us") cfg.order_interval = std::chrono::microseconds{std::atoi(next(a))};
        else if (a == "--tick-us")           cfg.tick_interval = std::chrono::microseconds{std::atoi(next(a))};
        else if (a == "--rx-cpu")            cfg.rx_cpu = std::atoi(next(a));
        else if (a == "--tx-cpu")            cfg.tx_cpu = std::atoi(next(a));
        else if (a == "--main-cpu")          cfg.main_cpu = std::atoi(next(a));
        else if (a == "--help") {
            std::fprintf(stderr,
                "Usage: %s [EAL args] -- [--server-ip IP] [--port P]\n"
                "       [--local-ip IP] [--gateway-ip IP] [--dpdk-port N] [--local-port N]\n"
                "       [--symbols SYM1,SYM2] [--duration SEC] [--order-interval-us US]\n"
                "       [--tick-us US] [--rx-cpu N] [--tx-cpu N] [--main-cpu N]\n",
                "bench_order_rtt_dpdk");
            return 0;
        }
        else {
            spdlog::error("Unknown app arg: {}", a);
            return 1;
        }
    }

    if (local_ip.empty() || gateway_ip.empty()) {
        spdlog::error("--local-ip and --gateway-ip are required");
        return 1;
    }

    // ── Pin main thread ─────────────────────────────────────────────────────
    if (cfg.main_cpu >= 0) {
        (void)eph::utils::set_thread_affinity(cfg.main_cpu, "main");
    }

    // ── Calibrate TSC ───────────────────────────────────────────────────────
    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC initialization failed -- cannot measure latency");
        return 1;
    }
    spdlog::info("TSC: {:.4f} ns/cycle", eph::utils::TSC::get_ns_per_cycle().value());

    // ── Initialize DPDK EAL ─────────────────────────────────────────────────
    spdlog::info("Initializing DPDK EAL...");
    auto eal = eph::dpdk::EalGuard::init(eal_argc, argv);
    if (!eal) {
        spdlog::error("EAL init failed: {}", eal.error());
        return 1;
    }

    spdlog::info("bench_order_rtt_dpdk: server={}:{}, local={}, gateway={}, "
                 "order_interval={}us, duration={}s",
                 cfg.server_ip, cfg.server_port, local_ip, gateway_ip,
                 cfg.order_interval.count(), cfg.duration.count());

    // ── Start mock WS server (kernel TCP on NIC-A, same process) ────────────
    // Start mock BEFORE DPDK connect so it is listening when we connect.
    bench::mock::MockServerConfig mock_cfg{
        .bind_ip = cfg.server_ip,
        .port = cfg.server_port,
        .symbols = cfg.symbols,
        .tick_interval = cfg.tick_interval,
        .order_mode = true,
    };
    std::atomic<bool> mock_running{true};
    std::thread mock_thread([&] {
        bench::mock::run_mock_ws_server(mock_cfg, mock_running);
    });

    // Give mock server time to start listening
    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    // ── Build TransportConfig for mock server (no TLS, no pings) ────────────
    eph::net::TransportConfig tc;
    tc.remote_host = cfg.server_ip;
    tc.remote_port = cfg.server_port;
    tc.ws_path = "/ws";
    tc.use_tls = false;
    tc.ping_interval = std::chrono::seconds{0};
    tc.max_reconnect_attempts = 0;
    tc.skip_utf8_validation = true;

    // ── Latency histograms ──────────────────────────────────────────────────
    eph::utils::HdrHistogram rtt_hist{10, 1'000'000'000ULL, 3};
    eph::utils::HdrHistogram resp_latency_hist{10, 1'000'000'000ULL, 3};
    std::atomic<uint64_t> last_order_tsc{0};
    uint64_t order_count = 0;
    uint64_t response_count = 0;

    tc.on_message = [&](const uint8_t* data, uint16_t len, uint8_t /*opcode*/) {
        std::string_view json(reinterpret_cast<const char*>(data), len);

        // Check if this is an executionReport (order response)
        if (json.find("\"e\":\"executionReport\"") != std::string_view::npos) {
            // Order RTT: TSC::now() - last_order_tsc
            uint64_t send_tsc = last_order_tsc.load(std::memory_order_acquire);
            if (send_tsc > 0) {
                uint64_t now = eph::utils::TSC::now();
                if (now > send_tsc) {
                    auto ns = eph::utils::TSC::to_ns(now - send_tsc);
                    if (ns) rtt_hist.record(static_cast<uint64_t>(*ns));
                }
            }
            // Response latency: mock "T" -> now
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
        // Market data messages are ignored for RTT (but they exercise the pipeline)
    };

    if (cfg.rx_cpu >= 0) tc.rx_cpu = cfg.rx_cpu;
    if (cfg.tx_cpu >= 0) tc.tx_cpu = cfg.tx_cpu;

    // ── Resolve server IP and create DPDK connection ────────────────────────
    eph::dpdk::DpdkEndpoint ep{.local_ip = local_ip, .gateway_ip = gateway_ip};
    eph::dpdk::ConnectorOptions opts{
        .platform = {.port_id = dpdk_port},
        .local_port = local_port,
    };

    using BenchTransport = eph::net::Transport<
        eph::dpdk::TcpSession<>, eph::net::WsFramer, 4096, 1024>;

    auto conn = eph::dpdk::connect<BenchTransport>(ep, tc, opts);
    if (!conn) {
        spdlog::error("DPDK connect failed: {}", conn.error());
        mock_running = false;
        mock_thread.join();
        return 1;
    }
    spdlog::info("Connected via DPDK to {}:{}", cfg.server_ip, cfg.server_port);

    auto& transport = *conn->transport;

    // ── Main loop: send orders periodically, receive responses ──────────────
    spdlog::info("Order RTT bench (DPDK) started: interval={}us, duration={}s",
                 cfg.order_interval.count(), cfg.duration.count());

    auto start = std::chrono::steady_clock::now();
    auto next_order = start;

    while (g_running.load(std::memory_order_acquire)
           && transport.is_running()) {
        auto now = std::chrono::steady_clock::now();
        if (now - start >= cfg.duration) break;

        if (now >= next_order) {
            // Build order JSON with T_send TSC
            uint64_t send_tsc = eph::utils::TSC::now();
            last_order_tsc.store(send_tsc, std::memory_order_release);

            char order_buf[256];
            int order_len = std::snprintf(order_buf, sizeof(order_buf),
                R"({"method":"order.place","symbol":"BTCUSDT",)"
                R"("side":"BUY","price":"50000.00",)"
                R"("quantity":"0.001","T_send":%lu})",
                static_cast<unsigned long>(send_tsc));

            transport.send_text(
                std::string_view(order_buf, static_cast<size_t>(order_len)));
            ++order_count;
            next_order += cfg.order_interval;
        }

        // Poll for received messages (on_message callback fires inside recv)
        if (!transport.recv([](const uint8_t*, size_t) {})) {
            eph::utils::cpu_relax();
        }
    }

    // ── Stop and report ─────────────────────────────────────────────────────
    transport.stop();
    mock_running = false;
    mock_thread.join();

    auto duration_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();

    spdlog::info("=== Order RTT Benchmark Results (DPDK) ===");
    spdlog::info("Duration: {}s, Orders sent: {}, Responses: {}, Rate: {} order/s",
                 duration_s, order_count, response_count,
                 duration_s > 0 ? order_count / static_cast<uint64_t>(duration_s) : 0);
    bench::print_latency("Order RTT (send -> response recv)",
                         bench::hdr_to_stats(rtt_hist));
    bench::print_latency("Response Latency (mock send -> app recv)",
                         bench::hdr_to_stats(resp_latency_hist));

    return 0;
}
