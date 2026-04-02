/// @file bench_market_single_dpdk.cpp
/// DPDK backend: single-connection multi-symbol market data benchmark.
///
/// Connects to the in-process mock WS server via DPDK kernel-bypass TCP
/// and measures pipeline latency (mock sendmsg TSC -> app on_message TSC)
/// using HdrHistogram.
///
/// Usage:
///   bench_market_single_dpdk [EAL args] -- [app args]
///
/// App args:
///   --server-ip IP       Mock server IP (default: 10.0.0.1)
///   --port PORT          Mock server port (default: 9999)
///   --local-ip IP        NIC-B IP for DPDK (required, e.g. 10.0.0.2)
///   --gateway-ip IP      NIC-A IP / gateway (required, e.g. 10.0.0.1)
///   --dpdk-port N        DPDK port id (default: 0)
///   --local-port N       Local TCP port (0 = ephemeral)
///   --symbols SYM1,SYM2  Symbols to subscribe (default: BTCUSDT,ETHUSDT,SOLUSDT)
///   --duration SEC       Benchmark duration (default: 10)
///   --tick-us US         Mock server tick interval (default: 100)
///   --rx-cpu N           RX thread CPU affinity
///   --tx-cpu N           TX thread CPU affinity
///   --main-cpu N         Main thread CPU affinity

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <string_view>

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
        if      (a == "--server-ip")   cfg.server_ip = next(a);
        else if (a == "--port")        cfg.server_port = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--local-ip")    local_ip = next(a);
        else if (a == "--gateway-ip")  gateway_ip = next(a);
        else if (a == "--dpdk-port")   dpdk_port = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--local-port")  local_port = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--symbols")     cfg.symbols = bench::split(std::string(next(a)), ',');
        else if (a == "--duration")    cfg.duration = std::chrono::seconds{std::atoi(next(a))};
        else if (a == "--tick-us")     cfg.tick_interval = std::chrono::microseconds{std::atoi(next(a))};
        else if (a == "--rx-cpu")      cfg.rx_cpu = std::atoi(next(a));
        else if (a == "--tx-cpu")      cfg.tx_cpu = std::atoi(next(a));
        else if (a == "--main-cpu")    cfg.main_cpu = std::atoi(next(a));
        else if (a == "--help") {
            std::fprintf(stderr,
                "Usage: %s [EAL args] -- [--server-ip IP] [--port P]\n"
                "       [--local-ip IP] [--gateway-ip IP] [--dpdk-port N] [--local-port N]\n"
                "       [--symbols SYM1,SYM2] [--duration SEC] [--tick-us US]\n"
                "       [--rx-cpu N] [--tx-cpu N] [--main-cpu N]\n",
                "bench_market_single_dpdk");
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

    spdlog::info("bench_market_single_dpdk: server={}:{}, local={}, gateway={}, "
                 "symbols={}, duration={}s",
                 cfg.server_ip, cfg.server_port, local_ip, gateway_ip,
                 cfg.symbols.size(), cfg.duration.count());

    // ── Build TransportConfig for mock server (no TLS, no pings) ────────────
    eph::net::TransportConfig tc;
    tc.remote_host = cfg.server_ip;
    tc.remote_port = cfg.server_port;
    tc.ws_path = "/ws";
    tc.use_tls = false;
    tc.ping_interval = std::chrono::seconds{0};
    tc.max_reconnect_attempts = 0;
    tc.skip_utf8_validation = true;
    if (cfg.rx_cpu >= 0) tc.rx_cpu = cfg.rx_cpu;
    if (cfg.tx_cpu >= 0) tc.tx_cpu = cfg.tx_cpu;

    // ── Resolve server IP and create DPDK connection ────────────────────────
    eph::dpdk::DpdkEndpoint ep{.local_ip = local_ip, .gateway_ip = gateway_ip};
    eph::dpdk::ConnectorOptions opts{
        .platform = {.port_id = dpdk_port},
        .local_port = local_port,
    };

    // Use eph::dpdk::connect() which handles Platform, ARP, TcpSession, and
    // Transport creation in one call.  The default TransportType is
    // DpdkTransport = Transport<TcpSession<>, WsFramer, ...>.
    using DpdkTransport = eph::dpdk::DpdkTransport;

    auto conn = eph::dpdk::connect<DpdkTransport>(ep, tc, opts);
    if (!conn) {
        spdlog::error("DPDK connect failed: {}", conn.error());
        return 1;
    }
    spdlog::info("Connected via DPDK to {}:{}", cfg.server_ip, cfg.server_port);

    // ── Start mock WS server (kernel TCP on NIC-A, same process) ────────────
    bench::mock::MockServerConfig mock_cfg{
        .bind_ip = cfg.server_ip,
        .port = cfg.server_port,
        .symbols = cfg.symbols,
        .tick_interval = cfg.tick_interval,
        .order_mode = false,
    };
    std::atomic<bool> mock_running{true};
    std::thread mock_thread([&] {
        bench::mock::run_mock_ws_server(mock_cfg, mock_running);
    });

    // Give mock server time to start listening
    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    // ── Latency histogram ───────────────────────────────────────────────────
    eph::utils::HdrHistogram latency_hist{10, 1'000'000'000ULL, 3};
    uint64_t msg_count = 0;

    auto& transport = *conn->transport;

    // ── Main loop: drain market data and record latency ─────────────────────
    spdlog::info("Market bench (DPDK) started: {} symbols, duration={}s",
                 cfg.symbols.size(), cfg.duration.count());

    auto start = std::chrono::steady_clock::now();
    auto deadline = start + cfg.duration;

    while (g_running.load(std::memory_order_acquire)
           && transport.is_running()
           && std::chrono::steady_clock::now() < deadline) {
        bool got = transport.recv([&](const uint8_t* data, size_t len) {
            uint64_t t_send = bench::parse_tsc_field(data, static_cast<uint16_t>(len));
            if (t_send > 0) {
                uint64_t now = eph::utils::TSC::now();
                if (now > t_send) {
                    auto ns = eph::utils::TSC::to_ns(now - t_send);
                    if (ns) latency_hist.record(static_cast<uint64_t>(*ns));
                }
            }
            ++msg_count;
        });
        if (!got) eph::utils::cpu_relax();
    }

    // ── Stop and report ─────────────────────────────────────────────────────
    transport.stop();
    mock_running = false;
    mock_thread.join();

    auto duration_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();

    spdlog::info("=== Market Data Benchmark Results (DPDK) ===");
    spdlog::info("Duration: {}s, Messages: {}, Rate: {} msg/s",
                 duration_s, msg_count,
                 duration_s > 0 ? msg_count / static_cast<uint64_t>(duration_s) : 0);
    bench::print_latency("Pipeline Latency (mock send -> app recv)",
                         bench::hdr_to_stats(latency_hist));

    return 0;
}
