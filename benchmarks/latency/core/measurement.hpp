/// @file core/measurement.hpp
/// Header-only bench measurement helpers (Phase 10).
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
/// Rationale (plan D-6): bench client uses `clock_gettime(CLOCK_MONOTONIC_RAW)`
/// instead of `eph::utils::TSC` because (1) Python mocks call the same clock
/// via ctypes, giving one-way scenarios a shared time base, (2) on invariant-
/// TSC x86_64 Linux the vDSO path uses `rdtsc` internally so cost is within
/// 10 ns of direct `rdtsc`, and (3) ns output avoids cycle-to-ns calibration
/// entirely.
///
/// Rationale (plan D-2): Recorder now accepts raw ns via `record_ns()` —
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
#include <string_view>

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
inline void install_signal_handler() noexcept {
    std::signal(SIGINT, &bench_signal_handler);
    std::signal(SIGTERM, &bench_signal_handler);
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
/// `wall_time_ns` (plan Phase 11.0 §Interface): when non-zero, used to
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
        std::printf("latency_ns:\n");
        std::printf("  min    = --\n");
        std::printf("  p50    = --\n");
        std::printf("  p90    = --\n");
        std::printf("  p99    = --\n");
        std::printf("  p99.9  = --\n");
        std::printf("  max    = --\n");
        std::printf("  avg    = --\n");
        std::printf("  stddev = --\n");
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

    // Stats fields are `double` (ns). Cast to integer for display because
    // sub-nanosecond precision is noise.
    std::printf("latency_ns:\n");
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

} // namespace bench
