/// @file bench_market_persymbol_dpdk.cpp
/// Per-symbol independent connection benchmark — DPDK (kernel-bypass).
///
/// Each symbol gets its own Transport + TcpSession + TLS + WS connection,
/// all sharing a single NIC RX queue via SharedRxDispatcher. This eliminates
/// the combined stream's batch processing bottleneck: each TLS record
/// contains exactly 1 WS frame.
///
/// Architecture:
///   [NIC queue 0] → [SharedRxDispatcher] → ring₁ → TcpSession₁ → Transport₁
///                                        → ring₂ → TcpSession₂ → Transport₂
///                                        → ring₃ → TcpSession₃ → Transport₃
///
/// Usage:
///   sudo ./bench_market_persymbol_dpdk -a 0000:28:00.0 -l 4-7
///       -- --local-ip 172.31.23.112 --gateway-ip 172.31.16.1
///       --symbols btcusdt,ethusdt,solusdt --duration 30
///       --dispatch-cpu 8 --main-cpu 14

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
#include "eph/dpdk/shared_rx.hpp"
#include "eph/utils/hdr_histogram.hpp"
#include "eph/utils/time.hpp"
#include "eph/utils/cpu.hpp"

// Per-symbol: small payload (single bookTicker), latest-only delivery.
using BenchTransport = eph::net::Transport<
    eph::dpdk::TcpSession<>,
    eph::net::WsFramer,
    16384, 1024,
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
    int  dispatch_cpu      = -1;  // CPU for SharedRxDispatcher
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

static Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 0; i < argc; ++i) {
        std::string_view a = argv[i];
        auto next = [&](std::string_view n) -> const char* {
            if (i + 1 >= argc) { std::cerr << std::format("{} requires a value\n", n); std::exit(1); }
            return argv[++i];
        };
        if      (a == "--host")         c.host         = next(a);
        else if (a == "--port")         c.port         = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--symbols")      c.symbols      = split(next(a), ',');
        else if (a == "--local-ip")     c.local_ip     = next(a);
        else if (a == "--gateway-ip")   c.gateway_ip   = next(a);
        else if (a == "--dpdk-port")    c.dpdk_port    = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--duration")     c.duration     = std::atoi(next(a));
        else if (a == "--main-cpu")     c.main_cpu     = std::atoi(next(a));
        else if (a == "--dispatch-cpu") c.dispatch_cpu = std::atoi(next(a));
        else if (a == "--no-tls")       c.use_tls      = false;
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
    spdlog::info("Per-symbol benchmark: {} symbols via SharedRxDispatcher", N);

    spdlog::info("Calibrating TSC...");
    if (!eph::utils::TSC::init(std::chrono::milliseconds{200})) {
        spdlog::error("TSC calibration failed"); return 1;
    }

    spdlog::info("Initializing DPDK EAL...");
    auto eal = eph::dpdk::EalGuard::init(eal_argc, eal_argv);
    if (!eal) { spdlog::error("EAL init failed: {}", eal.error()); return 1; }

    // ── Step 1: Create Platform (single queue pair) ─────────────────────────
    eph::dpdk::PlatformConfig pcfg{
        .port_id      = cfg.dpdk_port,
        .nb_rx_queues = 1,
        .nb_tx_queues = 1,
        .mbuf_pool_size = 8191,
    };
    auto platform = eph::dpdk::Platform::create(pcfg);
    if (!platform) { spdlog::error("Platform: {}", platform.error()); return 1; }

    // ── Step 2: Connect all symbols sequentially (no RX threads yet) ────────
    // First connection resolves ARP; subsequent reuse gateway MAC.
    eph::dpdk::DpdkEndpoint ep{
        .local_ip = cfg.local_ip, .gateway_ip = cfg.gateway_ip};

    struct SessionInfo {
        std::string symbol;
        std::unique_ptr<BenchTransport> transport;
    };
    std::vector<SessionInfo> sessions;

    // We need to access the raw TcpSession to register with the dispatcher.
    // Strategy: connect via the standard connector (which handles ARP, TCP,
    // TLS, WS upgrade), then the Transport's internal TcpSession is already
    // connected. We set the shared_rx_source on it before Transport starts
    // its RX thread.
    //
    // But Transport::create() immediately starts threads. So we need a
    // different approach: use the first connection to get the gateway MAC,
    // then create all sessions via connector(platform&) which returns
    // unique_ptr<Transport>. The Transport starts RX immediately, but if
    // we've already set shared_rx_ring on the TcpSession, poll_rx will
    // use the ring.
    //
    // Problem: we can't access Transport's internal TcpSession easily.
    //
    // Simplest approach: Connect the first symbol with the full connector
    // (creates its own Platform), get the gateway MAC. Then for ALL symbols
    // (including the first), create fresh connections using the shared
    // Platform. This wastes the first connection but avoids complexity.

    // Resolve gateway MAC via a temporary connection on the shared platform
    spdlog::info("Resolving gateway MAC...");
    eph::dpdk::ConnectorOptions resolve_opts{};
    auto server_ip = eph::dpdk::resolve_hostname(cfg.host);
    if (!server_ip) { spdlog::error("DNS: {}", server_ip.error()); return 1; }

    rte_ether_addr src_mac{};
    rte_eth_macaddr_get(cfg.dpdk_port, &src_mac);
    auto gw_mac = eph::dpdk::arp::resolve(
        cfg.dpdk_port, 0, platform->mempool(),
        src_mac, eph::dpdk::net::parse_ipv4(cfg.local_ip.c_str()),
        eph::dpdk::net::parse_ipv4(cfg.gateway_ip.c_str()),
        std::chrono::milliseconds{3000});
    if (!gw_mac) { spdlog::error("ARP: {}", gw_mac.error()); return 1; }
    spdlog::info("Gateway MAC resolved");

    // Create dispatcher
    auto dispatcher = eph::dpdk::SharedRxDispatcher::create(cfg.dpdk_port, 0, N);
    if (!dispatcher) { spdlog::error("Dispatcher: {}", dispatcher.error()); return 1; }

    // Connect each symbol sequentially. TCP+TLS+WS handshake uses direct
    // rte_eth_rx_burst (no concurrent RX threads). After handshake, the
    // on_connected_before_threads hook registers with the dispatcher,
    // setting shared_rx_ring BEFORE the RX thread starts.
    //
    std::vector<eph::dpdk::TcpSession<>*> session_ptrs(N, nullptr);

    for (size_t i = 0; i < N; ++i) {
        auto& sym = cfg.symbols[i];
        auto ws_path = "/ws/" + sym + "@bookTicker";

        eph::dpdk::ConnectorOptions opts{};
        opts.gateway_mac = *gw_mac;

        eph::net::TransportConfig tc{
            .remote_host = cfg.host, .remote_port = cfg.port,
            .ws_path = ws_path, .use_tls = cfg.use_tls, .verify_peer = cfg.verify,
            .max_reconnect_attempts = 0,
            .ping_interval = std::chrono::seconds{0},
            .skip_utf8_validation = true,
            .on_state_change = [sym](eph::net::TransportEvent e, std::string_view d) {
                spdlog::info("[{}] {} — {}", sym, eph::net::transport_event_name(e), d);
            },
            .deferred_start = true,  // don't start RX/TX threads yet
        };

        // Build tcp_factory that captures session_ptr
        auto setup = eph::dpdk::detail::prepare_connection(
            ep, opts, tc, *server_ip, platform->mempool());
        if (!setup) {
            spdlog::error("Failed to prepare {}: {}", sym, setup.error());
            return 1;
        }

        // Wrap the factory to capture the session pointer
        auto inner_factory = std::move(setup->tcp_factory);
        auto tcp_factory = [&session_ptrs, i, inner = std::move(inner_factory)]() mutable
            -> std::expected<std::unique_ptr<eph::dpdk::TcpSession<>>, std::string> {
            auto result = inner();
            if (result) session_ptrs[i] = result->get();
            return result;
        };

        spdlog::info("Connecting {}...", sym);
        auto transport = BenchTransport::create(std::move(tcp_factory), tc);
        if (!transport) {
            spdlog::error("Failed to connect {}: {}", sym, transport.error().message());
            return 1;
        }
        sessions.push_back({sym, std::move(*transport)});
        spdlog::info("Connected: {}", sym);
    }

    // ── Step 3: Register all sessions → start dispatcher → start threads ──
    for (size_t i = 0; i < N; ++i) {
        if (!session_ptrs[i]) {
            spdlog::error("Session {} has null pointer", i);
            return 1;
        }
        auto reg = (*dispatcher)->register_session(*session_ptrs[i]);
        if (!reg) {
            spdlog::error("Failed to register {}: {}", cfg.symbols[i], reg.error());
            return 1;
        }
        spdlog::info("  {} registered with dispatcher", cfg.symbols[i]);
    }

    spdlog::info("Starting dispatcher (cpu={})...", cfg.dispatch_cpu);
    (*dispatcher)->start(cfg.dispatch_cpu);

    spdlog::info("Starting {} Transport RX/TX threads...", N);
    for (auto& s : sessions) {
        s.transport->start_threads();
    }
    spdlog::info("All {} symbols ready!", N);

    // ── Main loop ───────────────────────────────────────────────────────────
    auto start = std::chrono::steady_clock::now();
    auto deadline = cfg.duration > 0
        ? start + std::chrono::seconds(cfg.duration)
        : std::chrono::steady_clock::time_point::max();

    std::vector<uint64_t> msg_counts(N, 0);

    while (g_running.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) break;

        for (size_t i = 0; i < N; ++i) {
            auto& tp = *sessions[i].transport;
            if (!tp.is_running()) continue;
            tp.recv([&](const uint8_t*, size_t) { ++msg_counts[i]; });
        }
        eph::utils::cpu_relax();
    }

    // ── Report ──────────────────────────────────────────────────────────────
    (*dispatcher)->stop();
    for (auto& s : sessions) s.transport->stop();

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    spdlog::info("=== Per-Symbol Market Data Benchmark (DPDK + SharedRxDispatcher) ===");
    spdlog::info("Symbols: {} | Duration: {:.1f}s", N, elapsed_ms / 1000.0);

    for (size_t i = 0; i < N; ++i) {
        auto stats = sessions[i].transport->stats();
        auto& rx = stats.rx_latency;
        double avg_fpr = rx.count > 0
            ? static_cast<double>(stats.rx_packets) / rx.count : 0.0;

        spdlog::info("--- {} ---", sessions[i].symbol);
        spdlog::info("  Messages: {} | Avg frames/record: {:.1f}", msg_counts[i], avg_fpr);
        if (rx.count > 0) {
            spdlog::info("  RX: p50={} p99={} p99.9={} max={} ns",
                         rx.p50_ns, rx.p99_ns, rx.p999_ns, rx.max_ns);
        }
    }

    return 0;
}
