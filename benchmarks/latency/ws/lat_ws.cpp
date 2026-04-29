/// @file lat_ws.cpp
/// Latency benchmark: WebSocket RTT against the `ws` scenario of
/// `benchmarks/mockex/mockex`.
///
/// Real-server note: this scenario is inherently echo-RTT. Public
/// exchanges do not offer a WS echo endpoint, so the `endpoint =
/// wss://...` dual-mode switch from Phase 4 is not wired here —
/// there is no meaningful real-server counterpart to validate
/// against. Stay on the mock.
///
///   * Single-file scenario binary that reads `[lat_ws]` from bench.conf
///     (port / ws_path / payload_size / duration_seconds / use_tls)
///     plus the lowercase global `mock_ip`, `warmup_samples`.
///   * Uses `KernelTcpStream<WsCodec, EnableTls>` + `KernelPoller`
///     API directly — no raw socket() calls.
///   * `StreamConfig.ws_path` is set — this triggers the transparent
///     WebSocket handshake inside `KernelTcpStream::create()`,
///     so by the time we start the measurement loop the socket is ready
///     to exchange WS-binary frames with the mockex handler.
///   * Measurement clock is `bench::monotonic_raw_ns()`
///     (CLOCK_MONOTONIC_RAW via vDSO) — not TSC.
///   * Samples feed `eph::utils::Recorder::record_ns(ns)`.
///
/// Design note — WS frame encoding: `KernelTcpStream::send(bytes)` does
/// NOT run the bytes through the codec (the codec's encode path is for
/// user-assembled payloads), so we pre-encode a WS-binary frame once via
/// `WsCodec::encode` and reuse that buffer for every RTT sample. Decode
/// on the RX side IS driven through the codec (by the poller), so the
/// `on_message` callback fires once per decoded payload.
///
/// TLS: when `[scenarios.lat_ws].use_tls = true` (the default), the
/// bench loop instantiates `Stream<EnableTls=true>` so the WS
/// handshake rides on TLS 1.3. `verify_peer = false` because the mock
/// uses a self-signed cert (benchmarks/mockex/fixtures/tls/server.crt).
/// The DPDK variant uses `DpdkTcpStream`'s in-place mbuf TLS decrypt.
///
/// A second target `lat_ws_dpdk` is produced by the xmake auto-glob loop
/// with `EPH_USE_DPDK=1`.

#include <array>
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
#  include "eph/net/dpdk/poller.hpp"
#  include "eph/net/dpdk/tcp_stream.hpp"
#else
#  include "eph/net/kernel/config.hpp"
#  include "eph/net/kernel/poller.hpp"
#  include "eph/net/kernel/tcp_stream.hpp"
#endif

#include "core/measurement.hpp"
#include "core/pin_client.hpp"
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
template <bool EnableTls>
using StreamT = ed::DpdkTcpStream<ec::WsCodec, EnableTls>;
using Poller  = ed::DpdkPoller<>;
#else
namespace ek = eph::net::kernel;
template <bool EnableTls>
using StreamT = ek::KernelTcpStream<ec::WsCodec, EnableTls>;
using Poller  = ek::KernelPoller;
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

