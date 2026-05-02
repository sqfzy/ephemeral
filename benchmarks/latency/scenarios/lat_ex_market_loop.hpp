/// @file scenarios/lat_ex_market_loop.hpp
/// Reusable inner loop for lat_ex_market (one-way bookTicker push).
///
/// Two entry points:
///   - `run_lat_ex_market_measurement<StreamT, PollerT>`: the original
///     templated measurement body (poll-only loop with on_message
///     extracting `T":<ns>` from each WS frame). Used by lat_ex_market.cpp
///     main()'s real-server kernel path AND by run_lat_ex_market_loop.
///   - `run_lat_ex_market_loop<EnableTls>(BenchCtx&)`: the
///     parallel-bench v2 entry. Constructs Stream (DPDK+mock or
///     kernel+mock) and delegates to run_lat_ex_market_measurement.

#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include "eph/codec/ws_codec.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/utils/recorder.hpp"

#if defined(EPH_USE_DPDK)
#  include "eph/net/dpdk/poller.hpp"
#  include "eph/net/dpdk/tcp_stream.hpp"
#else
#  include "eph/net/kernel/config.hpp"
#  include "eph/net/kernel/poller.hpp"
#  include "eph/net/kernel/tcp_stream.hpp"
#endif

#include "core/bench_conf.hpp"
#include "core/bench_ctx.hpp"
#include "core/json_scan.hpp"
#include "core/measurement.hpp"
#if defined(EPH_USE_DPDK)
#  include "core/dpdk_env.hpp"
#endif

