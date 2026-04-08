/// @file bench_market_rx.cpp
/// WS market data RX benchmark — server pushes bookTicker, client measures
/// pipeline latency (mock_send → app_recv).
///
/// This is a 1-leg benchmark: only RTT (pipeline latency) is reported.
/// TX/RX/Server legs have samples=0.
///
/// Compiled twice by xmake:
///   - bench_market_rx:      kernel SocketTransport
///   - bench_market_rx_dpdk: DPDK TcpSession
///
/// Usage (kernel):
///   bench_market_rx --server-ip IP [--symbols SYM1,SYM2]
///       [--duration 10] [--warmup 2] [--poll-cpu 2] [--output FILE.jsonl]

#include <cstdint>
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

// ── Market RX poll loop ──────────────────────────────────────────────────

template <typename TransportT>
static void run_market_rx_loop(TransportT& transport, bench::BenchConfig& cfg,
                                const char* label, bench::JsonlWriter& jsonl,
                                const char* transport_name) {
    eph::utils::HdrHistogram pipeline_hist{10, 1'000'000'000ULL, 3};
    uint64_t msg_count = 0;
    bool in_warmup = true;

    auto& tc = const_cast<eph::net::TransportConfig&>(transport.config());
    tc.on_message = [&](const uint8_t* data, uint16_t len, uint8_t) {
        ++msg_count;
        if (in_warmup) return;

        uint64_t t_send = parse_tsc_T(data, len);
        if (t_send > 0) {
            uint64_t now = eph::utils::TSC::now();
            if (now > t_send) {
                auto ns = eph::utils::TSC::to_ns(now - t_send);
                if (ns) [[maybe_unused]] auto _ = pipeline_hist.record(
                    static_cast<uint64_t>(*ns));
            }
        }
    };

    bench::pin_or_die(cfg.poll_cpu, "bench-poll");

    spdlog::info("{}: symbols={}, tick_interval={}us, warmup={}s, duration={}s",
                 label, cfg.symbols.size(), cfg.tick_interval.count(),
                 cfg.warmup.count(), cfg.duration.count());

    bench::BenchTimer timer;
    timer.start(cfg.warmup, cfg.duration);

    while (timer.is_running() && bench::g_running.load(std::memory_order_relaxed)
           && transport.is_running()) {
        in_warmup = timer.is_warmup();
        [[maybe_unused]] auto poll_result = transport.poll();
    }

    auto pipeline_stats = bench::compute_stats(pipeline_hist);

    // 1-leg result: only RTT (pipeline latency), others empty
    auto result = bench::BenchResult{
        .rtt = pipeline_stats,
        .tx = {},
        .rx = {},
        .srv = {},
    };
    bench::print_bench_result(label, 0, result);
    jsonl.write("market_rx", transport_name, 0, result);
    spdlog::info("  messages: {}", msg_count);
}

// ── DPDK mock helper ─────────────────────────────────────────────────────

#if defined(EPH_USE_DPDK)
static bench::MockHandle start_market_mock(const bench::BenchConfig& cfg) {
    bench::MockHandle h;
    bench::mock::MockServerConfig mock_cfg{
        .bind_ip = cfg.server_ip,
        .port = cfg.server_port,
        .symbols = cfg.symbols,
        .tick_interval = cfg.tick_interval,
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

    auto mock = start_market_mock(cfg);

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
    run_market_rx_loop(*conn->transport, cfg, "Market RX (DPDK)", jsonl, "dpdk");
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
    run_market_rx_loop(**result, cfg, "Market RX (kernel)", jsonl, "kernel");
    (*result)->stop();
#endif

    return 0;
}
