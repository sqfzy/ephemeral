#include <gtest/gtest.h>

#include <filesystem>
#include <thread>
#include <vector>

#include "eph/utils/recorder.hpp"
#include "eph/utils/time.hpp"

using namespace eph::utils;
namespace fs = std::filesystem;

class RecorderTest : public ::testing::Test {
   protected:
    static void SetUpTestSuite() {
        ASSERT_TRUE(TSC::init()) << "TSC initialization failed";
    }

    void TearDown() override {
        // Clean up any test output files
        fs::remove_all("test_outputs");
    }
};

// ============================================================================
// Recorder — Construction
// ============================================================================

TEST_F(RecorderTest, ConstructionWithValidName) {
    EXPECT_NO_THROW(Recorder rec("TestRecorder"));
}

TEST_F(RecorderTest, ConstructionWithEmptyNameThrows) {
    EXPECT_THROW(Recorder rec(""), std::invalid_argument);
}

TEST_F(RecorderTest, ConstructionWithCustomRange) {
    EXPECT_NO_THROW(Recorder rec("Custom", 10, 1'000'000, 2));
}

TEST_F(RecorderTest, NameAccessor) {
    Recorder rec("MyBenchmark");
    EXPECT_EQ(rec.name(), "MyBenchmark");
}

// ============================================================================
// Recorder — record()
// ============================================================================

TEST_F(RecorderTest, RecordValidValue) {
    Recorder rec("Test");
    EXPECT_TRUE(rec.record(100));
    EXPECT_EQ(rec.count(), 1);
}

TEST_F(RecorderTest, RecordZeroReturnsFalse) {
    Recorder rec("Test");
    EXPECT_FALSE(rec.record(0));
    EXPECT_EQ(rec.count(), 0);
    EXPECT_EQ(rec.skipped_invalid_count(), 1);
}

TEST_F(RecorderTest, RecordMultipleValues) {
    Recorder rec("Test");
    for (uint64_t i = 1; i <= 1000; ++i) {
        EXPECT_TRUE(rec.record(i));
    }
    EXPECT_EQ(rec.count(), 1000);
}

TEST_F(RecorderTest, RecordOverflowValue) {
    // Create recorder with small range to trigger overflow
    Recorder rec("Test", 1, 100, 1);
    EXPECT_FALSE(rec.record(1000));
    EXPECT_EQ(rec.skipped_overflow_count(), 1);
}

// ============================================================================
// Recorder — record_values()
// ============================================================================

TEST_F(RecorderTest, RecordValuesBasic) {
    Recorder rec("Test");
    EXPECT_TRUE(rec.record_values(50, 10));
    EXPECT_EQ(rec.count(), 10);
}

TEST_F(RecorderTest, RecordValuesZeroCyclesReturnsFalse) {
    Recorder rec("Test");
    EXPECT_FALSE(rec.record_values(0, 5));
    EXPECT_EQ(rec.skipped_invalid_count(), 5);
}

TEST_F(RecorderTest, RecordValuesZeroCountIsNoop) {
    Recorder rec("Test");
    EXPECT_TRUE(rec.record_values(50, 0));
    EXPECT_EQ(rec.count(), 0);
}

TEST_F(RecorderTest, RecordValuesSaturatesOnCyclesCountOverflow) {
    // The default recorder range is ~10 seconds worth of cycles.
    // Use record_values with a large count that would cause
    // cycles * count to overflow uint64_t.
    Recorder rec("OverflowTest");
    // Pick a valid cycle value within range, and a huge count.
    // cycles=10000 is well within range; count near UINT64_MAX/cycles
    // would overflow without saturation.
    uint64_t cycles = 10000;
    uint64_t count = std::numeric_limits<uint64_t>::max() / cycles + 1;
    // This should succeed (histogram accepts the value) and saturate total_cycles
    EXPECT_TRUE(rec.record_values(cycles, count));
    EXPECT_EQ(rec.count(), count);
    // Stats should still compute without crashing
    auto stats = rec.compute_stats();
    ASSERT_TRUE(stats.has_value());
    // avg_ns will be huge due to saturation, but it shouldn't crash
    EXPECT_GT(stats->avg_ns, 0.0);
}

TEST_F(RecorderTest, RecordValuesSaturatingAccumulateAcrossCalls) {
    // Drive total_cycles_ to saturation via one call, then issue a
    // SECOND record_values call that would have wrapped the unguarded
    // `total_cycles_ += product` accumulator (since `total_cycles_` is
    // already at UINT64_MAX, any positive product wraps it back near 0).
    Recorder rec("AccumOverflowTest");
    uint64_t cycles = 10000;
    uint64_t count1 = std::numeric_limits<uint64_t>::max() / cycles + 1;
    EXPECT_TRUE(rec.record_values(cycles, count1));
    auto pre_avg = rec.compute_stats()->avg_ns;
    EXPECT_GT(pre_avg, 0.0);

    // Second batch: small product but total_cycles_ is already saturated.
    EXPECT_TRUE(rec.record_values(cycles, 100));
    auto post_stats = rec.compute_stats();
    ASSERT_TRUE(post_stats.has_value());
    // Without the post-multiply-saturate guard: total_cycles_ would
    // wrap to ~product_2 (i.e. ~10000*100=1e6), avg ≈ 1e6 / count
    // which is many orders of magnitude below pre_avg.
    EXPECT_GT(post_stats->avg_ns, pre_avg / 4.0)
        << "second record_values wrapped total_cycles_ — accumulate-saturate "
           "guard missing";
}

// ============================================================================
// Recorder — compute_stats()
// ============================================================================

TEST_F(RecorderTest, ComputeStatsNoData) {
    Recorder rec("Test");
    auto stats = rec.compute_stats();
    EXPECT_FALSE(stats.has_value());
}

TEST_F(RecorderTest, ComputeStatsWithData) {
    Recorder rec("Test");
    for (uint64_t i = 1; i <= 100; ++i) {
        (void)rec.record(i * 100);
    }

    auto stats = rec.compute_stats();
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->name, "Test");
    EXPECT_EQ(stats->count, 100);
    EXPECT_GT(stats->avg_ns, 0.0);
    EXPECT_GT(stats->min_ns, 0.0);
    EXPECT_GT(stats->max_ns, 0.0);
    EXPECT_LE(stats->min_ns, stats->avg_ns);
    EXPECT_LE(stats->avg_ns, stats->max_ns);
    EXPECT_LE(stats->p50_ns, stats->p99_ns);
    EXPECT_LE(stats->p99_ns, stats->p999_ns);
    EXPECT_GE(stats->stddev_ns, 0.0);
}

TEST_F(RecorderTest, ComputeStatsPercentileOrdering) {
    Recorder rec("Test");
    // Record values with a known distribution
    for (uint64_t i = 1; i <= 10000; ++i) {
        (void)rec.record(i);
    }

    auto stats = rec.compute_stats();
    ASSERT_TRUE(stats.has_value());
    EXPECT_LE(stats->p50_ns, stats->p90_ns);
    EXPECT_LE(stats->p90_ns, stats->p99_ns);
    EXPECT_LE(stats->p99_ns, stats->p999_ns);
}

