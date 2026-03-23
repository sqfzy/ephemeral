#include <gtest/gtest.h>

#include <vector>

#include "eph/utils/hdr_histogram.hpp"

using namespace eph::utils;

// ============================================================================
// Construction
// ============================================================================

TEST(HdrHistogramTest, ConstructWithValidParams) {
    EXPECT_NO_THROW(HdrHistogram(1, 3600'000'000ULL, 3));
    EXPECT_NO_THROW(HdrHistogram(1, 100, 1));
    EXPECT_NO_THROW(HdrHistogram(1, 100, 5));
    EXPECT_NO_THROW(HdrHistogram(100, 1'000'000, 2));
}

TEST(HdrHistogramTest, ConstructWithLowestZeroThrows) {
    EXPECT_THROW(HdrHistogram(0, 100, 3), std::invalid_argument);
}

TEST(HdrHistogramTest, ConstructWithHighestTooSmallThrows) {
    // highest must be >= 2 * lowest
    EXPECT_THROW(HdrHistogram(10, 15, 3), std::invalid_argument);
    EXPECT_THROW(HdrHistogram(10, 19, 3), std::invalid_argument);
    EXPECT_NO_THROW(HdrHistogram(10, 20, 3));  // exactly 2x is OK
}

TEST(HdrHistogramTest, ConstructWithInvalidSignificantFiguresThrows) {
    EXPECT_THROW(HdrHistogram(1, 100, 0), std::invalid_argument);
    EXPECT_THROW(HdrHistogram(1, 100, 6), std::invalid_argument);
    EXPECT_THROW(HdrHistogram(1, 100, -1), std::invalid_argument);
}

TEST(HdrHistogramTest, DefaultConstructedIsEmpty) {
    HdrHistogram h;
    EXPECT_EQ(h.get_total_count(), 0);
    EXPECT_EQ(h.get_min_value(), 0);
    EXPECT_EQ(h.get_max_value(), 0);
    EXPECT_DOUBLE_EQ(h.get_mean(), 0.0);
    EXPECT_DOUBLE_EQ(h.get_std_deviation(), 0.0);
}

// ============================================================================
// Record
// ============================================================================

