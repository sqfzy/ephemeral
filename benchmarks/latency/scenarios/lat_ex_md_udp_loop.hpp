/// @file scenarios/lat_ex_md_udp_loop.hpp
/// Reusable inner loop for lat_ex_md_udp. Structurally identical to
/// lat_udp_loop.hpp; only payload filler byte (0xEF vs 0xCD) and the
/// Recorder/scenario name differ.

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

#include "eph/codec/raw_datagram_codec.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/utils/recorder.hpp"

#if defined(EPH_USE_DPDK)
#  include "eph/net/dpdk/poller.hpp"
#  include "eph/net/dpdk/udp_socket.hpp"
#else
#  include "eph/net/kernel/config.hpp"
#  include "eph/net/kernel/poller.hpp"
#  include "eph/net/kernel/udp_socket.hpp"
#endif

#include "core/bench_conf.hpp"
#include "core/bench_ctx.hpp"
#include "core/measurement.hpp"
#include "core/timestamp_proto.hpp"
#if defined(EPH_USE_DPDK)
#  include "core/dpdk_env.hpp"
#endif

namespace bench::scenarios {

inline int run_lat_ex_md_udp_loop(::bench::BenchCtx& ctx) noexcept {
    namespace ec = ::eph::codec;
    namespace eu = ::eph::utils;
    namespace en = ::eph::net;

    const auto& bench_cfg = *ctx.bench_cfg;
    const auto& scenario  = *ctx.scenario_cfg;

    const uint16_t port = scenario.get_or<uint16_t>("port", 0);
    if (port == 0) {
        std::fprintf(stderr, "run_lat_ex_md_udp_loop: scenarios.<...>.port required\n");
        return 1;
    }
    const std::size_t payload_size =
        scenario.get_or<uint32_t>("payload_size", 256);
    if (payload_size < ::bench::kTimestampBlockSize) {
        std::fprintf(stderr, "run_lat_ex_md_udp_loop: payload_size=%zu < kTimestampBlockSize=%zu\n",
                     payload_size, ::bench::kTimestampBlockSize);
        return 1;
    }
    // UDP wire ceiling 64 KiB; ITCH/Mold64 datagrams are typically <=
    // 1.5 KiB. Cap at 16 KiB to keep the noexcept body's std::vector
    // payload allocation safely under bad_alloc threshold.
    constexpr std::size_t kMaxPayloadBytes = 16ull * 1024ull;
    if (payload_size > kMaxPayloadBytes) {
        std::fprintf(stderr, "run_lat_ex_md_udp_loop: payload_size=%zu exceeds max %zu\n",
                     payload_size, kMaxPayloadBytes);
        return 1;
    }
    const uint64_t duration_s =
        scenario.get_or<uint32_t>("duration_seconds", 30);
    const uint64_t warmup_samples = bench_cfg.measurement.warmup_samples;

    const std::string mock_ip_str = bench_cfg.networking.server_ip.empty()
                                        ? std::string{"127.0.0.1"}
                                        : bench_cfg.networking.server_ip;
    auto ip_r = en::Ipv4Addr::parse(mock_ip_str);
    if (!ip_r) {
        std::fprintf(stderr, "run_lat_ex_md_udp_loop: invalid mock_ip '%s': %s\n",
                     mock_ip_str.c_str(), ip_r.error().detail);
        return 1;
    }
    const en::SocketAddr remote{ip_r.value(), port};

#if defined(EPH_USE_DPDK)
    namespace ed = ::eph::net::dpdk;
    using Socket = ed::DpdkUdpSocket<ec::RawDatagramCodec>;
    using Poller = ed::DpdkPoller<>;
    auto* poller = static_cast<Poller*>(ctx.poller);

    if (ctx.view == nullptr) {
        std::fprintf(stderr, "run_lat_ex_md_udp_loop: ctx.view is null\n");
        return 3;
    }
    auto& view = *ctx.view;

    const uint16_t local_src_port = ::bench::random_src_port();
    ed::UdpConfig sock_cfg{};
    sock_cfg.legacy = view.make_udp_config(local_src_port, port);
    if (ctx.slot_index >= 0) {
        sock_cfg.pin_to_queue = ctx.queue_id;
    }

    auto sock_r = Socket::create_and_attach(std::move(sock_cfg), view.platform);
#else
    namespace ek = ::eph::net::kernel;
    using Socket = ek::KernelUdpSocket<ec::RawDatagramCodec>;
    using Poller = ek::KernelPoller;
    auto* poller = static_cast<Poller*>(ctx.poller);

    const std::string client_ip_str = bench_cfg.networking.client_ip.empty()
                                          ? std::string{"0.0.0.0"}
                                          : bench_cfg.networking.client_ip;
    auto client_ip_r = en::Ipv4Addr::parse(client_ip_str);
    if (!client_ip_r) {
        std::fprintf(stderr, "run_lat_ex_md_udp_loop: invalid client_ip '%s': %s\n",
                     client_ip_str.c_str(), client_ip_r.error().detail);
        return 1;
    }

    ek::UdpConfig sock_cfg{};
    sock_cfg.bind = en::SocketAddr{client_ip_r.value(), 0};

    auto sock_r = Socket::create(sock_cfg);
#endif
    if (!sock_r) {
        std::fprintf(stderr, "run_lat_ex_md_udp_loop: Socket::create failed: %s\n",
                     sock_r.error().detail);
        return 3;
    }
    auto sock = std::move(sock_r.value());

    bool got_echo = false;
    std::array<uint8_t, ::bench::kTimestampBlockSize> ts_buf{};
    std::size_t ts_filled = 0;
    sock->on_datagram = [&](std::span<const uint8_t> app_datagram,
                            const en::SocketAddr& /*src*/) {
        got_echo = true;
        ts_filled = 0;
        if (app_datagram.size() >= ::bench::kTimestampBlockSize) {
            std::memcpy(ts_buf.data(), app_datagram.data(),
                        ::bench::kTimestampBlockSize);
            ts_filled = ::bench::kTimestampBlockSize;
        }
    };

#if !defined(EPH_USE_DPDK)
    if (auto r = poller->add(sock.get()); !r) {
        std::fprintf(stderr, "run_lat_ex_md_udp_loop: poller->add failed: %s\n",
                     r.error().detail);
        return 3;
    }
#endif

    constexpr const char* backend =
#if defined(EPH_USE_DPDK)
        "dpdk";
#else
        "kernel";
#endif
    const std::string suffix =
        (ctx.slot_index < 0) ? std::string{}
                              : std::string{"_slot"} + std::to_string(ctx.slot_index);

    eu::Recorder rec_rtt{std::string{"lat_ex_md_udp_"} + backend + "_rtt" + suffix};
    eu::Recorder rec_tx {std::string{"lat_ex_md_udp_"} + backend + "_tx"  + suffix};
    eu::Recorder rec_rx {std::string{"lat_ex_md_udp_"} + backend + "_rx"  + suffix};

    std::vector<uint8_t> payload(payload_size, 0xEF);

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

        auto send_r = sock->send_to(std::span<const uint8_t>{payload}, remote);
        if (!send_r) {
            std::fprintf(stderr,
                         "run_lat_ex_md_udp_loop: send_to failed at sample %llu: %s\n",
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
                             "run_lat_ex_md_udp_loop: echo timeout at sample %llu\n",
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
    ::bench::print_leg_report("lat_ex_md_udp", backend, rec_rtt, rec_tx, rec_rx,
                              warmup_samples, wall_time_ns);
    (void)::bench::export_legs(rec_rtt, rec_tx, rec_rx);

    (void)poller->remove(sock.get());

    return timed_out ? 4 : 0;
}

} // namespace bench::scenarios
