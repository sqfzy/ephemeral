#include <gtest/gtest.h>
#include <filesystem>
#include <thread>
#include <vector>

#include "eph/utils/record.hpp"
#include "eph/utils/time.hpp"

using namespace eph::utils;

class RecordTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    ASSERT_TRUE(TSC::init()) << "TSC initialization failed";
  }
};

// ============================================================================
// measure_tsc / ScopedTSC
// ============================================================================

TEST_F(RecordTest, MeasureFunction) {
  int counter = 0;
  auto cycles = measure_tsc([&] { counter++; });

  EXPECT_EQ(counter, 1);
  EXPECT_GT(cycles, 0);
}

TEST_F(RecordTest, MeasureFunctionWithArgs) {
  std::vector<int> vec{3, 1, 2};
  auto cycles = measure_tsc(std::sort<decltype(vec.begin())>,
                        vec.begin(), vec.end());

  EXPECT_GT(cycles, 0);
  EXPECT_TRUE(std::is_sorted(vec.begin(), vec.end()));
}

TEST_F(RecordTest, ScopedTscBasic) {
  uint64_t latency = 0;

  {
    ScopedTSC timer(latency);
    volatile int x = 0;
    for (int i = 0; i < 100; ++i) {
      x += i;
    }
  }

  EXPECT_GT(latency, 0);
}

// ============================================================================
// HdrHistogram
// ============================================================================

TEST_F(RecordTest, HdrHistogramBasic) {
  HdrHistogram hist(1, 1000000, 3);

  EXPECT_TRUE(hist.record(100));
  EXPECT_TRUE(hist.record(500));
  EXPECT_TRUE(hist.record(1000));

  EXPECT_EQ(hist.get_total_count(), 3);
  EXPECT_EQ(hist.get_min_value(), 100);
  EXPECT_EQ(hist.get_max_value(), 1000);
}

TEST_F(RecordTest, HdrHistogramPercentiles) {
  HdrHistogram hist(1, 1000000, 3);

  for (uint64_t i = 1; i <= 100; ++i) {
    EXPECT_TRUE(hist.record(i));
  }

  uint64_t p50 = hist.get_value_at_percentile(50.0);
  uint64_t p90 = hist.get_value_at_percentile(90.0);
  uint64_t p99 = hist.get_value_at_percentile(99.0);

  EXPECT_GT(p50, 40);
  EXPECT_LT(p50, 60);
  EXPECT_GT(p90, 85);
  EXPECT_GT(p99, 95);
}

TEST_F(RecordTest, HdrHistogramBatchPercentiles) {
  HdrHistogram hist(1, 1000000, 3);

  for (uint64_t i = 1; i <= 100; ++i) {
    hist.record(i);
  }

  auto percentiles = hist.get_percentiles({50.0, 90.0, 99.0, 99.9});

  EXPECT_EQ(percentiles.size(), 4);
  EXPECT_GT(percentiles[0], 0);
  EXPECT_GT(percentiles[1], percentiles[0]);
  EXPECT_GT(percentiles[2], percentiles[1]);
}

TEST_F(RecordTest, HdrHistogramOutOfRange) {
  HdrHistogram hist(100, 10000, 2);

  EXPECT_FALSE(hist.record(50));
  EXPECT_FALSE(hist.record(20000));
  EXPECT_TRUE(hist.record(500));

  EXPECT_EQ(hist.get_total_count(), 1);
}

TEST_F(RecordTest, HdrHistogramMerge) {
  HdrHistogram hist1(1, 100000, 2);
  HdrHistogram hist2(1, 100000, 2);

  hist1.record(100);
  hist1.record(200);

  hist2.record(300);
  hist2.record(400);

  EXPECT_TRUE(hist1.merge(hist2));
  EXPECT_EQ(hist1.get_total_count(), 4);
  EXPECT_EQ(hist1.get_min_value(), 100);
  EXPECT_EQ(hist1.get_max_value(), 400);
}

TEST_F(RecordTest, HdrHistogramStatistics) {
  HdrHistogram hist(1, 1000000, 3);

  for (int i = 0; i < 1000; ++i) {
    hist.record(500);
  }

  double mean = hist.get_mean();
  double stddev = hist.get_std_deviation();

  EXPECT_NEAR(mean, 500.0, 50.0);
  EXPECT_GE(stddev, 0.0);
}

// ============================================================================
// Recorder (单线程)
// ============================================================================

TEST_F(RecordTest, RecorderBasic) {
  Recorder rec("TestRecorder");

  EXPECT_FALSE(rec.has_data());

  uint64_t start = TSC::now();
  volatile int x = 0;
  for (int i = 0; i < 1000; ++i) x += i;
  uint64_t end = TSC::now();

  EXPECT_TRUE(rec.record(end - start));
  EXPECT_TRUE(rec.has_data());
  EXPECT_EQ(rec.count(), 1);
}

