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

TEST(CpuTest, PinThread) {
  // Binding to CPU 0 should succeed and return a value
  reset_pin_registry_for_tests();
  auto r0 = pin_thread(0);
  EXPECT_TRUE(r0.has_value()) << "Failed to pin to cpu 0: " << r0.error();

  // Binding to the last CPU should also succeed
  reset_pin_registry_for_tests();
  unsigned last_cpu = std::thread::hardware_concurrency() - 1;
  auto r1 = pin_thread(last_cpu);
  EXPECT_TRUE(r1.has_value()) << "Failed to pin to cpu " << last_cpu << ": " << r1.error();
}

TEST(CpuTest, PinThreadNegativeCpuRejected) {
  // Behavior change vs legacy set_thread_affinity: pin_thread treats cpu<0
  // as an explicit error, not a silent no-op. Callers that want "don't
  // bind" should not call pin_thread at all.
  auto r = pin_thread(-1);
  EXPECT_FALSE(r.has_value());
  EXPECT_FALSE(r.error().empty());
}

TEST(CpuTest, PinThreadInvalidCpuReturnsError) {
  // An impossibly large cpu_id should fail. pin_thread doesn't carry the
  // old explicit CPU_SETSIZE pre-check, so we only assert the call fails
  // with a non-empty error — the exact message comes from pthread.
  reset_pin_registry_for_tests();
  auto r = pin_thread(99999);
  EXPECT_FALSE(r.has_value());
  EXPECT_FALSE(r.error().empty());
}

TEST(CpuTest, PinThreadAtHwcBoundary) {
  // Binding to exactly hardware_concurrency() should fail (0-indexed)
  reset_pin_registry_for_tests();
  unsigned hwc = std::thread::hardware_concurrency();
  auto r = pin_thread(static_cast<int>(hwc));
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
// spin_for_ns — busy-wait helper
// ─────────────────────────────────────────────────────────────────────────────

TEST(CpuTest, SpinForNsZeroIsNoOp) {
  // ns <= 0 must short-circuit; the function should return immediately
  // without consuming meaningful CPU time.
  EXPECT_NO_THROW(spin_for_ns(0));
  EXPECT_NO_THROW(spin_for_ns(-1));
  EXPECT_NO_THROW(spin_for_ns(-1'000'000));
}

TEST(CpuTest, SpinForNsBlocksApproximately) {
  // TSC must be initialized for spin_for_ns to do real work.
  if (!TSC::is_initialized()) {
    ASSERT_TRUE(TSC::init()) << "TSC init must succeed for this test";
  }
  // Measure that a 1ms spin actually waits at least ~500us. We're
  // intentionally lax on the upper bound — CI/VM hosts can be jittery —
  // but the lower bound of ~half the requested time guards against the
  // function silently returning early on a TSC mis-conversion.
  const auto start = std::chrono::steady_clock::now();
  spin_for_ns(1'000'000);  // 1 ms
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const auto elapsed_us =
      std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
  EXPECT_GE(elapsed_us, 500)
      << "spin_for_ns(1ms) returned in only " << elapsed_us
      << "us — well under the requested duration; check TSC calibration";
  // Loose upper bound — VM jitter may push a 1ms spin into low-tens-of-ms.
  EXPECT_LE(elapsed_us, 100'000)
      << "spin_for_ns(1ms) took " << elapsed_us
      << "us — wildly over budget; possible thread-suspend or measurement bug";
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

TEST(CpuTest, CpuTopologyInfoFormat) {
  CpuTopologyInfo info{.socket_id = 0, .core_id = 2, .hw_thread_id = 4};
  auto formatted = std::format("{}", info);
  EXPECT_NE(formatted.find("socket=0"), std::string::npos);
  EXPECT_NE(formatted.find("core=2"), std::string::npos);
  EXPECT_NE(formatted.find("thread=4"), std::string::npos);
}

TEST(CpuTest, PinThreadDoesNotCrash) {
  std::atomic<bool> ready{false};

  std::thread t([&] {
    reset_pin_registry_for_tests();
    (void)pin_thread(0);

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
