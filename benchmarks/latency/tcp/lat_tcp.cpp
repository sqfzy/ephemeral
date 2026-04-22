/// @file lat_tcp.cpp
/// Latency benchmark: raw TCP RTT against the `tcp` scenario served by
/// the unified C++ mock `benchmarks/mockex/mockex`.
///
///   * Single-file scenario binary that reads `[lat_tcp]` from bench.conf
///     (port / payload_size / duration_seconds) plus the lowercase global
///     `mock_ip`, `warmup_samples`.
///   * Uses `KernelTcpStream<RawStreamCodec, false>` + `KernelPoller`
///     API directly — no raw socket() calls.
///   * Measurement clock is `bench::monotonic_raw_ns()` (CLOCK_MONOTONIC_RAW
///     via vDSO) — not TSC.
///   * Samples feed `eph::utils::Recorder::record_ns(ns)`.
///
/// The binary does NOT manage mocks or NICs — the `lat` wrapper script forks
/// `mockex --scenario tcp` and drives the NIC state transition before
/// exec'ing this binary.
///
/// A second target `lat_tcp_dpdk` is produced by the xmake auto-glob loop with
/// `EPH_USE_DPDK=1`.

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
#include "eph/codec/raw_stream_codec.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/utils/recorder.hpp"

#if defined(EPH_USE_DPDK)
// Real DPDK measurement loop via DpdkTcpStream + DpdkPoller over NIC_B. The
// structure below (#if EPH_USE_DPDK branch) mirrors the kernel branch 1-to-1
// — only the stream/poller types differ.
#  include "eph/net/dpdk/poller.hpp"
#  include "eph/net/dpdk/tcp_stream.hpp"
#else
#  include "eph/net/kernel/config.hpp"
#  include "eph/net/kernel/poller.hpp"
#  include "eph/net/kernel/tcp_stream.hpp"
#endif

// benchmarks/latency/core helpers. xmake includes
// `benchmarks/latency/` as an include dir so the header lives at
// `core/config.hpp` from the compiler's perspective.
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
using Stream = ed::DpdkTcpStream<ec::RawStreamCodec, /*EnableTls=*/false>;
using Poller = ed::DpdkPoller<>;
#else
namespace ek = eph::net::kernel;
using Stream = ek::KernelTcpStream<ec::RawStreamCodec, /*EnableTls=*/false>;
using Poller = ek::KernelPoller;
#endif

/// Default bench.conf path if no `--config <path>` is passed. The `lat`
/// wrapper script always passes `--config` via BENCH_CONFIG, but running the
/// binary directly for loopback smoke tests needs a reasonable default.
constexpr const char* kDefaultConfigPath = "benchmarks/latency/bench.conf";

/// Walk argv looking for `--config <path>`. Unrecognised flags are ignored —
/// CommonConfig-style parsing is not needed for this scenario binary.
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

    // ── Load config.toml into structured BenchConfig.
    auto cfg_r = bench::load_bench_conf(conf_path);
    if (!cfg_r) {
        std::fprintf(stderr, "lat_tcp: %s\n",
                     bench::format_error(cfg_r.error()).c_str());
        return 1;
    }
    const bench::BenchConfig& bench_cfg = *cfg_r;
    const bench::Scenario* tcp = bench_cfg.scenario("lat_tcp");
    if (tcp == nullptr) {
        std::fprintf(stderr, "lat_tcp: [scenarios.lat_tcp] not found in %s\n",
                     conf_path);
        return 1;
    }
    const bench::Scenario& scenario = *tcp;

    bench::pin_client_from_cfg(bench_cfg, "lat_tcp");

    // Required: port.
    auto port_r = scenario.get<uint16_t>("port");
    if (!port_r) {
        std::fprintf(stderr, "lat_tcp: %s\n",
                     bench::format_error(port_r.error()).c_str());
        return 1;
    }
    const uint16_t port = *port_r;

    // Optional with defaults.
    const std::size_t payload_size =
        scenario.get_or<uint32_t>("payload_size", 256);

    // Every raw-payload RTT scenario prepends a 24 B timestamp block,
    // so the bench operator must configure payload_size
    // ≥ 24. Fail fast if not; the user has misconfigured config.toml.
    if (payload_size < bench::kTimestampBlockSize) {
        std::fprintf(stderr,
                     "lat_tcp: payload_size=%zu < kTimestampBlockSize=%zu "
                     "(TX/RX leg protocol requires a 24 B header)\n",
                     payload_size, bench::kTimestampBlockSize);
        return 1;
    }

    const uint64_t duration_s =
        scenario.get_or<uint32_t>("duration_seconds", 10);
    const uint64_t warmup_samples =
        bench_cfg.measurement.warmup_samples;

    // Mock IP: server_ip from [networking]; falls back to 127.0.0.1 if empty.
    const std::string mock_ip_str = bench_cfg.networking.server_ip.empty()
                                        ? std::string{"127.0.0.1"}
                                        : bench_cfg.networking.server_ip;
    auto ip_r = en::Ipv4Addr::parse(mock_ip_str);
    if (!ip_r) {
        std::fprintf(stderr, "lat_tcp: invalid mock_ip '%s': %s\n",
                     mock_ip_str.c_str(), ip_r.error().detail);
        return 1;
    }
    const en::SocketAddr remote{ip_r.value(), port};

    std::printf("=== lat_tcp ===\n");
    std::printf("config: mock=%s port=%u payload_size=%zu duration=%llus "
                "warmup_samples=%llu\n",
                mock_ip_str.c_str(),
                static_cast<unsigned>(port),
                payload_size,
                static_cast<unsigned long long>(duration_s),
                static_cast<unsigned long long>(warmup_samples));
    std::fflush(stdout);

    // ── Signal handler: SIGINT/SIGTERM flip `bench::shutdown_requested()`.
    bench::install_signal_handler();

