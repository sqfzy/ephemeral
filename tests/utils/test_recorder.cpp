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
        rec.record(i * 100);
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
        rec.record(i);
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
    rec.record(100);
    EXPECT_TRUE(rec.has_data());
}

// ============================================================================
// Recorder — reset()
// ============================================================================

TEST_F(RecorderTest, ResetClearsAllState) {
    Recorder rec("Test");
    rec.record(100);
    rec.record(0);  // triggers invalid skip

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
        rec.record(i * 100);
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
        rec.record(i * 100);
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
        rec.record(i * 100);
    }

    EXPECT_TRUE(rec.export_all("test_outputs"));
}

// ============================================================================
// Recorder — print_report() smoke test
// ============================================================================

TEST_F(RecorderTest, PrintReportWithData) {
    Recorder rec("Test");
    for (uint64_t i = 1; i <= 100; ++i) {
        rec.record(i * 100);
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

TEST_F(RecorderTest, RecordRealTscMeasurements) {
    Recorder rec("RealTSC");

    for (int i = 0; i < 100; ++i) {
        uint64_t start = TSC::now();
        volatile int x = 0;
        for (int j = 0; j < 100; ++j) {
            x += j;
        }
        uint64_t end = TSC::now();
        rec.record(end - start);
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
                rec.record(i * 10);
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
                rec.record(100);
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
                rec.record(value);
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
    rec.record(100);
    EXPECT_NO_THROW(rec.print_report());
}

TEST_F(ConcurrentRecorderTest, PrintReportNoData) {
    ConcurrentRecorder rec("ConcPrint");
    EXPECT_NO_THROW(rec.print_report());
}
