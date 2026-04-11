/// @file lat_tcp.cpp
/// Phase 10 latency benchmark: raw TCP RTT against a kernel Python echo mock.
///
/// Sub-phase 10.3 rewrite. Per plan-phase-10-latency-bench-20260411-040540.md
/// §Interface design → Client API pattern:
///
///   * Single-file scenario binary that reads `[lat_tcp]` from bench.conf
///     (port / payload_size / duration_seconds) plus the lowercase global
///     `mock_ip`, `warmup_samples`.
///   * Uses the v3.3 `KernelTcpStream<RawStreamCodec, false>` + `KernelPoller`
///     API directly — no raw socket() calls, no legacy eph-transport.
///   * Measurement clock is `bench::monotonic_raw_ns()` (CLOCK_MONOTONIC_RAW
///     via vDSO, per plan D-6) — not TSC.
///   * Samples feed `eph::utils::Recorder::record_ns(ns)` (plan D-2).
///
/// The binary does NOT manage mocks or NICs — the `lat` wrapper script forks
/// the Python echo mock (`benchmarks/latency/mocks/tcp_echo.py`) and the NIC
/// state transition before exec'ing this binary.
///
/// A second target `lat_tcp_dpdk` is produced by the xmake auto-glob loop with
/// `EPH_USE_DPDK=1`. For now that build falls back to a kernel-identical code
/// path because the v3.3 DPDK Stream API surface is still Phase 6 scaffolding
/// (vcpkg-openssl / aws-lc TU clash, see Phase 5 notes). The `_dpdk` target
/// therefore currently links the kernel stream as well so the build stays
/// green — this matches the behaviour of the pre-10.3 demonstrator.

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
#include "eph/codec/raw_stream_codec.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/utils/recorder.hpp"

#if defined(EPH_USE_DPDK)
// Phase 11.0: real DPDK measurement loop via DpdkTcpStream + DpdkPoller over
// NIC_B. The structure below (#if EPH_USE_DPDK branch) mirrors the kernel
// branch 1-to-1 — only the stream/poller types differ. Fairness contract:
// see memory/feedback_bench_no_loopback.md.
#  include "eph/net/dpdk/poller.hpp"
#  include "eph/net/dpdk/tcp_stream.hpp"
#else
#  include "eph/net/kernel/config.hpp"
#  include "eph/net/kernel/poller.hpp"
#  include "eph/net/kernel/tcp_stream.hpp"
#endif

// benchmarks/latency/core helpers (added in sub-phase 10.1). xmake includes
// `benchmarks/latency/` as an include dir so the header lives at
// `core/config.hpp` from the compiler's perspective.
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

    // ── Load config: globals (mock_ip, warmup_samples) + [lat_tcp] section.
    auto globals_r = bench::ScenarioConfig::load_globals(conf_path);
    if (!globals_r) {
        std::fprintf(stderr, "lat_tcp: %s\n", globals_r.error().c_str());
        return 1;
    }
    auto scenario_r = bench::ScenarioConfig::load(conf_path, "lat_tcp");
    if (!scenario_r) {
        std::fprintf(stderr, "lat_tcp: %s\n", scenario_r.error().c_str());
        return 1;
    }
    const auto& globals  = globals_r.value();
    const auto& scenario = scenario_r.value();

    // Required: port (no sensible default).
    auto port_r = scenario.get_u32("port");
    if (!port_r) {
        std::fprintf(stderr, "lat_tcp: %s\n", port_r.error().c_str());
        return 1;
    }
    const uint16_t port = static_cast<uint16_t>(port_r.value());

    // Optional with defaults.
    auto payload_r = scenario.get_u32("payload_size", 256);
    if (!payload_r) {
        std::fprintf(stderr, "lat_tcp: %s\n", payload_r.error().c_str());
        return 1;
    }
    const std::size_t payload_size = payload_r.value();

    // Phase 11.1 D-7: every raw-payload RTT scenario prepends a 24 B
    // timestamp block, so the bench operator must configure payload_size
    // ≥ 24. Fail fast if not; the user has misconfigured bench.conf.
    if (payload_size < bench::kTimestampBlockSize) {
        std::fprintf(stderr,
                     "lat_tcp: payload_size=%zu < kTimestampBlockSize=%zu "
                     "(TX/RX leg protocol requires a 24 B header)\n",
                     payload_size, bench::kTimestampBlockSize);
        return 1;
    }

    auto duration_r = scenario.get_u32("duration_seconds", 10);
    if (!duration_r) {
        std::fprintf(stderr, "lat_tcp: %s\n", duration_r.error().c_str());
        return 1;
    }
    const uint64_t duration_s = duration_r.value();

    auto warmup_r = globals.get_u64("warmup_samples", 1000);
    if (!warmup_r) {
        std::fprintf(stderr, "lat_tcp: %s\n", warmup_r.error().c_str());
        return 1;
    }
    const uint64_t warmup_samples = warmup_r.value();

    // Mock IP: global `mock_ip` key, defaults to loopback so the smoke test
    // from a trivial `/tmp/bench-smoke.conf` without mock_ip still works.
    const std::string mock_ip_str = globals.get_string("mock_ip", "127.0.0.1");
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
    auto env_r = bench::load_dpdk_env(globals, /*port_id=*/0);
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
#else
    ek::StreamConfig cfg{};
    cfg.remote          = remote;
    cfg.reasm_capacity  = std::max<std::size_t>(64 * 1024, payload_size * 4);
    cfg.connect_timeout = std::chrono::milliseconds{3000};
#endif

    auto stream_r = Stream::create(cfg);
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
    // Phase 11.1: additionally we need the first 24 B of the echoed
    // payload (the timestamp block the mock rewrote in place). Since
    // TCP may split across multiple on_message calls, we copy into a
    // fixed 24 B buffer as bytes arrive until filled.
    std::size_t rx_bytes = 0;
    std::array<uint8_t, bench::kTimestampBlockSize> ts_buf{};
    std::size_t ts_filled = 0;
    stream->on_message = [&](const uint8_t* data, uint16_t n) {
        if (ts_filled < ts_buf.size()) {
            const std::size_t want = ts_buf.size() - ts_filled;
            const std::size_t copy = (n < want) ? n : want;
            std::memcpy(ts_buf.data() + ts_filled, data, copy);
            ts_filled += copy;
        }
        rx_bytes += n;
    };

    if (auto r = poller->add(stream.get()); !r) {
        std::fprintf(stderr, "lat_tcp: poller->add failed: %s\n",
                     r.error().detail);
        return 3;
    }

    // ── Measurement loop ─────────────────────────────────────────────────
    // Literally identical between kernel and DPDK branches (per plan
    // §实施计划 4). Only the `Stream` / `Poller` typedefs differ above.
    //
    // Phase 11.1: three Recorders (RTT / TX / RX) fed from the 24 B
    // timestamp-block protocol. `rec_rtt` is what the fairness gate
    // compares across backends; TX/RX break down the wire legs so the
    // operator can see where DPDK's win comes from.
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
    //    about, and the Python mock will notice the FIN regardless.
    (void)stream->close_gracefully();
#if !defined(EPH_USE_DPDK)
    poller->poll(std::chrono::milliseconds{50});
#endif
    (void)poller->remove(stream.get());
    stream.reset();
    poller.reset();

    return timed_out ? 4 : 0;
}
