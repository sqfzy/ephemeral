#include <gtest/gtest.h>
#include <thread>

#include "eph/utils/time.hpp"

using namespace eph::utils;

class TimeTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    // 在所有测试前初始化 TSC
    ASSERT_TRUE(TSC::init()) << "TSC initialization failed";
  }
};

TEST_F(TimeTest, TscNowIsMonotonic) {
  uint64_t t1 = TSC::now();
  uint64_t t2 = TSC::now();
  uint64_t t3 = TSC::now();
  
  EXPECT_LE(t1, t2);
  EXPECT_LE(t2, t3);
}

TEST_F(TimeTest, TscToNsConversion) {
  uint64_t cycles = 1000;
  auto ns = TSC::to_ns(cycles);
  
  ASSERT_TRUE(ns.has_value());
  EXPECT_GT(*ns, 0.0);
  
  // 1K 周期在 1-10 GHz CPU 上应该是 100ns - 2us
  EXPECT_GT(*ns, 100.0);
  EXPECT_LT(*ns, 2000.0);
}

TEST_F(TimeTest, TscToCyclesConversion) {
  double ns = 1000.0; // 1 microsecond
  auto cycles = TSC::to_cycles(ns);
  
  ASSERT_TRUE(cycles.has_value());
  EXPECT_GT(*cycles, 0);
  
  // 1us 在 1-10 GHz CPU 上应该是 1000 - 10000 周期
  EXPECT_GT(*cycles, 500);
  EXPECT_LT(*cycles, 20000);
}

TEST_F(TimeTest, TscChronoDurationConversion) {
  auto cycles = TSC::to_cycles(std::chrono::microseconds(100));
  
  ASSERT_TRUE(cycles.has_value());
  EXPECT_GT(*cycles, 0);
}

TEST_F(TimeTest, TscRoundTrip) {
  uint64_t original_cycles = 5000000;
  
  auto ns = TSC::to_ns(original_cycles);
  ASSERT_TRUE(ns.has_value());
  
  auto cycles = TSC::to_cycles(*ns);
  ASSERT_TRUE(cycles.has_value());
  
  // 允许小的舍入误差（< 0.1%）
  double error = std::abs(static_cast<double>(*cycles - original_cycles)) / 
                 original_cycles;
  EXPECT_LT(error, 0.001);
}

TEST_F(TimeTest, TscMeasureActualDelay) {
  uint64_t start = TSC::now();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  uint64_t end = TSC::now();
  
  auto elapsed_ns = TSC::to_ns(end - start);
  ASSERT_TRUE(elapsed_ns.has_value());
  
  // 应该至少睡了 10ms（考虑调度延迟，上限放宽到 50ms）
  EXPECT_GT(*elapsed_ns, 10'000'000.0);  // > 10ms
  EXPECT_LT(*elapsed_ns, 50'000'000.0);  // < 50ms
}

TEST_F(TimeTest, TscIsInitialized) {
  EXPECT_TRUE(TSC::is_initialized());
  
  auto ns_per_cycle = TSC::get_ns_per_cycle();
  ASSERT_TRUE(ns_per_cycle.has_value());
  EXPECT_GT(*ns_per_cycle, 0.0);
}