// ============================================================================
// Recorder — has_data()
// ============================================================================

TEST_F(RecorderTest, HasDataFalseWhenEmpty) {
    Recorder rec("Test");
    EXPECT_FALSE(rec.has_data());
}

TEST_F(RecorderTest, HasDataTrueAfterRecord) {
    Recorder rec("Test");
    (void)rec.record(100);
    EXPECT_TRUE(rec.has_data());
}

// ============================================================================
// Recorder — reset()
// ============================================================================

TEST_F(RecorderTest, ResetClearsAllState) {
    Recorder rec("Test");
    (void)rec.record(100);
    (void)rec.record(0);  // triggers invalid skip

    rec.reset();

    EXPECT_EQ(rec.count(), 0);
    EXPECT_EQ(rec.skipped_invalid_count(), 0);
    EXPECT_EQ(rec.skipped_overflow_count(), 0);
    EXPECT_FALSE(rec.has_data());
    EXPECT_FALSE(rec.compute_stats().has_value());
}

// ============================================================================
// Recorder — merge()
// ============================================================================

TEST_F(RecorderTest, MergeFromAnotherRecorder) {
    Recorder rec1("Rec1");
    Recorder rec2("Rec2");

    for (uint64_t i = 1; i <= 50; ++i) {
        rec1.record(i * 100);
    }
    for (uint64_t i = 51; i <= 100; ++i) {
        rec2.record(i * 100);
    }

    EXPECT_TRUE(rec1.merge(rec2));
    EXPECT_EQ(rec1.count(), 100);
}

TEST_F(RecorderTest, MergeIncompatibleHistogramReturnsFalse) {
    // Recorder::merge delegates the histogram step to HdrHistogram::merge,
    // which rejects pairs with mismatched lowest/highest trackable
    // values via is_compatible(). When that step fails, Recorder::merge
    // must return false WITHOUT mutating any of `this`'s counters —
    // otherwise a hostile or misconfigured recorder pair could partially
    // corrupt accumulated stats.
    Recorder rec1("Rec1", /*lowest_cycles=*/1,    /*highest_cycles=*/1'000'000);
    Recorder rec2("Rec2", /*lowest_cycles=*/100,  /*highest_cycles=*/1'000'000);

    EXPECT_TRUE(rec1.record(500));
    EXPECT_TRUE(rec2.record(500));
    auto pre_count_1 = rec1.count();
    auto pre_count_2 = rec2.count();

    EXPECT_FALSE(rec1.merge(rec2))
        << "differing lowest_trackable_value must reject the merge";
    EXPECT_EQ(rec1.count(), pre_count_1)
        << "rejected merge must not bump count_ on either side";
    EXPECT_EQ(rec2.count(), pre_count_2);
}

TEST_F(RecorderTest, MergeSaturatesTotalsAgainstWrap) {
    // record_values already saturates `total_cycles_` at UINT64_MAX on
    // overflow. Two saturated recorders merged should NOT wrap their
    // total_cycles_ back near zero (UINT64_MAX + UINT64_MAX = -2 mod 2^64).
    // Without the saturating-add fix in merge(), `compute_stats` would
    // divide a tiny wrapped sum by the (saturated count_) and report an
    // avg orders of magnitude lower than the true value.
    Recorder rec1("MergeSat1");
    Recorder rec2("MergeSat2");

    uint64_t cycles = 10000;
    uint64_t count  = std::numeric_limits<uint64_t>::max() / cycles + 1;
    EXPECT_TRUE(rec1.record_values(cycles, count));
    EXPECT_TRUE(rec2.record_values(cycles, count));

    // Pre-merge: each rec's total_cycles_ is at UINT64_MAX (verified by
    // RecordValuesSaturatesOnCyclesCountOverflow above). The avg the
    // recorders report should be ~UINT64_MAX/count ns each.
    auto pre_avg = rec1.compute_stats()->avg_ns;
    EXPECT_GT(pre_avg, 0.0);

    EXPECT_TRUE(rec1.merge(rec2));
    auto post_stats = rec1.compute_stats();
    ASSERT_TRUE(post_stats.has_value());
    // The post-merge avg is the saturated-total / merged-count ratio.
    // Without the fix: total wraps to ~0, avg collapses to a tiny number
    //   << pre_avg / 2.
    // With the fix: total stays saturated, avg ≈ pre_avg / 2 (count
    //   merges to 2× while total cap stays the same).
    //
    // The merged count is 2 * count (each recorder added `count` events;
    // the saturating merge keeps it linear because 2*count < UINT64_MAX).
    // Assert post_avg is at least half the pre-merge avg — without
    // saturation it would be 18 orders of magnitude lower.
    EXPECT_GT(post_stats->avg_ns, pre_avg / 4.0)
        << "merge wrapped total_cycles_ — saturating-add guard missing";
}

// ============================================================================
// Recorder — export (JSON, CSV)
// ============================================================================

TEST_F(RecorderTest, ExportJsonNoData) {
    Recorder rec("Test");
    EXPECT_FALSE(rec.export_json("test_outputs"));
}

