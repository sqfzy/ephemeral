/// @file scenarios/lat_tcp_loop.hpp
/// Reusable inner-loop function for the `lat_tcp` scenario. Extracted
/// from `lat_tcp.cpp:182-393`'s `run<EnableTls>()` lambda so two callers
/// can drive it:
///
///   1. `lat_tcp.cpp` main() — single-binary command (`lat tcp --dpdk`)
///      Constructs a single-Platform `DpdkBenchEnv` (or kernel SocketAddr),
///      builds a BenchCtx with `slot_index = -1` (= no `_slot` suffix on
///      Recorder names → byte-equal with pre-reshape JSON output), and
///      calls `run_lat_tcp_loop<EnableTls>(ctx)`.
///
///   2. `lat_multi_dpdk.cpp` (added in Phase 7) — multi-scenario runner
///      Each EAL worker lcore has its own BenchCtx with `slot_index = i`
///      from `[parallel].runs[i]`, sharing one Platform via DpdkBenchView.
///      The function constructs its own Stream pinned to `ctx.queue_id`
///      via `pin_to_queue` so RSS routes the reverse traffic to a queue
///      this worker owns.
///
/// **Byte-equal invariant**: when `ctx.slot_index < 0` the function
/// produces identical output (Recorder names, JSON filenames, log lines)
/// to the pre-reshape `lat_tcp_dpdk` / `lat_tcp` binary. The 30s parity
/// gate (≤5%) verifies hot-path equivalence.

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

#include "eph/codec/raw_stream_codec.hpp"
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

