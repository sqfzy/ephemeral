/// @file tests/unit/bench/test_cpu_pin.cpp
/// Unit tests for the strict CPU pinning helper.
///
/// These tests are intentionally permissive about absolute outcomes
/// (whether pinning succeeds depends entirely on the host's isolcpus
/// configuration). They lock down the *contracts*:
///   - require_isolcpus=true on a non-isolated cpu must FAIL
///   - relaxing require_isolcpus must allow that same cpu to succeed
///   - sibling-conflict detection rejects subsequent pin to a sibling
///   - registry can be cleared by tests via reset_pin_registry_for_tests()

#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

#include <gtest/gtest.h>

#include "core/cpu_pin.hpp"

using namespace bench;

namespace {

// Read the same isolated set the production helper reads.
std::set<int> isolated_cpus() {
    return cpu_pin_detail::read_cpu_list_file(
        "/sys/devices/system/cpu/isolated");
}

// Find any cpu that is NOT in the isolated set (and is online).
int pick_non_isolated_cpu() {
    auto iso = isolated_cpus();
    int n = static_cast<int>(std::thread::hardware_concurrency());
    for (int c = 0; c < n; ++c) {
        if (iso.find(c) == iso.end()) return c;
    }
    return -1;
}

// Find a sibling of `cpu` (or -1 if cpu has no SMT sibling).
int sibling_of(int cpu) {
    auto siblings = cpu_pin_detail::read_cpu_list_file(
        "/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
        "/topology/thread_siblings_list");
    for (int s : siblings) if (s != cpu) return s;
    return -1;
}

} // namespace

TEST(BenchCpuPin, RequireIsolcpusFailsOnNonIsolatedCpu) {
    reset_pin_registry_for_tests();
    int cpu = pick_non_isolated_cpu();
    if (cpu < 0) {
        GTEST_SKIP() << "All cpus are in isolated set on this host; "
                        "cannot test the negative path.";
    }
    CpuPinPolicy strict;
    auto result = pin_thread_strict(cpu, "test-strict", strict);
    EXPECT_FALSE(result.has_value());
    if (!result.has_value()) {
        EXPECT_NE(result.error().find("isolated"), std::string::npos);
    }
}

TEST(BenchCpuPin, NonIsolatedCpuSucceedsWhenPolicyRelaxed) {
    reset_pin_registry_for_tests();
    int cpu = pick_non_isolated_cpu();
    if (cpu < 0) GTEST_SKIP() << "no non-isolated cpu available";

    CpuPinPolicy relaxed;
    relaxed.require_isolcpus = false;
    relaxed.warn_irq_overlap = false; // tests should not log noise
    auto result = pin_thread_strict(cpu, "test-relaxed", relaxed);
    EXPECT_TRUE(result.has_value()) << (result ? "" : result.error());
}

TEST(BenchCpuPin, RegistryDetectsSiblingConflict) {
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

    // First pin succeeds.
    auto a = pin_thread_strict(cpu, "test-sibA", relaxed);
    ASSERT_TRUE(a.has_value()) << (a ? "" : a.error());

    // Pinning to the SMT sibling must fail.
    auto b = pin_thread_strict(sib, "test-sibB", relaxed);
    EXPECT_FALSE(b.has_value());
    if (!b.has_value()) {
        EXPECT_NE(b.error().find("SMT"), std::string::npos);
    }
}

TEST(BenchCpuPin, NegativeCpuRejected) {
    reset_pin_registry_for_tests();
    auto r = pin_thread_strict(-1, "neg");
    EXPECT_FALSE(r.has_value());
}
