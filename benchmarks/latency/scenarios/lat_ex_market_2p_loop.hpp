/// @file scenarios/lat_ex_market_2p_loop.hpp
/// Two-phase multi-symbol bookTicker measurement (naive vs twophase).
/// Two entry points:
///   - run_lat_ex_market_2p_measurement<StreamT, PollerT> (existing
///     templated body, signature: PollerT& instead of unique_ptr<>&)
///   - run_lat_ex_market_2p_loop<EnableTls>(BenchCtx&) for parallel-bench v2.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include "eph/codec/ws_codec.hpp"
#include "eph/json/adapters/binance.hpp"
#include "eph/json/parser.hpp"
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

inline constexpr std::size_t kEx2pMaxSymbols  = 64;
inline constexpr std::size_t kEx2pMaxFrameSize = 512;

/// Per-symbol slot for two-phase mode. Reused from lat_ex_market_2p.cpp.
struct Slot2P {
    uint32_t hash     = 0;
    bool     active   = false;
    uint16_t len      = 0;
    uint64_t t_server = 0;
    uint8_t  data[kEx2pMaxFrameSize]{};
};

enum class Mode2P { kNaive, kTwophase };

[[nodiscard]] inline Mode2P parse_mode_2p(std::string_view s) noexcept {
    if (s == "naive") return Mode2P::kNaive;
    return Mode2P::kTwophase;
}

template <class StreamT, class PollerT>
int run_lat_ex_market_2p_measurement(std::unique_ptr<StreamT> stream,
                                     PollerT& poller,
                                     ::eph::utils::Recorder& rec,
                                     Mode2P mode,
                                     uint32_t burst_size,
                                     uint64_t duration_s,
                                     uint64_t warmup_samples,
                                     const char* mode_label,
                                     const char* backend) noexcept {
    namespace ej = ::eph::json;
    (void)burst_size;

    uint64_t sample_idx      = 0;
    uint64_t total_frames    = 0;
    uint64_t parsed_frames   = 0;
    uint64_t malformed       = 0;
    uint64_t clock_skew      = 0;
    uint64_t slot_overflow   = 0;
    // Two-phase mode latches frames into per-symbol slots whose data buffer
    // is fixed at kEx2pMaxFrameSize. When `n > kEx2pMaxFrameSize` the
    // memcpy below silently truncates and the deferred ej::parse() will
    // fail (incomplete JSON), inflating `malformed` with no breadcrumb to
    // distinguish "server sent bad JSON" from "frame larger than slot
    // buffer". Track truncation explicitly so an operator can tell which
    // root cause is in play. Naive mode parses inline and is unaffected.
    uint64_t frame_truncated = 0;
    uint64_t t_measure_start = 0;

    Slot2P slots[kEx2pMaxSymbols]{};
    std::size_t n_slots = 0;

    if (mode == Mode2P::kNaive) {
        stream->on_message = [&](std::span<const uint8_t> app_frame) {
            ++total_frames;
            const auto* d = app_frame.data();
            const auto  n = app_frame.size();
            auto json_r = ej::parse(d, n);
            if (!json_r) { ++malformed; return; }
            auto ticker = ej::binance::BookTicker::from(json_r.value());
            if (!ticker) { ++malformed; return; }
            ++parsed_frames;
            auto t_server_opt = ::bench::scan_json_uint_field(d, n, "T");
            if (!t_server_opt) { ++malformed; return; }
            const uint64_t t_server = *t_server_opt;
            const uint64_t t_actionable = ::bench::monotonic_raw_ns();
            if (t_actionable <= t_server) { ++clock_skew; return; }
            if (sample_idx == warmup_samples) t_measure_start = t_actionable;
            if (sample_idx >= warmup_samples) rec.record_ns(t_actionable - t_server);
            ++sample_idx;
        };
    } else {
        stream->on_message = [&](std::span<const uint8_t> app_frame) {
            ++total_frames;
            const auto* d = app_frame.data();
            const auto  n = app_frame.size();
            const uint32_t hash = ej::binance::symbol_hash(d, n);
            if (hash == 0) { ++malformed; return; }
            auto t_server_opt = ::bench::scan_json_uint_field(d, n, "T");
            if (!t_server_opt) { ++malformed; return; }
            Slot2P* target = nullptr;
            for (std::size_t i = 0; i < n_slots; ++i) {
                if (slots[i].hash == hash) { target = &slots[i]; break; }
            }
            if (!target) {
                if (n_slots >= kEx2pMaxSymbols) { ++slot_overflow; return; }
                target = &slots[n_slots++];
                target->hash = hash;
            }
            // Track silent truncation for triage — the deferred parse will
            // fail with kIncomplete and bump `malformed`, but without this
            // counter operators cannot tell whether the root cause is
            // genuine bad JSON or undersized slot buffers.
            if (n > kEx2pMaxFrameSize) [[unlikely]] {
                ++frame_truncated;
            }
            const uint16_t copy_len =
                static_cast<uint16_t>(std::min<std::size_t>(n, kEx2pMaxFrameSize));
            std::memcpy(target->data, d, copy_len);
            target->len      = copy_len;
            target->t_server = *t_server_opt;
            target->active   = true;
        };
    }

    const uint64_t t_start = ::bench::monotonic_raw_ns();
    const uint64_t t_deadline =
        t_start + (duration_s + 2) * 1'000'000'000ull;

    while (::bench::monotonic_raw_ns() < t_deadline &&
           !::bench::shutdown_requested()) {
        poller.poll();
        if (mode == Mode2P::kTwophase) {
            for (std::size_t i = 0; i < n_slots; ++i) {
                if (!slots[i].active) continue;
                slots[i].active = false;
                auto json_r = ej::parse(slots[i].data, slots[i].len);
                if (!json_r) { ++malformed; continue; }
                auto ticker = ej::binance::BookTicker::from(json_r.value());
                if (!ticker) { ++malformed; continue; }
                ++parsed_frames;
                const uint64_t t_actionable = ::bench::monotonic_raw_ns();
                if (t_actionable <= slots[i].t_server) { ++clock_skew; continue; }
                if (sample_idx == warmup_samples) t_measure_start = t_actionable;
                if (sample_idx >= warmup_samples)
                    rec.record_ns(t_actionable - slots[i].t_server);
                ++sample_idx;
            }
        }
    }

    if (malformed > 0 || clock_skew > 0 || slot_overflow > 0 ||
        frame_truncated > 0) {
        std::fprintf(stderr,
            "lat_ex_market_2p: skipped %llu malformed + %llu clock-skew"
            " + %llu slot-overflow samples (frame_truncated=%llu — raise "
            "kEx2pMaxFrameSize if non-zero)\n",
            static_cast<unsigned long long>(malformed),
            static_cast<unsigned long long>(clock_skew),
            static_cast<unsigned long long>(slot_overflow),
            static_cast<unsigned long long>(frame_truncated));
    }

    const uint64_t wall_time_ns =
        (t_measure_start != 0)
            ? (::bench::monotonic_raw_ns() - t_measure_start)
            : 0;
    ::bench::print_report("lat_ex_market_2p", backend, rec,
                          warmup_samples, wall_time_ns);

    const double skip_ratio =
        (total_frames > 0)
            ? 1.0 - static_cast<double>(parsed_frames) /
                     static_cast<double>(total_frames)
            : 0.0;
    std::printf("\n--- two-phase stats ---\n");
    std::printf("mode:          %s\n", mode_label);
    std::printf("total_frames:  %llu\n",
                static_cast<unsigned long long>(total_frames));
    std::printf("parsed_frames: %llu\n",
                static_cast<unsigned long long>(parsed_frames));
    std::printf("skip_ratio:    %.2f%%\n", skip_ratio * 100.0);
    if (mode == Mode2P::kTwophase) {
        std::printf("active_slots:  %zu\n", n_slots);
    }
    std::fflush(stdout);

    if (!rec.export_json("benchmarks/latency/outputs")) {
        std::fprintf(stderr,
                     "[WARN] lat_ex_market_2p: export_json failed\n");
    }

    (void)stream->close_gracefully();
#if !defined(EPH_USE_DPDK)
    poller.poll(std::chrono::milliseconds{50});
#endif
    (void)poller.remove(stream.get());
    return 0;
}

