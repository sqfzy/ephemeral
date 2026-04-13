/// @file lat_ex_order.cpp
/// Latency benchmark: strict one-at-a-time WebSocket order RTT against the
/// Python echo mock `benchmarks/latency/mocks/ex_order_echo.py`.
///
/// Semantics:
///
///   * Strict request→response: send one order, wait for its echo,
///     record legs, send next. No pipelining, no slot table.
///   * Each order gets a unique strictly-incrementing `id` and carries
///     its `t_client` CLOCK_MONOTONIC_RAW timestamp as a JSON field.
///   * The mock inserts `t_mock_recv` / `t_mock_send` around its own
///     `json.loads` / `json.dumps`, so the client recovers all four
///     timestamps and computes RTT / TX / RX cleanly per order.
///   * Why strict serial: TX/RX leg decomposition assumes each order is
///     a self-contained measurement; pipelined sends introduce queue
///     correlation between TX and RX percentiles that breaks the
///     `p50(TX) + p50(RX) ≤ p50(RTT)` sanity invariant even though
///     per-sample `TX_k + SRV_k + RX_k = RTT_k` always holds.
///   * Loop terminates on whichever happens first: duration deadline,
///     `order_count + warmup_samples` orders completed, send error, or
///     SIGINT.
///
/// Measurement clock: `bench::monotonic_raw_ns()`.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

// eph-* headers.
#include "eph/codec/ws_codec.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/utils/recorder.hpp"

#if defined(EPH_USE_DPDK)
// DpdkTcpStream<WsCodec> serial request/response order RTT.
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
#include "core/timestamp_proto.hpp"
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

    // ── Load config: globals + [lat_ex_order] section ───────────────────
    auto globals_r = bench::ScenarioConfig::load_globals(conf_path);
    if (!globals_r) {
        std::fprintf(stderr, "lat_ex_order: %s\n", globals_r.error().c_str());
        return 1;
    }
    auto scenario_r = bench::ScenarioConfig::load(conf_path, "lat_ex_order");
    if (!scenario_r) {
        std::fprintf(stderr, "lat_ex_order: %s\n", scenario_r.error().c_str());
        return 1;
    }
    const auto& globals  = globals_r.value();
    const auto& scenario = scenario_r.value();

    auto port_r = scenario.get_u32("port");
    if (!port_r) {
        std::fprintf(stderr, "lat_ex_order: %s\n", port_r.error().c_str());
        return 1;
    }
    const uint16_t port = static_cast<uint16_t>(port_r.value());

    const std::string ws_path = scenario.get_string("ws_path", "/ws/order");

    auto order_count_r = scenario.get_u64("order_count", 10000);
    if (!order_count_r) {
        std::fprintf(stderr, "lat_ex_order: %s\n", order_count_r.error().c_str());
        return 1;
    }
    const uint64_t order_count = order_count_r.value();

    auto duration_r = scenario.get_u32("duration_seconds", 30);
    if (!duration_r) {
        std::fprintf(stderr, "lat_ex_order: %s\n", duration_r.error().c_str());
        return 1;
    }
    const uint64_t duration_s = duration_r.value();

    auto warmup_r = globals.get_u64("warmup_samples", 1000);
    if (!warmup_r) {
        std::fprintf(stderr, "lat_ex_order: %s\n", warmup_r.error().c_str());
        return 1;
    }
    const uint64_t warmup_samples = warmup_r.value();

    const std::string mock_ip_str = globals.get_string("mock_ip", "127.0.0.1");
    auto ip_r = en::Ipv4Addr::parse(mock_ip_str);
    if (!ip_r) {
        std::fprintf(stderr, "lat_ex_order: invalid mock_ip '%s': %s\n",
                     mock_ip_str.c_str(), ip_r.error().detail);
        return 1;
    }
    const en::SocketAddr remote{ip_r.value(), port};

    std::printf("=== lat_ex_order ===\n");
    std::printf("config: mock=%s port=%u ws_path=%s order_count=%llu "
                "duration=%llus warmup_samples=%llu\n",
                mock_ip_str.c_str(),
                static_cast<unsigned>(port),
                ws_path.c_str(),
                static_cast<unsigned long long>(order_count),
                static_cast<unsigned long long>(duration_s),
                static_cast<unsigned long long>(warmup_samples));
    std::fflush(stdout);

    bench::install_signal_handler();

    // Construct the Recorders BEFORE Stream::create so that TSC::init's
    // ~1 s calibration spin runs at startup, not while the WS handshake
    // is completing. Same pattern as lat_ex_market.
    //
    // Three Recorders (RTT / TX / RX). ex_order is the only scenario
    // that carries timestamps as JSON fields
    // (`t_client` / `t_mock_recv` / `t_mock_send`) rather than a
    // binary 24 B prefix — WS text-ish JSON wouldn't survive a binary
    // header, so we extract via the existing scan_json_uint_field.
    const char* backend =
