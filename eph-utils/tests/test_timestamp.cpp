#include <gtest/gtest.h>

#include "eph/utils/timestamp.hpp"

using namespace eph::utils;

// ---------------------------------------------------------------------------
// Constexpr unit conversions
// ---------------------------------------------------------------------------

TEST(Timestamp, ms_to_ns_basic) {
    static_assert(ms_to_ns(1) == 1'000'000);
    static_assert(ms_to_ns(1000) == 1'000'000'000);
    static_assert(ms_to_ns(0) == 0);

    // Typical Binance timestamp (2026-03-28 ~00:00 UTC)
    constexpr int64_t binance_ts = 1'774'656'000'000LL; // ms
    constexpr uint64_t ns = ms_to_ns(binance_ts);
    static_assert(ns == 1'774'656'000'000'000'000ULL);
    EXPECT_EQ(ns, 1'774'656'000'000'000'000ULL);
}

TEST(Timestamp, ns_to_ms_truncates) {
    static_assert(ns_to_ms(1'000'000) == 1);
    static_assert(ns_to_ms(1'999'999) == 1); // truncation, not rounding
    static_assert(ns_to_ms(0) == 0);
    EXPECT_EQ(ns_to_ms(999'999), 0);
}

TEST(Timestamp, us_to_ns_basic) {
    static_assert(us_to_ns(1) == 1'000);
    static_assert(us_to_ns(1'000'000) == 1'000'000'000);
    static_assert(us_to_ns(0) == 0);
    EXPECT_EQ(us_to_ns(500), 500'000);
}

TEST(Timestamp, ms_ns_round_trip) {
    constexpr int64_t original_ms = 1'774'656'000'123LL;
    static_assert(ns_to_ms(ms_to_ns(original_ms)) == original_ms);
    EXPECT_EQ(ns_to_ms(ms_to_ns(original_ms)), original_ms);
}

// ---------------------------------------------------------------------------
// ITCH timestamp conversion
// ---------------------------------------------------------------------------

TEST(Timestamp, itch_ts_to_epoch_ns_basic) {
    // Midnight of 2026-03-28 in epoch nanoseconds
    constexpr uint64_t midnight = 1'774'656'000'000'000'000ULL;
    // 10:30:00.123456789 = 10*3600 + 30*60 = 37800 seconds
    constexpr uint64_t ns_since_midnight =
        37'800ULL * 1'000'000'000ULL + 123'456'789ULL;

    constexpr uint64_t result = itch_ts_to_epoch_ns(ns_since_midnight, midnight);
    static_assert(result == midnight + ns_since_midnight);
    EXPECT_EQ(result, midnight + ns_since_midnight);
}

TEST(Timestamp, itch_ts_to_epoch_ns_zero_offset) {
    constexpr uint64_t midnight = 1'774'656'000'000'000'000ULL;
    static_assert(itch_ts_to_epoch_ns(0, midnight) == midnight);
    EXPECT_EQ(itch_ts_to_epoch_ns(0, midnight), midnight);
}

// ---------------------------------------------------------------------------
// Wall-clock: now_ns / now_ms
// ---------------------------------------------------------------------------

TEST(Timestamp, now_ns_is_reasonable) {
    uint64_t ts = now_ns();
    // Must be after 2020-01-01T00:00:00Z = 1577836800 seconds
    constexpr uint64_t min_2020 = 1'577'836'800ULL * 1'000'000'000ULL;
    EXPECT_GT(ts, min_2020) << "now_ns() returned a value before year 2020";
}

TEST(Timestamp, now_ms_is_reasonable) {
    int64_t ts = now_ms();
    constexpr int64_t min_2020 = 1'577'836'800'000LL;
    EXPECT_GT(ts, min_2020) << "now_ms() returned a value before year 2020";
}

TEST(Timestamp, now_ns_is_monotonic) {
    uint64_t t1 = now_ns();
    uint64_t t2 = now_ns();
    EXPECT_LE(t1, t2);
}

// ---------------------------------------------------------------------------
// Feed latency
// ---------------------------------------------------------------------------

TEST(Timestamp, feed_latency_ns_is_small) {
    // Exchange timestamp = right now → latency should be < 1 second
    uint64_t exchange_ts = now_ns();
    int64_t latency = feed_latency_ns(exchange_ts);
    EXPECT_GE(latency, 0);
    EXPECT_LT(latency, 1'000'000'000LL) << "latency >= 1s on same machine";
}

TEST(Timestamp, feed_latency_ns_from_ms_is_small) {
    int64_t exchange_ts = now_ms();
    int64_t latency = feed_latency_ns_from_ms(exchange_ts);
    // Truncation of now_ms could add up to ~1ms, so allow a bit more.
    EXPECT_GE(latency, 0);
    EXPECT_LT(latency, 1'000'000'000LL) << "latency >= 1s on same machine";
}

// ---------------------------------------------------------------------------
// ISO 8601 formatting
// ---------------------------------------------------------------------------

TEST(Timestamp, format_timestamp_ns_known_value) {
    // 2026-03-28T00:00:00.000000000Z
    constexpr uint64_t epoch_ns = 1'774'656'000'000'000'000ULL;
    std::string s = format_timestamp_ns(epoch_ns);
    EXPECT_EQ(s, "2026-03-28T00:00:00.000000000Z");
}

TEST(Timestamp, format_timestamp_ns_with_fractional) {
    // 2026-03-28T00:00:00.123456789Z
    constexpr uint64_t epoch_ns = 1'774'656'000'123'456'789ULL;
    std::string s = format_timestamp_ns(epoch_ns);
    EXPECT_EQ(s, "2026-03-28T00:00:00.123456789Z");
}

TEST(Timestamp, format_timestamp_ms_known_value) {
    constexpr int64_t epoch_ms = 1'774'656'000'000LL;
    std::string s = format_timestamp_ms(epoch_ms);
    EXPECT_EQ(s, "2026-03-28T00:00:00.000Z");
}

TEST(Timestamp, format_timestamp_ms_with_fractional) {
    constexpr int64_t epoch_ms = 1'774'656'000'456LL;
    std::string s = format_timestamp_ms(epoch_ms);
    EXPECT_EQ(s, "2026-03-28T00:00:00.456Z");
}

// ---------------------------------------------------------------------------
// Edge cases and boundary conditions
// ---------------------------------------------------------------------------

TEST(Timestamp, format_timestamp_ns_epoch_zero) {
    // Unix epoch start: 1970-01-01T00:00:00.000000000Z
    std::string s = format_timestamp_ns(0);
    EXPECT_EQ(s, "1970-01-01T00:00:00.000000000Z");
}

TEST(Timestamp, format_timestamp_ms_epoch_zero) {
    std::string s = format_timestamp_ms(0);
    EXPECT_EQ(s, "1970-01-01T00:00:00.000Z");
}

TEST(Timestamp, ms_to_ns_large_value_no_overflow) {
    // Year ~2100 in ms: approximately 4'102'444'800'000 ms
    constexpr int64_t far_future_ms = 4'102'444'800'000LL;
    constexpr uint64_t result = ms_to_ns(far_future_ms);
    // Verify round-trip
    EXPECT_EQ(ns_to_ms(result), far_future_ms);
}

TEST(Timestamp, ns_to_ms_single_nanosecond) {
    // A single nanosecond should truncate to 0 ms
    static_assert(ns_to_ms(1) == 0);
    EXPECT_EQ(ns_to_ms(1), 0);
}

TEST(Timestamp, us_to_ns_large_value) {
    // 1 hour in microseconds
    constexpr int64_t one_hour_us = 3'600'000'000LL;
    constexpr uint64_t result = us_to_ns(one_hour_us);
    EXPECT_EQ(result, 3'600'000'000'000ULL);
}

TEST(Timestamp, format_timestamp_ns_end_of_day) {
    // 2026-03-28T23:59:59.999999999Z
    constexpr uint64_t epoch_ns = 1'774'656'000'000'000'000ULL
                                + 86'399ULL * 1'000'000'000ULL
                                + 999'999'999ULL;
    std::string s = format_timestamp_ns(epoch_ns);
    EXPECT_EQ(s, "2026-03-28T23:59:59.999999999Z");
}

TEST(Timestamp, now_ns_called_twice_returns_increasing_values) {
    // Stronger monotonicity test with multiple samples
    uint64_t prev = now_ns();
    for (int i = 0; i < 100; ++i) {
        uint64_t curr = now_ns();
        EXPECT_GE(curr, prev) << "now_ns() went backwards at iteration " << i;
        prev = curr;
    }
}

TEST(Timestamp, feed_latency_ns_from_old_timestamp) {
    // An exchange timestamp from 1 second ago should yield ~1s latency
    uint64_t one_sec_ago = now_ns() - 1'000'000'000ULL;
    int64_t latency = feed_latency_ns(one_sec_ago);
    // Should be close to 1 billion ns (1 second), allow some tolerance
    EXPECT_GT(latency, 900'000'000LL);
    EXPECT_LT(latency, 1'100'000'000LL);
}

// ---------------------------------------------------------------------------
// Negative / pre-epoch timestamps
// ---------------------------------------------------------------------------

TEST(Timestamp, format_timestamp_ms_negative_one_ms) {
    // -1 ms should be 1969-12-31T23:59:59.999Z
    std::string s = format_timestamp_ms(-1);
    EXPECT_EQ(s, "1969-12-31T23:59:59.999Z");
}

TEST(Timestamp, format_timestamp_ms_negative_1000_ms) {
    // -1000 ms = -1 second exactly
    std::string s = format_timestamp_ms(-1000);
    EXPECT_EQ(s, "1969-12-31T23:59:59.000Z");
}

TEST(Timestamp, format_timestamp_ms_negative_1001_ms) {
    // -1001 ms = 1 second and 1 ms before epoch
    std::string s = format_timestamp_ms(-1001);
    EXPECT_EQ(s, "1969-12-31T23:59:58.999Z");
}

TEST(Timestamp, format_timestamp_ms_negative_500_ms) {
    // -500 ms = half a second before epoch
    std::string s = format_timestamp_ms(-500);
    EXPECT_EQ(s, "1969-12-31T23:59:59.500Z");
}

// ---------------------------------------------------------------------------
// Boundary: far future timestamps
// ---------------------------------------------------------------------------

TEST(Timestamp, format_timestamp_ns_year_2100) {
    // 2100-01-01T00:00:00Z = 4102444800 seconds since epoch
    constexpr uint64_t epoch_ns = 4'102'444'800ULL * 1'000'000'000ULL;
    std::string s = format_timestamp_ns(epoch_ns);
    EXPECT_EQ(s, "2100-01-01T00:00:00.000000000Z");
}

TEST(Timestamp, format_timestamp_ms_year_2100) {
    constexpr int64_t epoch_ms = 4'102'444'800'000LL;
    std::string s = format_timestamp_ms(epoch_ms);
    EXPECT_EQ(s, "2100-01-01T00:00:00.000Z");
}

// ---------------------------------------------------------------------------
// count_delta tests (from hdr_histogram.hpp Stats)
// ---------------------------------------------------------------------------

TEST(Timestamp, feed_latency_ns_future_timestamp_yields_negative) {
    // Exchange timestamp in the future should give negative latency (clock skew)
    uint64_t future_ts = now_ns() + 1'000'000'000ULL;
    int64_t latency = feed_latency_ns(future_ts);
    EXPECT_LT(latency, 0);
}

TEST(Timestamp, format_timestamp_ns_max_nanoseconds_within_second) {
    // Last nanosecond of 2026-03-28: 23:59:59.999999999Z
    constexpr uint64_t epoch_ns = 1'774'656'000'000'000'000ULL
                                + 86'399ULL * 1'000'000'000ULL
                                + 999'999'999ULL;
    std::string s = format_timestamp_ns(epoch_ns);
    EXPECT_TRUE(s.ends_with("999999999Z"));
}

TEST(Timestamp, ms_to_ns_boundary_max_safe_ms) {
    // Largest ms value that fits in uint64_t after *1e6:
    // UINT64_MAX / 1'000'000 = 18'446'744'073'709
    constexpr int64_t max_safe_ms = 18'446'744'073'709LL;
    constexpr uint64_t result = ms_to_ns(max_safe_ms);
    // Should not overflow
    EXPECT_EQ(ns_to_ms(result), max_safe_ms);
}

TEST(Timestamp, us_to_ns_boundary_max_safe_us) {
    // Largest us value that fits in uint64_t after *1e3:
    // UINT64_MAX / 1'000 = 18'446'744'073'709'551
    constexpr int64_t max_safe_us = 18'446'744'073'709'551LL;
    constexpr uint64_t result = us_to_ns(max_safe_us);
    EXPECT_EQ(result, static_cast<uint64_t>(max_safe_us) * 1'000);
}

// ---------------------------------------------------------------------------
// Overflow protection: values beyond max safe threshold return 0
// ---------------------------------------------------------------------------

TEST(Timestamp, ms_to_ns_overflow_returns_zero) {
    // One past the max safe value should return 0 (overflow protection)
    constexpr int64_t overflow_ms = 18'446'744'073'710LL;  // max_safe + 1
    EXPECT_EQ(ms_to_ns(overflow_ms), 0u)
        << "ms_to_ns must return 0 for values that would overflow uint64_t";

    // INT64_MAX would definitely overflow
    EXPECT_EQ(ms_to_ns(INT64_MAX), 0u)
        << "ms_to_ns(INT64_MAX) must return 0 to prevent silent overflow";
}

TEST(Timestamp, us_to_ns_overflow_returns_zero) {
    // One past the max safe value should return 0 (overflow protection)
    constexpr int64_t overflow_us = 18'446'744'073'709'552LL;  // max_safe + 1
    EXPECT_EQ(us_to_ns(overflow_us), 0u)
        << "us_to_ns must return 0 for values that would overflow uint64_t";

    // INT64_MAX would definitely overflow
    EXPECT_EQ(us_to_ns(INT64_MAX), 0u)
        << "us_to_ns(INT64_MAX) must return 0 to prevent silent overflow";
}

TEST(Timestamp, ms_to_ns_negative_returns_zero) {
    // Negative values should return 0 (existing behavior, regression test)
    EXPECT_EQ(ms_to_ns(-1), 0u);
    EXPECT_EQ(ms_to_ns(INT64_MIN), 0u);
}

TEST(Timestamp, us_to_ns_negative_returns_zero) {
    EXPECT_EQ(us_to_ns(-1), 0u);
    EXPECT_EQ(us_to_ns(INT64_MIN), 0u);
}

TEST(Timestamp, ms_to_ns_at_exact_boundary_succeeds) {
    // The exact max safe value should still work
    constexpr int64_t max_safe_ms = 18'446'744'073'709LL;
    constexpr uint64_t result = ms_to_ns(max_safe_ms);
    EXPECT_GT(result, 0u);
    EXPECT_EQ(ns_to_ms(result), max_safe_ms);
}

TEST(Timestamp, us_to_ns_at_exact_boundary_succeeds) {
    constexpr int64_t max_safe_us = 18'446'744'073'709'551LL;
    constexpr uint64_t result = us_to_ns(max_safe_us);
    EXPECT_GT(result, 0u);
    EXPECT_EQ(result, static_cast<uint64_t>(max_safe_us) * 1'000);
}