template <bool EnableTls>
inline int run_lat_ex_market_2p_loop(::bench::BenchCtx& ctx) noexcept {
    namespace ec = ::eph::codec;
    namespace eu = ::eph::utils;
    namespace en = ::eph::net;

    const auto& bench_cfg = *ctx.bench_cfg;
    const auto& scenario  = *ctx.scenario_cfg;

    const uint16_t port = scenario.get_or<uint16_t>("port", 0);
    if (port == 0) {
        std::fprintf(stderr, "run_lat_ex_market_2p_loop: scenarios.<...>.port required\n");
        return 1;
    }
    const std::string ws_path =
        scenario.get_or<std::string>("ws_path", "/ws/bookticker");
    const uint64_t duration_s =
        scenario.get_or<uint32_t>("duration_seconds", 300);
    const uint32_t burst_size =
        scenario.get_or<uint32_t>("burst_size", 10);
    const std::string mode_str =
        scenario.get_or<std::string>("mode", "twophase");
    const Mode2P mode = parse_mode_2p(mode_str);
    const char* mode_label = (mode == Mode2P::kNaive) ? "naive" : "twophase";
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
    eu::Recorder rec{std::string{"lat_ex_market_2p_"} + backend +
                     "_" + mode_label + "_oneway" + suffix};

#if defined(EPH_USE_DPDK)
    namespace ed = ::eph::net::dpdk;
    using Stream = ed::DpdkTcpStream<ec::WsCodec, EnableTls>;
    using Poller = ed::DpdkPoller<>;

    if (ctx.view == nullptr) {
        std::fprintf(stderr, "run_lat_ex_market_2p_loop: ctx.view is null\n");
        return 3;
    }
    auto& view = *ctx.view;

    ed::StreamConfig cfg{};
    cfg.dpdk.tcp_low_level = view.make_tcp_config(::bench::random_src_port(), port);
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
        std::fprintf(stderr, "run_lat_ex_market_2p_loop: invalid mock_ip '%s'\n",
                     mock_ip_str.c_str());
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
        std::fprintf(stderr, "run_lat_ex_market_2p_loop: Stream::create failed: %s\n",
                     stream_r.error().detail);
        return 3;
    }

    Poller& poller = *static_cast<Poller*>(ctx.poller);

#if !defined(EPH_USE_DPDK)
    if (auto r = poller.add(stream_r.value().get()); !r) {
        std::fprintf(stderr, "run_lat_ex_market_2p_loop: poller->add failed: %s\n",
                     r.error().detail);
        return 3;
    }
#endif

    return run_lat_ex_market_2p_measurement<Stream, Poller>(
        std::move(stream_r.value()), poller, rec, mode, burst_size,
        duration_s, warmup_samples, mode_label, backend);
}

} // namespace bench::scenarios