#if defined(EPH_USE_DPDK)
    // ── DPDK bootstrap: EAL + Platform + ARP resolve via shared helper.
    auto env_r = bench::load_dpdk_env(bench_cfg, /*port_id=*/0);
    if (!env_r) {
        std::fprintf(stderr, "lat_tcp: %s\n", env_r.error().c_str());
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
        std::fprintf(stderr, "lat_tcp: Poller::create failed: %s\n",
                     poller_r.error().detail);
        return 2;
    }
    auto poller = std::move(poller_r.value());

#if defined(EPH_USE_DPDK)
    ed::StreamConfig cfg{};
    cfg.legacy          = env.make_tcp_config(bench::random_src_port(), port);
    cfg.pool            = env.pool;
    cfg.connect_timeout = std::chrono::milliseconds{3000};

    // Register Poller with Platform so Stream::create_and_attach can find
    // it via poller_for_queue.  Single-stream lat scenario → single
    // Poller on queue 0; the post-PR-2.5 RETA-collapse fix in
    // Platform::create guarantees all RX traffic lands on queue 0 when
    // dispatch_mode is pinned to Software (i.e. nb_rx_queues>1 but RSS
    // not actively configured).  Default config (nb_rx_queues=1) is
    // unaffected.
    if (auto rr = env.platform.register_poller(0, poller.get()); !rr) {
        std::fprintf(stderr, "lat_tcp: register_poller failed: %s\n",
                     rr.error().detail);
        return 3;
    }
    auto stream_r = Stream::create_and_attach(std::move(cfg), env.platform);
#else
    ek::StreamConfig cfg{};
    cfg.remote          = remote;
    cfg.reasm_capacity  = std::max<std::size_t>(64 * 1024, payload_size * 4);
    cfg.connect_timeout = std::chrono::milliseconds{3000};

    auto stream_r = Stream::create(cfg);
#endif

    if (!stream_r) {
        std::fprintf(stderr, "lat_tcp: Stream::create failed: %s\n",
                     stream_r.error().detail);
        return 3;
    }
    auto stream = std::move(stream_r.value());

    // Latest RX buffer: on_message stamps it, the poll loop drains it.
    // We use byte_count to tolerate partial RX delivery — with
    // RawStreamCodec the mock's `sendall` may split the echo across
    // multiple recv() calls, so we wait until we've seen `payload_size`
    // bytes in total (which equals one round-trip since the client sends
    // exactly one payload before waiting).
    //
    // Additionally we need the first 24 B of the echoed payload (the
    // timestamp block the mock rewrote in place). Since
    // TCP may split across multiple on_message calls, we copy into a
    // fixed 24 B buffer as bytes arrive until filled.
    std::size_t rx_bytes = 0;
    std::array<uint8_t, bench::kTimestampBlockSize> ts_buf{};
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
    // Kernel backend: explicit attach (no turnkey factory).  DPDK path
    // already attached inside Stream::create_and_attach above.
    if (auto r = poller->add(stream.get()); !r) {
        std::fprintf(stderr, "lat_tcp: poller->add failed: %s\n",
                     r.error().detail);
        return 3;
    }
