/// @file lat_ex_market.cpp
/// Latency benchmark: exchange bookTicker push (WebSocket one-way) against
/// the mockex `ex_market_push` scenario (benchmarks/mockex/). The same
/// binary can dial a real exchange directly via `endpoint = wss://...`
/// in bench.conf — see benchmarks/mockex/README.md.
///
/// Structure:
///
///   * Single-file scenario binary that reads `[lat_ex_market]` from
///     bench.conf (port / ws_path / push_rate_hz / duration_seconds)
///     plus the lowercase global `mock_ip`, `warmup_samples`.
///   * Uses `KernelTcpStream<WsCodec, false>` with
///     `StreamConfig.ws_path` set — the transparent WS upgrade runs
///     inside `create()`. Once connected we run the measurement
///     loop by polling the stream; the client sends NOTHING after the
///     handshake because the mock pushes frames unilaterally at
///     `push_rate_hz` for `duration_seconds`.
///   * One-way latency is measured entirely inside `on_message`:
///     the client stamps `t_recv = monotonic_raw_ns()` on arrival and
///     extracts the server-stamped `T":<ns>` field from the JSON
///     payload via `bench::scan_json_uint_field` (we do NOT
///     pull eph-json into the bench path). Sample is `t_recv - t_server`.
///   * Measurement clock is `bench::monotonic_raw_ns()`
///     (CLOCK_MONOTONIC_RAW via vDSO). The mock stamps
///     `T` via the same clock, so the two ends share a time base on
///     the same host.
///
/// Per-sample lifetime: `rec` and `sample_idx` are captured by reference
/// in the `on_message` lambda. They are stack-local in main() and
/// outlive the poller loop (the stream is released before main returns).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include <arpa/inet.h>        // inet_ntop
#include <netdb.h>            // getaddrinfo (Phase 4 real-server mode)
#include <netinet/in.h>
#include <sys/socket.h>


#include <spdlog/spdlog.h>

// eph-* headers.
#include "eph/codec/ws_codec.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/utils/recorder.hpp"

#if defined(EPH_USE_DPDK)
// DpdkTcpStream<WsCodec> one-way bookTicker measurement. Same on_message +
// scan_json_uint_field("T") logic as the kernel branch.
#  include "eph/net/dpdk/poller.hpp"
#  include "eph/net/dpdk/tcp_stream.hpp"
#else
#  include "eph/net/kernel/config.hpp"
#  include "eph/net/kernel/poller.hpp"
#  include "eph/net/kernel/tcp_stream.hpp"
#endif

#include "core/bench_ctx.hpp"
#include "core/endpoint.hpp"          // Phase 4: resolve_endpoint
#include "core/json_scan.hpp"
#include "core/measurement.hpp"
#include "core/pin_client.hpp"
#if defined(EPH_USE_DPDK)
#  include "core/dpdk_env.hpp"
#endif

#include "scenarios/lat_ex_market_loop.hpp"

