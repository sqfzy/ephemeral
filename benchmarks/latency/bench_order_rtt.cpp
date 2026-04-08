/// @file bench_order_rtt.cpp
/// WS order submit + execution report RTT benchmark with 4-leg breakdown.
///
/// Client sends order requests at order_interval, mock server responds with
/// ExecutionReport containing T_recv/T timestamps for 4-leg decomposition:
///   - RTT:    client_send → client_recv
///   - TX:     client_send → server_recv
///   - RX:     server_send → client_recv
///   - Server: server_recv → server_send
///
/// Compiled twice by xmake:
///   - bench_order_rtt:      kernel SocketTransport
///   - bench_order_rtt_dpdk: DPDK TcpSession
///
/// Usage (kernel):
///   bench_order_rtt --server-ip IP [--order-interval 1000]
///       [--duration 10] [--warmup 2] [--poll-cpu 2] [--output FILE.jsonl]

#include <cstdint>
#include <cstdio>
#include <string_view>

#include <spdlog/spdlog.h>

#include "bench_config.hpp"
#include "bench_loop.hpp"
#include "eph/utils/hdr_histogram.hpp"
#include "eph/utils/time.hpp"

#include "eph/transport/direct_transport.hpp"
#include "eph/transport/transport_types.hpp"

#if defined(EPH_USE_DPDK)
#include "eph/dpdk/connector.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/tcp.hpp"
#include "mock/mock_ws_server.hpp"
using BenchTcpImpl = eph::dpdk::TcpSession<>;
#else
#include "eph/net/socket_transport.hpp"
#include "eph/net/socket_config.hpp"
using BenchTcpImpl = eph::net::SocketTransport;
#endif

// ── TSC parsing ──────────────────────────────────────────────────────────

static uint64_t parse_json_tsc(std::string_view json, std::string_view key) {
    auto pos = json.find(key);
    if (pos == std::string_view::npos) return 0;
    pos += key.size();
    while (pos < json.size() && json[pos] == ' ') ++pos;
    uint64_t val = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + static_cast<uint64_t>(json[pos] - '0');
        ++pos;
    }
    return val;
}

static uint64_t parse_tsc_T(const uint8_t* data, size_t len) {
    std::string_view json(reinterpret_cast<const char*>(data), len);
    uint64_t val = parse_json_tsc(json, ",\"T\":");
    if (val > 0) return val;
    return parse_json_tsc(json, "{\"T\":");
}

static uint64_t parse_tsc_T_recv(const uint8_t* data, size_t len) {
    std::string_view json(reinterpret_cast<const char*>(data), len);
    return parse_json_tsc(json, "\"T_recv\":");
}

static uint64_t tsc_to_ns(uint64_t cycles) {
    return static_cast<uint64_t>(eph::utils::TSC::to_ns(cycles).value_or(0.0));
}

static eph::net::TransportConfig make_bench_tc(const bench::BenchConfig& cfg) {
    eph::net::TransportConfig tc;
    tc.remote_host = cfg.server_ip;
    tc.remote_port = cfg.server_port;
    tc.ws_path = "/ws";
    tc.use_tls = false;
    tc.ping_interval = std::chrono::seconds{0};
    tc.max_reconnect_attempts = 0;
    tc.skip_utf8_validation = true;
    return tc;
}

// ── Order RTT poll loop ──────────────────────────────────────────────────

