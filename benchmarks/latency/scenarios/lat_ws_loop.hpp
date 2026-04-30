/// @file scenarios/lat_ws_loop.hpp
/// Reusable inner-loop function for the `lat_ws` scenario.
/// Templated on EnableTls (mock+TLS path uses `dpdk_tls`/`kernel_tls`
/// backend label). Constructs Stream<EnableTls>, sets WS handshake via
/// cfg.ws.path, drives the WsCodec-encoded RTT measurement loop.

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

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
#include "core/measurement.hpp"
#include "core/timestamp_proto.hpp"
#if defined(EPH_USE_DPDK)
#  include "core/dpdk_env.hpp"
#endif

namespace bench::scenarios {

template <bool EnableTls>
inline int run_lat_ws_loop(::bench::BenchCtx& ctx) noexcept {
    namespace ec = ::eph::codec;
    namespace eu = ::eph::utils;
    namespace en = ::eph::net;

    const auto& bench_cfg = *ctx.bench_cfg;
    const auto& scenario  = *ctx.scenario_cfg;

    const uint16_t port = scenario.get_or<uint16_t>("port", 0);
    if (port == 0) {
        std::fprintf(stderr, "run_lat_ws_loop: scenarios.<...>.port required\n");
        return 1;
    }
    const std::string ws_path =
        scenario.get_or<std::string>("ws_path", "/echo");
    const std::size_t payload_size =
        scenario.get_or<uint32_t>("payload_size", 64);
    if (payload_size < ::bench::kTimestampBlockSize) {
        std::fprintf(stderr, "run_lat_ws_loop: payload_size=%zu < kTimestampBlockSize=%zu\n",
                     payload_size, ::bench::kTimestampBlockSize);
        return 1;
    }
    // Cap payload_size at 16 MiB. The bench frame buffer is allocated as a
    // single std::vector in a noexcept function — a config typo of e.g.
    // 4 GiB would terminate the process via bad_alloc instead of producing
    // a clean error. 16 MiB is well above any realistic exchange payload.
    constexpr std::size_t kMaxPayloadBytes = 16ull * 1024ull * 1024ull;
    if (payload_size > kMaxPayloadBytes) {
        std::fprintf(stderr, "run_lat_ws_loop: payload_size=%zu exceeds max %zu\n",
                     payload_size, kMaxPayloadBytes);
        return 1;
    }
    const uint64_t duration_s =
        scenario.get_or<uint32_t>("duration_seconds", 10);
    const uint64_t warmup_samples = bench_cfg.measurement.warmup_samples;

    const std::string mock_ip_str = bench_cfg.networking.server_ip.empty()
                                        ? std::string{"127.0.0.1"}
                                        : bench_cfg.networking.server_ip;

#if defined(EPH_USE_DPDK)
    namespace ed = ::eph::net::dpdk;
    using Stream = ed::DpdkTcpStream<ec::WsCodec, EnableTls>;
    using Poller = ed::DpdkPoller<>;
    auto* poller = static_cast<Poller*>(ctx.poller);

    if (ctx.view == nullptr) {
        std::fprintf(stderr, "run_lat_ws_loop: ctx.view is null in DPDK build\n");
        return 3;
    }
    auto& view = *ctx.view;

    ed::StreamConfig cfg{};
    cfg.dpdk.tcp_low_level = view.make_tcp_config(::bench::random_src_port(), port);
    cfg.dpdk.pool          = view.pool;
    cfg.connect_timeout    = std::chrono::milliseconds{3000};
    cfg.ws.path            = ws_path;
    cfg.ws.timeout         = std::chrono::seconds{10};
    if (ctx.slot_index >= 0) {
        cfg.dpdk.pin_to_queue = ctx.queue_id;
    }
    if constexpr (EnableTls) {
        cfg.tls.hostname    = mock_ip_str;
        cfg.tls.verify_peer = false;
    }

    auto stream_r = Stream::create_and_attach(std::move(cfg), view.platform);
#else
    namespace ek = ::eph::net::kernel;
    using Stream = ek::KernelTcpStream<ec::WsCodec, EnableTls>;
    using Poller = ek::KernelPoller;
    auto* poller = static_cast<Poller*>(ctx.poller);

    auto ip_r = en::Ipv4Addr::parse(mock_ip_str);
    if (!ip_r) {
        std::fprintf(stderr, "run_lat_ws_loop: invalid mock_ip '%s': %s\n",
                     mock_ip_str.c_str(), ip_r.error().detail);
        return 1;
    }
    const en::SocketAddr remote{ip_r.value(), port};

    ek::StreamConfig cfg{};
    cfg.remote          = remote;
    cfg.reasm_capacity  = std::max<std::size_t>(64 * 1024, payload_size * 4);
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
        std::fprintf(stderr, "run_lat_ws_loop: Stream::create failed: %s\n",
                     stream_r.error().detail);
        return 3;
    }
    auto stream = std::move(stream_r.value());