namespace {

namespace ec = eph::codec;
namespace eu = eph::utils;
namespace en = eph::net;

#if defined(EPH_USE_DPDK)
namespace ed = eph::net::dpdk;
using Stream    = ed::DpdkTcpStream<ec::WsCodec, /*EnableTls=*/false>;
// DPDK supports TLS via DpdkTcpStream's in-place mbuf decrypt; used
// only for `[scenarios.lat_ex_market].use_tls = true` (mock+TLS).
// The `endpoint = wss://...` real-server flow stays kernel-only.
using StreamTls = ed::DpdkTcpStream<ec::WsCodec, /*EnableTls=*/true>;
using Poller    = ed::DpdkPoller<>;
#else
namespace ek = eph::net::kernel;
using Stream    = ek::KernelTcpStream<ec::WsCodec, /*EnableTls=*/false>;
using StreamTls = ek::KernelTcpStream<ec::WsCodec, /*EnableTls=*/true>;
using Poller    = ek::KernelPoller;
#endif

constexpr const char* kDefaultConfigPath = "benchmarks/latency/bench.conf";

[[nodiscard, maybe_unused]] const char* parse_config_path(int argc, char** argv) noexcept {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0) {
            return argv[i + 1];
        }
    }
    if (const char* env = std::getenv("BENCH_CONFIG"); env && *env) {
        return env;
    }
    return kDefaultConfigPath;
}

} // namespace

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::info);

    const char* conf_path = parse_config_path(argc, argv);

    // ── Load config.toml into structured BenchConfig.
    auto cfg_r = bench::load_bench_conf(conf_path);
    if (!cfg_r) {
        std::fprintf(stderr, "lat_ex_market: %s\n",
                     bench::format_error(cfg_r.error()).c_str());
        return 1;
    }
    const bench::BenchConfig& bench_cfg = *cfg_r;
    const bench::Scenario* sc = bench_cfg.scenario("lat_ex_market");
    if (sc == nullptr) {
        std::fprintf(stderr,
                     "lat_ex_market: [scenarios.lat_ex_market] not found in %s\n",
                     conf_path);
        return 1;
    }
    const bench::Scenario& scenario = *sc;

    bench::pin_client_from_cfg(bench_cfg, "lat_ex_market");

    auto port_r = scenario.get<uint16_t>("port");
    if (!port_r) {
        std::fprintf(stderr, "lat_ex_market: %s\n",
                     bench::format_error(port_r.error()).c_str());
        return 1;
    }
    const uint16_t port = *port_r;

    const std::string ws_path =
        scenario.get_or<std::string>("ws_path", "/ws/bookticker");

    const uint64_t duration_s =
        scenario.get_or<uint32_t>("duration_seconds", 30);

    // push_rate_hz is informational at the client — only the mock consults
    // it to pace its sender loop. We print it in the banner so bench logs
    // are self-describing.
    const uint32_t push_rate_hz =
        scenario.get_or<uint32_t>("push_rate_hz", 100000);

    const uint64_t warmup_samples = bench_cfg.measurement.warmup_samples;
    const bool     use_tls = scenario.get_or<bool>("use_tls", false);

    // Phase 4: resolve the endpoint. `mock` (default) keeps the legacy
    // server_ip:port/ws_path wiring; `wss://host[:port]/path` switches to
    // real-server mode, which (on the kernel backend) instantiates the
    // TLS-enabled stream typedef below.
    auto endpoint_r = bench::resolve_endpoint(bench_cfg, scenario);
    if (!endpoint_r) {
        std::fprintf(stderr, "lat_ex_market: %s\n", endpoint_r.error().c_str());
        return 1;
    }
    const auto endpoint = std::move(*endpoint_r);

#if defined(EPH_USE_DPDK)
    if (endpoint.is_real_server) {
        const std::string ep = scenario.get_or<std::string>("endpoint", "mock");
        std::fprintf(stderr,
            "lat_ex_market: real-server endpoint ('%s') is not supported on "
            "the DPDK build — the PMD path has no outbound WAN routing.\n"
            "Run the kernel build (lat_ex_market) against the real server "
            "and keep the DPDK build on the mock.\n",
            ep.c_str());
        return 1;
    }
