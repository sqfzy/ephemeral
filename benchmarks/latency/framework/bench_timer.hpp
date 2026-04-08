/// @file framework/bench_timer.hpp
/// Two-phase timer: warmup (samples discarded) → measurement (samples recorded).

#pragma once

#include <chrono>
#include <cstdint>

namespace bench {

/// Tracks warmup and measurement phases. Zero-overhead — every check
/// is a simple comparison against a steady_clock::time_point.
class BenchTimer {
public:
    /// Start the timer. Total wall time = warmup + duration.
    void start(std::chrono::seconds warmup, std::chrono::seconds duration) noexcept {
        start_ = std::chrono::steady_clock::now();
        warmup_end_ = start_ + warmup;
        measure_end_ = warmup_end_ + duration;
    }

    /// True during warmup phase — samples should be discarded.
    [[nodiscard]] bool is_warmup() const noexcept {
        return std::chrono::steady_clock::now() < warmup_end_;
    }

    /// True while either warmup or measurement is active.
    /// Returns false after warmup + duration has elapsed.
    [[nodiscard]] bool is_running() const noexcept {
        return std::chrono::steady_clock::now() < measure_end_;
    }

    /// Seconds since start().
    [[nodiscard]] int64_t elapsed_s() const noexcept {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_).count();
    }

private:
    std::chrono::steady_clock::time_point start_{};
    std::chrono::steady_clock::time_point warmup_end_{};
    std::chrono::steady_clock::time_point measure_end_{};
};

} // namespace bench
