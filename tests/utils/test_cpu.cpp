#include <gtest/gtest.h>
#include <thread>

#include "eph/utils/cpu.hpp"

using namespace eph::utils;

TEST(CpuTest, GetCpuTopology) {
  auto topology = get_cpu_topology();
  
  // 基本检查
  EXPECT_FALSE(topology.empty());
  EXPECT_EQ(topology.size(), std::thread::hardware_concurrency());
  
  // 检查 hw_thread_id 排序
  for (size_t i = 0; i < topology.size(); ++i) {
    EXPECT_EQ(topology[i].hw_thread_id, i);
  }
}

TEST(CpuTest, SetThreadAffinity) {
  // Binding to CPU 0 should succeed and return a value
  auto r0 = set_thread_affinity(0);
  EXPECT_TRUE(r0.has_value()) << "Failed to pin to cpu 0: " << r0.error();

  // Binding to the last CPU should also succeed
  unsigned last_cpu = std::thread::hardware_concurrency() - 1;
  auto r1 = set_thread_affinity(last_cpu);
  EXPECT_TRUE(r1.has_value()) << "Failed to pin to cpu " << last_cpu << ": " << r1.error();
}

TEST(CpuTest, SetThreadAffinityNegativeCpuIsNoop) {
  // Negative cpu_id means "don't bind" — should succeed silently
  auto r = set_thread_affinity(-1);
  EXPECT_TRUE(r.has_value());
}

TEST(CpuTest, SetThreadAffinityInvalidCpuReturnsError) {
  // An impossibly large cpu_id should fail
  auto r = set_thread_affinity(99999);
  EXPECT_FALSE(r.has_value());
  EXPECT_FALSE(r.error().empty());
}

TEST(CpuTest, GetCpuBaseFrequency) {
  double freq = get_cpu_base_frequency();
  
  // 频率应该在合理范围内（0.5 - 10 GHz）
  EXPECT_GT(freq, 0.5);
  EXPECT_LT(freq, 10.0);
}

TEST(CpuTest, CpuRelax) {
  // cpu_relax 应该可以安全调用
  EXPECT_NO_THROW({
    for (int i = 0; i < 100; ++i) {
      cpu_relax();
    }
  });
}

TEST(CpuTest, ThreadAffinityDoesNotCrash) {
  std::atomic<bool> ready{false};
  
  std::thread t([&] {
    (void)set_thread_affinity(0);
    
    // 模拟自旋等待
    int count = 0;
    while (!ready.load() && count++ < 1000) {
      cpu_relax();
    }
  });
  
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ready.store(true);
  t.join();
  
  SUCCEED();
}
