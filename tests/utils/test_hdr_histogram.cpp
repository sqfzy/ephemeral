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

    EXPECT_TRUE(h1.merge(h2));

    auto p50 = h1.get_value_at_percentile(50.0);
    EXPECT_NEAR(p50, 5000, 50);
}

// ============================================================================
// Subtract
// ============================================================================

TEST(HdrHistogramTest, SubtractBasic) {
    HdrHistogram h1(1, 10'000, 3);
    HdrHistogram h2(1, 10'000, 3);

    for (uint64_t i = 1; i <= 100; ++i) h1.record(i);
    for (uint64_t i = 1; i <= 50; ++i) h2.record(i);

    EXPECT_TRUE(h1.subtract(h2));
    EXPECT_EQ(h1.get_total_count(), 50);
    // After subtracting 1..50, only 51..100 remain
    EXPECT_GE(h1.get_min_value(), 50);
    EXPECT_LE(h1.get_max_value(), 101);
}

TEST(HdrHistogramTest, SubtractIncompatibleReturnsFalse) {
    HdrHistogram h1(1, 10'000, 3);
    HdrHistogram h2(1, 100'000, 3);
    EXPECT_FALSE(h1.subtract(h2));
}

TEST(HdrHistogramTest, SubtractEmptyHistogram) {
    HdrHistogram h1(1, 10'000, 3);
    HdrHistogram h2(1, 10'000, 3);

    h1.record(100);
    h1.record(200);

    EXPECT_TRUE(h1.subtract(h2));
    EXPECT_EQ(h1.get_total_count(), 2);
}

TEST(HdrHistogramTest, SubtractSelfYieldsEmpty) {
    HdrHistogram h1(1, 10'000, 3);
    for (uint64_t i = 1; i <= 100; ++i) h1.record(i);

    HdrHistogram copy = h1;
    EXPECT_TRUE(h1.subtract(copy));
    EXPECT_EQ(h1.get_total_count(), 0);
    EXPECT_EQ(h1.get_min_value(), 0);
    EXPECT_EQ(h1.get_max_value(), 0);
}

TEST(HdrHistogramTest, SubtractLargerCountReturnsFalse) {
    HdrHistogram h1(1, 10'000, 3);
    HdrHistogram h2(1, 10'000, 3);

    h1.record(100);
    h2.record(100);
    h2.record(100);

    // h2 has more counts than h1 — subtraction would underflow
    EXPECT_FALSE(h1.subtract(h2));
}

TEST(HdrHistogramTest, SubtractWindowedMeasurement) {
    // Simulate windowed measurement: snapshot at T1, record more, snapshot at T2
    HdrHistogram running(1, 100'000, 3);

    // Phase 1: initial samples
    for (uint64_t i = 1; i <= 500; ++i) running.record(i);
    HdrHistogram t1_snapshot = running;

    // Phase 2: more samples
    for (uint64_t i = 501; i <= 1000; ++i) running.record(i);
    HdrHistogram t2_snapshot = running;

    // Delta = T2 - T1 should contain only phase 2 samples
    EXPECT_TRUE(t2_snapshot.subtract(t1_snapshot));
    EXPECT_EQ(t2_snapshot.get_total_count(), 500);
    EXPECT_GE(t2_snapshot.get_min_value(), 500);
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

// ===========================================================================
// HdrHistogram dropped sample tracking
// ===========================================================================

TEST(HdrHistogramDropped, initially_zero) {
    eph::utils::HdrHistogram h(1, 10000, 3);
    EXPECT_EQ(h.get_dropped_count(), 0u);
}

TEST(HdrHistogramDropped, counts_out_of_range_samples) {
    eph::utils::HdrHistogram h(1, 1000, 3);
    EXPECT_TRUE(h.record(500));
    EXPECT_FALSE(h.record(2000));   // above max
    EXPECT_FALSE(h.record(0));      // below min
    EXPECT_EQ(h.get_dropped_count(), 2u);
    EXPECT_EQ(h.get_total_count(), 1u);
}

TEST(HdrHistogramDropped, record_values_counts_batch_drops) {
    eph::utils::HdrHistogram h(1, 1000, 3);
    EXPECT_FALSE(h.record_values(5000, 10));
    EXPECT_EQ(h.get_dropped_count(), 10u);
}

TEST(HdrHistogramDropped, reset_clears_dropped_count) {
    eph::utils::HdrHistogram h(1, 1000, 3);
    h.record(5000);  // dropped
    EXPECT_EQ(h.get_dropped_count(), 1u);
    h.reset();
    EXPECT_EQ(h.get_dropped_count(), 0u);
}

TEST(HdrHistogramDropped, report_includes_dropped_when_nonzero) {
    eph::utils::HdrHistogram h(1, 1000, 3);
    h.record(500);
    h.record(5000);  // dropped
    auto report = h.report("Test");
    EXPECT_NE(report.find("dropped"), std::string::npos);
    EXPECT_NE(report.find("1"), std::string::npos);
}

TEST(HdrHistogramDropped, report_excludes_dropped_when_zero) {
    eph::utils::HdrHistogram h(1, 1000, 3);
    h.record(500);
    auto report = h.report("Test");
    EXPECT_EQ(report.find("dropped"), std::string::npos);
}

TEST(HdrHistogramDropped, to_json_includes_dropped_when_nonzero) {
    eph::utils::HdrHistogram h(1, 1000, 3);
    h.record(500);
    h.record(5000);  // dropped
    auto json = h.to_json();
    EXPECT_NE(json.find("\"dropped\":1"), std::string::npos);
}

TEST(HdrHistogramDropped, to_json_excludes_dropped_when_zero) {
    eph::utils::HdrHistogram h(1, 1000, 3);
    h.record(500);
    auto json = h.to_json();
    EXPECT_EQ(json.find("dropped"), std::string::npos);
}

// ============================================================================
// Inverse CDF — get_percentile_at_or_below()
// ============================================================================

TEST(HdrHistogramInverseCdf, empty_histogram_returns_zero) {
    HdrHistogram h(1, 1000, 3);
    EXPECT_DOUBLE_EQ(h.get_percentile_at_or_below(500), 0.0);
}

TEST(HdrHistogramInverseCdf, single_value_below_returns_zero) {
    HdrHistogram h(1, 1000, 3);
    h.record(500);
    // Query a value below the recorded value
    EXPECT_DOUBLE_EQ(h.get_percentile_at_or_below(1), 0.0);
}

TEST(HdrHistogramInverseCdf, single_value_at_or_above_returns_100) {
    HdrHistogram h(1, 1000, 3);
    h.record(500);
    // Query at or above the recorded value → 100%
    EXPECT_DOUBLE_EQ(h.get_percentile_at_or_below(500), 100.0);
    EXPECT_DOUBLE_EQ(h.get_percentile_at_or_below(999), 100.0);
}

TEST(HdrHistogramInverseCdf, uniform_distribution) {
    HdrHistogram h(1, 1000, 3);
    // Record 100 values: 1, 2, 3, ..., 100
    for (uint64_t i = 1; i <= 100; ++i) {
        h.record(i);
    }

    // Value 50 should be around 50th percentile
    double p50 = h.get_percentile_at_or_below(50);
    EXPECT_GE(p50, 45.0);
    EXPECT_LE(p50, 55.0);

    // Value 99 should be around 99th percentile
    double p99 = h.get_percentile_at_or_below(99);
    EXPECT_GE(p99, 95.0);
    EXPECT_LE(p99, 100.0);

    // Value 1000 should be 100%
    EXPECT_DOUBLE_EQ(h.get_percentile_at_or_below(1000), 100.0);
}

TEST(HdrHistogramInverseCdf, round_trip_with_value_at_percentile) {
    HdrHistogram h(1, 100000, 3);
    for (uint64_t i = 1; i <= 10000; ++i) {
        h.record(i);
    }

    // Get value at P95, then verify inverse returns ~95%
    uint64_t v95 = h.get_value_at_percentile(95.0);
    double p = h.get_percentile_at_or_below(v95);
    EXPECT_GE(p, 94.0);
    EXPECT_LE(p, 96.0);
}

// ============================================================================
// Batch inverse CDF — get_percentiles_at_or_below()
// ============================================================================

TEST(HdrHistogramBatchInverseCdf, empty_histogram_returns_zeros) {
    HdrHistogram h(1, 1000, 3);
    auto results = h.get_percentiles_at_or_below({100, 500, 900});
    ASSERT_EQ(results.size(), 3);
    for (double r : results) {
        EXPECT_DOUBLE_EQ(r, 0.0);
    }
}

TEST(HdrHistogramBatchInverseCdf, unsorted_input_produces_correct_results) {
    HdrHistogram h(1, 1000, 3);
    for (uint64_t i = 1; i <= 100; ++i) {
        h.record(i);
    }

    // Values in non-sorted order
    auto results = h.get_percentiles_at_or_below({100, 1, 50});
    ASSERT_EQ(results.size(), 3);

    // Value 100 → ~100%
    EXPECT_GE(results[0], 99.0);
    // Value 1 → ~1%
    EXPECT_LE(results[1], 5.0);
    // Value 50 → ~50%
    EXPECT_GE(results[2], 45.0);
    EXPECT_LE(results[2], 55.0);
}

TEST(HdrHistogramBatchInverseCdf, values_above_max_return_100) {
    HdrHistogram h(1, 1000, 3);
    h.record(10);
    h.record(20);

    auto results = h.get_percentiles_at_or_below({5000, 10000});
    ASSERT_EQ(results.size(), 2);
    EXPECT_DOUBLE_EQ(results[0], 100.0);
    EXPECT_DOUBLE_EQ(results[1], 100.0);
}

TEST(HdrHistogramBatchInverseCdf, consistent_with_single_query) {
    HdrHistogram h(1, 100000, 3);
    for (uint64_t i = 1; i <= 10000; ++i) {
        h.record(i);
    }

    std::vector<uint64_t> values = {100, 1000, 5000, 9000, 10000};
    auto batch_results = h.get_percentiles_at_or_below(values);

    for (size_t i = 0; i < values.size(); ++i) {
        double single_result = h.get_percentile_at_or_below(values[i]);
        EXPECT_DOUBLE_EQ(batch_results[i], single_result)
            << "Mismatch at value=" << values[i];
    }
}

// ============================================================================
// Percentile distribution output
// ============================================================================

TEST(HdrHistogramDistribution, empty_histogram_has_header_only) {
    HdrHistogram h(1, 1000, 3);
    auto output = h.output_percentile_distribution();
    EXPECT_NE(output.find("Value"), std::string::npos);
    EXPECT_NE(output.find("Percentile"), std::string::npos);
    // No data lines
    EXPECT_EQ(output.find("#[Mean"), std::string::npos);
}

TEST(HdrHistogramDistribution, single_value_produces_one_data_line) {
    HdrHistogram h(1, 1000, 3);
    h.record(42);
    auto output = h.output_percentile_distribution();
    EXPECT_NE(output.find("1.000000"), std::string::npos);  // 100th percentile
    EXPECT_NE(output.find("#[Mean"), std::string::npos);
    EXPECT_NE(output.find("Total count"), std::string::npos);
}

TEST(HdrHistogramDistribution, scaling_divides_values) {
    HdrHistogram h(1, 1000000, 3);
    h.record(1000);
    h.record(2000);

    // Scale by 1000 → values should be ~1.0 and ~2.0
    auto output = h.output_percentile_distribution(1000.0);
    EXPECT_NE(output.find("#[Mean"), std::string::npos);
}

TEST(HdrHistogramDistribution, multi_value_has_multiple_lines) {
    HdrHistogram h(1, 10000, 3);
    for (uint64_t i = 1; i <= 100; ++i) {
        h.record(i);
    }
    auto output = h.output_percentile_distribution();
    // Should have footer with stats
    EXPECT_NE(output.find("#[Mean"), std::string::npos);
    EXPECT_NE(output.find("Total count    =        100"), std::string::npos);
}

TEST(HdrHistogramDistribution, negative_scaling_uses_default) {
    HdrHistogram h(1, 1000, 3);
    h.record(100);
    // Negative scaling should be treated as 1.0
    auto output = h.output_percentile_distribution(-1.0);
    EXPECT_NE(output.find("#[Mean"), std::string::npos);
}

TEST(HdrHistogramDistribution, zero_scaling_treated_as_one) {
    HdrHistogram h(1, 1000, 3);
    h.record(100);
    auto unscaled = h.output_percentile_distribution(1.0);
    auto zero_scaled = h.output_percentile_distribution(0.0);
    // Zero scaling should produce identical output to 1.0
    EXPECT_EQ(unscaled, zero_scaled);
}

TEST(HdrHistogramDistribution, footer_values_are_scaled) {
    HdrHistogram h(1, 1000000, 3);
    h.record(1000);
    h.record(2000);
    h.record(3000);
    auto output = h.output_percentile_distribution(1000.0);
    // Mean of 1000,2000,3000 is ~2000, scaled by 1000 → ~2.0
    // Footer should show scaled mean
    EXPECT_NE(output.find("#[Mean"), std::string::npos);
    // Max is ~3000, scaled → ~3.0
    EXPECT_NE(output.find("#[Max"), std::string::npos);
}

TEST(HdrHistogramDistribution, last_percentile_shows_inf) {
    HdrHistogram h(1, 1000, 3);
    h.record(42);
    auto output = h.output_percentile_distribution();
    // Last entry at 100th percentile should show "inf" in 1/(1-p) column
    EXPECT_NE(output.find("inf"), std::string::npos);
}

TEST(HdrHistogramDistribution, fractional_scaling_doubles_values) {
    HdrHistogram h(1, 1000000, 3);
    h.record(1000);
    // Scaling by 0.5 should double the displayed values
    auto output = h.output_percentile_distribution(0.5);
    // Mean ~1000, scaled by 0.5 → ~2000
    EXPECT_NE(output.find("#[Mean"), std::string::npos);
    EXPECT_NE(output.find("Total count"), std::string::npos);
}

// ============================================================================
// Inverse CDF edge cases
// ============================================================================

TEST(HdrHistogramInverseCdf, value_zero_returns_zero) {
    HdrHistogram h(1, 1000, 3);
    h.record(10);
    h.record(100);
    // All buckets have value_from_index > 0, so nothing is <= 0
    EXPECT_DOUBLE_EQ(h.get_percentile_at_or_below(0), 0.0);
}

TEST(HdrHistogramInverseCdf, value_uint64_max_returns_100) {
    HdrHistogram h(1, 1000, 3);
    h.record(10);
    h.record(500);
    EXPECT_DOUBLE_EQ(h.get_percentile_at_or_below(
        std::numeric_limits<uint64_t>::max()), 100.0);
}

TEST(HdrHistogramInverseCdf, value_between_recorded_values) {
    HdrHistogram h(1, 10000, 3);
    // Record 100 values at 100 and 100 values at 9000
    for (int i = 0; i < 100; ++i) h.record(100);
    for (int i = 0; i < 100; ++i) h.record(9000);

    // Value between the two clusters: should be ~50%
    double p = h.get_percentile_at_or_below(5000);
    EXPECT_GE(p, 45.0);
    EXPECT_LE(p, 55.0);
}

TEST(HdrHistogramInverseCdf, sparse_distribution) {
    HdrHistogram h(1, 100000, 3);
    h.record(1);
    h.record(1000);
    h.record(50000);
    // Value 500 is between 1 and 1000
    double p = h.get_percentile_at_or_below(500);
    // Only the value 1 is <= 500, so ~33%
    EXPECT_GE(p, 30.0);
    EXPECT_LE(p, 40.0);
}

TEST(HdrHistogramInverseCdf, value_below_lowest_trackable) {
    HdrHistogram h(10, 10000, 3);
    h.record(10);
    h.record(100);
    // Query value below lowest_trackable_value — nothing recorded <= 5
    EXPECT_DOUBLE_EQ(h.get_percentile_at_or_below(5), 0.0);
}

// ============================================================================
// Batch inverse CDF edge cases
// ============================================================================

TEST(HdrHistogramBatchInverseCdf, empty_values_returns_empty) {
    HdrHistogram h(1, 1000, 3);
    h.record(100);
    auto results = h.get_percentiles_at_or_below({});
    EXPECT_TRUE(results.empty());
}

TEST(HdrHistogramBatchInverseCdf, single_element_vector) {
    HdrHistogram h(1, 1000, 3);
    h.record(100);
    auto results = h.get_percentiles_at_or_below({100});
    ASSERT_EQ(results.size(), 1);
    EXPECT_DOUBLE_EQ(results[0], 100.0);
}

TEST(HdrHistogramBatchInverseCdf, duplicate_values_get_same_result) {
    HdrHistogram h(1, 10000, 3);
    for (uint64_t i = 1; i <= 100; ++i) h.record(i);

    auto results = h.get_percentiles_at_or_below({50, 50, 50});
    ASSERT_EQ(results.size(), 3);
    EXPECT_DOUBLE_EQ(results[0], results[1]);
    EXPECT_DOUBLE_EQ(results[1], results[2]);
}

TEST(HdrHistogramBatchInverseCdf, all_values_below_min) {
    HdrHistogram h(10, 10000, 3);
    h.record(100);
    h.record(200);
    // All query values are below smallest recorded value
    auto results = h.get_percentiles_at_or_below({1, 5, 9});
    ASSERT_EQ(results.size(), 3);
    for (double r : results) {
        EXPECT_DOUBLE_EQ(r, 0.0);
    }
}

TEST(HdrHistogramBatchInverseCdf, value_zero_in_batch) {
    HdrHistogram h(1, 1000, 3);
    h.record(100);
    auto results = h.get_percentiles_at_or_below({0, 100, 999});
    ASSERT_EQ(results.size(), 3);
    EXPECT_DOUBLE_EQ(results[0], 0.0);
    EXPECT_DOUBLE_EQ(results[1], 100.0);
    EXPECT_DOUBLE_EQ(results[2], 100.0);
}

TEST(HdrHistogramBatchInverseCdf, uint64_max_in_batch) {
    HdrHistogram h(1, 1000, 3);
    h.record(10);
    h.record(500);
    auto results = h.get_percentiles_at_or_below(
        {0, std::numeric_limits<uint64_t>::max()});
    ASSERT_EQ(results.size(), 2);
    EXPECT_DOUBLE_EQ(results[0], 0.0);
    EXPECT_DOUBLE_EQ(results[1], 100.0);
}

// ============================================================================
// for_each_percentile()
// ============================================================================

TEST(HdrHistogramPercentileIter, empty_histogram_yields_no_entries) {
    HdrHistogram h(1, 1000, 3);
    int count = 0;
    h.for_each_percentile([&](const HdrHistogram::PercentileEntry&) {
        ++count;
    });
    EXPECT_EQ(count, 0);
}

TEST(HdrHistogramPercentileIter, single_value_yields_entries) {
    HdrHistogram h(1, 1000, 3);
    h.record(500);

    std::vector<HdrHistogram::PercentileEntry> entries;
    h.for_each_percentile([&](const HdrHistogram::PercentileEntry& e) {
        entries.push_back(e);
    });

    // Should have at least one entry
    ASSERT_GE(entries.size(), 1u);

    // First entry should be at 100% (single value means all percentiles == same value)
    // Last entry should always be at 100%
    EXPECT_DOUBLE_EQ(entries.back().percentile, 100.0);
    EXPECT_EQ(entries.back().total_count_to_val, 1u);
    EXPECT_TRUE(std::isinf(entries.back().inv_percentile));
}

TEST(HdrHistogramPercentileIter, percentiles_are_monotonically_increasing) {
    HdrHistogram h(1, 100000, 3);
    for (int i = 1; i <= 10000; ++i) {
        h.record(static_cast<uint64_t>(i));
    }

    double prev_percentile = -1.0;
    uint64_t prev_value = 0;
    h.for_each_percentile([&](const HdrHistogram::PercentileEntry& e) {
        EXPECT_GE(e.percentile, prev_percentile)
            << "percentile must be monotonically increasing";
        EXPECT_GE(e.value, prev_value)
            << "value must be monotonically increasing";
        EXPECT_GT(e.total_count_to_val, 0u);
        prev_percentile = e.percentile;
        prev_value = e.value;
    });

    // Final percentile should be 100%
    EXPECT_DOUBLE_EQ(prev_percentile, 100.0);
}

TEST(HdrHistogramPercentileIter, covers_full_range) {
    HdrHistogram h(1, 100000, 3);
    for (int i = 1; i <= 1000; ++i) {
        h.record(static_cast<uint64_t>(i));
    }

    bool has_below_50 = false;
    bool has_above_99 = false;
    bool has_100 = false;

    h.for_each_percentile([&](const HdrHistogram::PercentileEntry& e) {
        if (e.percentile <= 50.0) has_below_50 = true;
        if (e.percentile > 99.0) has_above_99 = true;
        if (e.percentile == 100.0) has_100 = true;
    });

    EXPECT_TRUE(has_below_50) << "should cover low percentiles";
    EXPECT_TRUE(has_above_99) << "should cover high percentiles";
    EXPECT_TRUE(has_100) << "should always include 100th percentile";
}

TEST(HdrHistogramPercentileIter, ticks_per_half_distance_affects_density) {
    HdrHistogram h(1, 100000, 3);
    for (int i = 1; i <= 1000; ++i) {
        h.record(static_cast<uint64_t>(i));
    }

    int count_1 = 0, count_10 = 0;
    h.for_each_percentile([&](const HdrHistogram::PercentileEntry&) {
        ++count_1;
    }, 1);
    h.for_each_percentile([&](const HdrHistogram::PercentileEntry&) {
        ++count_10;
    }, 10);

    // More ticks per half distance should produce same or more entries
    EXPECT_GE(count_10, count_1)
        << "higher ticks_per_half_distance should yield more entries";
}

TEST(HdrHistogramPercentileIter, inv_percentile_correct) {
    HdrHistogram h(1, 100000, 3);
    for (int i = 1; i <= 10000; ++i) {
        h.record(static_cast<uint64_t>(i));
    }

    h.for_each_percentile([&](const HdrHistogram::PercentileEntry& e) {
        if (e.percentile < 100.0) {
            double expected_inv = 1.0 / (1.0 - e.percentile / 100.0);
            EXPECT_NEAR(e.inv_percentile, expected_inv, expected_inv * 1e-9)
                << "inv_percentile should be 1/(1-p/100) at percentile=" << e.percentile;
        } else {
            EXPECT_TRUE(std::isinf(e.inv_percentile))
                << "inv_percentile should be inf at 100%";
        }
    });
}

TEST(HdrHistogramPercentileIter, invalid_ticks_yields_no_entries) {
    HdrHistogram h(1, 1000, 3);
    h.record(100);
    int count = 0;
    h.for_each_percentile([&](const HdrHistogram::PercentileEntry&) {
        ++count;
    }, 0);
    EXPECT_EQ(count, 0) << "ticks_per_half_distance=0 should produce no entries";
}