#endif

    // Resolve the target host. For the mock path it's always a plain IPv4
    // literal from bench.conf. For real-server mode we may have a DNS
    // name (e.g. `stream.binance.com`) — resolve it via getaddrinfo here.
    // eph-net does not ship a resolver (DNS is outside its "zero-syscall
    // on the hot path" scope), and this is a one-shot startup call.
    const std::string mock_ip_str = endpoint.host;
    auto ip_r = en::Ipv4Addr::parse(mock_ip_str);
    std::string resolved_ip = mock_ip_str;
    if (!ip_r) {
        if (!endpoint.is_real_server) {
            std::fprintf(stderr, "lat_ex_market: invalid mock_ip '%s': %s\n",
                         mock_ip_str.c_str(), ip_r.error().detail);
            return 1;
        }
        // Real-server mode with a hostname — resolve via getaddrinfo.
        struct addrinfo hints{};
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* ai = nullptr;
        const int gai = ::getaddrinfo(mock_ip_str.c_str(), nullptr, &hints, &ai);
        if (gai != 0 || ai == nullptr) {
            std::fprintf(stderr,
                "lat_ex_market: getaddrinfo('%s') failed: %s\n",
                mock_ip_str.c_str(), ::gai_strerror(gai));
            if (ai) ::freeaddrinfo(ai);
            return 1;
        }
        char buf[INET_ADDRSTRLEN];
        const auto* sin = reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
        ::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
        resolved_ip = buf;
        ::freeaddrinfo(ai);
        ip_r = en::Ipv4Addr::parse(resolved_ip);
        if (!ip_r) {
            std::fprintf(stderr,
                "lat_ex_market: failed to re-parse resolved IP '%s'\n",
                resolved_ip.c_str());
            return 1;
        }
        std::printf("lat_ex_market: DNS %s → %s\n",
                    mock_ip_str.c_str(), resolved_ip.c_str());
    }
    const en::SocketAddr remote{ip_r.value(), endpoint.port};

    const std::string effective_ws_path =
        endpoint.is_real_server ? endpoint.ws_path : ws_path;

    std::printf("=== lat_ex_market ===\n");
    std::printf("config: target=%s:%u ws_path=%s push_rate_hz=%u "
                "duration=%llus warmup_samples=%llu mode=%s\n",
                endpoint.host.c_str(),
                static_cast<unsigned>(endpoint.port),
                effective_ws_path.c_str(),
                static_cast<unsigned>(push_rate_hz),
                static_cast<unsigned long long>(duration_s),
                static_cast<unsigned long long>(warmup_samples),
                endpoint.is_real_server ? "real-server+tls" : "mock");
    std::fflush(stdout);
    (void)port;  // superseded by endpoint.port

    bench::install_signal_handler();

    // Construct the Recorder early so its TSC::init() calibration spin
    // (~1 second on a freshly-started process) happens BEFORE we open
    // the WS connection to the mock. Otherwise the mock starts pushing
    // frames while the client is still calibrating, the kernel recv
    // buffer fills, and once polling begins we see a burst of
    // artificially-huge "latencies" (recv_time − server_time) because
    // the frames actually sat in kernel buffers for up to a second.
    //
    // Recorder named `lat_ex_market_<backend>_oneway` so
    // Recorder::export_json produces a uniquely-prefixed JSON file
    // alongside the leg files from the other 5 RTT scenarios.
    const char* backend =
#if defined(EPH_USE_DPDK)
        use_tls ? "dpdk_tls" : "dpdk";
#else
        (endpoint.is_real_server || use_tls) ? "kernel_tls" : "kernel";
#endif
    eu::Recorder rec{std::string{"lat_ex_market_"} + backend + "_oneway"};

#if defined(EPH_USE_DPDK)
    auto env_r = bench::load_dpdk_env(bench_cfg, /*port_id=*/0);
    if (!env_r) {
        std::fprintf(stderr, "lat_ex_market: %s\n", env_r.error().c_str());
        return 1;
    }
    auto env = std::move(*env_r);
    bench::print_dpdk_config_echo(env);

    ed::PollerConfig poller_cfg{};
    poller_cfg.port_id     = env.port_id;
    poller_cfg.rx_queue_id = 0;
    auto poller_r = Poller::create(poller_cfg);
#else
    auto poller_r = ek::KernelPoller::create({});
#endif
    if (!poller_r) {
        std::fprintf(stderr, "lat_ex_market: Poller::create failed: %s\n",
                     poller_r.error().detail);
        return 2;
    }
    auto poller = std::move(poller_r.value());

