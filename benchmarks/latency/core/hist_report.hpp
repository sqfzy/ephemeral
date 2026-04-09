/// @file core/hist_report.hpp
/// HdrHistogram → percentile stats → spdlog reporter.
///
/// Stats are nanoseconds. Histogram tracks 10 ns to 1 s with 3 sigfigs,
/// covering the entire realistic range of intra-process to LAN latency.
#pragma once

#include <cstdint>
#include <string_view>

#include <spdlog/spdlog.h>

#include "eph/utils/hdr_histogram.hpp"

namespace bench {

inline constexpr uint64_t kHistMin = 10;
inline constexpr uint64_t kHistMax = 1'000'000'000ULL;
inline constexpr int      kHistSig = 3;

struct LegStats {
    uint64_t count   = 0;
    uint64_t min_ns  = 0;
    uint64_t max_ns  = 0;
    uint64_t p50_ns  = 0;
    uint64_t p99_ns  = 0;
    uint64_t p999_ns = 0;
    double   mean_ns = 0.0;
};

inline LegStats compute_stats(const eph::utils::HdrHistogram& h) noexcept {
    LegStats s;
    s.count = h.get_total_count();
    if (s.count == 0) return s;
    s.min_ns  = h.get_min_value();
    s.max_ns  = h.get_max_value();
    s.mean_ns = h.get_mean();
    s.p50_ns  = h.get_value_at_percentile(50.0);
    s.p99_ns  = h.get_value_at_percentile(99.0);
    s.p999_ns = h.get_value_at_percentile(99.9);
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
    spdlog::info("    {:<6} n={:>9} min={:>7} p50={:>7} p99={:>7} p999={:>7} max={:>7}",
                 label, s.count, s.min_ns, s.p50_ns, s.p99_ns, s.p999_ns, s.max_ns);
}

} // namespace detail

/// Print 4 legs of a single bench result line. `payload` may be 0 (1-leg
/// scenarios). `label` typically combines scenario + transport.
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
