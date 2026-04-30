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

TEST(ConsoleSinkTest, TagValueWithEmbeddedQuoteIsEscaped) {
    // Embedded `"` and `\` inside a quoted tag value must be escaped so
    // the closing quote isn't matched prematurely. Without escaping a
    // value like `foo"bar` would render as `key="foo"bar"`, breaking
    // any downstream parser keyed on quoted regions.
    ConsoleSink sink;
    eph::core::MetricTag tags[] = {
        {"name",  "foo\"bar"},     // embedded double-quote
        {"path",  "C:\\trade\\x"}, // embedded backslashes
    };
    EXPECT_NO_THROW(sink.push_counter("test_metric", 1, tags));
}

// ---------------------------------------------------------------------------
// format_metric_tags — verify the actual output format, not just "no throw".
//
// The pre-existing tests above only checked that push_* doesn't throw, which
// would silently pass even if the escape / quote logic was deleted. These
// tests pin the contract by asserting the exact rendered string.
// ---------------------------------------------------------------------------

TEST(FormatMetricTags, EmptyTagsRendersBraces) {
    EXPECT_EQ(eph::utils::detail::format_metric_tags({}), " {}");
}

TEST(FormatMetricTags, SimpleTagPairUnquoted) {
    eph::core::MetricTag tags[] = {{"region", "us-east-1"}};
    EXPECT_EQ(eph::utils::detail::format_metric_tags(tags),
              " {region=us-east-1}");
}

TEST(FormatMetricTags, MultipleTagsCommaSeparated) {
    eph::core::MetricTag tags[] = {
        {"region", "us-east-1"},
        {"venue",  "binance"},
    };
    EXPECT_EQ(eph::utils::detail::format_metric_tags(tags),
              " {region=us-east-1, venue=binance}");
}

TEST(FormatMetricTags, ValueWithCommaIsQuoted) {
    // Comma in value would otherwise look like a tag separator.
    eph::core::MetricTag tags[] = {{"regions", "us-east-1,us-west-2"}};
    EXPECT_EQ(eph::utils::detail::format_metric_tags(tags),
              " {regions=\"us-east-1,us-west-2\"}");
}

TEST(FormatMetricTags, ValueWithEqualsIsQuoted) {
    eph::core::MetricTag tags[] = {{"filter", "status=active"}};
    EXPECT_EQ(eph::utils::detail::format_metric_tags(tags),
              " {filter=\"status=active\"}");
}

TEST(FormatMetricTags, ValueWithBraceIsQuoted) {
    eph::core::MetricTag tags[] = {{"json", "{a}"}};
    EXPECT_EQ(eph::utils::detail::format_metric_tags(tags),
              " {json=\"{a}\"}");
}

TEST(FormatMetricTags, EmbeddedQuoteIsBackslashEscaped) {
    // Without escaping `foo"bar` would render as `name="foo"bar"`, and a
    // downstream parser keyed on quoted regions would see two empty
    // adjacent quoted strings or stop early at the first inner quote.
    eph::core::MetricTag tags[] = {{"name", "foo\"bar"}};
    EXPECT_EQ(eph::utils::detail::format_metric_tags(tags),
              " {name=\"foo\\\"bar\"}");
}

TEST(FormatMetricTags, EmbeddedBackslashIsEscaped) {
    // `\` is escape-sensitive itself — without backslash-doubling, a
    // literal `C:\trade` followed by anything starting with `"` could
    // be misread as `C:\trade"`. Mirror C/JSON escape semantics.
    eph::core::MetricTag tags[] = {{"path", "C:\\trade"}};
    EXPECT_EQ(eph::utils::detail::format_metric_tags(tags),
              " {path=\"C:\\\\trade\"}");
}

TEST(FormatMetricTags, MixedQuotedAndUnquotedTags) {
    // Verify selective quoting: only the value with special chars gets
    // quoted; the plain one stays bare.
    eph::core::MetricTag tags[] = {
        {"plain",  "abc"},
        {"hostile", "x=1,y=2"},
        {"plain2", "def"},
    };
    EXPECT_EQ(eph::utils::detail::format_metric_tags(tags),
              " {plain=abc, hostile=\"x=1,y=2\", plain2=def}");
}
