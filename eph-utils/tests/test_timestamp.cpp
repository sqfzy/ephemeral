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
