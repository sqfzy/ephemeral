/// @file bench_common.hpp
/// Shared utilities for all benchmark binaries.
///
/// Provides signal handling, string splitting, Binance JSON parsing,
/// HdrHistogram conversion, feed latency recording, and latency printing.
/// All functions are inline or in namespace bench{} for header-only use.
///
/// Include this header in bench_*.cpp files to avoid code duplication.

#pragma once

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/transport/transport_types.hpp"
#include "eph/utils/hdr_histogram.hpp"
#include "eph/utils/time.hpp"

// ── Signal handling ────────────────────────────────────────────────────────
// Shared across all bench binaries: SIGINT/SIGTERM set g_running=false.

inline std::atomic<bool> g_running{true};
inline void sig(int) { g_running.store(false, std::memory_order_release); }

namespace bench {

// ── String utilities ───────────────────────────────────────────────────────

/// Split string s by delimiter delim, skipping empty tokens.
inline std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> tokens;
    std::istringstream ss(s);
    std::string token;
    while (std::getline(ss, token, delim))
        if (!token.empty()) tokens.push_back(token);
    return tokens;
}

// ── Binance JSON helpers ───────────────────────────────────────────────────

/// 4-byte direct hash for Binance stream symbol extraction.
/// Reads 4 bytes at byte offset 11 (the start of the symbol in
/// {"stream":"<symbol>..." or single-stream {"<first 4 bytes of symbol>}).
/// Returns 0 if the payload is too short or not a JSON object.
inline uint32_t binance_symbol_hash(const uint8_t* data, size_t len) {
    if (len < 15) [[unlikely]] return 0;
    if (data[0] != '{') [[unlikely]] return 0;
    uint32_t h;
    std::memcpy(&h, data + 11, 4);
    return h;
}

/// Extract Binance event timestamp ("E" field) from JSON payload.
/// Scans for "\"E\":" and parses the integer value that follows.
/// Returns nanoseconds when connecting to a mock server (time.time_ns()),
/// or milliseconds when connecting to real Binance (Unix epoch ms).
/// Returns 0 on parse failure.
inline int64_t binance_event_time(const uint8_t* data, size_t len) {
    std::string_view json(reinterpret_cast<const char*>(data), len);
    auto pos = json.find("\"E\":");
    if (pos == std::string_view::npos) return 0;
    pos += 4;  // skip past "E":
    while (pos < len && json[pos] == ' ') ++pos;
    int64_t val = 0;
    while (pos < len && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + (json[pos] - '0');
        ++pos;
    }
    return val;
}

/// Detect whether a timestamp is in nanoseconds or milliseconds.
/// Nanosecond epoch values (2017+) exceed 1.5e18; millisecond values are ~1.7e12.
inline bool is_nanosecond_timestamp(int64_t ts) {
    return ts > 1'500'000'000'000'000'000LL;
}

// ── HdrHistogram conversion ────────────────────────────────────────────────

/// Convert HdrHistogram to RttStats.
/// Returns a zero-initialized RttStats when the histogram has no samples.
inline eph::net::RttStats hdr_to_stats(const eph::utils::HdrHistogram& h) noexcept {
    if (h.get_total_count() == 0) return {};
    return {
        .count   = h.get_total_count(),
        .min_ns  = h.get_min_value(),
        .max_ns  = h.get_max_value(),
        .mean_ns = h.get_mean(),
        .p50_ns  = h.get_value_at_percentile(50.0),
        .p99_ns  = h.get_value_at_percentile(99.0),
        .p999_ns = h.get_value_at_percentile(99.9),
    };
}

// ── Latency reporting ──────────────────────────────────────────────────────

/// Print an RttStats summary to spdlog at INFO level.
/// Prints "(no samples)" when s.count == 0.
inline void print_latency(std::string_view label, const eph::net::RttStats& s) {
    spdlog::info("--- {} ---", label);
    if (s.count > 0) {
        spdlog::info("  samples: {}", s.count);
        spdlog::info("  min:     {:.0f} ns", static_cast<double>(s.min_ns));
        spdlog::info("  p50:     {:.0f} ns", static_cast<double>(s.p50_ns));
        spdlog::info("  p99:     {:.0f} ns", static_cast<double>(s.p99_ns));
        spdlog::info("  p99.9:   {:.0f} ns", static_cast<double>(s.p999_ns));
        spdlog::info("  max:     {:.0f} ns", static_cast<double>(s.max_ns));
    } else {
        spdlog::info("  (no samples)");
    }
}

// ── Feed latency recording ─────────────────────────────────────────────────

/// Record a single feed latency sample into hist.
///
/// Parses the "E" event timestamp from the Binance JSON payload, detects
/// whether it is nanoseconds (mock server) or milliseconds (real Binance),
/// then computes delta = local_wall_clock - event_ts and records it.
/// Silently skips the sample if parsing fails or the delta is non-positive.
///
/// @param data  Pointer to raw WebSocket frame payload.
/// @param len   Payload length in bytes.
/// @param hist  HdrHistogram to record the latency sample into.
inline void record_feed_latency(const uint8_t* data, size_t len,
                                eph::utils::HdrHistogram& hist) {
    int64_t event_ts = binance_event_time(data, len);
    if (event_ts <= 0) return;

    int64_t now_ns;
    if (is_nanosecond_timestamp(event_ts)) {
        // Mock server: event_ts is already nanoseconds
        now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    } else {
        // Real Binance: event_ts is milliseconds — normalise both to ns
        now_ns = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        now_ns   *= 1'000'000;
        event_ts *= 1'000'000;
    }

    if (now_ns > event_ts) {
        hist.record(static_cast<uint64_t>(now_ns - event_ts));
    }
}

}  // namespace bench