/// Templated bench body. EnableTls flips the stream type (and the
/// TlsConfig population in cfg). Same body for both transports.
template <bool EnableTls>
int run_ws(const bench::BenchConfig& bench_cfg,
           [[maybe_unused]] const en::SocketAddr& remote,
           const std::string& mock_ip_str,
           [[maybe_unused]] uint16_t port,
           const std::string& ws_path,
           std::size_t payload_size,
           uint64_t duration_s,
           uint64_t warmup_samples) {
    using Stream = StreamT<EnableTls>;

    constexpr const char* backend =
#if defined(EPH_USE_DPDK)
        EnableTls ? "dpdk_tls" : "dpdk";
#else
        EnableTls ? "kernel_tls" : "kernel";
#endif

    eu::Recorder rec_rtt{std::string{"lat_ws_"} + backend + "_rtt"};
    eu::Recorder rec_tx {std::string{"lat_ws_"} + backend + "_tx" };
    eu::Recorder rec_rx {std::string{"lat_ws_"} + backend + "_rx" };

#if defined(EPH_USE_DPDK)
    auto env_r = bench::load_dpdk_env(bench_cfg, /*port_id=*/0);
    if (!env_r) {
        std::fprintf(stderr, "lat_ws: %s\n", env_r.error().c_str());
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
        std::fprintf(stderr, "lat_ws: Poller::create failed: %s\n",
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
    cfg.reasm_capacity  = std::max<std::size_t>(64 * 1024, payload_size * 4);
    cfg.connect_timeout = std::chrono::milliseconds{3000};
    cfg.ws.path         = ws_path;
    cfg.ws.timeout      = std::chrono::seconds{10};
#endif

    if constexpr (EnableTls) {
        // Bench-only: self-signed cert from benchmarks/mockex/fixtures/tls/.
        // Production code would set ca_cert_path or pin SPKI; here we
        // trust the IP-pinned mockex.
        cfg.tls.hostname    = mock_ip_str;
        cfg.tls.verify_peer = false;
    }

#if defined(EPH_USE_DPDK)
    if (auto rr = env.platform.register_poller(0, poller.get()); !rr) {
        std::fprintf(stderr, "lat_ws: register_poller failed: %s\n",
                     rr.error().detail);
        return 3;
    }
    auto stream_r = Stream::create_and_attach(std::move(cfg), env.platform);
#else
    auto stream_r = Stream::create(cfg);
#endif
    if (!stream_r) {
        std::fprintf(stderr, "lat_ws: Stream::create failed: %s\n",
                     stream_r.error().detail);
        return 3;
    }
    auto stream = std::move(stream_r.value());

    bool got_echo = false;
    std::array<uint8_t, bench::kTimestampBlockSize> ts_buf{};
    std::size_t ts_filled = 0;
    stream->on_message = [&](std::span<const uint8_t> app_frame) {
        got_echo = true;
        ts_filled = 0;
        if (app_frame.size() >= bench::kTimestampBlockSize) {
            std::memcpy(ts_buf.data(), app_frame.data(),
                        bench::kTimestampBlockSize);
            ts_filled = bench::kTimestampBlockSize;
        }
    };

#if !defined(EPH_USE_DPDK)
    if (auto r = poller->add(stream.get()); !r) {
        std::fprintf(stderr, "lat_ws: poller->add failed: %s\n",
                     r.error().detail);
        return 3;
    }
#endif

    std::vector<uint8_t> payload(payload_size, 0xAB);
    std::vector<uint8_t> frame(ec::WsCodec::max_overhead + payload_size);
    ec::WsCodec encoder{};

    const uint64_t t_start    = bench::monotonic_raw_ns();
    const uint64_t t_deadline = t_start + duration_s * 1'000'000'000ull;
    uint64_t       t_measure_start = 0;
    constexpr uint64_t kPerSampleTimeoutNs = 5ull * 1'000'000'000ull;

    uint64_t sample_idx = 0;
    bool     timed_out  = false;
    while (bench::monotonic_raw_ns() < t_deadline && !bench::shutdown_requested()) {
        const uint64_t t0 = bench::monotonic_raw_ns();
        bench::write_client_ts(
            std::span<uint8_t>{payload.data(), bench::kTimestampBlockSize},
            t0);

        auto enc_r = encoder.encode(frame.data(), frame.size(),
                                    std::span<const uint8_t>{payload});
        if (!enc_r) {
            std::fprintf(stderr, "lat_ws: WsCodec::encode failed: %s\n",
                         enc_r.error().detail);
            timed_out = true;
            break;
        }

        auto send_r = stream->send(
            std::span<const uint8_t>{frame.data(), *enc_r});
        if (!send_r) {
            std::fprintf(stderr, "lat_ws: send failed at sample %llu: %s\n",
                         static_cast<unsigned long long>(sample_idx),
                         send_r.error().detail);
            break;
        }

        got_echo  = false;
        ts_filled = 0;
        while (!got_echo && !bench::shutdown_requested()) {
            poller->poll();
            if (bench::monotonic_raw_ns() - t0 > kPerSampleTimeoutNs) {
                std::fprintf(stderr,
                             "lat_ws: echo timeout at sample %llu\n",
                             static_cast<unsigned long long>(sample_idx));
                timed_out = true;
                break;
            }
        }
        if (timed_out || bench::shutdown_requested()) break;

        const uint64_t t1 = bench::monotonic_raw_ns();
        if (sample_idx == warmup_samples) {
            t_measure_start = t0;
        }
        if (sample_idx >= warmup_samples &&
            ts_filled == bench::kTimestampBlockSize) {
            const auto ts  = bench::read_ts(
                std::span<const uint8_t>{ts_buf.data(), ts_buf.size()});
            const auto lgs = bench::compute_legs(ts, t1);
            rec_rtt.record_ns(lgs.rtt_ns);
            rec_tx .record_ns(lgs.tx_ns);
            rec_rx .record_ns(lgs.rx_ns);
        }
        ++sample_idx;
    }

    const uint64_t wall_time_ns =
        (t_measure_start != 0)
            ? (bench::monotonic_raw_ns() - t_measure_start)
            : 0;
    bench::print_leg_report("lat_ws", backend, rec_rtt, rec_tx, rec_rx,
                            warmup_samples, wall_time_ns);
    (void)bench::export_legs(rec_rtt, rec_tx, rec_rx);

    (void)stream->close_gracefully();
#if !defined(EPH_USE_DPDK)
    poller->poll(std::chrono::milliseconds{50});
#endif
    (void)poller->remove(stream.get());
    stream.reset();
    poller.reset();

    return timed_out ? 4 : 0;
}

} // namespace

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::info);

    const char* conf_path = parse_config_path(argc, argv);

    auto cfg_r = bench::load_bench_conf(conf_path);
    if (!cfg_r) {
        std::fprintf(stderr, "lat_ws: %s\n",
                     bench::format_error(cfg_r.error()).c_str());
        return 1;
    }
    const bench::BenchConfig& bench_cfg = *cfg_r;
    const bench::Scenario* sc = bench_cfg.scenario("lat_ws");
    if (sc == nullptr) {
        std::fprintf(stderr, "lat_ws: [scenarios.lat_ws] not found in %s\n",
                     conf_path);
        return 1;
    }
    const bench::Scenario& scenario = *sc;

    bench::pin_client_from_cfg(bench_cfg, "lat_ws");

    auto port_r = scenario.get<uint16_t>("port");
    if (!port_r) {
        std::fprintf(stderr, "lat_ws: %s\n",
                     bench::format_error(port_r.error()).c_str());
        return 1;
    }
    const uint16_t port = *port_r;

    const std::string ws_path =
        scenario.get_or<std::string>("ws_path", "/echo");

    const std::size_t payload_size =
        scenario.get_or<uint32_t>("payload_size", 64);

    if (payload_size < bench::kTimestampBlockSize) {
        std::fprintf(stderr,
                     "lat_ws: payload_size=%zu < kTimestampBlockSize=%zu "
                     "(TX/RX leg protocol requires a 24 B header)\n",
                     payload_size, bench::kTimestampBlockSize);
        return 1;
    }

    const uint64_t duration_s =
        scenario.get_or<uint32_t>("duration_seconds", 10);
    const uint64_t warmup_samples = bench_cfg.measurement.warmup_samples;
    const bool use_tls = scenario.get_or<bool>("use_tls", false);

    const std::string mock_ip_str = bench_cfg.networking.server_ip.empty()
                                        ? std::string{"127.0.0.1"}
                                        : bench_cfg.networking.server_ip;
    auto ip_r = en::Ipv4Addr::parse(mock_ip_str);
    if (!ip_r) {
        std::fprintf(stderr, "lat_ws: invalid mock_ip '%s': %s\n",
                     mock_ip_str.c_str(), ip_r.error().detail);
        return 1;
    }
    const en::SocketAddr remote{ip_r.value(), port};

    std::printf("=== lat_ws ===\n");
    std::printf("config: mock=%s port=%u ws_path=%s payload_size=%zu "
                "duration=%llus warmup_samples=%llu tls=%s\n",
                mock_ip_str.c_str(),
                static_cast<unsigned>(port),
                ws_path.c_str(),
                payload_size,
                static_cast<unsigned long long>(duration_s),
                static_cast<unsigned long long>(warmup_samples),
                use_tls ? "yes" : "no");
    std::fflush(stdout);

    bench::install_signal_handler();

    if (use_tls) {
        return run_ws<true>(bench_cfg, remote, mock_ip_str, port, ws_path,
                            payload_size, duration_s, warmup_samples);
    }
    return run_ws<false>(bench_cfg, remote, mock_ip_str, port, ws_path,
                         payload_size, duration_s, warmup_samples);
}
