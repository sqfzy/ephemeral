/// @file core/hist_report.hpp
/// Thin reporter over eph::utils::HdrHistogram → eph::utils::Stats.
///
/// Reuses the Stats struct from eph-utils (recorder.hpp produces it too).
/// Our compute_stats() is needed because we record *nanoseconds* directly
/// into the histogram rather than TSC cycles, so Recorder's cycle-based
/// compute_stats would over-convert.
#pragma once

#include <cstdint>
#include <string_view>

#include <spdlog/spdlog.h>

#include "eph/utils/hdr_histogram.hpp"

namespace bench {

inline constexpr uint64_t kHistMin = 10;
inline constexpr uint64_t kHistMax = 1'000'000'000ULL;
inline constexpr int      kHistSig = 3;

using LegStats = eph::utils::Stats;

/// Build a Stats directly from an HdrHistogram whose values are already
/// in nanoseconds. Fields not meaningful for ns-direct recording (p90,
/// stddev) are populated when cheap, zero otherwise.
inline LegStats compute_stats(const eph::utils::HdrHistogram& h,
                              std::string_view name = {}) noexcept {
    LegStats s;
    s.name = std::string{name};
    s.count = h.get_total_count();
    if (s.count == 0) return s;
    s.avg_ns  = h.get_mean();
    s.min_ns  = static_cast<double>(h.get_min_value());
    s.max_ns  = static_cast<double>(h.get_max_value());
    auto pct = h.get_percentiles({50.0, 90.0, 99.0, 99.9});
    s.p50_ns  = static_cast<double>(pct[0]);
    s.p90_ns  = static_cast<double>(pct[1]);
    s.p99_ns  = static_cast<double>(pct[2]);
    s.p999_ns = static_cast<double>(pct[3]);
    s.stddev_ns = h.get_std_deviation();
    return s;
}

struct BenchResult {
    LegStats rtt;
    LegStats tx;
    LegStats rx;
    LegStats srv;
};

namespace detail {

inline void print_one_leg(std::string_view label, const LegStats& s) {
    if (s.count == 0) {
        spdlog::info("    {:<6} (no samples)", label);
        return;
    }
    spdlog::info("    {:<6} n={:>9} min={:>7.0f} p50={:>7.0f} p99={:>7.0f} p999={:>7.0f} max={:>7.0f}",
                 label, s.count, s.min_ns, s.p50_ns, s.p99_ns, s.p999_ns, s.max_ns);
}

} // namespace detail

/// Print 4-leg report line. `payload=0` hides the payload tag (for 1-leg scenarios).
inline void print_bench_result(std::string_view label,
                               size_t payload,
                               const BenchResult& r) {
    if (payload > 0) {
        spdlog::info("== BENCH {} payload={}B ==", label, payload);
    } else {
        spdlog::info("== BENCH {} ==", label);
    }
    detail::print_one_leg("RTT", r.rtt);
    detail::print_one_leg("TX",  r.tx);
    detail::print_one_leg("RX",  r.rx);
    detail::print_one_leg("SRV", r.srv);
}

} // namespace bench