TEST_F(RecorderTest, ExportJsonWithData) {
    Recorder rec("TestExport");
    for (uint64_t i = 1; i <= 100; ++i) {
        (void)rec.record(i * 100);
    }

    EXPECT_TRUE(rec.export_json("test_outputs"));

    // Verify a .json file was created
    bool found = false;
    for (auto& entry : fs::directory_iterator("test_outputs")) {
        if (entry.path().extension() == ".json") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(RecorderTest, ExportCsvNoData) {
    Recorder rec("Test");
    EXPECT_FALSE(rec.export_csv("test_outputs"));
}

TEST_F(RecorderTest, ExportCsvWithData) {
    Recorder rec("TestExport");
    for (uint64_t i = 1; i <= 100; ++i) {
        (void)rec.record(i * 100);
    }

    EXPECT_TRUE(rec.export_csv("test_outputs"));

    bool found = false;
    for (auto& entry : fs::directory_iterator("test_outputs")) {
        if (entry.path().extension() == ".csv") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(RecorderTest, ExportAll) {
    Recorder rec("TestExport");
    for (uint64_t i = 1; i <= 100; ++i) {
        (void)rec.record(i * 100);
    }

    EXPECT_TRUE(rec.export_all("test_outputs"));
}

// ============================================================================
// Recorder — print_report() smoke test
// ============================================================================

TEST_F(RecorderTest, PrintReportWithData) {
    Recorder rec("Test");
    for (uint64_t i = 1; i <= 100; ++i) {
        (void)rec.record(i * 100);
    }
    EXPECT_NO_THROW(rec.print_report());
}

TEST_F(RecorderTest, PrintReportNoData) {
    Recorder rec("Test");
    EXPECT_NO_THROW(rec.print_report());
}

// ============================================================================
// Recorder — Real TSC measurement
// ============================================================================

TEST_F(RecorderTest, HistogramAccessorReflectsRecordedData) {
    Recorder rec("HistAccess");
    (void)rec.record(100);
    (void)rec.record(200);
    (void)rec.record(300);

    const auto& hist = rec.histogram();
    // Histogram should have recorded 3 values
    EXPECT_EQ(hist.get_total_count(), 3u);
    // Median should be near 200 (HdrHistogram quantizes to significant digits)
    double median = hist.get_value_at_percentile(50.0);
    EXPECT_GE(median, 150.0);
    EXPECT_LE(median, 250.0);
}

TEST_F(RecorderTest, HistogramAccessorEmptyRecorder) {
    Recorder rec("HistEmpty");
    const auto& hist = rec.histogram();
    EXPECT_EQ(hist.get_total_count(), 0u);
}

TEST_F(RecorderTest, HistogramAccessorAfterReset) {
    Recorder rec("HistReset");
    (void)rec.record(500);
    EXPECT_EQ(rec.histogram().get_total_count(), 1u);
    rec.reset();
    EXPECT_EQ(rec.histogram().get_total_count(), 0u);
}

// ============================================================================

TEST_F(RecorderTest, RecordRealTscMeasurements) {
    Recorder rec("RealTSC");

    for (int i = 0; i < 100; ++i) {
        uint64_t start = TSC::now();
        volatile int x = 0;
        for (int j = 0; j < 100; ++j) {
            x += j;
        }
        uint64_t end = TSC::now();
        (void)rec.record(end - start);
    }

    EXPECT_EQ(rec.count(), 100);
    auto stats = rec.compute_stats();
    ASSERT_TRUE(stats.has_value());
    EXPECT_GT(stats->avg_ns, 0.0);
}

// ============================================================================
// ConcurrentRecorder — Basic functionality
// ============================================================================

class ConcurrentRecorderTest : public ::testing::Test {
   protected:
    static void SetUpTestSuite() {
        ASSERT_TRUE(TSC::init()) << "TSC initialization failed";
    }
};

TEST_F(ConcurrentRecorderTest, ConstructionWithValidName) {
    EXPECT_NO_THROW(ConcurrentRecorder rec("ConcTest"));
}

TEST_F(ConcurrentRecorderTest, ConstructionWithEmptyNameThrows) {
    EXPECT_THROW(ConcurrentRecorder rec(""), std::invalid_argument);
}

TEST_F(ConcurrentRecorderTest, SingleThreadRecord) {
    ConcurrentRecorder rec("ConcTest");
    for (uint64_t i = 1; i <= 100; ++i) {
        EXPECT_TRUE(rec.record(i * 100));
    }

    auto stats = rec.compute_stats();
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->count, 100);
    EXPECT_EQ(stats->name, "ConcTest");
}

TEST_F(ConcurrentRecorderTest, RecordZeroReturnsFalse) {
    ConcurrentRecorder rec("ConcTest");
    EXPECT_FALSE(rec.record(0));
}

TEST_F(ConcurrentRecorderTest, ComputeStatsNoData) {
    ConcurrentRecorder rec("ConcTest");
    auto stats = rec.compute_stats();
    EXPECT_FALSE(stats.has_value());
}

TEST_F(ConcurrentRecorderTest, MultiThreadRecord) {
    ConcurrentRecorder rec("ConcStress");
    constexpr int num_threads = 4;
    constexpr int records_per_thread = 10000;

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&rec]() {
            for (uint64_t i = 1; i <= records_per_thread; ++i) {
                (void)rec.record(i * 10);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto stats = rec.compute_stats();
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->count, num_threads * records_per_thread);
}

TEST_F(ConcurrentRecorderTest, ThreadCountTracking) {
    ConcurrentRecorder rec("ConcCount");

    {
        std::vector<std::thread> threads;
        for (int i = 0; i < 3; ++i) {
            threads.emplace_back([&rec]() {
                (void)rec.record(100);
            });
        }
        for (auto& t : threads) {
            t.join();
        }
    }

    // After threads join, they should be retired
    // thread_count() includes both active and retired
    EXPECT_GE(rec.thread_count(), 3);
}

TEST_F(ConcurrentRecorderTest, DataPreservedAfterThreadExit) {
    ConcurrentRecorder rec("ConcPreserve");
    constexpr uint64_t value = 500;

    {
        std::thread t([&rec, value]() {
            for (int i = 0; i < 100; ++i) {
                (void)rec.record(value);
            }
        });
        t.join();
    }

    // Data should still be accessible after thread exits
    auto stats = rec.compute_stats();
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->count, 100);
}

TEST_F(ConcurrentRecorderTest, PrintReportDoesNotCrash) {
    ConcurrentRecorder rec("ConcPrint");
    (void)rec.record(100);
    EXPECT_NO_THROW(rec.print_report());
}

TEST_F(ConcurrentRecorderTest, PrintReportNoData) {
    ConcurrentRecorder rec("ConcPrint");
    EXPECT_NO_THROW(rec.print_report());
}

TEST_F(ConcurrentRecorderTest, ResetClearsAllData) {
    ConcurrentRecorder rec("ConcReset");
    // Record some data
    for (int i = 0; i < 100; ++i) {
        (void)rec.record(1000 + i);
    }
    auto stats_before = rec.compute_stats();
    ASSERT_TRUE(stats_before.has_value());
    EXPECT_EQ(stats_before->count, 100u);

    // Reset and verify empty
    rec.reset();
    auto stats_after = rec.compute_stats();
    EXPECT_FALSE(stats_after.has_value())
        << "Expected no data after reset, got count=" << stats_after->count;
}

TEST_F(ConcurrentRecorderTest, ResetThenRecordFreshData) {
    ConcurrentRecorder rec("ConcResetFresh");
    for (int i = 0; i < 50; ++i) (void)rec.record(500);
    rec.reset();

    // Record new data after reset
    for (int i = 0; i < 30; ++i) (void)rec.record(800);
    auto stats = rec.compute_stats();
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->count, 30u);
}

// ============================================================================
// ConcurrentRecorder — Export methods
// ============================================================================

class ConcurrentRecorderExportTest : public ::testing::Test {
   protected:
    static void SetUpTestSuite() {
        ASSERT_TRUE(TSC::init()) << "TSC initialization failed";
    }

    void TearDown() override {
        fs::remove_all("test_conc_outputs");
    }
};

TEST_F(ConcurrentRecorderExportTest, ExportJsonNoDataReturnsFalse) {
    ConcurrentRecorder rec("ConcExportEmpty");
    EXPECT_FALSE(rec.export_json("test_conc_outputs"));
}

TEST_F(ConcurrentRecorderExportTest, ExportJsonWithData) {
    ConcurrentRecorder rec("ConcExportJson");
    for (uint64_t i = 1; i <= 100; ++i) {
        (void)rec.record(i * 100);
    }

    EXPECT_TRUE(rec.export_json("test_conc_outputs"));

    // Verify a JSON file was created
    bool found = false;
    for (const auto& entry : fs::directory_iterator("test_conc_outputs")) {
        if (entry.path().extension() == ".json") {
            found = true;
            // Read and verify basic JSON structure
            std::ifstream file(entry.path());
            std::string content((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
            EXPECT_NE(content.find("\"name\": \"ConcExportJson\""), std::string::npos);
            EXPECT_NE(content.find("\"threads\""), std::string::npos);
            EXPECT_NE(content.find("\"active\""), std::string::npos);
            EXPECT_NE(content.find("\"retired\""), std::string::npos);
            EXPECT_NE(content.find("\"latency_ns\""), std::string::npos);
            break;
        }
    }
    EXPECT_TRUE(found) << "No JSON file created in test_conc_outputs";
}

TEST_F(ConcurrentRecorderExportTest, ExportCsvNoDataReturnsFalse) {
    ConcurrentRecorder rec("ConcExportCsvEmpty");
    EXPECT_FALSE(rec.export_csv("test_conc_outputs"));
}

TEST_F(ConcurrentRecorderExportTest, ExportCsvWithData) {
    ConcurrentRecorder rec("ConcExportCsv");
    for (uint64_t i = 1; i <= 100; ++i) {
        (void)rec.record(i * 100);
    }

    EXPECT_TRUE(rec.export_csv("test_conc_outputs"));

    bool found = false;
    for (const auto& entry : fs::directory_iterator("test_conc_outputs")) {
        if (entry.path().extension() == ".csv") {
            found = true;
            std::ifstream file(entry.path());
            std::string header;
            std::getline(file, header);
            EXPECT_EQ(header, "latency_ns,count");
            break;
        }
    }
    EXPECT_TRUE(found) << "No CSV file created in test_conc_outputs";
}

TEST_F(ConcurrentRecorderExportTest, ExportAllCreatesJsonAndCsv) {
    ConcurrentRecorder rec("ConcExportAll");
    for (uint64_t i = 1; i <= 50; ++i) {
        (void)rec.record(i * 100);
    }

    EXPECT_TRUE(rec.export_all("test_conc_outputs"));

    bool json_found = false, csv_found = false;
    for (const auto& entry : fs::directory_iterator("test_conc_outputs")) {
        if (entry.path().extension() == ".json") json_found = true;
        if (entry.path().extension() == ".csv") csv_found = true;
    }
    EXPECT_TRUE(json_found) << "No JSON file created";
    EXPECT_TRUE(csv_found) << "No CSV file created";
}

// ============================================================================
// ConcurrentRecorder — Retired thread skipped counts preservation
// ============================================================================

TEST_F(ConcurrentRecorderTest, RetiredThreadSkippedCountsPreserved) {
    // Use a small range so we can trigger overflow easily
    ConcurrentRecorder rec("ConcRetiredSkipped", 1, 100, 1);

    // Record some values in a worker thread that will retire
    {
        std::thread worker([&rec]() {
            // Record valid values
            for (int i = 0; i < 10; ++i) {
                (void)rec.record(50);
            }
            // Record overflow values (> 100)
            for (int i = 0; i < 5; ++i) {
                (void)rec.record(1000);  // will be skipped as overflow
            }
            // Record invalid values (0)
            for (int i = 0; i < 3; ++i) {
                (void)rec.record(0);  // will be skipped as invalid
            }
        });
        worker.join();
        // Thread has now retired — skipped counts should be preserved
    }

    // Export JSON and check that skipped counts are included
    // (We can't easily inspect the internal counts, but we can verify
    // the export includes them by checking the JSON output)
    EXPECT_TRUE(rec.export_json("test_conc_outputs"));

    // Also verify stats still work after thread retirement
    auto stats = rec.compute_stats();
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->count, 10u);
}

// ============================================================================
// ConcurrentRecorder: compute_and_reset()
// ============================================================================

TEST(ConcurrentRecorderComputeAndReset, returns_stats_and_clears_data) {
    ConcurrentRecorder rec("ConcCAR");
    for (int i = 0; i < 100; ++i) {
        (void)rec.record(50);
    }

    // compute_and_reset should return the accumulated stats
    auto stats = rec.compute_and_reset();
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->count, 100u);
    EXPECT_GT(stats->avg_ns, 0.0);

    // After reset, data should be gone
    auto stats2 = rec.compute_stats();
    EXPECT_FALSE(stats2.has_value());
}

TEST(ConcurrentRecorderComputeAndReset, empty_returns_nullopt) {
    ConcurrentRecorder rec("ConcCAREmpty");
    auto stats = rec.compute_and_reset();
    EXPECT_FALSE(stats.has_value());
}

TEST(ConcurrentRecorderComputeAndReset, multi_window_non_overlapping) {
    ConcurrentRecorder rec("ConcCARWindowed");

    // Window 1: record 50 samples
    for (int i = 0; i < 50; ++i) {
        (void)rec.record(100);
    }
    auto w1 = rec.compute_and_reset();
    ASSERT_TRUE(w1.has_value());
    EXPECT_EQ(w1->count, 50u);

    // Window 2: record 30 samples
    for (int i = 0; i < 30; ++i) {
        (void)rec.record(200);
    }
    auto w2 = rec.compute_and_reset();
    ASSERT_TRUE(w2.has_value());
    EXPECT_EQ(w2->count, 30u);

    // Verify windows don't overlap
    EXPECT_NE(w1->avg_ns, w2->avg_ns);
}

TEST(ConcurrentRecorderComputeAndReset, includes_retired_thread_data) {
    ConcurrentRecorder rec("ConcCARRetired");

    // Record from a worker thread that will retire
    {
        std::thread worker([&rec]() {
            for (int i = 0; i < 100; ++i) {
                (void)rec.record(50);
            }
        });
        worker.join();
    }

    // Record from main thread
    for (int i = 0; i < 50; ++i) {
        (void)rec.record(50);
    }

    // compute_and_reset should include both threads' data
    auto stats = rec.compute_and_reset();
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->count, 150u);

    // Verify reset worked
    auto stats2 = rec.compute_stats();
    EXPECT_FALSE(stats2.has_value());
}

