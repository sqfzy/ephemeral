/// @file mock/lib/work_spin.hpp
/// Spin for N nanoseconds using TSC + cpu_relax (PAUSE / YIELD).
///
/// Used by RTT mocks to simulate "received request → did N ns of business
/// work → sent response" so the bench's server-leg histogram is non-zero.
/// The duration is real wall-clock ns (TSC-converted), not a fixed cycle
/// count, so the same `--server-work-ns` value is comparable across CPUs.
///
/// The loop body is a single PAUSE / YIELD — the cheapest possible spin
/// that yields pipeline resources to the SMT sibling and tells the CPU
/// to back off speculative execution.
#pragma once

#include <cstdint>

#include "eph/utils/cpu.hpp"
#include "eph/utils/time.hpp"

#include "../../core/tsc_protocol.hpp"

namespace bench::mock {

/// Busy-wait for `ns` nanoseconds. No-op if `ns <= 0`. The actual
/// duration is `ns` ± a few cycles of TSC overhead.
inline void work_spin(long ns) noexcept {
    if (ns <= 0) return;
    uint64_t target_cycles = bench::tsc::ns_to_cycles(static_cast<uint64_t>(ns));
    if (target_cycles == 0) return;
    uint64_t start = eph::utils::TSC::now();
    uint64_t deadline = start + target_cycles;
    while (eph::utils::TSC::now() < deadline) {
        eph::utils::cpu_relax();
    }
}

} // namespace bench::mock
