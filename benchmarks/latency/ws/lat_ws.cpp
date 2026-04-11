/// @file lat_ws.cpp
/// Phase 10 latency benchmark: WebSocket RTT against a kernel Python echo
/// mock (`benchmarks/latency/mocks/ws_echo.py`).
///
/// Sub-phase 10.4 rewrite. Per
/// `.artifacts/plan-phase-10-latency-bench-20260411-040540.md` §Sub-phase
/// 10.4 and §Interface design → "Client API pattern":
///
///   * Single-file scenario binary that reads `[lat_ws]` from bench.conf
///     (port / ws_path / payload_size / duration_seconds) plus the
///     lowercase global `mock_ip`, `warmup_samples`.
///   * Uses the v3.3 `KernelTcpStream<WsCodec, false>` + `KernelPoller`
///     API directly — no legacy eph-transport, no bespoke loopback echoer,
///     no raw socket() calls.
///   * `StreamConfig.ws_path` is set — this triggers the Phase 9.5
///     transparent WebSocket handshake inside `KernelTcpStream::create()`,
///     so by the time we start the measurement loop the socket is ready
///     to exchange WS-binary frames with the Python mock.
///   * Measurement clock is `bench::monotonic_raw_ns()`
///     (CLOCK_MONOTONIC_RAW via vDSO, per plan D-6) — not TSC.
///   * Samples feed `eph::utils::Recorder::record_ns(ns)` (plan D-2).
///
/// Design note — WS frame encoding: `KernelTcpStream::send(bytes)` does
/// NOT run the bytes through the codec (the codec's encode path is for
/// user-assembled payloads), so we pre-encode a WS-binary frame once via
/// `WsCodec::encode` and reuse that buffer for every RTT sample. Decode
/// on the RX side IS driven through the codec (by the poller), so the
/// `on_message` callback fires once per decoded payload.
///
/// A second target `lat_ws_dpdk` is produced by the xmake auto-glob loop
/// with `EPH_USE_DPDK=1`. For now that build merely type-checks the v3.3
/// DPDK stream API surface and prints a deferred banner — the real DPDK
/// measurement loop is Phase-<later> work (see lat_tcp.cpp for the same
/// pattern).

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

// eph-* headers (plan D-4: v3.3 API only).
#include "eph/codec/ws_codec.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/utils/recorder.hpp"

#if defined(EPH_USE_DPDK)
// Phase 11.0: DpdkTcpStream<WsCodec> real measurement loop. Structure
// mirrors lat_tcp exactly with `WsCodec` + `cfg.ws_path` set so the
// DpdkTcpStream::create path performs the RFC 6455 handshake during setup.
#  include "eph/net/dpdk/poller.hpp"
#  include "eph/net/dpdk/tcp_stream.hpp"
#else
#  include "eph/net/kernel/config.hpp"
#  include "eph/net/kernel/poller.hpp"
#  include "eph/net/kernel/tcp_stream.hpp"
#endif

#include "core/config.hpp"
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

/// Default bench.conf path if no `--config <path>` is passed. The `lat`
/// wrapper always passes `--config` via argv; this default only applies
/// when the binary is invoked directly (loopback smoke tests).
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

    // ── Load config: globals (mock_ip, warmup_samples) + [lat_ws] section.
    auto globals_r = bench::ScenarioConfig::load_globals(conf_path);
    if (!globals_r) {
        std::fprintf(stderr, "lat_ws: %s\n", globals_r.error().c_str());
        return 1;
    }
    auto scenario_r = bench::ScenarioConfig::load(conf_path, "lat_ws");
    if (!scenario_r) {
        std::fprintf(stderr, "lat_ws: %s\n", scenario_r.error().c_str());
        return 1;
    }
    const auto& globals  = globals_r.value();
    const auto& scenario = scenario_r.value();

    // Required: port (no sensible default).
    auto port_r = scenario.get_u32("port");
    if (!port_r) {
        std::fprintf(stderr, "lat_ws: %s\n", port_r.error().c_str());
        return 1;
    }
    const uint16_t port = static_cast<uint16_t>(port_r.value());

    // Optional with defaults.
    const std::string ws_path = scenario.get_string("ws_path", "/echo");

    auto payload_r = scenario.get_u32("payload_size", 64);
    if (!payload_r) {
        std::fprintf(stderr, "lat_ws: %s\n", payload_r.error().c_str());
        return 1;
    }
    const std::size_t payload_size = payload_r.value();

    // Phase 11.1 D-7: 24 B timestamp-block header requirement.
    if (payload_size < bench::kTimestampBlockSize) {
        std::fprintf(stderr,
                     "lat_ws: payload_size=%zu < kTimestampBlockSize=%zu "
                     "(TX/RX leg protocol requires a 24 B header)\n",
                     payload_size, bench::kTimestampBlockSize);
        return 1;
    }

    auto duration_r = scenario.get_u32("duration_seconds", 10);
    if (!duration_r) {
        std::fprintf(stderr, "lat_ws: %s\n", duration_r.error().c_str());
        return 1;
    }
    const uint64_t duration_s = duration_r.value();

    auto warmup_r = globals.get_u64("warmup_samples", 1000);
    if (!warmup_r) {
        std::fprintf(stderr, "lat_ws: %s\n", warmup_r.error().c_str());
        return 1;
    }
    const uint64_t warmup_samples = warmup_r.value();

    const std::string mock_ip_str = globals.get_string("mock_ip", "127.0.0.1");
    auto ip_r = en::Ipv4Addr::parse(mock_ip_str);
    if (!ip_r) {
        std::fprintf(stderr, "lat_ws: invalid mock_ip '%s': %s\n",
                     mock_ip_str.c_str(), ip_r.error().detail);
        return 1;
    }
    const en::SocketAddr remote{ip_r.value(), port};

    std::printf("=== lat_ws ===\n");
    std::printf("config: mock=%s port=%u ws_path=%s payload_size=%zu "
                "duration=%llus warmup_samples=%llu\n",
                mock_ip_str.c_str(),
                static_cast<unsigned>(port),
                ws_path.c_str(),
                payload_size,
                static_cast<unsigned long long>(duration_s),
                static_cast<unsigned long long>(warmup_samples));
    std::fflush(stdout);

    bench::install_signal_handler();

    // Construct the Recorders early so their TSC::init() calibration
    // spins (~1 second on process start) happen before we hit the WS
    // handshake. For an RTT scenario this is less critical than for
    // one-way lat_ex_market, but it keeps the two binaries symmetric.
    const char* backend =
