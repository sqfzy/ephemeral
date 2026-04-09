/// @file eph-utils/tests/test_cpu_pin.cpp
/// Contract tests for eph::utils::pin_thread_strict and friends.
///
/// These tests are intentionally permissive about absolute outcomes —
/// whether pinning succeeds depends entirely on the host's isolcpus
/// configuration. They lock down the *contracts*:
///   - require_isolcpus=true on a non-isolated cpu must FAIL
///   - relaxing require_isolcpus must allow that same cpu to succeed
///   - sibling-conflict detection rejects subsequent pin to a sibling
///   - registry can be cleared between test cases via reset_pin_registry_for_tests()

#include <set>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "eph/utils/cpu_pin.hpp"

using namespace eph::utils;

namespace {

std::set<int> isolated_cpus() {
    return cpu_pin_detail::read_cpu_list_file(
        "/sys/devices/system/cpu/isolated");
}

int pick_non_isolated_cpu() {
    auto iso = isolated_cpus();
    int n = static_cast<int>(std::thread::hardware_concurrency());
    for (int c = 0; c < n; ++c) {
        if (!iso.contains(c)) return c;
    }
    return -1;
}

int sibling_of(int cpu) {
    auto siblings = cpu_pin_detail::read_cpu_list_file(
        "/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
        "/topology/thread_siblings_list");
    for (int s : siblings) if (s != cpu) return s;
    return -1;
}

} // namespace

TEST(CpuPinStrict, RequireIsolcpusFailsOnNonIsolatedCpu) {
    reset_pin_registry_for_tests();
    int cpu = pick_non_isolated_cpu();
    if (cpu < 0) {
        GTEST_SKIP() << "all cpus are in isolated set — cannot test negative path";
    }
    CpuPinPolicy strict;
    auto r = pin_thread_strict(cpu, "test-strict", strict);
    EXPECT_FALSE(r.has_value());
    if (!r.has_value()) {
        EXPECT_NE(r.error().find("isolated"), std::string::npos);
    }
}

TEST(CpuPinStrict, NonIsolatedCpuSucceedsWhenPolicyRelaxed) {
    reset_pin_registry_for_tests();
    int cpu = pick_non_isolated_cpu();
    if (cpu < 0) GTEST_SKIP() << "no non-isolated cpu available";

    CpuPinPolicy relaxed;
    relaxed.require_isolcpus = false;
    relaxed.warn_irq_overlap = false;
    auto r = pin_thread_strict(cpu, "test-relaxed", relaxed);
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error());
}

TEST(CpuPinStrict, RegistryDetectsSiblingConflict) {
    reset_pin_registry_for_tests();
    int cpu = pick_non_isolated_cpu();
    if (cpu < 0) GTEST_SKIP() << "no non-isolated cpu available";
    int sib = sibling_of(cpu);
    if (sib < 0) {
        GTEST_SKIP() << "cpu " << cpu << " has no SMT sibling on this host";
    }

    CpuPinPolicy relaxed;
    relaxed.require_isolcpus = false;
    relaxed.warn_irq_overlap = false;

    auto a = pin_thread_strict(cpu, "test-sibA", relaxed);
    ASSERT_TRUE(a.has_value()) << (a ? "" : a.error());

    auto b = pin_thread_strict(sib, "test-sibB", relaxed);
    EXPECT_FALSE(b.has_value());
    if (!b.has_value()) {
        EXPECT_NE(b.error().find("SMT"), std::string::npos);
    }
}

TEST(CpuPinStrict, NegativeCpuRejected) {
    reset_pin_registry_for_tests();
    auto r = pin_thread_strict(-1, "neg");
    EXPECT_FALSE(r.has_value());
}
