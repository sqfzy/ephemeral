/// @file core/timer.hpp
/// Phased timer for warmup → measurement windows.
///
/// `BenchTimer::start(warmup, duration)` records two TSC deadlines:
/// `warmup_end_tsc` and `measure_end_tsc`. `is_warmup()` and `is_running()`
/// are TSC-only (no syscall) so the hot loop polls them at zero cost.
#pragma once

#include <chrono>
#include <cstdint>

#include "eph/utils/time.hpp"
#include "tsc_protocol.hpp"

namespace bench {

class BenchTimer {
public:
    void start(std::chrono::seconds warmup,
               std::chrono::seconds measurement) noexcept {
        uint64_t now = eph::utils::TSC::now();
        uint64_t warmup_cycles  = tsc::ns_to_cycles(
            static_cast<uint64_t>(warmup.count()) * 1'000'000'000ULL);
        uint64_t measure_cycles = tsc::ns_to_cycles(
            static_cast<uint64_t>(measurement.count()) * 1'000'000'000ULL);
        warmup_end_tsc_  = now + warmup_cycles;
        measure_end_tsc_ = warmup_end_tsc_ + measure_cycles;
        started_ = true;
    }

    [[nodiscard]] bool is_warmup() const noexcept {
        return started_ && eph::utils::TSC::now() < warmup_end_tsc_;
    }

    [[nodiscard]] bool is_running() const noexcept {
        return started_ && eph::utils::TSC::now() < measure_end_tsc_;
    }

private:
    bool     started_         = false;
    uint64_t warmup_end_tsc_  = 0;
    uint64_t measure_end_tsc_ = 0;
};

} // namespace bench