template <typename TransportT>
static void run_order_rtt_loop(TransportT& transport, bench::BenchConfig& cfg,
                                const char* label, bench::JsonlWriter& jsonl,
                                const char* transport_name) {
    eph::utils::HdrHistogram rtt_hist{10, 1'000'000'000ULL, 3};
    eph::utils::HdrHistogram tx_hist{10, 1'000'000'000ULL, 3};
    eph::utils::HdrHistogram rx_hist{10, 1'000'000'000ULL, 3};
    eph::utils::HdrHistogram srv_hist{10, 1'000'000'000ULL, 3};

    uint64_t last_order_tsc = 0;
    uint64_t order_count = 0, response_count = 0;
    bool in_warmup = true;

    auto& tc = const_cast<eph::net::TransportConfig&>(transport.config());
    tc.on_message = [&](const uint8_t* data, uint16_t len, uint8_t) {
        std::string_view json(reinterpret_cast<const char*>(data), len);
        if (json.find("\"e\":\"executionReport\"") == std::string_view::npos) return;

        ++response_count;
        if (in_warmup) return;

        uint64_t client_recv_tsc = eph::utils::TSC::now();

        // RTT
        if (last_order_tsc > 0 && client_recv_tsc > last_order_tsc) {
            [[maybe_unused]] auto _ = rtt_hist.record(
                tsc_to_ns(client_recv_tsc - last_order_tsc));
        }

        // RX: server_send (T) → client_recv
        uint64_t t_send = parse_tsc_T(data, len);
        if (t_send > 0 && client_recv_tsc > t_send) {
            [[maybe_unused]] auto _ = rx_hist.record(
                tsc_to_ns(client_recv_tsc - t_send));
        }

        // TX: client_send → server_recv (T_recv)
        uint64_t t_recv = parse_tsc_T_recv(data, len);
        if (t_recv > 0 && last_order_tsc > 0 && t_recv > last_order_tsc) {
            [[maybe_unused]] auto _ = tx_hist.record(
                tsc_to_ns(t_recv - last_order_tsc));
        }

        // Server: recv → send
        if (t_recv > 0 && t_send > 0 && t_send > t_recv) {
            [[maybe_unused]] auto _ = srv_hist.record(
                tsc_to_ns(t_send - t_recv));
        }
    };

    bench::pin_or_die(cfg.poll_cpu, "bench-poll");

    spdlog::info("{}: order_interval={}us, warmup={}s, duration={}s",
                 label, cfg.order_interval.count(),
                 cfg.warmup.count(), cfg.duration.count());

    bench::BenchTimer timer;
    timer.start(cfg.warmup, cfg.duration);

    auto next_order = std::chrono::steady_clock::now();

    while (timer.is_running() && bench::g_running.load(std::memory_order_relaxed)
           && transport.is_running()) {
        in_warmup = timer.is_warmup();

        auto now = std::chrono::steady_clock::now();
        if (now >= next_order) {
            last_order_tsc = eph::utils::TSC::now();
            char buf[256];
            int n = std::snprintf(buf, sizeof(buf),
                R"({"method":"order.place","symbol":"BTCUSDT",)"
                R"("side":"BUY","price":"50000.00",)"
                R"("quantity":"0.001","T_send":%llu})",
                static_cast<unsigned long long>(last_order_tsc));
            [[maybe_unused]] auto err = transport.send_text(
                std::string_view(buf, static_cast<size_t>(n)));
            ++order_count;
            next_order += cfg.order_interval;
        }

        [[maybe_unused]] auto poll_result = transport.poll();
    }

    auto result = bench::BenchResult{
        bench::compute_stats(rtt_hist), bench::compute_stats(tx_hist),
        bench::compute_stats(rx_hist), bench::compute_stats(srv_hist),
    };
    bench::print_bench_result(label, 0, result);
    jsonl.write("order_rtt", transport_name, 0, result);
    spdlog::info("  orders: {}, responses: {}", order_count, response_count);
}

// ── DPDK mock helper ─────────────────────────────────────────────────────

#if defined(EPH_USE_DPDK)
static bench::MockHandle start_order_mock(const bench::BenchConfig& cfg) {
    bench::MockHandle h;
    bench::mock::MockServerConfig mock_cfg{
        .bind_ip = cfg.server_ip,
        .port = cfg.server_port,
        .symbols = cfg.symbols,
        .tick_interval = cfg.tick_interval,
        .order_mode = true,
    };
    int cpu = cfg.mock_cpu;
    h.thread = std::thread([mock_cfg, cpu, &running = *h.running] {
        bench::pin_or_die(cpu, "mock-server");
        bench::mock::run_mock_ws_server(mock_cfg, running);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    return h;
}
#endif

// ── main ─────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    bench::install_signal_handlers();

    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC calibration failed");
        return 1;
    }

#if defined(EPH_USE_DPDK)
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
    if (!eal) { spdlog::error("EAL init failed: {}", eal.error()); return 1; }

    auto cfg = bench::parse_bench_config(app_argc, app_argv);
    if (cfg.server_ip.empty() || cfg.local_ip.empty() || cfg.gateway_ip.empty()) {
        spdlog::error("--server-ip, --local-ip, --gateway-ip are all required");
        return 1;
    }

    auto mock = start_order_mock(cfg);

    using BenchTransport = eph::net::DirectTransport<BenchTcpImpl, eph::net::WsFramer, 4096>;
    auto tc = make_bench_tc(cfg);
    eph::dpdk::DpdkEndpoint ep{.local_ip = cfg.local_ip, .gateway_ip = cfg.gateway_ip};
    eph::dpdk::ConnectorOptions opts{.platform = {.port_id = cfg.dpdk_port_id}};

    auto conn = eph::dpdk::connect<BenchTransport>(ep, tc, opts);
    if (!conn) {
        spdlog::error("DPDK connect failed: {}", conn.error());
        bench::stop_mock(mock);
        return 1;
    }

    bench::JsonlWriter jsonl(cfg.output_path);
    run_order_rtt_loop(*conn->transport, cfg, "Order RTT (DPDK)", jsonl, "dpdk");
    conn->transport->stop();
    bench::stop_mock(mock);

#else
    auto cfg = bench::parse_bench_config(argc, argv);
    if (cfg.server_ip.empty()) {
        spdlog::error("--server-ip is required");
        return 1;
    }

    using BenchTransport = eph::net::DirectTransport<BenchTcpImpl, eph::net::WsFramer, 4096>;
    auto tc = make_bench_tc(cfg);

    auto tcp_factory = [&]() -> std::expected<std::unique_ptr<BenchTcpImpl>, std::string> {
        eph::net::SocketConfig sc{.host = cfg.server_ip, .port = cfg.server_port,
                                  .tcp_nodelay = true};
        auto tcp = std::make_unique<BenchTcpImpl>(sc);
        auto r = tcp->connect(std::chrono::milliseconds{3000});
        if (!r) return std::unexpected(r.error());
        return tcp;
    };

    auto result = BenchTransport::create(std::move(tcp_factory), tc);
    if (!result) {
        spdlog::error("Transport create failed: {}", result.error().message());
        return 1;
    }

    bench::JsonlWriter jsonl(cfg.output_path);
    run_order_rtt_loop(**result, cfg, "Order RTT (kernel)", jsonl, "kernel");
    (*result)->stop();
#endif

    return 0;
}