#if defined(EPH_USE_DPDK)
        "dpdk";
#else
        "kernel";
#endif
    eu::Recorder rec_rtt{std::string{"lat_ex_order_"} + backend + "_rtt"};
    eu::Recorder rec_tx {std::string{"lat_ex_order_"} + backend + "_tx" };
    eu::Recorder rec_rx {std::string{"lat_ex_order_"} + backend + "_rx" };

#if defined(EPH_USE_DPDK)
    auto env_r = bench::load_dpdk_env(globals, /*port_id=*/0);
    if (!env_r) {
        std::fprintf(stderr, "lat_ex_order: %s\n", env_r.error().c_str());
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
        std::fprintf(stderr, "lat_ex_order: Poller::create failed: %s\n",
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
    ek::StreamConfig cfg{};
    cfg.remote          = remote;
    // Order payloads are tiny (~30 B); 64 KiB reassembly is ample.
    cfg.reasm_capacity  = 64 * 1024;
    cfg.connect_timeout = std::chrono::milliseconds{3000};
    cfg.ws_path         = ws_path;
    cfg.ws_timeout      = std::chrono::seconds{10};
#endif

    auto stream_r = Stream::create(cfg);
    if (!stream_r) {
        std::fprintf(stderr, "lat_ex_order: Stream::create failed: %s\n",
                     stream_r.error().detail);
        return 3;
    }
    auto stream = std::move(stream_r.value());

    // ── Strict-serial state (captured by reference in on_message) ───────
    //
    // Only one order is ever in flight. `current_id` is the id of the
    // outstanding order (0 = pipeline idle). `response_seen` flips to
    // true when the mock's echo arrives and passes validation; the main
    // loop polls it to advance to the next send.
    uint64_t current_id      = 0;     // 0 = no order in flight
    bool     response_seen   = false; // mock echoed current_id
    uint64_t sample_idx      = 0;     // completed orders, incl. warmup
    uint64_t recorded_count  = 0;     // post-warmup recorded samples
    uint64_t stale_count     = 0;     // id mismatch / unexpected response
    uint64_t malformed_count = 0;     // missing "id" or timestamp fields
    uint64_t t_measure_start = 0;     // first post-warmup completion time

    stream->on_message = [&](const uint8_t* d, uint16_t n) {
        const uint64_t t_recv = bench::monotonic_raw_ns();
        const std::size_t nsz = static_cast<std::size_t>(n);

        auto id_opt = bench::scan_json_uint_field(d, nsz, "id");
        if (!id_opt) {
            ++malformed_count;
            return;
        }
        if (current_id == 0 || *id_opt != current_id) {
            // Response with no matching outstanding order — duplicate
            // echo or out-of-order leftover. Skip without polluting the
            // histogram.
            ++stale_count;
            return;
        }

        // Pull the three timestamps from the echoed JSON. The mock
        // (`ex_order_echo.py`) inserts `t_mock_recv` and `t_mock_send`
        // around its `json.loads`/`json.dumps`; `t_client` comes back
        // unchanged. Any missing field → drop the sample and move on.
        auto tc_opt = bench::scan_json_uint_field(d, nsz, "t_client");
        auto tr_opt = bench::scan_json_uint_field(d, nsz, "t_mock_recv");
        auto ts_opt = bench::scan_json_uint_field(d, nsz, "t_mock_send");

        if (sample_idx == warmup_samples) {
            t_measure_start = t_recv;
        }
        if (sample_idx >= warmup_samples) {
            if (tc_opt && tr_opt && ts_opt &&
                t_recv >= *tc_opt && *tr_opt >= *tc_opt && t_recv >= *ts_opt) {
                const bench::TimestampBlock tsb{
                    .client_ns    = *tc_opt,
                    .mock_recv_ns = *tr_opt,
                    .mock_send_ns = *ts_opt,
                };
                const auto lgs = bench::compute_legs(tsb, t_recv);
                rec_rtt.record_ns(lgs.rtt_ns);
                rec_tx .record_ns(lgs.tx_ns);
                rec_rx .record_ns(lgs.rx_ns);
                ++recorded_count;
            } else {
                ++malformed_count;
                SPDLOG_DEBUG("lat_ex_order: dropping sample id={} — "
                             "missing/inconsistent t_client/t_mock_*",
                             *id_opt);
            }
        }
        ++sample_idx;
        response_seen = true;
    };

    if (auto r = poller->add(stream.get()); !r) {
        std::fprintf(stderr, "lat_ex_order: poller->add failed: %s\n",
                     r.error().detail);
        return 3;
    }

    // ── Encode buffer for outbound WS-binary frames ─────────────────────
    //
    // Each sample encodes
    //   `{"e":"NewOrder","id":NNN,"t_client":NNNNNNNNNNNNNNNNNNNN}`
    // Worst case: 20-digit id + 20-digit t_client ≈ 80 bytes. A 128-byte
    // JSON buffer plus WsCodec::max_overhead covers every case.
    constexpr std::size_t kJsonBufSize = 128;
    char      json_buf[kJsonBufSize];
    std::vector<uint8_t> frame(ec::WsCodec::max_overhead + kJsonBufSize);
    ec::WsCodec encoder{};

    // ── Measurement loop (strict request/response) ──────────────────────
    //
    // For each order: send → poll until response_seen → record in
    // on_message → next. Total orders issued = order_count + warmup.
    //
    // Exit conditions:
    //   1. duration_seconds deadline elapses
    //   2. all planned orders completed
    //   3. SIGINT/SIGTERM sets the shutdown flag
    //   4. send() error (mock died)
    //   5. per-sample recv timeout (5s — mock stuck)
    const uint64_t t_start = bench::monotonic_raw_ns();
    const uint64_t t_deadline =
        t_start + (duration_s + 2) * 1'000'000'000ull;
    const uint64_t plan_total = order_count + warmup_samples;
    constexpr uint64_t kPerSampleTimeoutNs = 5ull * 1'000'000'000ull;

    bool send_failed = false;
    bool timed_out   = false;
    uint64_t next_id = 1;
    while (next_id <= plan_total && !bench::shutdown_requested()) {
        if (bench::monotonic_raw_ns() >= t_deadline) break;

        const uint64_t id = next_id;
        const uint64_t t_client = bench::monotonic_raw_ns();
        const int jn = std::snprintf(
            json_buf, sizeof(json_buf),
            "{\"e\":\"NewOrder\",\"id\":%llu,\"t_client\":%llu}",
            static_cast<unsigned long long>(id),
            static_cast<unsigned long long>(t_client));
        if (jn <= 0 || static_cast<std::size_t>(jn) >= sizeof(json_buf)) {
            std::fprintf(stderr,
                         "lat_ex_order: json snprintf overflow at id=%llu\n",
                         static_cast<unsigned long long>(id));
            send_failed = true;
            break;
        }

        auto enc_r = encoder.encode(
            frame.data(), frame.size(),
            std::span<const uint8_t>{
                reinterpret_cast<const uint8_t*>(json_buf),
                static_cast<std::size_t>(jn)});
        if (!enc_r) {
            std::fprintf(stderr, "lat_ex_order: WsCodec::encode failed: %s\n",
                         enc_r.error().detail);
            send_failed = true;
            break;
        }

        current_id    = id;
        response_seen = false;

        auto send_r = stream->send(
            std::span<const uint8_t>{frame.data(), *enc_r});
        if (!send_r) {
            std::fprintf(stderr,
                         "lat_ex_order: send failed at id=%llu: %s\n",
                         static_cast<unsigned long long>(id),
                         send_r.error().detail);
            send_failed = true;
            break;
        }

        const uint64_t t0 = t_client;
        while (!response_seen && !bench::shutdown_requested()) {
            poller->poll();
            if (bench::monotonic_raw_ns() - t0 > kPerSampleTimeoutNs) {
                std::fprintf(stderr,
                             "lat_ex_order: response timeout at id=%llu\n",
                             static_cast<unsigned long long>(id));
                timed_out = true;
                break;
            }
        }
        if (timed_out || bench::shutdown_requested()) break;

        current_id = 0;
        ++next_id;
    }

    if (malformed_count > 0 || stale_count > 0) {
        std::fprintf(stderr,
                     "lat_ex_order: skipped %llu malformed + %llu stale responses\n",
                     static_cast<unsigned long long>(malformed_count),
                     static_cast<unsigned long long>(stale_count));
    }
    std::fprintf(stderr,
                 "lat_ex_order: completed %llu orders (recorded %llu post-warmup)\n",
                 static_cast<unsigned long long>(sample_idx),
                 static_cast<unsigned long long>(recorded_count));

    const uint64_t wall_time_ns =
        (t_measure_start != 0)
            ? (bench::monotonic_raw_ns() - t_measure_start)
            : 0;
    bench::print_leg_report("lat_ex_order", backend, rec_rtt, rec_tx, rec_rx,
                            warmup_samples, wall_time_ns);
    (void)bench::export_legs(rec_rtt, rec_tx, rec_rx);

    (void)stream->close_gracefully();
#if !defined(EPH_USE_DPDK)
    poller->poll(std::chrono::milliseconds{50});
#endif
    (void)poller->remove(stream.get());
    stream.reset();
    poller.reset();

    if (send_failed) return 4;
    if (timed_out)   return 5;
    return 0;
}
