/// @file lat_ex_market.cpp
/// Phase 10 latency benchmark: exchange bookTicker push (WebSocket one-way)
/// against the Python mock `benchmarks/latency/mocks/ex_market_push.py`.
///
/// Sub-phase 10.4 rewrite. Per
/// `.artifacts/plan-phase-10-latency-bench-20260411-040540.md` §Sub-phase
/// 10.4 and §Interface design → "Client API pattern" (one-way variant).
///
/// Structure:
///
///   * Single-file scenario binary that reads `[lat_ex_market]` from
///     bench.conf (port / ws_path / push_rate_hz / duration_seconds)
///     plus the lowercase global `mock_ip`, `warmup_samples`.
///   * Uses `KernelTcpStream<WsCodec, false>` with
///     `StreamConfig.ws_path` set — the Phase 9.5 transparent WS upgrade
///     runs inside `create()`. Once connected we run the measurement
///     loop by polling the stream; the client sends NOTHING after the
///     handshake because the mock pushes frames unilaterally at
///     `push_rate_hz` for `duration_seconds`.
///   * One-way latency is measured entirely inside `on_message`:
///     the client stamps `t_recv = monotonic_raw_ns()` on arrival and
///     extracts the server-stamped `T":<ns>` field from the JSON
///     payload via `bench::scan_json_uint_field` (plan D-5 — we do NOT
///     pull eph-json into the bench path). Sample is `t_recv - t_server`.
///   * Measurement clock is `bench::monotonic_raw_ns()`
///     (CLOCK_MONOTONIC_RAW via vDSO, per plan D-6). The mock stamps
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
#include <string>
#include <utility>


#include <spdlog/spdlog.h>

// eph-* headers (plan D-4: v3.3 API only).
#include "eph/codec/ws_codec.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/utils/recorder.hpp"

#if defined(EPH_USE_DPDK)
// Phase 11.0: DpdkTcpStream<WsCodec> real one-way bookTicker measurement.
// Same on_message + scan_json_uint_field("T") logic as the kernel branch.
#  include "eph/net/dpdk/poller.hpp"
#  include "eph/net/dpdk/tcp_stream.hpp"
#else
#  include "eph/net/kernel/config.hpp"
#  include "eph/net/kernel/poller.hpp"
#  include "eph/net/kernel/tcp_stream.hpp"
#endif

#include "core/config.hpp"
#include "core/json_scan.hpp"
#include "core/measurement.hpp"
#if defined(EPH_USE_DPDK)
#  include "core/dpdk_env.hpp"
#endif

namespace {

namespace ec = eph::codec;
namespace eu = eph::utils;
namespace en = eph::net;

#if defined(EPH_USE_DPDK)
namespace ed = eph::net::dpdk;
using Stream = ed::DpdkTcpStream<ec::WsCodec, /*EnableTls=*/false>;
using Poller = ed::DpdkPoller<>;
#else
namespace ek = eph::net::kernel;
using Stream = ek::KernelTcpStream<ec::WsCodec, /*EnableTls=*/false>;
using Poller = ek::KernelPoller;
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

    // ── Load config: globals + [lat_ex_market] section.
    auto globals_r = bench::ScenarioConfig::load_globals(conf_path);
    if (!globals_r) {
        std::fprintf(stderr, "lat_ex_market: %s\n", globals_r.error().c_str());
        return 1;
    }
    auto scenario_r = bench::ScenarioConfig::load(conf_path, "lat_ex_market");
    if (!scenario_r) {
        std::fprintf(stderr, "lat_ex_market: %s\n", scenario_r.error().c_str());
        return 1;
    }
    const auto& globals  = globals_r.value();
    const auto& scenario = scenario_r.value();

    auto port_r = scenario.get_u32("port");
    if (!port_r) {
        std::fprintf(stderr, "lat_ex_market: %s\n", port_r.error().c_str());
        return 1;
    }
    const uint16_t port = static_cast<uint16_t>(port_r.value());

