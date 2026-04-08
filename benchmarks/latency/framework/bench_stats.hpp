/// @file framework/bench_stats.hpp
/// LegStats / BenchResult / compute_stats / print_stats / JsonlWriter.
///
/// Pure computation (compute_stats) is separated from output (print, jsonl)
/// so spdlog and JSONL writers can share the same intermediate result.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

#include "eph/utils/hdr_histogram.hpp"

namespace bench {

// ── Statistics ───────────────────────────────────────────────────────────

struct LegStats {
    double p50_us  = 0;
    double p99_us  = 0;
    double p999_us = 0;
    double max_us  = 0;
    uint64_t samples = 0;
};

struct BenchResult {
    LegStats rtt;
    LegStats tx;
    LegStats rx;
    LegStats srv;
};

/// Pure computation: extract percentile statistics from a histogram.
/// Returns zeroed stats if the histogram is empty.
[[nodiscard]] inline LegStats compute_stats(const eph::utils::HdrHistogram& h) {
    if (h.empty()) return {};
    return LegStats{
        .p50_us  = h.get_value_at_percentile(50.0) / 1000.0,
        .p99_us  = h.get_value_at_percentile(99.0) / 1000.0,
        .p999_us = h.get_value_at_percentile(99.9) / 1000.0,
        .max_us  = h.get_max_value() / 1000.0,
        .samples = h.get_total_count(),
    };
}

inline void print_stats(const char* label, const LegStats& s) {
    if (s.samples == 0) return;
    spdlog::info("  {:<12s} p50={:.1f}us  p99={:.1f}us  p999={:.1f}us  max={:.1f}us  n={}",
                 label, s.p50_us, s.p99_us, s.p999_us, s.max_us, s.samples);
}

/// Print full 4-leg benchmark result. payload_size=0 means "no sweep"
/// (e.g., market_rx, order_rtt with fixed JSON payload).
inline void print_bench_result(const char* scenario_label, size_t payload_size,
                               const BenchResult& r) {
    spdlog::info("──────────────────────────────────────────");
    if (payload_size > 0) {
        spdlog::info("  {} | payload={}B", scenario_label, payload_size);
    } else {
        spdlog::info("  {}", scenario_label);
    }
    spdlog::info("──────────────────────────────────────────");
    print_stats("RTT", r.rtt);
    print_stats("TX (c→s)", r.tx);
    print_stats("RX (s→c)", r.rx);
    print_stats("Server", r.srv);
    spdlog::info("──────────────────────────────────────────");
}

// ── JSONL Writer ─────────────────────────────────────────────────────────

/// Writes machine-readable JSONL (one JSON object per line) to a file.
/// Construct with empty path to disable output (all writes become no-ops).
class JsonlWriter {
public:
    explicit JsonlWriter(const std::string& path) {
        if (path.empty()) return;
        file_ = std::fopen(path.c_str(), "a");
        if (!file_) {
            spdlog::error("JsonlWriter: failed to open {}", path);
        } else {
            spdlog::info("JsonlWriter: writing to {}", path);
        }
    }

    ~JsonlWriter() {
        if (file_) std::fclose(file_);
    }

    JsonlWriter(const JsonlWriter&) = delete;
    JsonlWriter& operator=(const JsonlWriter&) = delete;

    /// Write one 4-leg measurement record. Skips legs with samples=0.
    void write(std::string_view scenario, std::string_view transport,
               size_t payload_size, const BenchResult& result) {
        if (!file_) return;
        write_leg(scenario, transport, payload_size, "rtt", result.rtt);
        write_leg(scenario, transport, payload_size, "tx", result.tx);
        write_leg(scenario, transport, payload_size, "rx", result.rx);
        write_leg(scenario, transport, payload_size, "srv", result.srv);
        std::fflush(file_);
    }

private:
    void write_leg(std::string_view scenario, std::string_view transport,
                   size_t payload, const char* leg, const LegStats& s) {
        if (s.samples == 0) return;
        std::fprintf(file_,
            "{\"scenario\":\"%.*s\",\"transport\":\"%.*s\","
            "\"payload\":%zu,\"leg\":\"%s\","
            "\"p50_us\":%.1f,\"p99_us\":%.1f,\"p999_us\":%.1f,"
            "\"max_us\":%.1f,\"samples\":%llu}\n",
            static_cast<int>(scenario.size()), scenario.data(),
            static_cast<int>(transport.size()), transport.data(),
            payload, leg,
            s.p50_us, s.p99_us, s.p999_us, s.max_us,
            static_cast<unsigned long long>(s.samples));
    }

    FILE* file_ = nullptr;
};

} // namespace bench