namespace bench::scenarios {

/// Templated measurement body — shared between plain-TCP (mock) and
/// TLS (real-server) stream flavours, kernel and DPDK builds. Identical
/// logic to the original run_measurement that lived inside lat_ex_market.cpp,
/// only the poller arg changed from `unique_ptr<PollerT>&` to `PollerT&`
/// so callers can pass either an owning unique_ptr or a non-owning ref.
template <class StreamT, class PollerT>
int run_lat_ex_market_measurement(std::unique_ptr<StreamT> stream,
                                  PollerT& poller,
                                  ::eph::utils::Recorder& rec,
                                  uint64_t warmup_samples,
                                  uint64_t duration_s,
                                  const char* backend) noexcept {
    uint64_t sample_idx      = 0;
    uint64_t malformed       = 0;
    uint64_t clock_skew      = 0;
    uint64_t t_measure_start = 0;

    stream->on_message = [&](std::span<const uint8_t> app_frame) {
        const uint64_t t_recv = ::bench::monotonic_raw_ns();
        auto t_server_opt = ::bench::scan_json_uint_field(
            app_frame.data(), app_frame.size(), "T");
        if (!t_server_opt) { ++malformed; return; }
        const uint64_t t_server = *t_server_opt;
        if (t_recv <= t_server) { ++clock_skew; return; }
        if (sample_idx == warmup_samples) t_measure_start = t_recv;
        if (sample_idx >= warmup_samples) rec.record_ns(t_recv - t_server);
        ++sample_idx;
    };

    const uint64_t t_start = ::bench::monotonic_raw_ns();
    const uint64_t t_deadline = t_start + (duration_s + 2) * 1'000'000'000ull;

    while (::bench::monotonic_raw_ns() < t_deadline &&
           !::bench::shutdown_requested()) {
        poller.poll();
    }

    if (malformed > 0 || clock_skew > 0) {
        std::fprintf(stderr,
                     "lat_ex_market: skipped %llu malformed + %llu clock-skew samples\n",
                     static_cast<unsigned long long>(malformed),
                     static_cast<unsigned long long>(clock_skew));
    }

    const uint64_t wall_time_ns =
        (t_measure_start != 0)
            ? (::bench::monotonic_raw_ns() - t_measure_start)
            : 0;
    ::bench::print_report("lat_ex_market", backend, rec,
                          warmup_samples, wall_time_ns);
    if (!rec.export_json("benchmarks/latency/outputs")) {
        std::fprintf(stderr,
                     "[WARN] lat_ex_market: export_json failed\n");
    }

    (void)stream->close_gracefully();
#if !defined(EPH_USE_DPDK)
    poller.poll(std::chrono::milliseconds{50});
#endif
    (void)poller.remove(stream.get());
    return 0;
}

/// BenchCtx-driven entry: DPDK+mock or kernel+mock paths only
/// (real-server resolution stays in lat_ex_market.cpp main).
template <bool EnableTls>
inline int run_lat_ex_market_loop(::bench::BenchCtx& ctx) noexcept {
    namespace ec = ::eph::codec;
    namespace eu = ::eph::utils;
    namespace en = ::eph::net;

    const auto& bench_cfg = *ctx.bench_cfg;
    const auto& scenario  = *ctx.scenario_cfg;

    const uint16_t port = scenario.get_or<uint16_t>("port", 0);
    if (port == 0) {
        std::fprintf(stderr, "run_lat_ex_market_loop: scenarios.<...>.port required\n");
        return 1;
    }
    const std::string ws_path =
        scenario.get_or<std::string>("ws_path", "/ws/bookticker");
    const uint64_t duration_s =
        scenario.get_or<uint32_t>("duration_seconds", 30);
    const uint64_t warmup_samples = bench_cfg.measurement.warmup_samples;

    const std::string mock_ip_str = bench_cfg.networking.server_ip.empty()
                                        ? std::string{"127.0.0.1"}
                                        : bench_cfg.networking.server_ip;

    constexpr const char* backend =
#if defined(EPH_USE_DPDK)
        EnableTls ? "dpdk_tls" : "dpdk";
#else
        EnableTls ? "kernel_tls" : "kernel";
#endif
    const std::string suffix = ::bench::mp_output_suffix();
    eu::Recorder rec{std::string{"lat_ex_market_"} + backend + "_oneway" + suffix};

#if defined(EPH_USE_DPDK)
    namespace ed = ::eph::net::dpdk;
    using Stream = ed::DpdkTcpStream<ec::WsCodec, EnableTls>;
    using Poller = ed::DpdkPoller<>;

    if (ctx.view == nullptr) {
        std::fprintf(stderr, "run_lat_ex_market_loop: ctx.view is null\n");
        return 3;
    }
    auto& view = *ctx.view;

    ed::StreamConfig cfg{};
    cfg.dpdk.wire = view.make_tcp_config(::bench::random_src_port(), port);
    cfg.dpdk.pool          = view.pool;
    cfg.connect_timeout    = std::chrono::milliseconds{3000};
    cfg.ws.path            = ws_path;
    cfg.ws.timeout         = std::chrono::seconds{10};
    cfg.dpdk.pin_to_queue = ctx.queue_id;
    if constexpr (EnableTls) {
        cfg.tls.hostname    = mock_ip_str;
        cfg.tls.verify_peer = false;
    }
    auto stream_r = Stream::create_and_attach(std::move(cfg), view.platform);
#else
    namespace ek = ::eph::net::kernel;
    using Stream = ek::KernelTcpStream<ec::WsCodec, EnableTls>;
    using Poller = ek::KernelPoller;

    auto ip_r = en::Ipv4Addr::parse(mock_ip_str);
    if (!ip_r) {
        std::fprintf(stderr, "run_lat_ex_market_loop: invalid mock_ip '%s': %s\n",
                     mock_ip_str.c_str(), ip_r.error().detail);
        return 1;
    }
    const en::SocketAddr remote{ip_r.value(), port};

    ek::StreamConfig cfg{};
    cfg.remote          = remote;
    cfg.reasm_capacity  = 256 * 1024;
    cfg.connect_timeout = std::chrono::milliseconds{3000};
    cfg.ws.path         = ws_path;
    cfg.ws.timeout      = std::chrono::seconds{10};
    if constexpr (EnableTls) {
        cfg.tls.hostname    = mock_ip_str;
        cfg.tls.verify_peer = false;
    }
    auto stream_r = Stream::create(cfg);
#endif
    if (!stream_r) {
        std::fprintf(stderr, "run_lat_ex_market_loop: Stream::create failed: %s\n",
                     stream_r.error().detail);
        return 3;
    }

    Poller& poller = *static_cast<Poller*>(ctx.poller);

#if !defined(EPH_USE_DPDK)
    if (auto r = poller.add(stream_r.value().get()); !r) {
        std::fprintf(stderr, "run_lat_ex_market_loop: poller->add failed: %s\n",
                     r.error().detail);
        return 3;
    }
#endif

    return run_lat_ex_market_measurement<Stream, Poller>(
        std::move(stream_r.value()), poller, rec,
        warmup_samples, duration_s, backend);
}

} // namespace bench::scenarios