/// Run the lat_tcp measurement body to completion (or shutdown signal).
///
/// Caller must have:
///   - `ctx.bench_cfg` and `ctx.scenario_cfg` non-null
///   - `ctx.poller` non-null and (DPDK) registered with the Platform
///     for `ctx.queue_id`, or (kernel) ready to accept
///     `poller->add(stream)`
///   - `ctx.view` non-null in DPDK builds
///   - `ctx.running` non-null (or this binary's
///     `bench::shutdown_requested()` proxy still works — we read both)
///
/// Returns:
///   0  - measurement window completed normally
///   3  - Stream::create / create_and_attach failed
///   4  - per-sample echo timeout (mock died)
template <bool EnableTls>
inline int run_lat_tcp_loop(::bench::BenchCtx& ctx) noexcept {
    namespace ec = ::eph::codec;
    namespace eu = ::eph::utils;
    namespace en = ::eph::net;

    const auto& bench_cfg = *ctx.bench_cfg;
    const auto& scenario  = *ctx.scenario_cfg;

    // ── Re-derive scenario-level params (same logic as lat_tcp.cpp main) ──
    const uint16_t port = scenario.get_or<uint16_t>("port", 0);
    if (port == 0) {
        std::fprintf(stderr, "run_lat_tcp_loop: scenarios.<...>.port required\n");
        return 1;
    }
    const std::size_t payload_size =
        scenario.get_or<uint32_t>("payload_size", 256);
    if (payload_size < ::bench::kTimestampBlockSize) {
        std::fprintf(stderr,
                     "run_lat_tcp_loop: payload_size=%zu < kTimestampBlockSize=%zu\n",
                     payload_size, ::bench::kTimestampBlockSize);
        return 1;
    }
    // Cap payload_size: bench buffers (payload + reasm = payload * 4) are
    // allocated as plain std::vectors in a noexcept function — a config
    // typo of e.g. 1 GiB would terminate the process via bad_alloc rather
    // than producing a clean error. 16 MiB exceeds any realistic exchange
    // payload and keeps reasm * 4 well below 100 MiB.
    constexpr std::size_t kMaxPayloadBytes = 16ull * 1024ull * 1024ull;
    if (payload_size > kMaxPayloadBytes) {
        std::fprintf(stderr,
                     "run_lat_tcp_loop: payload_size=%zu exceeds max %zu\n",
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
    using Stream = ed::DpdkTcpStream<ec::RawStreamCodec, EnableTls>;
    using Poller = ed::DpdkPoller<>;
    auto* poller = static_cast<Poller*>(ctx.poller);

    if (ctx.view == nullptr) {
        std::fprintf(stderr, "run_lat_tcp_loop: ctx.view is null in DPDK build\n");
        return 3;
    }
    auto& view = *ctx.view;

    ed::StreamConfig cfg{};
    cfg.dpdk.tcp_low_level = view.make_tcp_config(::bench::random_src_port(), port);
    cfg.dpdk.pool          = view.pool;
    cfg.connect_timeout    = std::chrono::milliseconds{3000};
    // Multi-scenario mode (slot_index >= 0) pins src_port to this run's
    // queue so RSS routes the inbound reply to the worker's owned queue.
    // Single-binary mode leaves pin_to_queue unset — equivalent to
    // pre-reshape behaviour where the post-PR-2.5 RETA-collapse fix
    // (single-queue Software dispatch) routes traffic to queue 0.
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
    using Stream = ek::KernelTcpStream<ec::RawStreamCodec, EnableTls>;
    using Poller = ek::KernelPoller;
    auto* poller = static_cast<Poller*>(ctx.poller);

    auto ip_r = en::Ipv4Addr::parse(mock_ip_str);
    if (!ip_r) {
        std::fprintf(stderr, "run_lat_tcp_loop: invalid mock_ip '%s': %s\n",
                     mock_ip_str.c_str(), ip_r.error().detail);
        return 1;
    }
    const en::SocketAddr remote{ip_r.value(), port};

    ek::StreamConfig cfg{};
    cfg.remote          = remote;
    cfg.reasm_capacity  = std::max<std::size_t>(64 * 1024, payload_size * 4);
    cfg.connect_timeout = std::chrono::milliseconds{3000};

    if constexpr (EnableTls) {
        cfg.tls.hostname    = mock_ip_str;
        cfg.tls.verify_peer = false;
    }

    auto stream_r = Stream::create(cfg);
#endif
    if (!stream_r) {
        std::fprintf(stderr, "run_lat_tcp_loop: Stream::create failed: %s\n",
                     stream_r.error().detail);
        return 3;
    }
    auto stream = std::move(stream_r.value());

    // on_message: copy the first 24 B of the echoed payload into ts_buf,
    // count rx_bytes for partial-recv tolerance.
    std::size_t rx_bytes = 0;
    std::array<uint8_t, ::bench::kTimestampBlockSize> ts_buf{};
    std::size_t ts_filled = 0;
    stream->on_message = [&](std::span<const uint8_t> app_frame) {
        if (ts_filled < ts_buf.size()) {
            const std::size_t want = ts_buf.size() - ts_filled;
            const std::size_t copy = (app_frame.size() < want)
                                       ? app_frame.size() : want;
            std::memcpy(ts_buf.data() + ts_filled, app_frame.data(), copy);
            ts_filled += copy;
        }
        rx_bytes += app_frame.size();
    };

#if !defined(EPH_USE_DPDK)
    if (auto r = poller->add(stream.get()); !r) {
        std::fprintf(stderr, "run_lat_tcp_loop: poller->add failed: %s\n",
                     r.error().detail);
        return 3;
    }
#endif

    // ── Recorders ─────────────────────────────────────────────────────
    constexpr const char* backend =
#if defined(EPH_USE_DPDK)
        EnableTls ? "dpdk_tls" : "dpdk";
#else
        EnableTls ? "kernel_tls" : "kernel";
#endif
    // slot_index < 0 → single-binary mode → no _slot suffix → byte-equal
    // JSON filenames with pre-reshape output.
    const std::string suffix =
        (ctx.slot_index < 0) ? std::string{}
                              : std::string{"_slot"} + std::to_string(ctx.slot_index);

    eu::Recorder rec_rtt{std::string{"lat_tcp_"} + backend + "_rtt" + suffix};
    eu::Recorder rec_tx {std::string{"lat_tcp_"} + backend + "_tx"  + suffix};
    eu::Recorder rec_rx {std::string{"lat_tcp_"} + backend + "_rx"  + suffix};

    // ── Measurement loop (literal copy from lat_tcp.cpp:295-368) ──────
    std::vector<uint8_t> payload(payload_size, 0xAB);

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

        auto send_r = stream->send(std::span<const uint8_t>{payload});
        if (!send_r) {
            std::fprintf(stderr, "run_lat_tcp_loop: send failed at sample %llu: %s\n",
                         static_cast<unsigned long long>(sample_idx),
                         send_r.error().detail);
            break;
        }

        rx_bytes  = 0;
        ts_filled = 0;
        while (rx_bytes < payload_size && !::bench::shutdown_requested()) {
            poller->poll();
            if (::bench::monotonic_raw_ns() - t0 > kPerSampleTimeoutNs) {
                std::fprintf(stderr,
                             "run_lat_tcp_loop: echo timeout at sample %llu (rx=%zu/%zu)\n",
                             static_cast<unsigned long long>(sample_idx),
                             rx_bytes, payload_size);
                timed_out = true;
                break;
            }
        }
        if (timed_out || ::bench::shutdown_requested()) break;

        const uint64_t t1 = ::bench::monotonic_raw_ns();
        if (sample_idx == warmup_samples) {
            t_measure_start = t0;
        }
        if (sample_idx >= warmup_samples) {
            const auto ts  = ::bench::read_ts(
                std::span<const uint8_t>{ts_buf.data(), ts_buf.size()});
            const auto lgs = ::bench::compute_legs(ts, t1);
            rec_rtt.record_ns(lgs.rtt_ns);
            rec_tx .record_ns(lgs.tx_ns);
            rec_rx .record_ns(lgs.rx_ns);
        }
        ++sample_idx;
    }

    // ── Report ───────────────────────────────────────────────────────
    const uint64_t wall_time_ns =
        (t_measure_start != 0)
            ? (::bench::monotonic_raw_ns() - t_measure_start)
            : 0;
    ::bench::print_leg_report("lat_tcp", backend, rec_rtt, rec_tx, rec_rx,
                              warmup_samples, wall_time_ns);
    (void)::bench::export_legs(rec_rtt, rec_tx, rec_rx);

    // ── Graceful close ───────────────────────────────────────────────
    (void)stream->close_gracefully();
#if !defined(EPH_USE_DPDK)
    poller->poll(std::chrono::milliseconds{50});
#endif
    (void)poller->remove(stream.get());

    return timed_out ? 4 : 0;
}

} // namespace bench::scenarios
