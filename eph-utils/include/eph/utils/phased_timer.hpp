/// @file phased_timer.hpp
/// @brief Two-phase TSC deadline timer for warmup + measurement windows.
///
/// Standard benchmark pattern:
///
/// @code
///   PhasedTimer t;
///   t.start(1s, 10s);              // 1s warmup, then 10s measurement
///   while (t.is_running()) {
///       do_one_iteration();
///       if (!t.is_warmup()) {
///           histogram.record(sample);
///       }
///   }
/// @endcode
///
/// Both checks are TSC-only — no syscalls — so the hot loop pays ~20 cycles
/// per iteration. Requires `TSC::init()` to have run before `start()`.

#pragma once

#include <chrono>
#include <cstdint>
#include <limits>

#include "eph/utils/time.hpp"

namespace eph::utils {

/// @brief Two-phase TSC deadline timer: a warmup window followed by a
///        measurement window, both checked without syscalls.
///
/// Useful for benchmarks that must discard cold-cache / JIT / power-state
/// transients before recording samples. `is_warmup()` tells the caller
/// whether to skip recording; `is_running()` tells it whether the combined
/// (warmup + measurement) window is still open.
///
/// @note Not thread-safe — each thread needs its own instance. The timer
///       is a plain POD so copies and moves are trivial.
/// @note `TSC::init()` must have been called before `start()` or deadlines
///       fall back to 0 and the hot loop will exit immediately.
class PhasedTimer {
public:
    /// @brief Arm both deadlines relative to the current TSC reading.
    ///
    /// Captures `TSC::now()`, adds `warmup` to it for the warmup end, then
    /// adds `measurement` on top for the measurement end. Safe to call
    /// multiple times to restart the timer.
    ///
    /// @param warmup      Duration to discard samples for. 0 = no warmup.
    /// @param measurement Duration to collect samples for after warmup ends.
    ///
    /// @note If TSC is uncalibrated, `TSC::to_cycles` returns `nullopt`
    ///       and both windows collapse to 0 cycles — `is_running()` will
    ///       return `false` on the first check.
    void start(std::chrono::nanoseconds warmup,
               std::chrono::nanoseconds measurement) noexcept {
        const uint64_t now = TSC::now();
        const auto warmup_cycles  = TSC::to_cycles(warmup).value_or(0);
        const auto measure_cycles = TSC::to_cycles(measurement).value_or(0);
        // Saturating add: TSC::to_cycles saturates at UINT64_MAX on overflow
        // (e.g. caller passing nanoseconds::max for an unbounded benchmark).
        // The unguarded `now + UINT64_MAX` wraps to `now - 1` and the
        // subsequent `is_running()` check `TSC::now() < measure_end_` reads
        // false on the very next tick — collapsing the documented "window
        // still open" intent to "exit immediately". Clamp to UINT64_MAX so
        // the deadline stays in the future until TSC itself wraps
        // (~195 years at 3 GHz).
        warmup_end_ = (warmup_cycles >
                       std::numeric_limits<uint64_t>::max() - now)
                          ? std::numeric_limits<uint64_t>::max()
                          : now + warmup_cycles;
        measure_end_ = (measure_cycles >
                        std::numeric_limits<uint64_t>::max() - warmup_end_)
                           ? std::numeric_limits<uint64_t>::max()
                           : warmup_end_ + measure_cycles;
        started_ = true;
    }

    /// True while we're still in the warmup window (don't record).
    [[nodiscard]] bool is_warmup() const noexcept {
        return started_ && TSC::now() < warmup_end_;
    }

    /// True while the (warmup + measurement) window is still open.
    [[nodiscard]] bool is_running() const noexcept {
        return started_ && TSC::now() < measure_end_;
    }

private:
    bool     started_     = false;
    uint64_t warmup_end_  = 0;
    uint64_t measure_end_ = 0;
};

} // namespace eph::utils
