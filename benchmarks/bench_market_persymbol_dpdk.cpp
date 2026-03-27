/// @file bench_market_persymbol_dpdk.cpp
/// Per-symbol independent connection benchmark — DPDK (kernel-bypass).
///
/// Each symbol gets its own Transport + TcpSession + TLS session + WS connection,
/// using a separate RX/TX queue pair on the same NIC. This eliminates the
/// combined stream's batch processing bottleneck: each TLS record contains
/// exactly 1 WS frame, so p99 approaches single-frame decrypt+decode time.
///
/// Usage (3 symbols = 3 queue pairs = 6 dedicated cores):
///   sudo ./bench_market_persymbol_dpdk -a 0000:28:00.0 -l 4-7
///       -- --local-ip 172.31.23.112 --gateway-ip 172.31.16.1
///       --symbols btcusdt,ethusdt,solusdt --duration 30
///       --rx-cpus 8,9,10 --tx-cpus 11,12,13 --main-cpu 14

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/containers/evicting_queue.hpp"
#include "eph/dpdk/connector.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/utils/hdr_histogram.hpp"
#include "eph/utils/time.hpp"
#include "eph/utils/cpu.hpp"

// Per-symbol: small payload (single bookTicker), latest-only delivery.
using BenchTransport = eph::net::Transport<
    eph::dpdk::TcpSession<>,
    eph::net::WsFramer,
    4096, 1024,
    eph::containers::EvictingQueue,
    true   // LastOnlyDeliver — only latest snapshot per symbol
>;

/// Convert HdrHistogram to RttStats.
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

struct Config {
    std::string host       = "fstream.binance.com";
    uint16_t    port       = 443;
    std::vector<std::string> symbols = {"btcusdt", "ethusdt", "solusdt"};
    std::string local_ip{};
    std::string gateway_ip{};
    uint16_t    dpdk_port  = 0;
    int  duration          = 30;
    bool use_tls           = true;
    bool verify            = false;
    int  main_cpu          = -1;
    std::vector<int> rx_cpus;   // one per symbol
    std::vector<int> tx_cpus;   // one per symbol
};

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

static std::vector<int> parse_ints(const std::string& s) {
    std::vector<int> result;
    for (auto& tok : split(s, ','))
        result.push_back(std::atoi(tok.c_str()));
    return result;
}

static Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 0; i < argc; ++i) {
        std::string_view a = argv[i];
        auto next = [&](std::string_view n) -> const char* {
            if (i + 1 >= argc) { std::cerr << std::format("{} requires a value\n", n); std::exit(1); }
            return argv[++i];
        };
        if      (a == "--host")       c.host       = next(a);
        else if (a == "--port")       c.port       = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--symbols")    c.symbols    = split(next(a), ',');
        else if (a == "--local-ip")   c.local_ip   = next(a);
        else if (a == "--gateway-ip") c.gateway_ip = next(a);
        else if (a == "--dpdk-port")  c.dpdk_port  = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--duration")   c.duration   = std::atoi(next(a));
        else if (a == "--main-cpu")   c.main_cpu   = std::atoi(next(a));
        else if (a == "--rx-cpus")    c.rx_cpus    = parse_ints(next(a));
        else if (a == "--tx-cpus")    c.tx_cpus    = parse_ints(next(a));
        else if (a == "--no-tls")     c.use_tls    = false;
        else if (a == "--help") {
            std::cerr << std::format(
                "Usage: {} [EAL args] -- [--host H] [--port P] [--symbols S1,S2,S3]\n"
                "       [--local-ip IP] [--gateway-ip IP] [--dpdk-port N]\n"
                "       [--duration SEC] [--main-cpu N] [--rx-cpus N,N,N] [--tx-cpus N,N,N]\n",
                "bench_market_persymbol_dpdk");
            std::exit(0);
        }
    }
    return c;
}

