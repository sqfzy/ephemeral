#include <gtest/gtest.h>
#include <thread>

#include "eph/utils/cpu.hpp"

using namespace eph::utils;

TEST(CpuTest, GetCpuTopology) {
  auto result = get_cpu_topology();
  ASSERT_TRUE(result.has_value()) << "get_cpu_topology failed: " << result.error();

  auto& topology = *result;

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
  // Error message should be actionable: include cpu_id and errno description
  EXPECT_NE(r.error().find("99999"), std::string::npos);
}

TEST(CpuTest, SetThreadAffinityAtHwcBoundary) {
  // Binding to exactly hardware_concurrency() should fail (0-indexed)
  unsigned hwc = std::thread::hardware_concurrency();
  auto r = set_thread_affinity(static_cast<int>(hwc));
  EXPECT_FALSE(r.has_value());
}

TEST(CpuTest, GetCpuBaseFrequency) {
  auto freq = get_cpu_base_frequency();

  // On Linux with /proc/cpuinfo available, expect a valid frequency.
  // On other platforms or minimal environments, nullopt is acceptable.
  if (freq) {
    // 频率应该在合理范围内（0.5 - 10 GHz）
    EXPECT_GT(*freq, 0.5);
    EXPECT_LT(*freq, 10.0);
  }
}

TEST(CpuTest, CpuRelax) {
  // cpu_relax 应该可以安全调用
  EXPECT_NO_THROW({
    for (int i = 0; i < 100; ++i) {
      cpu_relax();
    }
  });
}

// ─────────────────────────────────────────────────────────────────────────────
// set_thread_realtime
// ─────────────────────────────────────────────────────────────────────────────

TEST(CpuTest, SetThreadRealtimeFifoSucceedsOrEperm) {
  auto r = set_thread_realtime(RealtimePolicy::Fifo, -1, "test-fifo");
  if (r.has_value()) {
    // Succeeded — we had permissions (root or CAP_SYS_NICE)
    SUCCEED();
  } else {
    // Expected failure without privileges: EPERM
    EXPECT_NE(r.error().find("setcap"), std::string::npos)
        << "Error should contain hint: " << r.error();
  }
}

TEST(CpuTest, SetThreadRealtimeRoundRobinSucceedsOrEperm) {
  auto r = set_thread_realtime(RealtimePolicy::RoundRobin, -1, "test-rr");
  if (r.has_value()) {
    SUCCEED();
  } else {
    EXPECT_NE(r.error().find("SCHED_RR"), std::string::npos)
        << "Error should mention policy: " << r.error();
  }
}

TEST(CpuTest, SetThreadRealtimeExplicitPriority) {
  // Priority 1 is the lowest valid RT priority
  auto r = set_thread_realtime(RealtimePolicy::Fifo, 1, "test-low-prio");
  if (r.has_value()) {
    SUCCEED();
  } else {
    // Without privileges, any RT priority fails
    EXPECT_FALSE(r.error().empty());
  }
}

TEST(CpuTest, SetThreadRealtimePriorityClampsToMax) {
  // Priority 9999 should be clamped to system max (usually 99), not error
  auto r = set_thread_realtime(RealtimePolicy::Fifo, 9999, "test-clamp");
  if (r.has_value()) {
    SUCCEED();
  } else {
    // Failure is EPERM, not "invalid priority"
    EXPECT_NE(r.error().find("Failed"), std::string::npos);
  }
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
