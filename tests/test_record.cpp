#include <gtest/gtest.h>
#include <filesystem>

#include "eph/utils/record.hpp"
#include "eph/utils/time.hpp"

using namespace eph::utils;

class RecordTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    ASSERT_TRUE(TSC::init()) << "TSC initialization failed";
  }
};

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
  
  // 插入 1-100 的值
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
  EXPECT_GT(percentiles[0], 0); // P50
  EXPECT_GT(percentiles[1], percentiles[0]); // P90 > P50
  EXPECT_GT(percentiles[2], percentiles[1]); // P99 > P90
}

TEST_F(RecordTest, HdrHistogramOutOfRange) {
  HdrHistogram hist(100, 10000, 2);
  
  EXPECT_FALSE(hist.record(50));     // 小于最小值
  EXPECT_FALSE(hist.record(20000));  // 大于最大值
  EXPECT_TRUE(hist.record(500));     // 正常范围
  
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
  
  EXPECT_FALSE(rec.record(0));  // 零值
  EXPECT_FALSE(rec.record(std::nan("")));  // NaN
  EXPECT_FALSE(rec.record(std::numeric_limits<double>::infinity()));  // Inf
  
  EXPECT_EQ(rec.count(), 0);
  EXPECT_GT(rec.skipped_invalid_count(), 0);
}

TEST_F(RecordTest, RecorderExportJson) {
  Recorder rec("ExportTest");
  
  for (int i = 0; i < 10; ++i) {
    rec.record(1000 + i * 100);
  }
  
  std::string test_dir = "test_outputs";
  EXPECT_TRUE(rec.export_json(test_dir));
  
  // 验证文件存在
  EXPECT_TRUE(std::filesystem::exists(test_dir));
  
  // 清理
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
  
  // 清理
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