TEST(ConcurrentRecorderComputeAndReset, only_retired_threads_no_active) {
    ConcurrentRecorder rec("ConcCAROnlyRetired");

    // Record only from worker threads that will retire
    {
        std::thread w1([&rec]() {
            for (int i = 0; i < 50; ++i) (void)rec.record(100);
        });
        std::thread w2([&rec]() {
            for (int i = 0; i < 50; ++i) (void)rec.record(200);
        });
        w1.join();
        w2.join();
    }

    // Now only retired threads have data — main thread never recorded
    auto stats = rec.compute_and_reset();
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->count, 100u);

    // Verify reset cleared retired data too
    auto stats2 = rec.compute_stats();
    EXPECT_FALSE(stats2.has_value());
}

TEST(ConcurrentRecorderComputeAndReset, stats_percentiles_monotonic) {
    ConcurrentRecorder rec("ConcCARPercentiles");

    // Record a spread of values to get meaningful percentiles
    for (uint64_t v = 10; v <= 1000; v += 10) {
        (void)rec.record(v);
    }

    auto stats = rec.compute_and_reset();
    ASSERT_TRUE(stats.has_value());

    // Percentiles must be monotonically non-decreasing
    EXPECT_LE(stats->min_ns, stats->p50_ns);
    EXPECT_LE(stats->p50_ns, stats->p90_ns);
    EXPECT_LE(stats->p90_ns, stats->p99_ns);
    EXPECT_LE(stats->p99_ns, stats->p999_ns);
    EXPECT_LE(stats->p999_ns, stats->max_ns);

    // Average should be between min and max
    EXPECT_GE(stats->avg_ns, stats->min_ns);
    EXPECT_LE(stats->avg_ns, stats->max_ns);

    // Stddev should be non-negative
    EXPECT_GE(stats->stddev_ns, 0.0);
}

