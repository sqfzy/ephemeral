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
  // 绑定到 CPU 0 应该不会崩溃
  EXPECT_NO_THROW(set_thread_affinity(0));
  
  // 绑定到最后一个 CPU
  unsigned last_cpu = std::thread::hardware_concurrency() - 1;
  EXPECT_NO_THROW(set_thread_affinity(last_cpu));
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
    set_thread_affinity(0);
    
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
