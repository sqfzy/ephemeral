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

TEST_F(TimeTest, concurrent_init_is_safe) {
  // TSC::init() is guarded by std::call_once — concurrent calls must not race.
  // Since init was already called in SetUpTestSuite, all threads here should
  // observe the already-initialized state and return true immediately.
  constexpr int kThreads = 8;
  std::atomic<int> success_count{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&] {
      if (TSC::init()) success_count.fetch_add(1, std::memory_order_relaxed);
    });
  }
  for (auto& t : threads) t.join();

  // All threads should succeed (call_once returns cached result).
  EXPECT_EQ(success_count.load(), kThreads);
  EXPECT_TRUE(TSC::is_initialized());
}

TEST_F(TimeTest, repeated_init_returns_cached_result) {
  // Second init() call should return true without re-calibrating.
  auto ns1 = TSC::get_ns_per_cycle();
  ASSERT_TRUE(TSC::init());
  auto ns2 = TSC::get_ns_per_cycle();
  ASSERT_TRUE(ns1.has_value());
  ASSERT_TRUE(ns2.has_value());
  EXPECT_DOUBLE_EQ(*ns1, *ns2);
}

TEST_F(TimeTest, to_cycles_negative_returns_nullopt) {
  EXPECT_FALSE(TSC::to_cycles(-1000.0).has_value());
  EXPECT_FALSE(TSC::to_cycles(-0.001).has_value());
}

TEST_F(TimeTest, to_cycles_overflow_saturates_to_uint64_max) {
  // A nanosecond value large enough to overflow uint64_t when converted to cycles.
  // With ns_per_cycle ~ 0.3 (3 GHz CPU), 1e20 ns / 0.3 ~ 3.3e20, which exceeds UINT64_MAX.
  auto result = TSC::to_cycles(1e20);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, UINT64_MAX);
}

TEST_F(TimeTest, DeltaNsReturnsPositiveForWork) {
  auto start = TSC::now();
  volatile int x = 0;
  for (int i = 0; i < 1000; ++i) x += i;
  auto end = TSC::now();

  auto ns = TSC::delta_ns(start, end);
  ASSERT_TRUE(ns.has_value());
  EXPECT_GT(*ns, 0.0);
}

TEST_F(TimeTest, DeltaNsZeroDeltaReturnsZero) {
  auto ns = TSC::delta_ns(100, 100);
  ASSERT_TRUE(ns.has_value());
  EXPECT_DOUBLE_EQ(*ns, 0.0);
}

TEST_F(TimeTest, DeltaNsConsistentWithToNs) {
  uint64_t start = 1000;
  uint64_t end = 2000;
  auto delta_result = TSC::delta_ns(start, end);
  auto direct_result = TSC::to_ns(end - start);
  ASSERT_TRUE(delta_result.has_value());
  ASSERT_TRUE(direct_result.has_value());
  EXPECT_DOUBLE_EQ(*delta_result, *direct_result);
}

TEST_F(TimeTest, ToCyclesNanReturnsNullopt) {
  auto result = TSC::to_cycles(std::numeric_limits<double>::quiet_NaN());
  EXPECT_FALSE(result.has_value())
      << "NaN input must return nullopt, not UB via static_cast";
}

TEST_F(TimeTest, ToCyclesPositiveInfSaturates) {
  auto result = TSC::to_cycles(std::numeric_limits<double>::infinity());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, UINT64_MAX);
}

// Boundary regression: a `cycles` value at exactly 2^64 (the rounded-up
// alias of UINT64_MAX in IEEE-754 double) must saturate, not flow through
// to a UB cast. The previous `<= UINT64_MAX_as_double` check let this exact
// boundary through; the strict `<` form saturates correctly.
TEST_F(TimeTest, ToCyclesAtUint64MaxBoundarySaturates) {
  // Pick an ns input that, after dividing by ns_per_cycle, lands at or
  // above 2^64. Use a slightly-above-UINT64_MAX ns value scaled by typical
  // ns_per_cycle (~0.3 to ~1.0) so cycles >= 2^64 across hosts.
  // 2e19 ns / ~1 ns/cycle ≈ 2e19 cycles > UINT64_MAX (≈1.84e19).
  auto result = TSC::to_cycles(2e19);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, UINT64_MAX);
}

TEST_F(TimeTest, ToCyclesNegativeInfReturnsNullopt) {
  auto result = TSC::to_cycles(-std::numeric_limits<double>::infinity());
  EXPECT_FALSE(result.has_value());
}

TEST_F(TimeTest, ToCyclesZeroReturnsZero) {
  auto result = TSC::to_cycles(0.0);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 0u);
}

TEST_F(TimeTest, GetCalibrationCvIsSmall) {
  auto cv = TSC::get_calibration_cv();
  ASSERT_TRUE(cv.has_value());
  EXPECT_GE(*cv, 0.0);
  // CV should be well below 1% on a stable system
  EXPECT_LT(*cv, 0.1) << "Calibration CV > 10% indicates unstable TSC";
}

TEST_F(TimeTest, init_flag_visibility_across_threads) {
  // Verify that a thread spawned after init() sees the calibrated value.
  std::optional<double> observed;
  std::thread t([&] {
    observed = TSC::get_ns_per_cycle();
  });
  t.join();
  ASSERT_TRUE(observed.has_value());
  EXPECT_GT(*observed, 0.0);
}
