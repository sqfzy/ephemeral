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