#endif

    // ── Measurement loop ─────────────────────────────────────────────────
    // Literally identical between kernel and DPDK branches — only the
    // `Stream` / `Poller` typedefs differ above.
    //
    // Three Recorders (RTT / TX / RX) fed from the 24 B timestamp-block
    // protocol. `rec_rtt` is what the fairness gate compares across
    // backends; TX/RX break down the wire legs so the operator can see
    // where DPDK's win comes from.
    std::vector<uint8_t> payload(payload_size, 0xAB);
    const char* backend =
#if defined(EPH_USE_DPDK)
        "dpdk";
#else
        "kernel";
#endif
    eu::Recorder rec_rtt{std::string{"lat_tcp_"} + backend + "_rtt"};
    eu::Recorder rec_tx {std::string{"lat_tcp_"} + backend + "_tx" };
    eu::Recorder rec_rx {std::string{"lat_tcp_"} + backend + "_rx" };

    const uint64_t t_start    = bench::monotonic_raw_ns();
    const uint64_t t_deadline = t_start + duration_s * 1'000'000'000ull;
    uint64_t       t_measure_start = 0;  // set once the warmup window closes

    // Safety: if the echo mock dies we must not spin forever.
    constexpr uint64_t kPerSampleTimeoutNs = 5ull * 1'000'000'000ull;

    uint64_t sample_idx = 0;
    bool     timed_out  = false;
    while (bench::monotonic_raw_ns() < t_deadline && !bench::shutdown_requested()) {
        const uint64_t t0 = bench::monotonic_raw_ns();
        // Stamp the TS block in the payload head before send. The mock
        // will overwrite bytes [8:24] with its recv/send timestamps
        // while leaving [24:] (the 0xAB filler) untouched.
        bench::write_client_ts(
            std::span<uint8_t>{payload.data(), bench::kTimestampBlockSize},
            t0);

        auto send_r = stream->send(std::span<const uint8_t>{payload});
        if (!send_r) {
            std::fprintf(stderr, "lat_tcp: send failed at sample %llu: %s\n",
                         static_cast<unsigned long long>(sample_idx),
                         send_r.error().detail);
            break;
        }

        rx_bytes  = 0;
        ts_filled = 0;
        while (rx_bytes < payload_size && !bench::shutdown_requested()) {
            poller->poll();
            if (bench::monotonic_raw_ns() - t0 > kPerSampleTimeoutNs) {
                std::fprintf(stderr,
                             "lat_tcp: echo timeout at sample %llu "
                             "(rx=%zu/%zu)\n",
                             static_cast<unsigned long long>(sample_idx),
                             rx_bytes, payload_size);
                timed_out = true;
                break;
            }
        }
        if (timed_out || bench::shutdown_requested()) break;

        const uint64_t t1 = bench::monotonic_raw_ns();
        if (sample_idx == warmup_samples) {
            // First post-warmup sample — stamp the measurement-window
            // start so the throughput line in the final report reflects
            // the effective measured window, not warmup bleed.
            t_measure_start = t0;
        }
        if (sample_idx >= warmup_samples) {
            // Decode mock timestamps and compute the three legs. The
            // TS buffer is guaranteed fully filled at this point (the
            // inner poll loop won't exit until rx_bytes ≥ payload_size,
            // and payload_size ≥ kTimestampBlockSize is enforced above).
            const auto ts  = bench::read_ts(
                std::span<const uint8_t>{ts_buf.data(), ts_buf.size()});
            const auto lgs = bench::compute_legs(ts, t1);
            rec_rtt.record_ns(lgs.rtt_ns);
            rec_tx .record_ns(lgs.tx_ns);
            rec_rx .record_ns(lgs.rx_ns);
        }
        ++sample_idx;
    }

    // ── Report ───────────────────────────────────────────────────────────
    const uint64_t wall_time_ns =
        (t_measure_start != 0)
            ? (bench::monotonic_raw_ns() - t_measure_start)
            : 0;
    bench::print_leg_report("lat_tcp", backend, rec_rtt, rec_tx, rec_rx,
                            warmup_samples, wall_time_ns);
    (void)bench::export_legs(rec_rtt, rec_tx, rec_rx);

    // ── Graceful close. Ignore errors: the report is what the user cares
    //    about, and the mock will notice the FIN regardless.
    (void)stream->close_gracefully();
#if !defined(EPH_USE_DPDK)
    poller->poll(std::chrono::milliseconds{50});
#endif
    (void)poller->remove(stream.get());
    stream.reset();
    poller.reset();

    return timed_out ? 4 : 0;
}