#if defined(EPH_USE_DPDK)
    ed::StreamConfig cfg{};
    cfg.dpdk.tcp_low_level          = env.make_tcp_config(bench::random_src_port(),
                                              endpoint.port);
    cfg.dpdk.pool            = env.pool;
    cfg.connect_timeout = std::chrono::milliseconds{3000};
    cfg.ws.path         = effective_ws_path;
    cfg.ws.timeout      = std::chrono::seconds{10};

    if (auto rr = env.platform.register_poller(0, poller.get()); !rr) {
        std::fprintf(stderr, "lat_ex_market: register_poller failed: %s\n",
                     rr.error().detail);
        return 3;
    }
    int rc;
    if (use_tls) {
        // Mock+TLS via DpdkTcpStream<WsCodec, EnableTls=true>. Self-signed
        // mock cert → verify_peer=false. SNI takes the mock IP string.
        cfg.tls.hostname    = endpoint.host;
        cfg.tls.verify_peer = false;
        auto stream_r = StreamTls::create_and_attach(std::move(cfg), env.platform);
        if (!stream_r) {
            std::fprintf(stderr, "lat_ex_market: StreamTls::create_and_attach failed: %s\n",
                         stream_r.error().detail);
            return 3;
        }
        rc = bench::scenarios::run_lat_ex_market_measurement(
            std::move(stream_r.value()), *poller,
            rec, warmup_samples, duration_s, backend);
    } else {
        auto stream_r = Stream::create_and_attach(std::move(cfg), env.platform);
        if (!stream_r) {
            std::fprintf(stderr, "lat_ex_market: Stream::create_and_attach failed: %s\n",
                         stream_r.error().detail);
            return 3;
        }
        rc = bench::scenarios::run_lat_ex_market_measurement(
            std::move(stream_r.value()), *poller,
            rec, warmup_samples, duration_s, backend);
    }
#else
    // Kernel backend: pick the TLS-enabled stream typedef when the
    // endpoint is a real exchange (wss://...), plain otherwise. The
    // post-connect loop is templated so both branches share it.
    // 256 KiB reasm buffer: ~200 B bookTicker JSON × ~1 k in-flight
    // frames before the epoll event → comfortable headroom.
    ek::StreamConfig cfg{};
    cfg.reasm_capacity  = 256 * 1024;
    cfg.connect_timeout = std::chrono::milliseconds{3000};
    cfg.ws.path         = effective_ws_path;
    cfg.ws.timeout      = std::chrono::seconds{10};

    int rc = 0;
    // Common kernel-side helper: explicit poller->add (DPDK pre-attaches
    // via create_and_attach above; this branch covers TLS + plain).
    auto attach_and_run = [&](auto stream_uptr) -> int {
        if (auto r = poller->add(stream_uptr.get()); !r) {
            std::fprintf(stderr, "lat_ex_market: poller->add failed: %s\n",
                         r.error().detail);
            return 3;
        }
        return bench::scenarios::run_lat_ex_market_measurement(
            std::move(stream_uptr), *poller,
            rec, warmup_samples, duration_s, backend);
    };

    if (endpoint.is_real_server) {
        // TLS path to a real exchange. SNI + Host header both take the
        // user-facing hostname (not the resolved IP) so the server routes
        // to the right vhost and the certificate matches. verify_peer
        // stays at its default (true) — real CA cert chain expected.
        cfg.remote          = remote;
        cfg.tls.hostname    = endpoint.host;
        cfg.ws.host         = endpoint.host;
        auto stream_r = StreamTls::create(cfg);
        if (!stream_r) {
            std::fprintf(stderr,
                "lat_ex_market: StreamTls::create('%s:%u%s') failed: %s\n",
                endpoint.host.c_str(), endpoint.port,
                effective_ws_path.c_str(),
                stream_r.error().detail);
            return 3;
        }
        rc = attach_and_run(std::move(stream_r.value()));
    } else if (use_tls) {
        // Mock+TLS: same KernelTcpStream<WsCodec,true> as the real-server
        // path, but verify_peer=false because the mock ships a self-signed
        // cert (benchmarks/mockex/fixtures/tls/server.crt).
        cfg.remote          = remote;
        cfg.tls.hostname    = endpoint.host;
        cfg.tls.verify_peer = false;
        auto stream_r = StreamTls::create(cfg);
        if (!stream_r) {
            std::fprintf(stderr, "lat_ex_market: mock+TLS StreamTls::create failed: %s\n",
                         stream_r.error().detail);
            return 3;
        }
        rc = attach_and_run(std::move(stream_r.value()));
    } else {
        cfg.remote = remote;
        auto stream_r = Stream::create(cfg);
        if (!stream_r) {
            std::fprintf(stderr, "lat_ex_market: Stream::create failed: %s\n",
                         stream_r.error().detail);
            return 3;
        }
        rc = attach_and_run(std::move(stream_r.value()));
    }
#endif

    poller.reset();
    return rc;
}