    const std::string ws_path = scenario.get_string("ws_path", "/ws/bookticker");

    auto duration_r = scenario.get_u32("duration_seconds", 30);
    if (!duration_r) {
        std::fprintf(stderr, "lat_ex_market: %s\n", duration_r.error().c_str());
        return 1;
    }
    const uint64_t duration_s = duration_r.value();

    // push_rate_hz is informational at the client — only the mock consults
    // it to pace its sender loop. We print it in the banner so bench logs
    // are self-describing.
    const uint32_t push_rate_hz = scenario.get_u32("push_rate_hz", 100000)
                                      .value_or(100000);

    auto warmup_r = globals.get_u64("warmup_samples", 1000);
    if (!warmup_r) {
        std::fprintf(stderr, "lat_ex_market: %s\n", warmup_r.error().c_str());
        return 1;
    }
    const uint64_t warmup_samples = warmup_r.value();

    const std::string mock_ip_str = globals.get_string("mock_ip", "127.0.0.1");
    auto ip_r = en::Ipv4Addr::parse(mock_ip_str);
    if (!ip_r) {
        std::fprintf(stderr, "lat_ex_market: invalid mock_ip '%s': %s\n",
                     mock_ip_str.c_str(), ip_r.error().detail);
        return 1;
    }
    const en::SocketAddr remote{ip_r.value(), port};

    std::printf("=== lat_ex_market ===\n");
    std::printf("config: mock=%s port=%u ws_path=%s push_rate_hz=%u "
                "duration=%llus warmup_samples=%llu\n",
                mock_ip_str.c_str(),
                static_cast<unsigned>(port),
                ws_path.c_str(),
                static_cast<unsigned>(push_rate_hz),
                static_cast<unsigned long long>(duration_s),
                static_cast<unsigned long long>(warmup_samples));
    std::fflush(stdout);

    bench::install_signal_handler();

    // Construct the Recorder early so its TSC::init() calibration spin
    // (~1 second on a freshly-started process) happens BEFORE we open
    // the WS connection to the mock. Otherwise the mock starts pushing
    // frames while the client is still calibrating, the kernel recv
    // buffer fills, and once polling begins we see a burst of
    // artificially-huge "latencies" (recv_time − server_time) because
    // the frames actually sat in kernel buffers for up to a second.
    //
    // Phase 11.1: rename Recorder to `lat_ex_market_<backend>_oneway`
    // so Recorder::export_json produces a uniquely-prefixed JSON file
    // alongside the leg files from the other 5 RTT scenarios.
    const char* backend =
#if defined(EPH_USE_DPDK)
        "dpdk";
#else
        "kernel";
#endif
    eu::Recorder rec{std::string{"lat_ex_market_"} + backend + "_oneway"};

#if defined(EPH_USE_DPDK)
    auto env_r = bench::load_dpdk_env(globals, /*port_id=*/0);
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
    cfg.legacy          = env.make_tcp_config(bench::random_src_port(), port);
    cfg.pool            = env.pool;
    cfg.connect_timeout = std::chrono::milliseconds{3000};
    cfg.ws_path         = ws_path;
    cfg.ws_timeout      = std::chrono::seconds{10};
#else
    // Phase 9.5 transparent WS handshake: setting ws_path makes
    // `create()` drive the HTTP/1.1 Upgrade after the TCP connect.
    // At high push rates (100 kHz × 30 s ≈ 3 M samples) the reassembly
    // buffer needs enough slack to absorb multiple frames per poll —
    // 256 KiB is comfortable headroom for ~200-byte bookTicker JSONs.
    ek::StreamConfig cfg{};
    cfg.remote          = remote;
    cfg.reasm_capacity  = 256 * 1024;
    cfg.connect_timeout = std::chrono::milliseconds{3000};
    cfg.ws_path         = ws_path;
    cfg.ws_timeout      = std::chrono::seconds{10};
#endif

    auto stream_r = Stream::create(cfg);
    if (!stream_r) {
        std::fprintf(stderr, "lat_ex_market: Stream::create failed: %s\n",
                     stream_r.error().detail);
        return 3;
    }
    auto stream = std::move(stream_r.value());

