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

#include <cassert>
#include <cstdint>
#include <ctime>
#include <format>
#include <string>

namespace eph::utils {

// ---------------------------------------------------------------------------
// Constexpr unit conversions
// ---------------------------------------------------------------------------

/// Convert milliseconds since Unix epoch to nanoseconds.
[[nodiscard]] constexpr uint64_t ms_to_ns(int64_t ms) noexcept {
    assert(ms >= 0 && "ms_to_ns: negative millisecond input would wrap on cast to uint64_t");
    return static_cast<uint64_t>(ms) * 1'000'000;
}

/// Convert nanoseconds to milliseconds (truncating).
[[nodiscard]] constexpr int64_t ns_to_ms(uint64_t ns) noexcept {
    return static_cast<int64_t>(ns / 1'000'000);
}

/// Convert microseconds to nanoseconds.
[[nodiscard]] constexpr uint64_t us_to_ns(int64_t us) noexcept {
    assert(us >= 0 && "us_to_ns: negative microsecond input would wrap on cast to uint64_t");
    return static_cast<uint64_t>(us) * 1'000;
}

/// Nanoseconds since midnight (ITCH timestamp format) to nanoseconds since epoch.
/// @param ns_since_midnight  Timestamp from ITCH feed.
/// @param midnight_epoch_ns  Epoch nanoseconds at midnight of the trading day.
[[nodiscard]] constexpr uint64_t itch_ts_to_epoch_ns(uint64_t ns_since_midnight,
                                       uint64_t midnight_epoch_ns) noexcept {
    return midnight_epoch_ns + ns_since_midnight;
}

// ---------------------------------------------------------------------------
// Wall-clock helpers (not constexpr — syscall)
// ---------------------------------------------------------------------------

/// Get current time as nanoseconds since Unix epoch.
[[nodiscard]] inline uint64_t now_ns() noexcept {
    struct timespec ts{};
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
}

/// Get current time as milliseconds since Unix epoch (Binance format).
[[nodiscard]] inline int64_t now_ms() noexcept {
    return ns_to_ms(now_ns());
}

/// Compute feed latency: current wall-clock minus exchange timestamp (ns).
/// Returns nanoseconds. Negative if clocks are skewed.
[[nodiscard]] inline int64_t feed_latency_ns(uint64_t exchange_ts_ns) noexcept {
    return static_cast<int64_t>(now_ns()) - static_cast<int64_t>(exchange_ts_ns);
}

/// Compute feed latency from a millisecond exchange timestamp (Binance).
[[nodiscard]] inline int64_t feed_latency_ns_from_ms(int64_t exchange_ts_ms) noexcept {
    return feed_latency_ns(ms_to_ns(exchange_ts_ms));
}

// ---------------------------------------------------------------------------
// Formatting (for logging — not hot-path)
// ---------------------------------------------------------------------------

/// Format nanosecond epoch timestamp as ISO 8601 string.
/// e.g., "2026-03-28T14:30:00.123456789Z"
[[nodiscard]] inline std::string format_timestamp_ns(uint64_t epoch_ns) {
    // M13: time_t must be 64-bit to avoid Y2K38 overflow on large epoch values.
    static_assert(sizeof(time_t) >= 8,
                  "time_t must be at least 64-bit to avoid Y2K38 overflow");

    auto secs  = static_cast<time_t>(epoch_ns / 1'000'000'000ULL);
    auto nanos = static_cast<uint32_t>(epoch_ns % 1'000'000'000ULL);

    struct tm utc{};
    gmtime_r(&secs, &utc);

    return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:09d}Z",
                       utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                       utc.tm_hour, utc.tm_min, utc.tm_sec, nanos);
}

/// Format millisecond epoch timestamp as ISO 8601 string.
/// e.g., "2026-03-28T14:30:00.123Z"
[[nodiscard]] inline std::string format_timestamp_ms(int64_t epoch_ms) {
    // M13: time_t must be 64-bit to avoid Y2K38 overflow on large epoch values.
    static_assert(sizeof(time_t) >= 8,
                  "time_t must be at least 64-bit to avoid Y2K38 overflow");

    // For negative timestamps (pre-epoch), C++ truncation division yields a
    // negative remainder. Use Euclidean division to get a non-negative
    // millisecond component and the correct floor'd seconds.
    // e.g., -1 ms => secs = -1, ms_part = 999 (i.e., 1969-12-31T23:59:59.999Z)
    auto secs = static_cast<time_t>(epoch_ms / 1'000);
    auto ms_rem = static_cast<int32_t>(epoch_ms % 1'000);
    if (ms_rem < 0) {
        ms_rem += 1'000;
        secs -= 1;
    }
    auto ms = static_cast<uint32_t>(ms_rem);

    struct tm utc{};
    gmtime_r(&secs, &utc);

    return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:03d}Z",
                       utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                       utc.tm_hour, utc.tm_min, utc.tm_sec, ms);
}

} // namespace eph::utils