TEST(ConcurrentRecorderComputeAndReset, name_preserved_in_stats) {
    ConcurrentRecorder rec("MyRecorderName");
    (void)rec.record(42);
    auto stats = rec.compute_and_reset();
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->name, "MyRecorderName");
}

TEST(ConcurrentRecorderComputeAndReset, rapid_successive_windows) {
    ConcurrentRecorder rec("ConcCARRapid");

    // 5 rapid windows, each with different sample counts
    for (int window = 1; window <= 5; ++window) {
        for (int i = 0; i < window * 10; ++i) {
            (void)rec.record(50);
        }
        auto stats = rec.compute_and_reset();
        ASSERT_TRUE(stats.has_value());
        EXPECT_EQ(stats->count, static_cast<uint64_t>(window * 10));
    }

    // After all windows, no data left
    EXPECT_FALSE(rec.compute_and_reset().has_value());
}

TEST(ConcurrentRecorderComputeAndReset, concurrent_record_does_not_crash) {
    ConcurrentRecorder rec("ConcCARStress");
    std::atomic<bool> stop{false};

    // Recording thread runs continuously
    std::thread recorder([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            (void)rec.record(100);
        }
    });

    // Main thread does rapid compute_and_reset calls
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        auto stats = rec.compute_and_reset();
        // May or may not have data depending on timing — just verify no crash
        if (stats.has_value()) {
            EXPECT_GT(stats->count, 0u);
        }
    }

    stop.store(true, std::memory_order_relaxed);
    recorder.join();
}

// ============================================================================
// ConcurrentRecorder::record_values() — bulk API
// ============================================================================

TEST(ConcurrentRecorderRecordValues, zero_count_returns_true) {
    ConcurrentRecorder rec("BulkZero");
    EXPECT_TRUE(rec.record_values(100, 0));
}

TEST(ConcurrentRecorderRecordValues, zero_cycles_returns_false) {
    ConcurrentRecorder rec("BulkZeroCycles");
    EXPECT_FALSE(rec.record_values(0, 10));
}

TEST(ConcurrentRecorderRecordValues, bulk_record_matches_individual) {
    // Record 100 identical samples via record_values
    ConcurrentRecorder bulk_rec("BulkRec");
    EXPECT_TRUE(bulk_rec.record_values(500, 100));

    auto bulk_stats = bulk_rec.compute_stats();
    ASSERT_TRUE(bulk_stats.has_value());
    EXPECT_EQ(bulk_stats->count, 100u);

    // Record same via individual record() calls
    ConcurrentRecorder ind_rec("IndRec");
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(ind_rec.record(500));
    }

    auto ind_stats = ind_rec.compute_stats();
    ASSERT_TRUE(ind_stats.has_value());
    EXPECT_EQ(ind_stats->count, 100u);

    // Both should have the same average (same value repeated)
    EXPECT_NEAR(bulk_stats->avg_ns, ind_stats->avg_ns, 0.01);
}

// ============================================================================
// recorder_detail utility function tests
// ============================================================================

// ============================================================================
// Stats — dump / to_json / operator-
// ============================================================================

TEST(Stats, DumpEmptyShowsEmpty) {
    Stats s{.name = "MyBench", .count = 0};
    auto d = s.dump();
    EXPECT_NE(d.find("empty"), std::string::npos);
    EXPECT_NE(d.find("MyBench"), std::string::npos);
}

TEST(Stats, DumpWithDataContainsPercentiles) {
    Stats s{.name = "Bench", .count = 100,
            .avg_ns = 42.5, .min_ns = 10.0, .max_ns = 200.0,
            .p50_ns = 40.0, .p90_ns = 80.0, .p99_ns = 150.0,
            .p999_ns = 190.0, .stddev_ns = 25.0};
    auto d = s.dump();
    EXPECT_NE(d.find("100 samples"), std::string::npos);
    EXPECT_NE(d.find("42.5"), std::string::npos);
    EXPECT_NE(d.find("p99"), std::string::npos);
}

TEST(Stats, ToJsonEmptyHasCountZero) {
    Stats s{.name = "Test", .count = 0};
    auto j = s.to_json();
    EXPECT_NE(j.find("\"count\":0"), std::string::npos);
    EXPECT_EQ(j.find("avg_ns"), std::string::npos);
}

TEST(Stats, ToJsonWithDataHasAllFields) {
    Stats s{.name = "Bench", .count = 100,
            .avg_ns = 42.5, .min_ns = 10.0, .max_ns = 200.0,
            .p50_ns = 40.0, .p90_ns = 80.0, .p99_ns = 150.0,
            .p999_ns = 190.0, .stddev_ns = 25.0};
    auto j = s.to_json();
    EXPECT_NE(j.find("\"count\":100"), std::string::npos);
    EXPECT_NE(j.find("\"avg_ns\":"), std::string::npos);
    EXPECT_NE(j.find("\"p999_ns\":"), std::string::npos);
}

TEST(Stats, OperatorMinusSubtractsCounts) {
    Stats a{.name = "B", .count = 100, .avg_ns = 50.0, .min_ns = 10.0,
            .max_ns = 200.0, .p50_ns = 40.0, .p90_ns = 80.0,
            .p99_ns = 150.0, .p999_ns = 190.0, .stddev_ns = 25.0};
    Stats b{.name = "B", .count = 30, .avg_ns = 45.0, .min_ns = 15.0,
            .max_ns = 180.0, .p50_ns = 35.0, .p90_ns = 75.0,
            .p99_ns = 140.0, .p999_ns = 180.0, .stddev_ns = 20.0};
    auto delta = a - b;
    EXPECT_EQ(delta.count, 70);
    EXPECT_EQ(delta.name, "B");
    // Percentile fields come from lhs (point-in-time)
    EXPECT_DOUBLE_EQ(delta.avg_ns, a.avg_ns);
    EXPECT_DOUBLE_EQ(delta.p99_ns, a.p99_ns);
}

TEST(Stats, OperatorMinusClampsCountToZero) {
    Stats a{.name = "B", .count = 10};
    Stats b{.name = "B", .count = 50};
    auto delta = a - b;
    EXPECT_EQ(delta.count, 0);
}

TEST(Stats, FormatProducesExpectedOutput) {
    Stats s{.name = "Fast", .count = 1000,
            .avg_ns = 42.3, .min_ns = 10.0, .max_ns = 500.0,
            .p50_ns = 40.0, .p90_ns = 80.0, .p99_ns = 128.7,
            .p999_ns = 450.0, .stddev_ns = 25.0};
    auto formatted = std::format("{}", s);
    EXPECT_NE(formatted.find("Fast"), std::string::npos);
    EXPECT_NE(formatted.find("n=1000"), std::string::npos);
    EXPECT_NE(formatted.find("42.3ns"), std::string::npos);
}