    bool got_echo = false;
    std::array<uint8_t, ::bench::kTimestampBlockSize> ts_buf{};
    std::size_t ts_filled = 0;
    stream->on_message = [&](std::span<const uint8_t> app_frame) {
        got_echo = true;
        ts_filled = 0;
        if (app_frame.size() >= ::bench::kTimestampBlockSize) {
            std::memcpy(ts_buf.data(), app_frame.data(),
                        ::bench::kTimestampBlockSize);
            ts_filled = ::bench::kTimestampBlockSize;
        }
    };

#if !defined(EPH_USE_DPDK)
    if (auto r = poller->add(stream.get()); !r) {
        std::fprintf(stderr, "run_lat_ws_loop: poller->add failed: %s\n",
                     r.error().detail);
        return 3;
    }
#endif

    constexpr const char* backend =
#if defined(EPH_USE_DPDK)
        EnableTls ? "dpdk_tls" : "dpdk";
#else
        EnableTls ? "kernel_tls" : "kernel";
#endif
    const std::string suffix =
        (ctx.slot_index < 0) ? std::string{}
                              : std::string{"_slot"} + std::to_string(ctx.slot_index);

    eu::Recorder rec_rtt{std::string{"lat_ws_"} + backend + "_rtt" + suffix};
    eu::Recorder rec_tx {std::string{"lat_ws_"} + backend + "_tx"  + suffix};
    eu::Recorder rec_rx {std::string{"lat_ws_"} + backend + "_rx"  + suffix};

    std::vector<uint8_t> payload(payload_size, 0xAB);
    std::vector<uint8_t> frame(ec::WsCodec::max_overhead + payload_size);
    ec::WsCodec encoder{};

    const uint64_t t_start    = ::bench::monotonic_raw_ns();
    const uint64_t t_deadline = t_start + duration_s * 1'000'000'000ull;
    uint64_t       t_measure_start = 0;
    constexpr uint64_t kPerSampleTimeoutNs = 5ull * 1'000'000'000ull;

    uint64_t sample_idx = 0;
    bool     timed_out  = false;
    while (::bench::monotonic_raw_ns() < t_deadline && !::bench::shutdown_requested()) {
        const uint64_t t0 = ::bench::monotonic_raw_ns();
        ::bench::write_client_ts(
            std::span<uint8_t>{payload.data(), ::bench::kTimestampBlockSize},
            t0);

        auto enc_r = encoder.encode(frame.data(), frame.size(),
                                    std::span<const uint8_t>{payload});
        if (!enc_r) {
            std::fprintf(stderr, "run_lat_ws_loop: WsCodec::encode failed: %s\n",
                         enc_r.error().detail);
            timed_out = true;
            break;
        }

        auto send_r = stream->send(
            std::span<const uint8_t>{frame.data(), *enc_r});
        if (!send_r) {
            std::fprintf(stderr, "run_lat_ws_loop: send failed at sample %llu: %s\n",
                         static_cast<unsigned long long>(sample_idx),
                         send_r.error().detail);
            break;
        }

        got_echo  = false;
        ts_filled = 0;
        while (!got_echo && !::bench::shutdown_requested()) {
            poller->poll();
            if (::bench::monotonic_raw_ns() - t0 > kPerSampleTimeoutNs) {
                std::fprintf(stderr,
                             "run_lat_ws_loop: echo timeout at sample %llu\n",
                             static_cast<unsigned long long>(sample_idx));
                timed_out = true;
                break;
            }
        }
        if (timed_out || ::bench::shutdown_requested()) break;

        const uint64_t t1 = ::bench::monotonic_raw_ns();
        if (sample_idx == warmup_samples) {
            t_measure_start = t0;
        }
        if (sample_idx >= warmup_samples &&
            ts_filled == ::bench::kTimestampBlockSize) {
            const auto ts  = ::bench::read_ts(
                std::span<const uint8_t>{ts_buf.data(), ts_buf.size()});
            const auto lgs = ::bench::compute_legs(ts, t1);
            rec_rtt.record_ns(lgs.rtt_ns);
            rec_tx .record_ns(lgs.tx_ns);
            rec_rx .record_ns(lgs.rx_ns);
        }
        ++sample_idx;
    }

    const uint64_t wall_time_ns =
        (t_measure_start != 0)
            ? (::bench::monotonic_raw_ns() - t_measure_start)
            : 0;
    ::bench::print_leg_report("lat_ws", backend, rec_rtt, rec_tx, rec_rx,
                              warmup_samples, wall_time_ns);
    (void)::bench::export_legs(rec_rtt, rec_tx, rec_rx);

    (void)stream->close_gracefully();
#if !defined(EPH_USE_DPDK)
    poller->poll(std::chrono::milliseconds{50});
#endif
    (void)poller->remove(stream.get());

    return timed_out ? 4 : 0;
}

} // namespace bench::scenarios
