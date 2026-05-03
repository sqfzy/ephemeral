/// @file core/measurement.hpp
/// Header-only bench measurement helpers.
///
/// Provides:
///   - `monotonic_raw_ns()`     — single-instruction timestamp helper,
///                                shared between C++ client and Python mock
///   - `install_signal_handler()` / `shutdown_requested()` — cooperative
///                                graceful shutdown flag for the measurement
///                                loop
///   - `print_report()`         — human-readable Stats summary compatible
///                                with `eph::utils::Recorder::compute_stats()`
///
/// Rationale: bench client uses `clock_gettime(CLOCK_MONOTONIC_RAW)`
/// instead of `eph::utils::TSC` because (1) Python mocks call the same clock
/// via ctypes, giving one-way scenarios a shared time base, (2) on invariant-
/// TSC x86_64 Linux the vDSO path uses `rdtsc` internally so cost is within
/// 10 ns of direct `rdtsc`, and (3) ns output avoids cycle-to-ns calibration
/// entirely.
///
/// Rationale: Recorder now accepts raw ns via `record_ns()` —
/// scenarios call `rec.record_ns(t1 - t0)` directly with no conversion wrapper.
///
/// Rationale: header-only because all three pieces are sub-1-KLOC and need
/// to be `inline` for zero-overhead (monotonic_raw_ns is on the hot path).

#pragma once

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

#include <eph/utils/recorder.hpp>