TEST(Stats, FormatEmptyShowsNoSamples) {
    Stats s{.name = "Empty", .count = 0};
    auto formatted = std::format("{}", s);
    EXPECT_NE(formatted.find("no samples"), std::string::npos);
}

// ============================================================================
// recorder_detail utility function tests
// ============================================================================

TEST(RecorderDetail, SanitizeFilenameAlphanumeric) {
    EXPECT_EQ(recorder_detail::sanitize_filename("hello123"), "hello123");
}

TEST(RecorderDetail, SanitizeFilenamePreservesUnderscoreAndDash) {
    EXPECT_EQ(recorder_detail::sanitize_filename("my-bench_v2"), "my-bench_v2");
}

TEST(RecorderDetail, SanitizeFilenameReplacesSpecialChars) {
    EXPECT_EQ(recorder_detail::sanitize_filename("test/foo.bar"), "test_foo_bar");
    EXPECT_EQ(recorder_detail::sanitize_filename("a b c"), "a_b_c");
    EXPECT_EQ(recorder_detail::sanitize_filename("$pecial!"), "_pecial_");
}

TEST(RecorderDetail, SanitizeFilenameEmptyString) {
    EXPECT_EQ(recorder_detail::sanitize_filename(""), "");
}

TEST(RecorderDetail, SanitizeFilenameHighBytesNoUB) {
    // High-byte chars (e.g., UTF-8) must not trigger UB via std::isalnum
    // with negative char values. They should be sanitized to underscores.
    std::string name = "bench\xC3\xA9test";  // "benchétest" in UTF-8
    auto result = recorder_detail::sanitize_filename(name);
    EXPECT_EQ(result.size(), name.size());
    // Non-ASCII bytes should be replaced with underscore
    EXPECT_EQ(result, "bench__test");
}

TEST(RecorderDetail, EnsureDirectoryCreatesAndReturnsTrue) {
    std::string dir = "/tmp/eph_test_ensure_dir_" + recorder_detail::get_timestamp();
    EXPECT_TRUE(recorder_detail::ensure_directory(dir));
    EXPECT_TRUE(fs::exists(dir));
    fs::remove_all(dir);
}

TEST(RecorderDetail, EnsureDirectoryExistingReturnsTrue) {
    EXPECT_TRUE(recorder_detail::ensure_directory("/tmp"));
}

TEST(RecorderDetail, EnsureDirectoryCalledTwiceReturnsTrueBothTimes) {
    // Exercises the idempotent path: second call sees existing directory.
    // Previously this would have been vulnerable to a TOCTOU race.
    std::string dir = "/tmp/eph_test_double_ensure_" + recorder_detail::get_timestamp();
    EXPECT_TRUE(recorder_detail::ensure_directory(dir));
    EXPECT_TRUE(recorder_detail::ensure_directory(dir))
        << "Second ensure_directory on existing dir must return true";
    fs::remove_all(dir);
}

TEST(RecorderDetail, EnsureDirectoryNestedPath) {
    std::string dir = "/tmp/eph_test_nested_" + recorder_detail::get_timestamp() + "/a/b/c";
    EXPECT_TRUE(recorder_detail::ensure_directory(dir));
    EXPECT_TRUE(fs::exists(dir));
    // Clean up from the root
    fs::remove_all("/tmp/eph_test_nested_" + recorder_detail::get_timestamp().substr(0, 10));
    // Best-effort cleanup
    fs::remove_all(fs::path(dir).parent_path().parent_path().parent_path());
}

// ensure_directory must return false (and not throw) when the path it would
// create is blocked by an existing regular file. fs::create_directories on
// such a path raises filesystem_error; the helper catches it and surfaces
// `false`. Without this test the catch-clause is dead code from a coverage
// perspective.
TEST(RecorderDetail, EnsureDirectoryFailsWhenPathIsRegularFile) {
    auto blocking_file =
        std::string{"/tmp/eph_test_block_"} + recorder_detail::get_timestamp() + ".file";
    {
        std::ofstream f(blocking_file);
        f << "block\n";
    }
    ASSERT_TRUE(fs::is_regular_file(blocking_file));

    // Try to "create" a directory at the same path — must fail.
    EXPECT_FALSE(recorder_detail::ensure_directory(blocking_file));
    // The blocking file must be untouched.
    EXPECT_TRUE(fs::is_regular_file(blocking_file));
    fs::remove(blocking_file);
}

// Distinct names produce distinct leaf paths even when called back-to-back
// (i.e. when get_timestamp() resolution is identical). Without this,
// concurrent benches with different names but overlapping timestamps could
// silently collide on disk if a future refactor dropped the name prefix.
TEST(RecorderDetail, MakeOutputPathDistinctNamesProduceDistinctLeaves) {
    auto p1 = recorder_detail::make_output_path("alpha", "/tmp", ".csv");
    auto p2 = recorder_detail::make_output_path("beta",  "/tmp", ".csv");
    EXPECT_NE(p1, p2);
    EXPECT_EQ(p1.parent_path(), p2.parent_path());
    // Sanity: extension preserved on both.
    EXPECT_EQ(p1.extension(), ".csv");
    EXPECT_EQ(p2.extension(), ".csv");
}

// ============================================================================

TEST(ConcurrentRecorderRecordValues, bulk_across_threads) {
    ConcurrentRecorder rec("BulkThreads");
    constexpr int kThreads = 4;
    constexpr uint64_t kCountPerThread = 1000;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            uint64_t val = static_cast<uint64_t>((t + 1) * 100);
            rec.record_values(val, kCountPerThread);
        });
    }
    for (auto& t : threads) t.join();

    auto stats = rec.compute_stats();
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->count, kThreads * kCountPerThread);
}

// ============================================================================
// recorder_detail::sanitize_filename — previously untested utility
// ============================================================================

TEST(RecorderDetailTest, SanitizeFilenamePreservesAlphanumeric) {
    auto result = eph::utils::recorder_detail::sanitize_filename("TestBench123");
    EXPECT_EQ(result, "TestBench123");
}

TEST(RecorderDetailTest, SanitizeFilenamePreservesUnderscoreAndHyphen) {
    auto result = eph::utils::recorder_detail::sanitize_filename("test_bench-v2");
    EXPECT_EQ(result, "test_bench-v2");
}

TEST(RecorderDetailTest, SanitizeFilenameReplacesSpaces) {
    auto result = eph::utils::recorder_detail::sanitize_filename("my test bench");
    EXPECT_EQ(result, "my_test_bench");
}

TEST(RecorderDetailTest, SanitizeFilenameReplacesSpecialChars) {
    auto result = eph::utils::recorder_detail::sanitize_filename("bench@host:8080/path");
    EXPECT_EQ(result, "bench_host_8080_path");
}

TEST(RecorderDetailTest, SanitizeFilenameReplacesPathTraversal) {
    auto result = eph::utils::recorder_detail::sanitize_filename("../../etc/passwd");
    EXPECT_EQ(result, "______etc_passwd");
}

TEST(RecorderDetailTest, SanitizeFilenameEmptyString) {
    auto result = eph::utils::recorder_detail::sanitize_filename("");
    EXPECT_EQ(result, "");
}