int main(int argc, char** argv) {
    std::signal(SIGINT, sig);
    std::signal(SIGTERM, sig);
    spdlog::set_level(spdlog::level::info);

    int eal_argc = argc; char** eal_argv = argv;
    int app_argc = 0;    char** app_argv = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--") == 0) {
            eal_argc = i; app_argc = argc - i - 1; app_argv = argv + i + 1; break;
        }
    }
    auto cfg = parse_args(app_argc, app_argv);
    if (cfg.main_cpu >= 0) (void)eph::utils::set_thread_affinity(cfg.main_cpu, "main");
    if (cfg.local_ip.empty() || cfg.gateway_ip.empty()) {
        spdlog::error("--local-ip and --gateway-ip are required"); return 1;
    }

    size_t N = cfg.symbols.size();
    spdlog::info("Per-symbol benchmark: {} symbols, {} connections", N, N);

    spdlog::info("Calibrating TSC...");
    if (!eph::utils::TSC::init(std::chrono::milliseconds{200})) {
        spdlog::error("TSC calibration failed"); return 1;
    }
    spdlog::info("TSC: {:.4f} ns/cycle", eph::utils::TSC::get_ns_per_cycle().value());

    spdlog::info("Initializing DPDK EAL...");
    auto eal = eph::dpdk::EalGuard::init(eal_argc, eal_argv);
    if (!eal) { spdlog::error("EAL init failed: {}", eal.error()); return 1; }

    // Connect each symbol on its own queue pair.
    // First connection creates Platform + resolves ARP; subsequent connections
    // reuse the shared Platform and pre-resolved gateway MAC.
    struct SymbolConnection {
        std::string symbol;
        std::unique_ptr<BenchTransport> transport;
    };
    std::vector<SymbolConnection> connections;

    eph::dpdk::DpdkEndpoint ep{
        .local_ip = cfg.local_ip, .gateway_ip = cfg.gateway_ip};

    // First symbol: create Platform (N queues) + ARP
    {
        auto& sym = cfg.symbols[0];
        auto ws_path = "/ws/" + sym + "@bookTicker";
        int rx_cpu = (!cfg.rx_cpus.empty()) ? cfg.rx_cpus[0] : -1;
        int tx_cpu = (!cfg.tx_cpus.empty()) ? cfg.tx_cpus[0] : -1;

        eph::net::TransportConfig tc{
            .remote_host = cfg.host, .remote_port = cfg.port,
            .ws_path = ws_path, .use_tls = cfg.use_tls, .verify_peer = cfg.verify,
            .max_reconnect_attempts = 3,
            .ping_interval = std::chrono::seconds{0},
            .skip_utf8_validation = true,
            .tx_cpu = tx_cpu, .rx_cpu = rx_cpu,
            .on_state_change = [sym](eph::net::TransportEvent e, std::string_view d) {
                spdlog::info("[{}] {} — {}", sym, eph::net::transport_event_name(e), d);
            },
        };

        eph::dpdk::ConnectorOptions opts{};
        opts.platform.port_id      = cfg.dpdk_port;
        opts.platform.nb_rx_queues = static_cast<uint16_t>(N);
        opts.platform.nb_tx_queues = static_cast<uint16_t>(N);
        opts.platform.mbuf_pool_size = 8191;

        spdlog::info("Connecting {} (queue pair 0, creates Platform with {} queues)...",
                     sym, N);
        auto result = eph::dpdk::connect<BenchTransport>(ep, tc, opts);
        if (!result) {
            spdlog::error("Failed to connect {}: {}", sym, result.error());
            return 1;
        }
        spdlog::info("Connected: {} (gateway MAC resolved)", sym);
        connections.push_back({sym, std::move(result->transport)});

        // Subsequent symbols: reuse Platform + gateway MAC
        for (size_t i = 1; i < N; ++i) {
            auto& sym2 = cfg.symbols[i];
            auto ws_path2 = "/ws/" + sym2 + "@bookTicker";
            int rx2 = (i < cfg.rx_cpus.size()) ? cfg.rx_cpus[i] : -1;
            int tx2 = (i < cfg.tx_cpus.size()) ? cfg.tx_cpus[i] : -1;

            eph::net::TransportConfig tc2{
                .remote_host = cfg.host, .remote_port = cfg.port,
                .ws_path = ws_path2, .use_tls = cfg.use_tls, .verify_peer = cfg.verify,
                .max_reconnect_attempts = 3,
                .ping_interval = std::chrono::seconds{0},
                .skip_utf8_validation = true,
                .tx_cpu = tx2, .rx_cpu = rx2,
                .on_state_change = [sym2](eph::net::TransportEvent e, std::string_view d) {
                    spdlog::info("[{}] {} — {}", sym2, eph::net::transport_event_name(e), d);
                },
            };

            eph::dpdk::ConnectorOptions opts2{};
            opts2.rx_queue_id = static_cast<uint16_t>(i);
            opts2.tx_queue_id = static_cast<uint16_t>(i);
            opts2.gateway_mac = result->gateway_mac;  // skip ARP

            spdlog::info("Connecting {} (queue pair {}, rx_cpu={}, tx_cpu={})...",
                         sym2, i, rx2, tx2);
            auto conn = eph::dpdk::connect<BenchTransport>(
                result->platform, ep, tc2, opts2);
            if (!conn) {
                spdlog::error("Failed to connect {}: {}", sym2, conn.error());
                return 1;
            }
            connections.push_back({sym2, std::move(*conn)});
            spdlog::info("Connected: {}", sym2);
        }
    }

    spdlog::info("All {} symbols connected!", N);

    // ── Main loop: drain all connections ────────────────────────────────────
    auto start = std::chrono::steady_clock::now();
    auto deadline = cfg.duration > 0
        ? start + std::chrono::seconds(cfg.duration)
        : std::chrono::steady_clock::time_point::max();

    std::vector<uint64_t> msg_counts(N, 0);

    while (g_running.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) break;

        for (size_t i = 0; i < N; ++i) {
            auto& tp = *connections[i].transport;
            if (!tp.is_running()) continue;
            tp.recv([&](const uint8_t*, size_t) {
                ++msg_counts[i];
            });
        }
        eph::utils::cpu_relax();
    }

    // ── Report ──────────────────────────────────────────────────────────────
    for (size_t i = 0; i < N; ++i) {
        connections[i].transport->stop();
    }
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    spdlog::info("=== Per-Symbol Market Data Benchmark (DPDK) ===");
    spdlog::info("Symbols: {} | Duration: {:.1f}s", N, elapsed_ms / 1000.0);

    for (size_t i = 0; i < N; ++i) {
        auto stats = connections[i].transport->stats();
        auto& rx = stats.rx_latency;
        double avg_fpr = rx.count > 0
            ? static_cast<double>(stats.rx_packets) / rx.count : 0.0;

        spdlog::info("--- {} ---", connections[i].symbol);
        spdlog::info("  Messages: {} | Avg frames/record: {:.1f}",
                     msg_counts[i], avg_fpr);
        if (rx.count > 0) {
            spdlog::info("  RX: p50={} p99={} p99.9={} max={} ns",
                         rx.p50_ns, rx.p99_ns, rx.p999_ns, rx.max_ns);
        }
    }

    return 0;
}