TEST_F(RecordTest, RecorderMultipleSamples) {
  Recorder rec("MultiSample", 1, 10000000, 2);

  for (int i = 0; i < 100; ++i) {
    rec.record(1000 + i);
  }

  auto stats = rec.compute_stats();
  ASSERT_TRUE(stats.has_value());

  EXPECT_EQ(stats->count, 100);
  EXPECT_GT(stats->avg_ns, 0.0);
  EXPECT_GT(stats->p99_ns, stats->p50_ns);
}

TEST_F(RecordTest, RecorderRejectsInvalidValues) {
  Recorder rec("InvalidTest");

  EXPECT_FALSE(rec.record(0));

  EXPECT_EQ(rec.count(), 0);
  EXPECT_GT(rec.skipped_invalid_count(), 0);
}

TEST_F(RecordTest, RecorderMerge) {
  Recorder rec1("MergeA", 1, 10000000, 2);
  Recorder rec2("MergeB", 1, 10000000, 2);

  for (int i = 0; i < 50; ++i) {
    rec1.record(1000 + i);
  }
  for (int i = 0; i < 50; ++i) {
    rec2.record(2000 + i);
  }

  EXPECT_TRUE(rec1.merge(rec2));
  EXPECT_EQ(rec1.count(), 100);

  auto stats = rec1.compute_stats();
  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(stats->count, 100);
}

TEST_F(RecordTest, RecorderExportJson) {
  Recorder rec("ExportTest");

  for (int i = 0; i < 10; ++i) {
    rec.record(1000 + i * 100);
  }

  std::string test_dir = "test_outputs";
  EXPECT_TRUE(rec.export_json(test_dir));

  EXPECT_TRUE(std::filesystem::exists(test_dir));
  std::filesystem::remove_all(test_dir);
}

TEST_F(RecordTest, RecorderExportCsv) {
  Recorder rec("CsvTest");

  for (int i = 0; i < 10; ++i) {
    rec.record(1000 + i * 100);
  }

  std::string test_dir = "test_outputs";
  EXPECT_TRUE(rec.export_csv(test_dir));

  EXPECT_TRUE(std::filesystem::exists(test_dir));
  std::filesystem::remove_all(test_dir);
}

TEST_F(RecordTest, RecorderReset) {
  Recorder rec("ResetTest");

  rec.record(1000);
  rec.record(2000);
  EXPECT_EQ(rec.count(), 2);

  rec.reset();
  EXPECT_EQ(rec.count(), 0);
  EXPECT_FALSE(rec.has_data());
}

// ============================================================================
// Recorder — Stats 结构体不含系统资源字段
// ============================================================================

TEST_F(RecordTest, StatsHasNoSystemResourceFields) {
  Recorder rec("PureLatency");
  rec.record(1000);
  rec.record(2000);

  auto stats = rec.compute_stats();
  ASSERT_TRUE(stats.has_value());

  // Stats 只有延迟统计字段
  EXPECT_EQ(stats->name, "PureLatency");
  EXPECT_EQ(stats->count, 2);
  EXPECT_GT(stats->avg_ns, 0.0);
  EXPECT_GT(stats->p50_ns, 0.0);
  EXPECT_GT(stats->p99_ns, 0.0);
}

// ============================================================================
// SystemStats (独立系统资源采集)
// ============================================================================

TEST_F(RecordTest, SystemStatsSnapshot) {
  SystemStats sys;

  // 做一些工作产生资源消耗
  volatile int sum = 0;
  for (int i = 0; i < 100000; ++i) {
    sum += i;
  }

  auto resource = sys.snapshot();

  // 至少 user CPU 应该有一些消耗
  EXPECT_GE(resource.user_cpu_s, 0.0);
  EXPECT_GE(resource.total_cpu_s, 0.0);
  EXPECT_GE(resource.minflt, 0);
}

TEST_F(RecordTest, SystemStatsReset) {
  SystemStats sys;

  volatile int sum = 0;
  for (int i = 0; i < 100000; ++i) sum += i;

  sys.reset();

  auto resource = sys.snapshot();
  // reset 后差值应该非常小
  EXPECT_GE(resource.user_cpu_s, 0.0);
}

// ============================================================================
// Stats — operator- and operator==
// ============================================================================