#if defined(EPH_USE_DPDK)
        "dpdk";
#else
        "kernel";
#endif
    eu::Recorder rec_rtt{std::string{"lat_ws_"} + backend + "_rtt"};
    eu::Recorder rec_tx {std::string{"lat_ws_"} + backend + "_tx" };
    eu::Recorder rec_rx {std::string{"lat_ws_"} + backend + "_rx" };

#if defined(EPH_USE_DPDK)
    auto env_r = bench::load_dpdk_env(globals, /*port_id=*/0);
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
    // StreamConfig.ws_path triggers the Phase 9.5 transparent WebSocket
    // handshake inside KernelTcpStream::create(). The returned stream is
    // already past the 101 Switching Protocols response and ready to
    // exchange WS frames.
    ek::StreamConfig cfg{};
    cfg.remote          = remote;
    cfg.reasm_capacity  = std::max<std::size_t>(64 * 1024, payload_size * 4);
    cfg.connect_timeout = std::chrono::milliseconds{3000};
    cfg.ws_path         = ws_path;
    // ws_host left empty → handshake falls back to remote.to_string().
    cfg.ws_timeout      = std::chrono::seconds{10};
#endif

    auto stream_r = Stream::create(cfg);
    if (!stream_r) {
        std::fprintf(stderr, "lat_ws: Stream::create failed: %s\n",
                     stream_r.error().detail);
        return 3;
    }
    auto stream = std::move(stream_r.value());

    // One decoded WS-binary frame per on_message call. We track a simple
    // "got a frame" flag: each RTT sample sends exactly one frame and
    // waits for exactly one response. Using a frame counter (vs a byte
    // counter like lat_tcp) is correct here because the codec delivers
    // the reassembled message in a single callback regardless of how
    // many TCP segments carried it.
    //
    // Phase 11.1: copy the first 24 B of the decoded payload into
    // `ts_buf` for leg decomposition. `on_message` gets plaintext
    // (post-unmask) from the WsCodec, so this is just a memcpy.
    bool got_echo = false;
    std::array<uint8_t, bench::kTimestampBlockSize> ts_buf{};
    std::size_t ts_filled = 0;
    stream->on_message = [&](const uint8_t* data, uint16_t n) {
        got_echo = true;
        ts_filled = 0;
        if (n >= bench::kTimestampBlockSize) {
            std::memcpy(ts_buf.data(), data, bench::kTimestampBlockSize);
            ts_filled = bench::kTimestampBlockSize;
        }
    };

    if (auto r = poller->add(stream.get()); !r) {
        std::fprintf(stderr, "lat_ws: poller->add failed: %s\n",
                     r.error().detail);
        return 3;
    }

    // Phase 11.1: we must re-encode a new WS frame for every sample
    // because the client MUST mask each frame with a fresh (or at
    // least per-frame) masking key per RFC 6455 §5.3, and the masking
    // interleaves with the timestamp bytes we overwrite each sample.
    // Re-encoding is cheap (a small memcpy + XOR over payload_size B)
    // and keeps the protocol compliant.
    std::vector<uint8_t> payload(payload_size, 0xAB);
    std::vector<uint8_t> frame(ec::WsCodec::max_overhead + payload_size);
    ec::WsCodec encoder{};

    const uint64_t t_start    = bench::monotonic_raw_ns();
    const uint64_t t_deadline = t_start + duration_s * 1'000'000'000ull;
    uint64_t       t_measure_start = 0;
    // Per-sample timeout: if the mock dies we bail rather than spin.
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