    // ── One-way measurement inside on_message ───────────────────────────
    //
    // Each decoded WS-binary frame carries a JSON blob of the form:
    //   {"e":"bookTicker","s":"BTCUSDT",...,"T":1712345678901234567}
    //
    // `scan_json_uint_field(data, n, "T")` finds the `"T":<digits>`
    // token and parses the unsigned integer. If the field is missing
    // or the payload arrives out of shape (mock warmup noise, partial
    // decode, ...) we skip that sample rather than poison the
    // histogram.
    //
    // Sanity gate: `t_recv > t_server` must hold — otherwise we've hit
    // a clock skew / instrumentation bug and should drop the sample.
    //
    // `rec` is declared above the poller (before Stream::create) so
    // TSC calibration doesn't steal a second of wall time between
    // "mock begins pushing" and "client begins polling".
    uint64_t sample_idx     = 0;
    uint64_t malformed      = 0;
    uint64_t clock_skew     = 0;
    uint64_t t_measure_start = 0;  // stamped at the first post-warmup sample

    stream->on_message = [&](const uint8_t* d, uint16_t n) {
        const uint64_t t_recv = bench::monotonic_raw_ns();

        auto t_server_opt = bench::scan_json_uint_field(d, n, "T");
        if (!t_server_opt) {
            ++malformed;
            return;
        }
        const uint64_t t_server = *t_server_opt;

        if (t_recv <= t_server) {
            ++clock_skew;
            return;
        }

        if (sample_idx == warmup_samples) {
            t_measure_start = t_recv;
        }
        if (sample_idx >= warmup_samples) {
            rec.record_ns(t_recv - t_server);
        }
        ++sample_idx;
    };

    if (auto r = poller->add(stream.get()); !r) {
        std::fprintf(stderr, "lat_ex_market: poller->add failed: %s\n",
                     r.error().detail);
        return 3;
    }

    // ── Poll loop ───────────────────────────────────────────────────────
    //
    // The client is passive: it only polls. The mock drives the rate.
    // We bound the run by `duration_seconds` from bench.conf plus a
    // cooperative shutdown flag so Ctrl-C still prints a report.
    //
    // `duration_s + 2` gives the mock an extra 2 s tail so slow-start
    // jitter doesn't truncate the sample window; the mock exits on
    // its own `duration_seconds` deadline and then `poll()` will stop
    // seeing frames — we fall out naturally on the outer time guard.
    const uint64_t t_start = bench::monotonic_raw_ns();
    const uint64_t t_deadline =
        t_start + (duration_s + 2) * 1'000'000'000ull;

    while (bench::monotonic_raw_ns() < t_deadline && !bench::shutdown_requested()) {
        poller->poll();
    }

    if (malformed > 0 || clock_skew > 0) {
        std::fprintf(stderr,
                     "lat_ex_market: skipped %llu malformed + %llu clock-skew "
                     "samples\n",
                     static_cast<unsigned long long>(malformed),
                     static_cast<unsigned long long>(clock_skew));
    }

    const uint64_t wall_time_ns =
        (t_measure_start != 0)
            ? (bench::monotonic_raw_ns() - t_measure_start)
            : 0;
    bench::print_report("lat_ex_market", backend, rec,
                        warmup_samples, wall_time_ns);
    // Phase 11.1: oneway JSON export (1 file per backend). WARN-only
    // on failure so disk issues don't fail the bench.
    if (!rec.export_json("benchmarks/latency/outputs")) {
        std::fprintf(stderr,
                     "[WARN] lat_ex_market: export_json to "
                     "'benchmarks/latency/outputs' failed\n");
    }

    (void)stream->close_gracefully();
#if !defined(EPH_USE_DPDK)
    poller->poll(std::chrono::milliseconds{50});
#endif
    (void)poller->remove(stream.get());
    stream.reset();
    poller.reset();

    return 0;
}