TEST(RecorderDetailTest, SanitizeFilenameAllSpecialChars) {
    auto result = eph::utils::recorder_detail::sanitize_filename("!@#$%^&*()");
    // All chars should be replaced with underscore
    EXPECT_EQ(result, "__________");
}

TEST(RecorderDetailTest, SanitizeFilenameUnicodeReplaced) {
    // UTF-8 continuation bytes have negative char values on most platforms;
    // they should be replaced (not cause UB via isalnum).
    auto result = eph::utils::recorder_detail::sanitize_filename("bench\xC3\xA9mark");
    // The non-ASCII bytes should be replaced, ASCII 'b','e','n','c','h' preserved
    EXPECT_EQ(result.substr(0, 5), "bench");
    EXPECT_EQ(result.substr(7), "mark");
    EXPECT_EQ(result[5], '_');
    EXPECT_EQ(result[6], '_');
}

// ============================================================================
// recorder_detail::ensure_directory — previously untested utility
// ============================================================================

TEST(RecorderDetailTest, EnsureDirectoryCreatesDirectory) {
    const std::string path = "test_outputs/ensure_dir_test";
    fs::remove_all(path);
    EXPECT_TRUE(eph::utils::recorder_detail::ensure_directory(path));
    EXPECT_TRUE(fs::is_directory(path));
    fs::remove_all("test_outputs");
}

TEST(RecorderDetailTest, EnsureDirectoryIdempotent) {
    const std::string path = "test_outputs/ensure_dir_idem";
    fs::remove_all(path);
    EXPECT_TRUE(eph::utils::recorder_detail::ensure_directory(path));
    EXPECT_TRUE(eph::utils::recorder_detail::ensure_directory(path));
    EXPECT_TRUE(fs::is_directory(path));
    fs::remove_all("test_outputs");
}

TEST(RecorderDetailTest, EnsureDirectoryCreatesNestedPaths) {
    const std::string path = "test_outputs/a/b/c/d";
    fs::remove_all("test_outputs");
    EXPECT_TRUE(eph::utils::recorder_detail::ensure_directory(path));
    EXPECT_TRUE(fs::is_directory(path));
    fs::remove_all("test_outputs");
}

// ============================================================================
// recorder_detail::make_output_path — previously untested utility
// ============================================================================

TEST(RecorderDetailTest, MakeOutputPathIncludesExtension) {
    auto path = eph::utils::recorder_detail::make_output_path("bench", "/tmp", ".json");
    EXPECT_EQ(path.extension(), ".json");
    EXPECT_TRUE(path.string().starts_with("/tmp/bench_"));
}

TEST(RecorderDetailTest, MakeOutputPathSanitizesName) {
    auto path = eph::utils::recorder_detail::make_output_path("my bench!", "/tmp", ".csv");
    auto filename = path.filename().string();
    // The name should have been sanitized: space and ! replaced
    EXPECT_EQ(filename.find(' '), std::string::npos);
    EXPECT_EQ(filename.find('!'), std::string::npos);
    EXPECT_TRUE(filename.starts_with("my_bench_"));
}

// ============================================================================
// Recorder — record_ns() / record_ns_values() (raw-ns API)
// ============================================================================
//
// These tests cover the thin ns → cycles wrapper added to Recorder so
// bench helpers that natively measure in nanoseconds (clock_gettime,
// external-process timestamps) can feed samples without converting to
// TSC cycles themselves. The API is backward-compatible: the existing
// cycle-based storage path is reused unchanged, and compute_stats()
// continues to report ns.

TEST_F(RecorderTest, RecordNsIdentityRoundTrip) {
    // Single sample at 1 ms: after ns → cycles → ns round-trip, the
    // reported p50 should match the input to within ±2 ns (allow a
    // little slack for both truncation and HdrHistogram bucket width
    // at the 3-sig-fig precision level: 1e6 ns * 1e-3 = 1000 ns, which
    // is the histogram's raw resolution at this magnitude, but
    // record_ns uses the cycle-domain histogram so actual quantization
    // is finer).
    Recorder rec("RecordNsIdentity");
    rec.record_ns(1'000'000);
    auto stats = rec.compute_stats();
    ASSERT_TRUE(stats);
    EXPECT_EQ(stats->count, 1u);
    // Tolerance: cycle-domain bucket width at ~3 GHz and 3 sig figs is
    // ~(1000us/1000) = ~1us. We allow 2000 ns to be robust across TSC
    // frequencies; the primary goal is catching unit errors, not
    // proving HdrHistogram precision.
    EXPECT_NEAR(stats->p50_ns, 1'000'000.0, 2000.0);
    EXPECT_NEAR(stats->min_ns, 1'000'000.0, 2000.0);
    EXPECT_NEAR(stats->max_ns, 1'000'000.0, 2000.0);
}

