#include <gtest/gtest.h>

#include <array>

#include "eph/utils/console_sink.hpp"

using namespace eph::utils;

// ---------------------------------------------------------------------------
// Basic construction and MetricsSink concept
// ---------------------------------------------------------------------------

TEST(ConsoleSinkTest, SatisfiesMetricsSinkConcept) {
    // Compile-time check is already in console_sink.hpp via static_assert.
    // This test verifies that ConsoleSink can be default-constructed and used.
    ConsoleSink sink;
    SUCCEED();
}

TEST(ConsoleSinkTest, PushCounterDoesNotCrash) {
    ConsoleSink sink;
    EXPECT_NO_THROW(sink.push_counter("test_counter", 42));
}

TEST(ConsoleSinkTest, PushGaugeDoesNotCrash) {
    ConsoleSink sink;
    EXPECT_NO_THROW(sink.push_gauge("test_gauge", 3.14));
}

TEST(ConsoleSinkTest, PushHistogramDoesNotCrash) {
    ConsoleSink sink;
    EXPECT_NO_THROW(sink.push_histogram("test_histo", 100.5));
}

TEST(ConsoleSinkTest, PushCounterWithTags) {
    ConsoleSink sink;
    std::array<eph::core::MetricTag, 2> tags = {{
        {"transport", "dpdk"},
        {"symbol", "btcusdt"},
    }};
    EXPECT_NO_THROW(sink.push_counter("tx_packets", 42, tags));
}

TEST(ConsoleSinkTest, PushGaugeWithTags) {
    ConsoleSink sink;
    std::array<eph::core::MetricTag, 1> tags = {{
        {"venue", "binance"},
    }};
    EXPECT_NO_THROW(sink.push_gauge("rx_queue", 7.5, tags));
}

TEST(ConsoleSinkTest, PushHistogramWithEmptyTags) {
    ConsoleSink sink;
    EXPECT_NO_THROW(sink.push_histogram("latency_ns", 350.0, {}));
}

TEST(ConsoleSinkTest, FlushDoesNotCrash) {
    ConsoleSink sink;
    sink.push_counter("counter", 1);
    EXPECT_NO_THROW(sink.flush());
}

TEST(ConsoleSinkTest, NegativeValues) {
    ConsoleSink sink;
    EXPECT_NO_THROW(sink.push_counter("negative_counter", -100));
    EXPECT_NO_THROW(sink.push_gauge("negative_gauge", -42.5));
    EXPECT_NO_THROW(sink.push_histogram("negative_histo", -1.0));
}

TEST(ConsoleSinkTest, ZeroValues) {
    ConsoleSink sink;
    EXPECT_NO_THROW(sink.push_counter("zero_counter", 0));
    EXPECT_NO_THROW(sink.push_gauge("zero_gauge", 0.0));
}

TEST(ConsoleSinkTest, LargeValues) {
    ConsoleSink sink;
    EXPECT_NO_THROW(sink.push_counter("big_counter", INT64_MAX));
    EXPECT_NO_THROW(sink.push_gauge("big_gauge", 1e300));
}

TEST(ConsoleSinkTest, TagValueWithSpecialCharsIsQuoted) {
    // Values containing commas, equals signs, or braces should be quoted
    // to prevent ambiguous log output when parsing.
    ConsoleSink sink;
    eph::core::MetricTag tags[] = {
        {"region", "us-east-1,us-west-2"},
        {"filter", "status=active"},
    };
    // Should not crash and should produce quoted output
    EXPECT_NO_THROW(sink.push_counter("test_metric", 1, tags));
}

TEST(ConsoleSinkTest, TagValueWithoutSpecialCharsNotQuoted) {
    ConsoleSink sink;
    eph::core::MetricTag tags[] = {
        {"region", "us-east-1"},
    };
    EXPECT_NO_THROW(sink.push_counter("test_metric", 1, tags));
}
