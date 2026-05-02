/// @file scenarios/lat_ex_order_loop.hpp
/// Strict-serial WS order RTT measurement (lat_ex_order). Per-sample
/// state (current_id, response_seen, json_buf, encoder, etc.) becomes
/// function-local. Templated on EnableTls.

#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
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
#include "core/json_scan.hpp"
#include "core/measurement.hpp"
#include "core/timestamp_proto.hpp"
#if defined(EPH_USE_DPDK)
#  include "core/dpdk_env.hpp"
#endif

namespace bench::scenarios {

template <bool EnableTls>
inline int run_lat_ex_order_loop(::bench::BenchCtx& ctx) noexcept {
    namespace ec = ::eph::codec;
    namespace eu = ::eph::utils;
    namespace en = ::eph::net;

    const auto& bench_cfg = *ctx.bench_cfg;
    const auto& scenario  = *ctx.scenario_cfg;

    const uint16_t port = scenario.get_or<uint16_t>("port", 0);
    if (port == 0) {
        std::fprintf(stderr, "run_lat_ex_order_loop: scenarios.<...>.port required\n");
        return 1;
    }
    const std::string ws_path =
        scenario.get_or<std::string>("ws_path", "/ws/order");
    const uint64_t order_count =
        scenario.get_or<uint64_t>("order_count", 10000);
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

    eu::Recorder rec_rtt{std::string{"lat_ex_order_"} + backend + "_rtt" + suffix};
    eu::Recorder rec_tx {std::string{"lat_ex_order_"} + backend + "_tx"  + suffix};
    eu::Recorder rec_rx {std::string{"lat_ex_order_"} + backend + "_rx"  + suffix};

#if defined(EPH_USE_DPDK)
    namespace ed = ::eph::net::dpdk;
    using Stream = ed::DpdkTcpStream<ec::WsCodec, EnableTls>;
    using Poller = ed::DpdkPoller<>;
    auto* poller = static_cast<Poller*>(ctx.poller);

    if (ctx.view == nullptr) {
        std::fprintf(stderr, "run_lat_ex_order_loop: ctx.view is null\n");
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
    auto* poller = static_cast<Poller*>(ctx.poller);

    auto ip_r = en::Ipv4Addr::parse(mock_ip_str);
    if (!ip_r) {
        std::fprintf(stderr, "run_lat_ex_order_loop: invalid mock_ip '%s'\n",
                     mock_ip_str.c_str());
        return 1;
    }
    const en::SocketAddr remote{ip_r.value(), port};

    ek::StreamConfig cfg{};
    cfg.remote          = remote;
    cfg.reasm_capacity  = 64 * 1024;
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
        std::fprintf(stderr, "run_lat_ex_order_loop: Stream::create failed: %s\n",
                     stream_r.error().detail);
        return 3;
    }
    auto stream = std::move(stream_r.value());

    uint64_t current_id      = 0;
    bool     response_seen   = false;
    uint64_t sample_idx      = 0;
    uint64_t recorded_count  = 0;
    uint64_t stale_count     = 0;
    uint64_t malformed_count = 0;
    uint64_t t_measure_start = 0;

    stream->on_message = [&](std::span<const uint8_t> app_frame) {
        const uint64_t t_recv = ::bench::monotonic_raw_ns();
        const auto* d   = app_frame.data();
        const std::size_t nsz = app_frame.size();

        auto id_opt = ::bench::scan_json_uint_field(d, nsz, "id");
        if (!id_opt) {
            ++malformed_count;
            return;
        }
        if (current_id == 0 || *id_opt != current_id) {
            ++stale_count;
            return;
        }

        auto tc_opt = ::bench::scan_json_uint_field(d, nsz, "t_client");
        auto tr_opt = ::bench::scan_json_uint_field(d, nsz, "t_mock_recv");
        auto ts_opt = ::bench::scan_json_uint_field(d, nsz, "t_mock_send");

        if (sample_idx == warmup_samples) {
            t_measure_start = t_recv;
        }
        if (sample_idx >= warmup_samples) {
            if (tc_opt && tr_opt && ts_opt &&
                t_recv >= *tc_opt && *tr_opt >= *tc_opt && t_recv >= *ts_opt) {
                const ::bench::TimestampBlock tsb{
                    .client_ns    = *tc_opt,
                    .mock_recv_ns = *tr_opt,
                    .mock_send_ns = *ts_opt,
                };
                const auto lgs = ::bench::compute_legs(tsb, t_recv);
                rec_rtt.record_ns(lgs.rtt_ns);
                rec_tx .record_ns(lgs.tx_ns);
                rec_rx .record_ns(lgs.rx_ns);
                ++recorded_count;
            } else {
                ++malformed_count;
            }
        }
        ++sample_idx;
        response_seen = true;
    };

#if !defined(EPH_USE_DPDK)
    if (auto r = poller->add(stream.get()); !r) {
        std::fprintf(stderr, "run_lat_ex_order_loop: poller->add failed: %s\n",
                     r.error().detail);
        return 3;
    }
#endif

    constexpr std::size_t kJsonBufSize = 128;
    char      json_buf[kJsonBufSize];
    std::vector<uint8_t> frame(ec::WsCodec::max_overhead + kJsonBufSize);
    ec::WsCodec encoder{};

    const uint64_t t_start = ::bench::monotonic_raw_ns();
    const uint64_t t_deadline =
        t_start + (duration_s + 2) * 1'000'000'000ull;
    const uint64_t plan_total = order_count + warmup_samples;
    constexpr uint64_t kPerSampleTimeoutNs = 5ull * 1'000'000'000ull;

    bool send_failed = false;
    bool timed_out   = false;
    uint64_t next_id = 1;
    while (next_id <= plan_total && !::bench::shutdown_requested()) {
        if (::bench::monotonic_raw_ns() >= t_deadline) break;

        const uint64_t id = next_id;
        const uint64_t t_client = ::bench::monotonic_raw_ns();
        const int jn = std::snprintf(
            json_buf, sizeof(json_buf),
            "{\"e\":\"NewOrder\",\"id\":%llu,\"t_client\":%llu}",
            static_cast<unsigned long long>(id),
            static_cast<unsigned long long>(t_client));
        if (jn <= 0 || static_cast<std::size_t>(jn) >= sizeof(json_buf)) {
            std::fprintf(stderr,
                         "run_lat_ex_order_loop: json snprintf overflow at id=%llu\n",
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
            std::fprintf(stderr, "run_lat_ex_order_loop: WsCodec::encode failed: %s\n",
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
                         "run_lat_ex_order_loop: send failed at id=%llu: %s\n",
                         static_cast<unsigned long long>(id),
                         send_r.error().detail);
            send_failed = true;
            break;
        }

        const uint64_t t0 = t_client;
        while (!response_seen && !::bench::shutdown_requested()) {
            poller->poll();
            if (::bench::monotonic_raw_ns() - t0 > kPerSampleTimeoutNs) {
                std::fprintf(stderr,
                             "run_lat_ex_order_loop: response timeout at id=%llu\n",
                             static_cast<unsigned long long>(id));
                timed_out = true;
                break;
            }
        }
        if (timed_out || ::bench::shutdown_requested()) break;

        current_id = 0;
        ++next_id;
    }

    if (malformed_count > 0 || stale_count > 0) {
        std::fprintf(stderr,
                     "run_lat_ex_order_loop: skipped %llu malformed + %llu stale responses\n",
                     static_cast<unsigned long long>(malformed_count),
                     static_cast<unsigned long long>(stale_count));
    }
    std::fprintf(stderr,
                 "run_lat_ex_order_loop: completed %llu orders (recorded %llu post-warmup)\n",
                 static_cast<unsigned long long>(sample_idx),
                 static_cast<unsigned long long>(recorded_count));

    const uint64_t wall_time_ns =
        (t_measure_start != 0)
            ? (::bench::monotonic_raw_ns() - t_measure_start)
            : 0;
    ::bench::print_leg_report("lat_ex_order", backend, rec_rtt, rec_tx, rec_rx,
                              warmup_samples, wall_time_ns);
    (void)::bench::export_legs(rec_rtt, rec_tx, rec_rx);

    (void)stream->close_gracefully();
#if !defined(EPH_USE_DPDK)
    poller->poll(std::chrono::milliseconds{50});
#endif
    (void)poller->remove(stream.get());

    if (send_failed) return 4;
    if (timed_out)   return 5;
    return 0;
}

} // namespace bench::scenarios