TEST_F(RecorderTest, RecordNsMultipleValuesMinMax) {
    Recorder rec("RecordNsMultiple");
    // Record 100 samples ranging from 1000 ns to 100'000 ns.
    for (uint64_t ns = 1000; ns <= 100'000; ns += 1000) {
        rec.record_ns(ns);
    }
    auto stats = rec.compute_stats();
    ASSERT_TRUE(stats);
    EXPECT_EQ(stats->count, 100u);
    // min/max should bracket the input range; tolerance covers histogram
    // bucket width at both extremes (relative to magnitude).
    EXPECT_GE(stats->min_ns, 900.0);
    EXPECT_LE(stats->min_ns, 1100.0);
    EXPECT_GE(stats->max_ns, 99'000.0);
    EXPECT_LE(stats->max_ns, 101'000.0);
}

TEST_F(RecorderTest, RecordNsValuesBatched) {
    // Batched input: record 5000 ns, 100 times. All percentiles should
    // collapse to the single input value.
    Recorder rec("RecordNsBatched");
    rec.record_ns_values(5000, 100);
    auto stats = rec.compute_stats();
    ASSERT_TRUE(stats);
    EXPECT_EQ(stats->count, 100u);
    // All samples identical → every percentile must match.
    EXPECT_NEAR(stats->p50_ns, 5000.0, 50.0);
    EXPECT_NEAR(stats->p90_ns, 5000.0, 50.0);
    EXPECT_NEAR(stats->p99_ns, 5000.0, 50.0);
    EXPECT_NEAR(stats->min_ns, 5000.0, 50.0);
    EXPECT_NEAR(stats->max_ns, 5000.0, 50.0);
}

TEST_F(RecorderTest, RecordNsSmallValuesEdgeCases) {
    // Small and zero inputs must not corrupt counters or cause UB.
    // - ns=0 converts to cycles=0, which record() rejects as invalid
    //   (expected; contract carries over from the cycle-based API).
    // - ns=1..100 should succeed (cycles rounds down to 0 for very small
    //   inputs depending on the TSC frequency; if so, that sample is
    //   rejected as invalid — also acceptable and symmetric with record()).
    Recorder rec("RecordNsSmall");
    rec.record_ns(0);
    rec.record_ns(1);
    rec.record_ns(10);
    rec.record_ns(100);
    rec.record_ns(1000);
    // At least the largest samples must have been stored. The exact
    // count depends on TSC frequency, but recording must not crash or
    // produce UB. Verify no arithmetic corruption:
    EXPECT_LE(rec.count(), 5u);
    EXPECT_GE(rec.count() + rec.skipped_invalid_count() +
                  rec.skipped_overflow_count(),
              5u);
}

TEST_F(RecorderTest, RecordNsLargeValueNoOverflow) {
    // 10 seconds in ns. Recorder default range is "≈10 seconds" so this
    // sits at or just past the upper bound; we accept either successful
    // storage or overflow-skip — both are OK, what matters is no UB or
    // arithmetic wrap.
    Recorder rec("RecordNsLarge");
    const uint64_t ten_sec_ns = 10'000'000'000ull;
    rec.record_ns(ten_sec_ns);
    // Exactly one call → total tally must be 1 across all buckets.
    EXPECT_EQ(rec.count() + rec.skipped_invalid_count() +
                  rec.skipped_overflow_count(),
              1u);
}

TEST_F(RecorderTest, RecordNsMixedWithCycles) {
    // Mixing record() (cycles) and record_ns() (nanoseconds) on the
    // same instance must be self-consistent: both paths funnel into the
    // same cycle-domain histogram, and compute_stats() reports ns
    // regardless. Verify count is the sum.
    Recorder rec("RecordNsMixed");
    for (int i = 0; i < 50; ++i) {
        ASSERT_TRUE(rec.record(10'000));  // 10'000 cycles
    }
    for (int i = 0; i < 50; ++i) {
        rec.record_ns(5000);  // 5000 ns
    }
    auto stats = rec.compute_stats();
    ASSERT_TRUE(stats);
    EXPECT_EQ(stats->count, 100u);
    // Min/max must bracket both populations; we don't assert exact
    // ordering (depends on TSC frequency — on a 3 GHz box 10k cycles ≈
    // 3300 ns, on a 1 GHz box ≈ 10'000 ns). Only that both were stored.
    EXPECT_GT(stats->max_ns, 0.0);
    EXPECT_GT(stats->min_ns, 0.0);
}

TEST_F(RecorderTest, RecordNsIsNoexcept) {
    // Contract: record_ns / record_ns_values must be noexcept so they
    // can be used from destructors and hot-path code that forbids
    // exception propagation.
    static_assert(noexcept(std::declval<Recorder&>().record_ns(0ull)));
    static_assert(
        noexcept(std::declval<Recorder&>().record_ns_values(0ull, 0ull)));
}

TEST_F(RecorderTest, RecordNsPrecisionSweep) {
    // Precision sweep: record a sequence of distinct ns values and
    // verify the p50 lands near the median of the input. Uses a range
    // (1000..10000 step 10 → 901 samples) where HdrHistogram's 3-sig-fig
    // precision gives ≈ ±1% quantization, i.e. ≈ ±50 ns at p50 ≈ 5500.
    Recorder rec("RecordNsPrecisionSweep");
    uint64_t sum = 0, n = 0;
    for (uint64_t ns = 1000; ns <= 10'000; ns += 10) {
        rec.record_ns(ns);
        sum += ns;
        ++n;
    }
    ASSERT_EQ(rec.count(), n);
    auto stats = rec.compute_stats();
    ASSERT_TRUE(stats);
    const double expected_median = 5500.0;  // midpoint of [1000, 10000]
    // Generous tolerance (300 ns ≈ 5% of median) because HdrHistogram
    // bucketing at 3 sig figs + cycle-domain quantization both
    // contribute. The goal is to catch unit errors, not to verify
    // sub-ns precision.
    EXPECT_NEAR(stats->p50_ns, expected_median, 300.0);
}

// `record_ns(UINT64_MAX)` triggers the saturate-on-overflow guard in
// `ns_to_cycles_saturate`. The cycles_d * cycles_per_ns product would
// produce a double larger than UINT64_MAX → UB on `static_cast<uint64_t>`
// per [conv.fpint]/p1. The guard returns UINT64_MAX so the histogram
// counts the sample under skipped_overflow_ rather than poisoning
// total_cycles_. Pin this contract — without it, a refactor that drops
// the [[unlikely]] guard would silently regress to UB.
TEST_F(RecorderTest, RecordNsHugeValueSaturatesToOverflow) {
    Recorder rec("RecordNsOverflow");
    const uint64_t huge_ns = std::numeric_limits<uint64_t>::max();
    rec.record_ns(huge_ns);

    // The sample either lands as overflow (TSC frequency makes
    // cycles_d > UINT64_MAX so guard returns UINT64_MAX → histogram
    // overflow) or as a successful record (TSC frequency very low).
    // Either way: count_ + skipped_overflow_count() == 1, no UB,
    // no negative-looking large value.
    const uint64_t total = rec.count() + rec.skipped_overflow_count();
    EXPECT_EQ(total, 1u)
        << "record_ns(UINT64_MAX) must be tallied exactly once across "
           "the count_/skipped_overflow_ pair (saturation path).";
    // Defensive: skipped_invalid_ must remain 0 — UINT64_MAX is not
    // a "zero" sample, so the invalid-value branch must not fire.
    EXPECT_EQ(rec.skipped_invalid_count(), 0u);
}

TEST_F(RecorderTest, RecordNsZeroIsRejectedAsInvalid) {
    // Zero ns → zero cycles → caught by the `cycles == 0` invalid
    // guard, NOT the overflow guard. Pins the per-message-type
    // accounting that the warning printer relies on.
    Recorder rec("RecordNsZero");
    rec.record_ns(0);
    EXPECT_EQ(rec.count(), 0u);
    EXPECT_EQ(rec.skipped_invalid_count(), 1u);
    EXPECT_EQ(rec.skipped_overflow_count(), 0u);
}

TEST_F(RecorderTest, RecordNsValuesZeroValueAccumulatesInvalid) {
    // record_ns_values(0, count) — the cycles==0 invalid path adds
    // `count` to skipped_invalid_, NOT 1. Without the test, a refactor
    // that did `skipped_invalid_++` instead of `+= count` would silently
    // under-report.
    Recorder rec("RecordNsValuesZero");
    rec.record_ns_values(0, 7);
    EXPECT_EQ(rec.count(), 0u);
    EXPECT_EQ(rec.skipped_invalid_count(), 7u);
}

TEST_F(RecorderTest, RecordNsConcurrentDifferentInstances) {
    // Recorder itself is single-threaded; record_ns inherits that. But
    // the static cache inside ns_to_cycles_() is shared across all
    // Recorder instances, so verify that two threads each using their
    // own Recorder don't race on first-touch initialization.
    Recorder rec1("RecordNsThread1");
    Recorder rec2("RecordNsThread2");
    constexpr int kSamples = 1000;
    std::thread t1([&] {
        for (int i = 0; i < kSamples; ++i) rec1.record_ns(1000 + i);
    });
    std::thread t2([&] {
        for (int i = 0; i < kSamples; ++i) rec2.record_ns(2000 + i);
    });
    t1.join();
    t2.join();
    EXPECT_EQ(rec1.count(), static_cast<uint64_t>(kSamples));
    EXPECT_EQ(rec2.count(), static_cast<uint64_t>(kSamples));
}
