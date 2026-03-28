#include <gtest/gtest.h>

#include "eph/core/metrics_concept.hpp"
#include "eph/utils/console_sink.hpp"

#include <array>
#include <string>
#include <vector>

using namespace eph::core;
using namespace eph::utils;

// ── NullSink tests ──────────────────────────────────────────────────────────

TEST(NullSink, SatisfiesMetricsSinkConcept) {
    static_assert(MetricsSink<NullSink>);
}

TEST(NullSink, PushCounterIsNoOp) {
    NullSink sink;
    sink.push_counter("test_counter", 42);
    sink.push_counter("test_counter", 42, {});
}

TEST(NullSink, PushGaugeIsNoOp) {
    NullSink sink;
    sink.push_gauge("test_gauge", 3.14);
}

TEST(NullSink, PushHistogramIsNoOp) {
    NullSink sink;
    sink.push_histogram("test_histo", 123.456);
}

TEST(NullSink, FlushIsNoOp) {
    NullSink sink;
    sink.flush();
}

TEST(NullSink, PushWithTags) {
    NullSink sink;
    std::array<MetricTag, 2> tags{{
        {"transport", "dpdk"},
        {"symbol", "btcusdt"},
    }};
    sink.push_counter("tx_packets", 100, tags);
    sink.push_gauge("queue_depth", 7.5, tags);
    sink.push_histogram("latency_ns", 350.0, tags);
}

// ── ConsoleSink tests ───────────────────────────────────────────────────────

TEST(ConsoleSink, SatisfiesMetricsSinkConcept) {
    static_assert(MetricsSink<ConsoleSink>);
}

TEST(ConsoleSink, PushCounterLogs) {
    ConsoleSink sink;
    sink.push_counter("packets_sent", 1000);
}

TEST(ConsoleSink, PushGaugeLogs) {
    ConsoleSink sink;
    sink.push_gauge("queue_fill_ratio", 0.75);
}

TEST(ConsoleSink, PushHistogramLogs) {
    ConsoleSink sink;
    sink.push_histogram("rx_latency_ns", 425.5);
}

TEST(ConsoleSink, PushWithTagsFormatsCorrectly) {
    ConsoleSink sink;
    std::array<MetricTag, 2> tags{{
        {"transport", "dpdk"},
        {"symbol", "ethusdt"},
    }};
    sink.push_counter("tx_bytes", 65536, tags);
    sink.push_gauge("spread_bps", 1.23, tags);
    sink.push_histogram("rtt_ns", 800.0, tags);
}

TEST(ConsoleSink, FlushDoesNotThrow) {
    ConsoleSink sink;
    sink.push_counter("test", 1);
    sink.flush();
}

TEST(ConsoleSink, EmptyTagsFormatted) {
    ConsoleSink sink;
    std::span<const MetricTag> empty_tags;
    sink.push_counter("test", 0, empty_tags);
}

// ── MetricTag tests ─────────────────────────────────────────────────────────

TEST(MetricTag, ConstructFromStringViews) {
    MetricTag tag{"key", "value"};
    EXPECT_EQ(tag.key, "key");
    EXPECT_EQ(tag.value, "value");
}

TEST(MetricTag, SpanFromArray) {
    std::array<MetricTag, 3> tags{{
        {"a", "1"},
        {"b", "2"},
        {"c", "3"},
    }};
    std::span<const MetricTag> span{tags};
    EXPECT_EQ(span.size(), 3);
    EXPECT_EQ(span[0].key, "a");
    EXPECT_EQ(span[2].value, "3");
}

// ── Custom sink test (verify concept works with user types) ─────────────────

struct RecordingSink {
    struct Record {
        std::string name;
        double value;
        std::string type;
    };
    std::vector<Record> records;

    void push_counter(std::string_view name, int64_t value,
                      std::span<const MetricTag> = {}) noexcept {
        records.push_back({std::string(name), static_cast<double>(value), "counter"});
    }
    void push_gauge(std::string_view name, double value,
                    std::span<const MetricTag> = {}) noexcept {
        records.push_back({std::string(name), value, "gauge"});
    }
    void push_histogram(std::string_view name, double value,
                        std::span<const MetricTag> = {}) noexcept {
        records.push_back({std::string(name), value, "histogram"});
    }
    void flush() noexcept {}
};

static_assert(MetricsSink<RecordingSink>);

TEST(CustomSink, RecordingSinkCapturesMetrics) {
    RecordingSink sink;
    sink.push_counter("packets", 42);
    sink.push_gauge("depth", 3.5);
    sink.push_histogram("latency", 100.0);

    ASSERT_EQ(sink.records.size(), 3);
    EXPECT_EQ(sink.records[0].name, "packets");
    EXPECT_EQ(sink.records[0].value, 42.0);
    EXPECT_EQ(sink.records[0].type, "counter");
    EXPECT_EQ(sink.records[1].name, "depth");
    EXPECT_DOUBLE_EQ(sink.records[1].value, 3.5);
    EXPECT_EQ(sink.records[2].type, "histogram");
}

// ── Generic function accepting any MetricsSink ──────────────────────────────

template <MetricsSink Sink>
void push_transport_stats(Sink& sink, int64_t tx, int64_t rx) {
    sink.push_counter("tx_packets", tx);
    sink.push_counter("rx_packets", rx);
    sink.push_gauge("tx_rx_ratio", static_cast<double>(tx) / static_cast<double>(rx));
}

TEST(GenericSink, TemplatedFunctionWorksWithNullSink) {
    NullSink sink;
    push_transport_stats(sink, 1000, 2000);
}

TEST(GenericSink, TemplatedFunctionWorksWithRecordingSink) {
    RecordingSink sink;
    push_transport_stats(sink, 100, 200);
    ASSERT_EQ(sink.records.size(), 3);
    EXPECT_EQ(sink.records[0].name, "tx_packets");
    EXPECT_EQ(sink.records[1].name, "rx_packets");
    EXPECT_EQ(sink.records[2].name, "tx_rx_ratio");
    EXPECT_DOUBLE_EQ(sink.records[2].value, 0.5);
}