TEST_F(RecordTest, StatsDiffOperator) {
  Stats s1{.name = "test", .count = 10, .avg_ns = 100.0, .min_ns = 50.0,
            .max_ns = 200.0, .p50_ns = 90.0, .p90_ns = 150.0,
            .p99_ns = 190.0, .p999_ns = 198.0, .stddev_ns = 30.0};
  Stats s2{.name = "test", .count = 30, .avg_ns = 120.0, .min_ns = 40.0,
            .max_ns = 250.0, .p50_ns = 100.0, .p90_ns = 180.0,
            .p99_ns = 230.0, .p999_ns = 245.0, .stddev_ns = 40.0};

  auto delta = s2 - s1;
  EXPECT_EQ(delta.name, "test");
  EXPECT_EQ(delta.count, 20u);
  // Point-in-time fields come from lhs (s2)
  EXPECT_DOUBLE_EQ(delta.avg_ns, 120.0);
  EXPECT_DOUBLE_EQ(delta.p50_ns, 100.0);
}

TEST_F(RecordTest, StatsEqualityOperator) {
  Stats s1{.name = "test", .count = 10, .avg_ns = 100.0, .min_ns = 50.0,
            .max_ns = 200.0, .p50_ns = 90.0, .p90_ns = 150.0,
            .p99_ns = 190.0, .p999_ns = 198.0, .stddev_ns = 30.0};
  Stats s2 = s1;
  EXPECT_EQ(s1, s2);

  s2.count = 11;
  EXPECT_NE(s1, s2);
}

// ============================================================================
// ConcurrentRecorder (多线程)
// ============================================================================

TEST_F(RecordTest, ConcurrentRecorderSingleThread) {
  ConcurrentRecorder rec("ConcSingle");

  for (int i = 0; i < 100; ++i) {
    rec.record(1000 + i);
  }

  auto stats = rec.compute_stats();
  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(stats->count, 100);
  EXPECT_GT(stats->avg_ns, 0.0);
  EXPECT_EQ(rec.thread_count(), 1);
}

TEST_F(RecordTest, ConcurrentRecorderMultiThread) {
  ConcurrentRecorder rec("ConcMulti");

  constexpr int num_threads = 4;
  constexpr int samples_per_thread = 1000;

  std::vector<std::thread> threads;
  threads.reserve(num_threads);

  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&rec, t] {
      for (int i = 0; i < samples_per_thread; ++i) {
        rec.record(1000 + t * 100 + i);
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  auto stats = rec.compute_stats();
  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(stats->count, num_threads * samples_per_thread);
  EXPECT_GT(stats->avg_ns, 0.0);
  EXPECT_GT(stats->p99_ns, stats->p50_ns);
}

TEST_F(RecordTest, ConcurrentRecorderRejectsZero) {
  ConcurrentRecorder rec("ConcZero");

  EXPECT_FALSE(rec.record(0));
  // 有效记录
  EXPECT_TRUE(rec.record(1000));

  auto stats = rec.compute_stats();
  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(stats->count, 1);
}

TEST_F(RecordTest, ConcurrentRecorderEmptyReport) {
  ConcurrentRecorder rec("ConcEmpty");

  auto stats = rec.compute_stats();
  EXPECT_FALSE(stats.has_value());
}

// ---------------------------------------------------------------------------
// HdrHistogram::report() tests
// ---------------------------------------------------------------------------

TEST(HdrHistogramReport, EmptyHistogramReportsEmpty) {
    HdrHistogram hist(1, 10000, 3);
    auto r = hist.report("Latency");
    EXPECT_NE(r.find("empty"), std::string::npos);
    EXPECT_NE(r.find("Latency"), std::string::npos);
}

TEST(HdrHistogramReport, PopulatedHistogramIncludesAllPercentiles) {
    HdrHistogram hist(1, 100000, 3);
    for (uint64_t i = 1; i <= 1000; ++i) {
        hist.record(i);
    }

    auto r = hist.report("Roundtrip", "ns");
    EXPECT_NE(r.find("Roundtrip"), std::string::npos);
    EXPECT_NE(r.find("1000 samples"), std::string::npos);
    EXPECT_NE(r.find("p50"), std::string::npos);
    EXPECT_NE(r.find("p90"), std::string::npos);
    EXPECT_NE(r.find("p99"), std::string::npos);
    EXPECT_NE(r.find("p99.9"), std::string::npos);
    EXPECT_NE(r.find("p99.99"), std::string::npos);
    EXPECT_NE(r.find("mean"), std::string::npos);
    EXPECT_NE(r.find("stddev"), std::string::npos);
    EXPECT_NE(r.find("ns"), std::string::npos);
}

TEST(HdrHistogramReport, ReportWithoutUnitOmitsUnitSuffix) {
    HdrHistogram hist(1, 10000, 3);
    hist.record(42);

    auto r = hist.report("Test");
    // Should have values but no unit suffix beyond the numbers
    EXPECT_NE(r.find("Test"), std::string::npos);
    EXPECT_NE(r.find("1 samples"), std::string::npos);
}
