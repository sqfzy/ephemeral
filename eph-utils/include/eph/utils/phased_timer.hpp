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

#include "eph/utils/time.hpp"

namespace eph::utils {

class PhasedTimer {
public:
    /// Record the warmup and measurement deadlines as TSC cycle counts.
    void start(std::chrono::nanoseconds warmup,
               std::chrono::nanoseconds measurement) noexcept {
        const uint64_t now = TSC::now();
        const auto warmup_cycles  = TSC::to_cycles(warmup).value_or(0);
        const auto measure_cycles = TSC::to_cycles(measurement).value_or(0);
        warmup_end_  = now + warmup_cycles;
        measure_end_ = warmup_end_ + measure_cycles;
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
