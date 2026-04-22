/// @file eph-utils/tests/test_cpu_pin.cpp
/// Contract tests for eph::utils::pin_thread and friends.
///
/// These tests are intentionally permissive about absolute outcomes —
/// whether pinning succeeds depends entirely on the host's isolcpus
/// configuration. They lock down the *contracts*:
///   - require_isolcpus=true on a non-isolated cpu must FAIL
///   - leaving require_isolcpus=false (the default) allows that same cpu to succeed
///   - sibling-conflict detection rejects subsequent pin to a sibling
///   - registry can be cleared between test cases via reset_pin_registry_for_tests()

#include <set>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "eph/utils/cpu.hpp"

using namespace eph::utils;

namespace {

std::set<int> isolated_cpus() {
    return detail::read_cpu_list_file(
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
    auto siblings = detail::read_cpu_list_file(
        "/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
        "/topology/thread_siblings_list");
    for (int s : siblings) if (s != cpu) return s;
    return -1;
}

// All-strict policy — inverse of CpuPinPolicy's relaxed default.
// Kept as a helper so every test case in this file uses the same
// "production-grade" policy.
constexpr CpuPinPolicy strict_policy() {
    return CpuPinPolicy{
        .require_isolcpus            = true,
        .require_no_sibling_conflict = true,
        .require_same_numa           = true,
        .warn_irq_overlap            = true,
    };
}

} // namespace

TEST(CpuPinStrict, RequireIsolcpusFailsOnNonIsolatedCpu) {
    reset_pin_registry_for_tests();
    int cpu = pick_non_isolated_cpu();
    if (cpu < 0) {
        GTEST_SKIP() << "all cpus are in isolated set — cannot test negative path";
    }
    auto r = pin_thread(cpu, "test-strict", strict_policy());
    EXPECT_FALSE(r.has_value());
    if (!r.has_value()) {
        EXPECT_NE(r.error().find("isolated"), std::string::npos);
    }
}

TEST(CpuPinStrict, NonIsolatedCpuSucceedsWhenPolicyRelaxed) {
    reset_pin_registry_for_tests();
    int cpu = pick_non_isolated_cpu();
    if (cpu < 0) GTEST_SKIP() << "no non-isolated cpu available";

    // Default-constructed policy is now fully relaxed.
    auto r = pin_thread(cpu, "test-relaxed");
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

    // Enable only the sibling-conflict check; leave isolcpus relaxed so
    // the first pin actually succeeds on a dev host.
    CpuPinPolicy sibling_strict;
    sibling_strict.require_no_sibling_conflict = true;

    auto a = pin_thread(cpu, "test-sibA", sibling_strict);
    ASSERT_TRUE(a.has_value()) << (a ? "" : a.error());

    auto b = pin_thread(sib, "test-sibB", sibling_strict);
    EXPECT_FALSE(b.has_value());
    if (!b.has_value()) {
        EXPECT_NE(b.error().find("SMT"), std::string::npos);
    }
}

TEST(CpuPinStrict, NegativeCpuRejected) {
    reset_pin_registry_for_tests();
    auto r = pin_thread(-1, "neg");
    EXPECT_FALSE(r.has_value());
}
