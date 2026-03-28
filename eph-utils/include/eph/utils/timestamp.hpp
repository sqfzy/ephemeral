#pragma once

/// @file timestamp.hpp
/// @brief Timestamp conversion utilities for HFT feed processing.
///
/// Different exchanges use different timestamp formats:
/// - Binance: milliseconds since Unix epoch (int64_t)
/// - ITCH:    nanoseconds since midnight (uint64_t)
/// - Internal: nanoseconds since Unix epoch (uint64_t)
///
/// This header provides constexpr conversion functions between them,
/// plus wall-clock helpers for latency measurement and logging.

#include <cstdint>
#include <ctime>
#include <string>

namespace eph::utils {

// ---------------------------------------------------------------------------
// Constexpr unit conversions
// ---------------------------------------------------------------------------

/// Convert milliseconds since Unix epoch to nanoseconds.
constexpr uint64_t ms_to_ns(int64_t ms) noexcept {
    return static_cast<uint64_t>(ms) * 1'000'000;
}

/// Convert nanoseconds to milliseconds (truncating).
constexpr int64_t ns_to_ms(uint64_t ns) noexcept {
    return static_cast<int64_t>(ns / 1'000'000);
}

/// Convert microseconds to nanoseconds.
constexpr uint64_t us_to_ns(int64_t us) noexcept {
    return static_cast<uint64_t>(us) * 1'000;
}

/// Nanoseconds since midnight (ITCH timestamp format) to nanoseconds since epoch.
/// @param ns_since_midnight  Timestamp from ITCH feed.
/// @param midnight_epoch_ns  Epoch nanoseconds at midnight of the trading day.
constexpr uint64_t itch_ts_to_epoch_ns(uint64_t ns_since_midnight,
                                       uint64_t midnight_epoch_ns) noexcept {
    return midnight_epoch_ns + ns_since_midnight;
}

// ---------------------------------------------------------------------------
// Wall-clock helpers (not constexpr — syscall)
// ---------------------------------------------------------------------------

/// Get current time as nanoseconds since Unix epoch.
inline uint64_t now_ns() noexcept {
    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
}

/// Get current time as milliseconds since Unix epoch (Binance format).
inline int64_t now_ms() noexcept {
    return ns_to_ms(now_ns());
}

/// Compute feed latency: current wall-clock minus exchange timestamp (ns).
/// Returns nanoseconds. Negative if clocks are skewed.
inline int64_t feed_latency_ns(uint64_t exchange_ts_ns) noexcept {
    return static_cast<int64_t>(now_ns()) - static_cast<int64_t>(exchange_ts_ns);
}

/// Compute feed latency from a millisecond exchange timestamp (Binance).
inline int64_t feed_latency_ns_from_ms(int64_t exchange_ts_ms) noexcept {
    return feed_latency_ns(ms_to_ns(exchange_ts_ms));
}

// ---------------------------------------------------------------------------
// Formatting (for logging — not hot-path)
// ---------------------------------------------------------------------------

/// Format nanosecond epoch timestamp as ISO 8601 string.
/// e.g., "2026-03-28T14:30:00.123456789Z"
inline std::string format_timestamp_ns(uint64_t epoch_ns) noexcept {
    auto secs  = static_cast<time_t>(epoch_ns / 1'000'000'000ULL);
    auto nanos = static_cast<uint32_t>(epoch_ns % 1'000'000'000ULL);

    struct tm utc{};
    gmtime_r(&secs, &utc);

    // "YYYY-MM-DDThh:mm:ss.nnnnnnnnnZ" = 30 chars + NUL
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &utc);

    char result[64];
    std::snprintf(result, sizeof(result), "%s.%09uZ", buf, nanos);
    return result;
}

/// Format millisecond epoch timestamp as ISO 8601 string.
/// e.g., "2026-03-28T14:30:00.123Z"
inline std::string format_timestamp_ms(int64_t epoch_ms) noexcept {
    auto secs = static_cast<time_t>(epoch_ms / 1'000);
    auto ms   = static_cast<uint32_t>(epoch_ms % 1'000);

    struct tm utc{};
    gmtime_r(&secs, &utc);

    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &utc);

    char result[48];
    std::snprintf(result, sizeof(result), "%s.%03uZ", buf, ms);
    return result;
}

} // namespace eph::utils