TEST(HdrHistogramTest, RecordSingleValue) {
    HdrHistogram h(1, 10'000, 3);
    EXPECT_TRUE(h.record(42));
    EXPECT_EQ(h.get_total_count(), 1);
    EXPECT_EQ(h.get_min_value(), 42);
    EXPECT_EQ(h.get_max_value(), 42);
}

TEST(HdrHistogramTest, RecordBoundaryValues) {
    HdrHistogram h(1, 10'000, 3);

    // Lowest trackable
    EXPECT_TRUE(h.record(1));
    // Highest trackable
    EXPECT_TRUE(h.record(10'000));
    EXPECT_EQ(h.get_total_count(), 2);
    EXPECT_EQ(h.get_min_value(), 1);
    EXPECT_EQ(h.get_max_value(), 10'000);
}

TEST(HdrHistogramTest, RecordBelowRangeReturnsFalse) {
    HdrHistogram h(10, 10'000, 3);
    EXPECT_FALSE(h.record(0));
    EXPECT_FALSE(h.record(9));
    EXPECT_EQ(h.get_total_count(), 0);
}

TEST(HdrHistogramTest, RecordAboveRangeReturnsFalse) {
    HdrHistogram h(1, 10'000, 3);
    EXPECT_FALSE(h.record(10'001));
    EXPECT_FALSE(h.record(UINT64_MAX));
    EXPECT_EQ(h.get_total_count(), 0);
}

TEST(HdrHistogramTest, RecordManyValues) {
    HdrHistogram h(1, 100'000, 3);

    for (uint64_t i = 1; i <= 10'000; ++i) {
        EXPECT_TRUE(h.record(i));
    }
    EXPECT_EQ(h.get_total_count(), 10'000);
    EXPECT_EQ(h.get_min_value(), 1);
    EXPECT_EQ(h.get_max_value(), 10'000);
}

// ============================================================================
// RecordValues (batch)
// ============================================================================

TEST(HdrHistogramTest, RecordValuesBasic) {
    HdrHistogram h(1, 10'000, 3);
    EXPECT_TRUE(h.record_values(100, 5));
    EXPECT_EQ(h.get_total_count(), 5);
}

TEST(HdrHistogramTest, RecordValuesZeroCountIsNoop) {
    HdrHistogram h(1, 10'000, 3);
    EXPECT_TRUE(h.record_values(100, 0));
    EXPECT_EQ(h.get_total_count(), 0);
}

TEST(HdrHistogramTest, RecordValuesOutOfRangeReturnsFalse) {
    HdrHistogram h(10, 10'000, 3);
    EXPECT_FALSE(h.record_values(5, 10));
    EXPECT_EQ(h.get_total_count(), 0);
}

// ============================================================================
// Reset
// ============================================================================

TEST(HdrHistogramTest, ResetClearsAllData) {
    HdrHistogram h(1, 10'000, 3);
    for (uint64_t i = 1; i <= 100; ++i) h.record(i);

    EXPECT_EQ(h.get_total_count(), 100);
    h.reset();
    EXPECT_EQ(h.get_total_count(), 0);
    EXPECT_EQ(h.get_min_value(), 0);
    EXPECT_EQ(h.get_max_value(), 0);
    EXPECT_DOUBLE_EQ(h.get_mean(), 0.0);
}

TEST(HdrHistogramTest, ResetAllowsReuse) {
    HdrHistogram h(1, 10'000, 3);
    h.record(42);
    h.reset();

    EXPECT_TRUE(h.record(99));
    EXPECT_EQ(h.get_total_count(), 1);
    EXPECT_EQ(h.get_min_value(), 99);
    EXPECT_EQ(h.get_max_value(), 99);
}

// ============================================================================
// Percentile queries
// ============================================================================

TEST(HdrHistogramTest, PercentileOnEmptyHistogramReturnsZero) {
    HdrHistogram h(1, 10'000, 3);
    EXPECT_EQ(h.get_value_at_percentile(50.0), 0);
    EXPECT_EQ(h.get_value_at_percentile(99.0), 0);
}

TEST(HdrHistogramTest, PercentileInvalidInputReturnsZero) {
    HdrHistogram h(1, 10'000, 3);
    h.record(100);
    EXPECT_EQ(h.get_value_at_percentile(-1.0), 0);
    EXPECT_EQ(h.get_value_at_percentile(101.0), 0);
}

TEST(HdrHistogramTest, PercentileSingleValue) {
    HdrHistogram h(1, 10'000, 3);
    h.record(500);

    // All percentiles should return the same value (approximately 500)
    auto p50 = h.get_value_at_percentile(50.0);
    auto p99 = h.get_value_at_percentile(99.0);
    auto p100 = h.get_value_at_percentile(100.0);

    EXPECT_NEAR(p50, 500, 1);
    EXPECT_NEAR(p99, 500, 1);
    EXPECT_NEAR(p100, 500, 1);
}

TEST(HdrHistogramTest, PercentileUniformDistribution) {
    HdrHistogram h(1, 10'000, 3);

    // Record uniform distribution 1..1000
    for (uint64_t i = 1; i <= 1000; ++i) {
        h.record(i);
    }

    auto p50 = h.get_value_at_percentile(50.0);
    auto p90 = h.get_value_at_percentile(90.0);
    auto p99 = h.get_value_at_percentile(99.0);

    // With 3 significant figures, expect ~0.1% relative error
    EXPECT_NEAR(p50, 500, 5);   // within 1%
    EXPECT_NEAR(p90, 900, 9);   // within 1%
    EXPECT_NEAR(p99, 990, 10);  // within 1%
}

TEST(HdrHistogramTest, PercentileP0ReturnsMinimum) {
    HdrHistogram h(1, 10'000, 3);
    for (uint64_t i = 100; i <= 200; ++i) h.record(i);

    // p0 returns the first recorded bucket (ceil(0) = 0, first accumulated > 0 matches)
    auto p0 = h.get_value_at_percentile(0.0);
    EXPECT_GE(p0, 100);
    EXPECT_LE(p0, 101);
}

TEST(HdrHistogramTest, PercentileP100ReturnsMaximum) {
    HdrHistogram h(1, 10'000, 3);
    for (uint64_t i = 100; i <= 200; ++i) h.record(i);

    auto p100 = h.get_value_at_percentile(100.0);
    EXPECT_NEAR(p100, 200, 2);
}

// ============================================================================
// Batch percentiles
// ============================================================================

TEST(HdrHistogramTest, BatchPercentilesEmpty) {
    HdrHistogram h(1, 10'000, 3);
    auto results = h.get_percentiles({50.0, 90.0, 99.0});
    EXPECT_EQ(results.size(), 3);
    for (auto v : results) EXPECT_EQ(v, 0);
}

TEST(HdrHistogramTest, BatchPercentilesEmptyInput) {
    HdrHistogram h(1, 10'000, 3);
    h.record(100);
    auto results = h.get_percentiles({});
    EXPECT_TRUE(results.empty());
}

TEST(HdrHistogramTest, BatchPercentilesMatchSingleQueries) {
    HdrHistogram h(1, 100'000, 3);
    for (uint64_t i = 1; i <= 10'000; ++i) h.record(i);

    std::vector<double> percentiles = {50.0, 90.0, 99.0, 99.9};
    auto batch = h.get_percentiles(percentiles);

    for (size_t i = 0; i < percentiles.size(); ++i) {
        auto single = h.get_value_at_percentile(percentiles[i]);
        EXPECT_EQ(batch[i], single)
            << "Mismatch at percentile " << percentiles[i];
    }
}

TEST(HdrHistogramTest, BatchPercentilesUnsortedInput) {
    HdrHistogram h(1, 10'000, 3);
    for (uint64_t i = 1; i <= 1000; ++i) h.record(i);

    // Percentiles provided out of order
    auto results = h.get_percentiles({99.0, 50.0, 90.0});
    auto p50_direct = h.get_value_at_percentile(50.0);
    auto p90_direct = h.get_value_at_percentile(90.0);
    auto p99_direct = h.get_value_at_percentile(99.0);

    EXPECT_EQ(results[0], p99_direct);
    EXPECT_EQ(results[1], p50_direct);
    EXPECT_EQ(results[2], p90_direct);
}

TEST(HdrHistogramTest, BatchPercentilesWithInvalidValues) {
    HdrHistogram h(1, 10'000, 3);
    h.record(500);

    auto results = h.get_percentiles({-5.0, 50.0, 150.0});
    EXPECT_EQ(results[0], 0);      // invalid negative
    EXPECT_GT(results[1], 0);      // valid p50
    EXPECT_EQ(results[2], 0);      // invalid > 100
}

// ============================================================================
// Statistical queries
// ============================================================================

TEST(HdrHistogramTest, MeanOfSingleValue) {
    HdrHistogram h(1, 10'000, 3);
    h.record(1000);
    EXPECT_NEAR(h.get_mean(), 1000.0, 1.0);
}

TEST(HdrHistogramTest, MeanOfUniformDistribution) {
    HdrHistogram h(1, 100'000, 3);
    for (uint64_t i = 1; i <= 10'000; ++i) h.record(i);

    // Expected mean ≈ 5000.5
    EXPECT_NEAR(h.get_mean(), 5000.5, 50.0);
}

TEST(HdrHistogramTest, StdDeviationOfConstantValue) {
    HdrHistogram h(1, 10'000, 3);
    for (int i = 0; i < 1000; ++i) h.record(500);

    // All same value → stddev ≈ 0
    EXPECT_NEAR(h.get_std_deviation(), 0.0, 1.0);
}

TEST(HdrHistogramTest, StdDeviationEmpty) {
    HdrHistogram h(1, 10'000, 3);
    EXPECT_DOUBLE_EQ(h.get_std_deviation(), 0.0);
}

TEST(HdrHistogramTest, MinMaxTracking) {
    HdrHistogram h(1, 100'000, 3);
    h.record(50);
    h.record(5000);
    h.record(100);

    EXPECT_EQ(h.get_min_value(), 50);
    EXPECT_EQ(h.get_max_value(), 5000);
}

TEST(HdrHistogramTest, MinValueEmptyReturnsZero) {
    HdrHistogram h(1, 10'000, 3);
    EXPECT_EQ(h.get_min_value(), 0);
}

// ============================================================================
// ForEachRecordedValue
// ============================================================================

TEST(HdrHistogramTest, ForEachRecordedValueVisitsAllBuckets) {
    HdrHistogram h(1, 10'000, 3);
    h.record(10);
    h.record(100);
    h.record(1000);

    uint64_t total = 0;
    int bucket_count = 0;
    h.for_each_recorded_value([&](uint64_t /*value*/, uint64_t count) {
        total += count;
        bucket_count++;
    });

    EXPECT_EQ(total, 3);
    // Each value lands in a different bucket (widely spaced)
    EXPECT_EQ(bucket_count, 3);
}

TEST(HdrHistogramTest, ForEachRecordedValueEmptyHistogram) {
    HdrHistogram h(1, 10'000, 3);
    int visits = 0;
    h.for_each_recorded_value([&](uint64_t, uint64_t) { visits++; });
    EXPECT_EQ(visits, 0);
}

// ============================================================================
// Merge
// ============================================================================

TEST(HdrHistogramTest, MergeCompatibleHistograms) {
    HdrHistogram h1(1, 10'000, 3);
    HdrHistogram h2(1, 10'000, 3);

    for (uint64_t i = 1; i <= 500; ++i) h1.record(i);
    for (uint64_t i = 501; i <= 1000; ++i) h2.record(i);

    EXPECT_TRUE(h1.merge(h2));
    EXPECT_EQ(h1.get_total_count(), 1000);
    EXPECT_EQ(h1.get_min_value(), 1);
    EXPECT_EQ(h1.get_max_value(), 1000);
}

TEST(HdrHistogramTest, MergeIncompatibleHistogramsReturnsFalse) {
    HdrHistogram h1(1, 10'000, 3);
    HdrHistogram h2(1, 100'000, 3);  // different range

    h1.record(100);
    h2.record(100);

    EXPECT_FALSE(h1.merge(h2));
    EXPECT_EQ(h1.get_total_count(), 1);  // unchanged
}

TEST(HdrHistogramTest, MergeEmptyHistogram) {
    HdrHistogram h1(1, 10'000, 3);
    HdrHistogram h2(1, 10'000, 3);

    h1.record(42);
    EXPECT_TRUE(h1.merge(h2));
    EXPECT_EQ(h1.get_total_count(), 1);
}

TEST(HdrHistogramTest, MergeIntoEmptyHistogram) {
    HdrHistogram h1(1, 10'000, 3);
    HdrHistogram h2(1, 10'000, 3);

    h2.record(42);
    EXPECT_TRUE(h1.merge(h2));
    EXPECT_EQ(h1.get_total_count(), 1);
    EXPECT_EQ(h1.get_min_value(), 42);
}

TEST(HdrHistogramTest, MergePreservesPercentileAccuracy) {
    HdrHistogram h1(1, 100'000, 3);
    HdrHistogram h2(1, 100'000, 3);

    // h1: lower half, h2: upper half
    for (uint64_t i = 1; i <= 5000; ++i) h1.record(i);
    for (uint64_t i = 5001; i <= 10'000; ++i) h2.record(i);

    h1.merge(h2);

    auto p50 = h1.get_value_at_percentile(50.0);
    EXPECT_NEAR(p50, 5000, 50);
}

// ============================================================================
// IsCompatible
// ============================================================================

TEST(HdrHistogramTest, IsCompatibleSameParams) {
    HdrHistogram h1(1, 10'000, 3);
    HdrHistogram h2(1, 10'000, 3);
    EXPECT_TRUE(h1.is_compatible(h2));
}

TEST(HdrHistogramTest, IsCompatibleDifferentRange) {
    HdrHistogram h1(1, 10'000, 3);
    HdrHistogram h2(1, 100'000, 3);
    EXPECT_FALSE(h1.is_compatible(h2));
}

TEST(HdrHistogramTest, IsCompatibleDifferentLowest) {
    HdrHistogram h1(1, 10'000, 3);
    HdrHistogram h2(10, 10'000, 3);
    EXPECT_FALSE(h1.is_compatible(h2));
}

// ============================================================================
// Memory and report
// ============================================================================

TEST(HdrHistogramTest, MemorySizeIsPositive) {
    HdrHistogram h(1, 10'000, 3);
    EXPECT_GT(h.get_memory_size(), sizeof(HdrHistogram));
}

TEST(HdrHistogramTest, MemorySizeGrowsWithRange) {
    HdrHistogram small(1, 100, 3);
    HdrHistogram large(1, 10'000'000, 3);
    EXPECT_GT(large.get_memory_size(), small.get_memory_size());
}

TEST(HdrHistogramTest, ReportEmptyHistogram) {
    HdrHistogram h(1, 10'000, 3);
    auto report = h.report("Test");
    EXPECT_TRUE(report.find("empty") != std::string::npos);
}

TEST(HdrHistogramTest, ReportWithData) {
    HdrHistogram h(1, 10'000, 3);
    for (uint64_t i = 1; i <= 100; ++i) h.record(i);

    auto report = h.report("Latency", "ns");
    EXPECT_TRUE(report.find("100 samples") != std::string::npos);
    EXPECT_TRUE(report.find("ns") != std::string::npos);
    EXPECT_TRUE(report.find("p50") != std::string::npos);
}

TEST(HdrHistogramTest, ToJsonEmptyHistogram) {
    HdrHistogram h(1, 10'000, 3);
    EXPECT_EQ(h.to_json(), "{\"count\":0}");
}

TEST(HdrHistogramTest, ToJsonWithData) {
    HdrHistogram h(1, 10'000, 3);
    h.record(500);

    auto json = h.to_json();
    EXPECT_TRUE(json.find("\"count\":1") != std::string::npos);
    EXPECT_TRUE(json.find("\"min\":500") != std::string::npos);
    EXPECT_TRUE(json.find("\"max\":500") != std::string::npos);
}

// ============================================================================
// Precision / significant figures
// ============================================================================

TEST(HdrHistogramTest, PrecisionOneDigit) {
    HdrHistogram h(1, 10'000, 1);
    h.record(1234);

    auto p100 = h.get_value_at_percentile(100.0);
    // With 1 significant digit, value should be in the right ballpark
    // but less precise than 3 digits
    EXPECT_GT(p100, 1000);
    EXPECT_LT(p100, 2000);
}

TEST(HdrHistogramTest, PrecisionFiveDigits) {
    HdrHistogram h(1, 10'000, 5);
    h.record(1234);

    auto p100 = h.get_value_at_percentile(100.0);
    // With 5 significant digits, should be very close
    EXPECT_NEAR(p100, 1234, 2);
}

TEST(HdrHistogramTest, HighPrecisionUsesMoreMemory) {
    HdrHistogram h1(1, 10'000, 1);
    HdrHistogram h5(1, 10'000, 5);
    EXPECT_GT(h5.get_memory_size(), h1.get_memory_size());
}

// ============================================================================
// Wide range (simulating latency recording: 1ns to 1hr)
// ============================================================================

TEST(HdrHistogramTest, WideRangeHistogram) {
    HdrHistogram h(1, 3'600'000'000'000ULL, 3);  // 1ns to 1hr

    EXPECT_TRUE(h.record(1));                       // 1 ns
    EXPECT_TRUE(h.record(1000));                    // 1 µs
    EXPECT_TRUE(h.record(1'000'000));               // 1 ms
    EXPECT_TRUE(h.record(1'000'000'000));           // 1 s
    EXPECT_TRUE(h.record(3'600'000'000'000ULL));    // 1 hr

    EXPECT_EQ(h.get_total_count(), 5);
    EXPECT_EQ(h.get_min_value(), 1);
    EXPECT_EQ(h.get_max_value(), 3'600'000'000'000ULL);
}

// ============================================================================
// Edge case: power-of-two boundaries
// ============================================================================

TEST(HdrHistogramTest, PowerOfTwoBoundaryValues) {
    HdrHistogram h(1, 1'000'000, 3);

    // Record values at power-of-two boundaries where bucket transitions happen
    for (int exp = 0; exp < 20; ++exp) {
        uint64_t val = 1ULL << exp;
        if (val >= 1 && val <= 1'000'000) {
            EXPECT_TRUE(h.record(val)) << "Failed at 2^" << exp;
        }
    }

    EXPECT_GE(h.get_total_count(), 15);  // at least 2^0 through 2^19
}

TEST(HdrHistogramTest, RecordValueAtExactHighest) {
    HdrHistogram h(1, 1024, 3);
    EXPECT_TRUE(h.record(1024));
    EXPECT_EQ(h.get_total_count(), 1);
    EXPECT_EQ(h.get_max_value(), 1024);
}

// ============================================================================
// Copy/Move semantics
// ============================================================================

TEST(HdrHistogramTest, CopyPreservesData) {
    HdrHistogram h1(1, 10'000, 3);
    for (uint64_t i = 1; i <= 100; ++i) h1.record(i);

    HdrHistogram h2 = h1;  // copy
    EXPECT_EQ(h2.get_total_count(), 100);
    EXPECT_EQ(h2.get_min_value(), 1);
    EXPECT_EQ(h2.get_max_value(), 100);

    // Original unaffected by further modifications to copy
    h2.record(5000);
    EXPECT_EQ(h1.get_total_count(), 100);
    EXPECT_EQ(h2.get_total_count(), 101);
}

TEST(HdrHistogramTest, MoveTransfersOwnership) {
    HdrHistogram h1(1, 10'000, 3);
    for (uint64_t i = 1; i <= 100; ++i) h1.record(i);

    HdrHistogram h2 = std::move(h1);
    EXPECT_EQ(h2.get_total_count(), 100);
    EXPECT_EQ(h2.get_min_value(), 1);
    EXPECT_EQ(h2.get_max_value(), 100);
}

// ============================================================================
// Stats serialization (dump / to_json / std::formatter)
// ============================================================================

static Stats make_test_stats() {
    return Stats{
        .name = "TestBench",
        .count = 1000,
        .avg_ns = 42.5,
        .min_ns = 10.0,
        .max_ns = 500.0,
        .p50_ns = 35.0,
        .p90_ns = 80.0,
        .p99_ns = 200.0,
        .p999_ns = 450.0,
        .stddev_ns = 25.3,
    };
}

TEST(StatsTest, DumpNonEmpty) {
    auto s = make_test_stats();
    auto d = s.dump();
    EXPECT_NE(d.find("TestBench"), std::string::npos);
    EXPECT_NE(d.find("1000 samples"), std::string::npos);
    EXPECT_NE(d.find("42.5"), std::string::npos);  // avg
    EXPECT_NE(d.find("p99"), std::string::npos);
}

TEST(StatsTest, DumpEmpty) {
    Stats s{.name = "Empty", .count = 0};
    auto d = s.dump();
    EXPECT_NE(d.find("empty"), std::string::npos);
}

TEST(StatsTest, ToJsonNonEmpty) {
    auto s = make_test_stats();
    auto j = s.to_json();
    // Verify it's valid JSON-ish structure
    EXPECT_EQ(j.front(), '{');
    EXPECT_EQ(j.back(), '}');
    EXPECT_NE(j.find("\"name\":\"TestBench\""), std::string::npos);
    EXPECT_NE(j.find("\"count\":1000"), std::string::npos);
    EXPECT_NE(j.find("\"p99_ns\":"), std::string::npos);
}

TEST(StatsTest, ToJsonEmpty) {
    Stats s{.name = "Empty", .count = 0};
    auto j = s.to_json();
    EXPECT_NE(j.find("\"count\":0"), std::string::npos);
}

TEST(StatsTest, Formatter) {
    auto s = make_test_stats();
    auto formatted = std::format("{}", s);
    EXPECT_NE(formatted.find("TestBench"), std::string::npos);
    EXPECT_NE(formatted.find("n=1000"), std::string::npos);
}

TEST(StatsTest, FormatterEmpty) {
    Stats s{.name = "NoData", .count = 0};
    auto formatted = std::format("{}", s);
    EXPECT_NE(formatted.find("no samples"), std::string::npos);
}

TEST(StatsTest, DumpSingleSample) {
    Stats s{.name = "One", .count = 1, .avg_ns = 100.0, .min_ns = 100.0,
            .max_ns = 100.0, .p50_ns = 100.0, .p90_ns = 100.0,
            .p99_ns = 100.0, .p999_ns = 100.0, .stddev_ns = 0.0};
    auto d = s.dump();
    EXPECT_NE(d.find("1 samples"), std::string::npos);
    EXPECT_NE(d.find("stddev: 0.0"), std::string::npos);
}

TEST(StatsTest, ToJsonExtremeValues) {
    Stats s{.name = "Extreme", .count = UINT64_MAX,
            .avg_ns = 1e18, .min_ns = 0.001, .max_ns = 1e18,
            .p50_ns = 1e9, .p90_ns = 1e12, .p99_ns = 1e15,
            .p999_ns = 1e18, .stddev_ns = 1e12};
    auto j = s.to_json();
    EXPECT_EQ(j.front(), '{');
    EXPECT_EQ(j.back(), '}');
    // Should contain the extreme count
    EXPECT_NE(j.find(std::to_string(UINT64_MAX)), std::string::npos);
}

TEST(StatsTest, ToJsonZeroValues) {
    Stats s{.name = "AllZero", .count = 42, .avg_ns = 0.0, .min_ns = 0.0,
            .max_ns = 0.0, .p50_ns = 0.0, .p90_ns = 0.0,
            .p99_ns = 0.0, .p999_ns = 0.0, .stddev_ns = 0.0};
    auto j = s.to_json();
    EXPECT_NE(j.find("\"count\":42"), std::string::npos);
    EXPECT_NE(j.find("\"avg_ns\":0.00"), std::string::npos);
}

TEST(StatsTest, AllThreeSerializationsConsistent) {
    auto s = make_test_stats();
    auto d = s.dump();
    auto j = s.to_json();
    auto f = std::format("{}", s);

    // All three should contain the name and count
    EXPECT_NE(d.find("TestBench"), std::string::npos);
    EXPECT_NE(j.find("TestBench"), std::string::npos);
    EXPECT_NE(f.find("TestBench"), std::string::npos);

    // dump and to_json should contain p99
    EXPECT_NE(d.find("200.0"), std::string::npos);   // p99 in dump
    EXPECT_NE(j.find("200.00"), std::string::npos);   // p99 in json
}