namespace bench {

/// Read `CLOCK_MONOTONIC_RAW` and return the time since an unspecified
/// epoch in nanoseconds. Implemented via `clock_gettime` which goes
/// through the vDSO on Linux — no syscall, ~20 ns on Intel/AMD with
/// invariant TSC.
///
/// CLOCK_MONOTONIC_RAW is chosen over CLOCK_MONOTONIC because it is not
/// slewed by NTP/PTP, which matters when bench runs span minutes and we
/// do not want the measurement clock to accelerate/decelerate underneath
/// us. On the bench hosts there is no live NTP during measurement anyway
/// but the distinction keeps the contract robust.
///
/// Must be `noexcept` because it is called from the measurement inner
/// loop where exceptions are forbidden.
[[nodiscard]] inline uint64_t monotonic_raw_ns() noexcept {
    struct timespec ts;
    // CLOCK_MONOTONIC_RAW never returns EINVAL on a kernel ≥ 2.6.28;
    // silencing the return value is safe here and keeps the hot path
    // branchless. If the syscall ever does fail we return 0 which makes
    // the measurement obviously wrong rather than corrupting it.
    if (::clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<uint64_t>(ts.tv_nsec);
}

/// Cooperative shutdown flag set by SIGINT/SIGTERM. Scenarios poll
/// `shutdown_requested()` in their measurement loop so Ctrl-C causes a
/// graceful finish (including report print) rather than a hard kill.
///
/// The flag lives in a translation-unit-scope inline variable (C++17)
/// so every TU that includes this header shares the same instance.
inline std::atomic<bool> g_shutdown_requested{false};

[[nodiscard]] inline bool shutdown_requested() noexcept {
    return g_shutdown_requested.load(std::memory_order_acquire);
}

/// Internal signal handler — set the flag and return. Async-signal-safe
/// because `std::atomic<bool>::store` on `lock_free` bool is guaranteed
/// async-signal-safe in practice on every modern ABI we target.
inline void bench_signal_handler(int /*signum*/) noexcept {
    g_shutdown_requested.store(true, std::memory_order_release);
}

/// Install SIGINT/SIGTERM handlers that set `g_shutdown_requested`.
/// Idempotent — calling twice is harmless; the handler is stateless.
///
/// Failure mode: matches `eph::utils::install_shutdown_handlers` —
/// `std::signal` returning `SIG_ERR` means the handler did not
/// install and the default disposition (TERMINATE) remains in
/// effect, so the first SIGINT/SIGTERM kills the bench binary
/// abruptly without ever flushing the Recorder. WARN-log the
/// failure rather than silently leaving the bench operator to
/// guess why "Ctrl-C lost my measurements".
inline void install_signal_handler() noexcept {
    if (std::signal(SIGINT, &bench_signal_handler) == SIG_ERR) {
        SPDLOG_WARN("install_signal_handler: std::signal(SIGINT) failed — "
                    "first Ctrl-C will TERMINATE without flushing recorder");
    }
    if (std::signal(SIGTERM, &bench_signal_handler) == SIG_ERR) {
        SPDLOG_WARN("install_signal_handler: std::signal(SIGTERM) failed — "
                    "first SIGTERM will TERMINATE without flushing recorder");
    }
}

/// Print a human-readable bench report to stdout.
///
/// Format (ASCII, stable):
/// ```
/// === <scenario> (<backend>) ===
/// samples: <N> (warmup <W> discarded)
/// latency_ns:
///   min    = <min>
///   p50    = <p50>
///   p90    = <p90>
///   p99    = <p99>
///   p99.9  = <p999>
///   max    = <max>
///   avg    = <avg>
///   stddev = <stddev>
/// throughput: <N/wall_s> samples/s          ← only if wall_time_ns > 0
/// ```
///
/// The numbers come from `eph::utils::Recorder::compute_stats()` which
/// returns `std::optional<Stats>`; an empty sample set prints
/// `samples: 0` and leaves the latency block with `--` placeholders.
///
/// `backend` should be "kernel" or "dpdk" (the scenario binary picks it
/// at compile time via EPH_USE_DPDK). `warmup_discarded` is informational —
/// scenarios that discard the first N samples via `if (idx >= warmup)`
/// gating pass `warmup` here so the report shows the effective count.
///
/// `wall_time_ns`: when non-zero, used to
/// compute `throughput = s.count / wall_time_s` and print an extra
/// `throughput: <N> samples/s` line. Scenarios pass
/// `monotonic_raw_ns() - t_measure_start`, where `t_measure_start` is
/// stamped when the warmup window ends. Default 0 preserves backwards
/// compatibility for any test that still calls the old 4-arg form.
///
/// `print_report` intentionally does NOT format the scenario config.
/// Scenarios that want to print their config (`port=20000, payload=256,
/// duration=10s`) should `std::puts` it themselves before calling this —
/// keeps the helper signature stable across scenarios with different
/// config shapes.
/// Internal helper: print a single named stats block ("latency_ns",
/// "RTT_ns", "TX_ns", "RX_ns"). Shared between `print_report` (1-leg)
/// and `print_leg_report` (3-leg). Not `static` because the file is a
/// header and every scenario TU includes it — `inline` gives linker
/// dedup without UB.
///
/// `label` is the block header without the trailing colon, e.g. "RTT_ns"
/// prints as `RTT_ns:` followed by the indented min/p50/... rows.
inline void print_stats_block(std::string_view label,
                              const eph::utils::Stats& s) noexcept {
    std::printf("%.*s:\n",
                static_cast<int>(label.size()), label.data());
    std::printf("  min    = %llu\n",
                static_cast<unsigned long long>(s.min_ns));
    std::printf("  p50    = %llu\n",
                static_cast<unsigned long long>(s.p50_ns));
    std::printf("  p90    = %llu\n",
                static_cast<unsigned long long>(s.p90_ns));
    std::printf("  p99    = %llu\n",
                static_cast<unsigned long long>(s.p99_ns));
    std::printf("  p99.9  = %llu\n",
                static_cast<unsigned long long>(s.p999_ns));
    std::printf("  max    = %llu\n",
                static_cast<unsigned long long>(s.max_ns));
    std::printf("  avg    = %llu\n",
                static_cast<unsigned long long>(s.avg_ns));
    std::printf("  stddev = %llu\n",
                static_cast<unsigned long long>(s.stddev_ns));
}

/// Internal helper: print an empty-stats placeholder block used by both
/// `print_report` and `print_leg_report` when the Recorder is empty.
inline void print_stats_block_empty(std::string_view label) noexcept {
    std::printf("%.*s:\n",
                static_cast<int>(label.size()), label.data());
    std::printf("  min    = --\n");
    std::printf("  p50    = --\n");
    std::printf("  p90    = --\n");
    std::printf("  p99    = --\n");
    std::printf("  p99.9  = --\n");
    std::printf("  max    = --\n");
    std::printf("  avg    = --\n");
    std::printf("  stddev = --\n");
}

inline void print_report(std::string_view scenario_name,
                         std::string_view backend,
                         eph::utils::Recorder& rec,
                         uint64_t warmup_discarded = 0,
                         uint64_t wall_time_ns = 0) noexcept {
    std::printf("=== %.*s (%.*s) ===\n",
                static_cast<int>(scenario_name.size()), scenario_name.data(),
                static_cast<int>(backend.size()),       backend.data());

    auto stats_opt = rec.compute_stats();
    if (!stats_opt) {
        std::printf("samples: 0 (no data recorded)\n");
        print_stats_block_empty("latency_ns");
        std::fflush(stdout);
        return;
    }
    const auto& s = *stats_opt;

    std::printf("samples: %llu",
                static_cast<unsigned long long>(s.count));
    if (warmup_discarded > 0) {
        std::printf(" (warmup %llu discarded)",
                    static_cast<unsigned long long>(warmup_discarded));
    }
    std::printf("\n");

    print_stats_block("latency_ns", s);

    // Throughput: samples per wall-clock second. Only printed when the
    // scenario passes a non-zero measurement window — avoids divide-by-
    // zero and keeps the legacy 4-arg callers (unit tests etc.) clean.
    if (wall_time_ns > 0 && s.count > 0) {
        const double wall_s =
            static_cast<double>(wall_time_ns) / 1'000'000'000.0;
        const double thr = static_cast<double>(s.count) / wall_s;
        std::printf("throughput: %llu samples/s\n",
                    static_cast<unsigned long long>(thr));
    }
    std::fflush(stdout);
}

/// Print a 3-leg RTT / TX / RX report to stdout.
///
/// Format (ASCII, stable — verification gates grep for the three block
/// labels):
/// ```
/// === <scenario> (<backend>) ===
/// samples: <N> (warmup <W> discarded)
/// RTT_ns:
///   min    = ...
///   p50    = ...
///   ...
/// TX_ns:
///   ...
/// RX_ns:
///   ...
/// throughput: <N/wall_s> samples/s
/// ```
///
/// `rec_rtt` drives the sample-count and throughput lines. `rec_tx` /
/// `rec_rx` contribute only their stats blocks. If any of the three
/// Recorders is empty the corresponding block prints `--` placeholders
/// (same as the 1-leg `print_report` empty path).
inline void print_leg_report(std::string_view scenario_name,
                             std::string_view backend,
                             eph::utils::Recorder& rec_rtt,
                             eph::utils::Recorder& rec_tx,
                             eph::utils::Recorder& rec_rx,
                             uint64_t warmup_discarded = 0,
                             uint64_t wall_time_ns = 0) noexcept {
    std::printf("=== %.*s (%.*s) ===\n",
                static_cast<int>(scenario_name.size()), scenario_name.data(),
                static_cast<int>(backend.size()),       backend.data());

    auto rtt_opt = rec_rtt.compute_stats();
    auto tx_opt  = rec_tx.compute_stats();
    auto rx_opt  = rec_rx.compute_stats();

    // `samples:` line always reflects rec_rtt (one record per sample),
    // so gate 5 (TX+RX sanity) grepping `p50` under RTT_ns / TX_ns / RX_ns
    // sees a consistent sample count.
    if (rtt_opt) {
        std::printf("samples: %llu",
                    static_cast<unsigned long long>(rtt_opt->count));
        if (warmup_discarded > 0) {
            std::printf(" (warmup %llu discarded)",
                        static_cast<unsigned long long>(warmup_discarded));
        }
        std::printf("\n");
    } else {
        std::printf("samples: 0 (no data recorded)\n");
    }

    if (rtt_opt) print_stats_block("RTT_ns", *rtt_opt);
    else         print_stats_block_empty("RTT_ns");
    if (tx_opt)  print_stats_block("TX_ns",  *tx_opt);
    else         print_stats_block_empty("TX_ns");
    if (rx_opt)  print_stats_block("RX_ns",  *rx_opt);
    else         print_stats_block_empty("RX_ns");

    if (wall_time_ns > 0 && rtt_opt && rtt_opt->count > 0) {
        const double wall_s =
            static_cast<double>(wall_time_ns) / 1'000'000'000.0;
        const double thr = static_cast<double>(rtt_opt->count) / wall_s;
        std::printf("throughput: %llu samples/s\n",
                    static_cast<unsigned long long>(thr));
    }
    std::fflush(stdout);
}

/// Dump all three leg Recorders as JSON files under `output_dir`.
/// `Recorder::export_json` derives the filename from the Recorder's
/// `name` + a timestamp, so three different names (`*_rtt`, `*_tx`,
/// `*_rx`) produce three distinct files. Any individual failure emits
/// a stderr WARN but does not abort the caller — disk failures should
/// not pollute bench results (stdout is the primary output).
///
/// @return true iff all three `export_json` calls succeeded.
[[nodiscard]] inline bool
export_legs(eph::utils::Recorder& rec_rtt,
            eph::utils::Recorder& rec_tx,
            eph::utils::Recorder& rec_rx,
            const std::string& output_dir =
                "benchmarks/latency/outputs") noexcept {
    // Each call is independent — we run all three even if one fails so
    // the user gets as much data on disk as the FS will take.
    const bool ok_rtt = rec_rtt.export_json(output_dir);
    if (!ok_rtt) {
        std::fprintf(stderr,
                     "[WARN] export_legs: rec_rtt (%s) export_json to '%s' "
                     "failed\n",
                     rec_rtt.name().c_str(), output_dir.c_str());
    }
    const bool ok_tx = rec_tx.export_json(output_dir);
    if (!ok_tx) {
        std::fprintf(stderr,
                     "[WARN] export_legs: rec_tx (%s) export_json to '%s' "
                     "failed\n",
                     rec_tx.name().c_str(), output_dir.c_str());
    }
    const bool ok_rx = rec_rx.export_json(output_dir);
    if (!ok_rx) {
        std::fprintf(stderr,
                     "[WARN] export_legs: rec_rx (%s) export_json to '%s' "
                     "failed\n",
                     rec_rx.name().c_str(), output_dir.c_str());
    }
    return ok_rtt && ok_tx && ok_rx;
}

} // namespace bench
