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

// ──────────────────────────────────────────────────────────────────────
// register_external_pin / unregister_external_pin / is_cpu_externally_pinned
// ──────────────────────────────────────────────────────────────────────

TEST(ExternalPin, RegisterAndQueryRoundtrip) {
    reset_pin_registry_for_tests();
    EXPECT_FALSE(is_cpu_externally_pinned(4));

    auto r = register_external_pin(4, "lcore-0(rx-worker)");
    ASSERT_TRUE(r.has_value()) << (r ? "" : r.error());
    EXPECT_TRUE(is_cpu_externally_pinned(4));

    unregister_external_pin(4);
    EXPECT_FALSE(is_cpu_externally_pinned(4));
}

TEST(ExternalPin, RejectsNegativeCpu) {
    reset_pin_registry_for_tests();
    auto r = register_external_pin(-1, "anything");
    EXPECT_FALSE(r.has_value());
    if (!r.has_value()) {
        EXPECT_NE(r.error().find("must be >= 0"), std::string::npos);
    }
    EXPECT_FALSE(is_cpu_externally_pinned(-1));
}

TEST(ExternalPin, DuplicateRegistrationRejectedWithRoleInMessage) {
    reset_pin_registry_for_tests();
    ASSERT_TRUE(register_external_pin(4, "lcore-0").has_value());

    auto r = register_external_pin(4, "lcore-9");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("already occupied"), std::string::npos);
    EXPECT_NE(r.error().find("lcore-0"), std::string::npos)
        << "error must name the existing owner: " << r.error();
}

TEST(ExternalPin, UnregisterIsIdempotent) {
    reset_pin_registry_for_tests();
    // Unregister never-registered cpu: silent no-op.
    unregister_external_pin(7);
    EXPECT_FALSE(is_cpu_externally_pinned(7));

    ASSERT_TRUE(register_external_pin(7, "lcore-1").has_value());
    unregister_external_pin(7);
    unregister_external_pin(7);  // second call must be safe
    EXPECT_FALSE(is_cpu_externally_pinned(7));
}

TEST(ExternalPin, NegativeCpuQueryReturnsFalse) {
    reset_pin_registry_for_tests();
    EXPECT_FALSE(is_cpu_externally_pinned(-1));
    EXPECT_FALSE(is_cpu_externally_pinned(-9999));
}

// ──────────────────────────────────────────────────────────────────────
// Cross-API contract: pin_thread and register_external_pin share the
// process-wide registry, so the second to claim a cpu loses regardless
// of which API claimed it first. (Stage 3 tightening: pin_thread no
// longer silently succeeds on a duplicate.)
// ──────────────────────────────────────────────────────────────────────

TEST(PinThreadDup, RepeatPinSameCpuRejected) {
    reset_pin_registry_for_tests();
    int cpu = pick_non_isolated_cpu();
    if (cpu < 0) GTEST_SKIP() << "no non-isolated cpu available";

    auto a = pin_thread(cpu, "first");
    ASSERT_TRUE(a.has_value()) << (a ? "" : a.error());

    auto b = pin_thread(cpu, "second");
    ASSERT_FALSE(b.has_value());
    EXPECT_NE(b.error().find("already pinned"), std::string::npos);
    EXPECT_NE(b.error().find("first"), std::string::npos)
        << "error must name the prior owner: " << b.error();
}

TEST(PinThreadDup, ExternalPinThenPinThreadSameCpuRejected) {
    reset_pin_registry_for_tests();
    int cpu = pick_non_isolated_cpu();
    if (cpu < 0) GTEST_SKIP() << "no non-isolated cpu available";

    ASSERT_TRUE(register_external_pin(cpu, "lcore-0(rx)").has_value());

    auto r = pin_thread(cpu, "app-thread");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("already pinned"), std::string::npos);
    EXPECT_NE(r.error().find("lcore-0(rx)"), std::string::npos)
        << "pin_thread error should name the lcore that owns the cpu: "
        << r.error();
}

TEST(PinThreadDup, PinThreadThenExternalPinSameCpuRejected) {
    reset_pin_registry_for_tests();
    int cpu = pick_non_isolated_cpu();
    if (cpu < 0) GTEST_SKIP() << "no non-isolated cpu available";

    // Hold the PinGuard alive in scope: a temporary
    // `pin_thread(...).has_value()` would destruct the guard immediately,
    // unregistering the cpu before register_external_pin runs and turning
    // the conflict into a false positive.
    auto a = pin_thread(cpu, "app-thread");
    ASSERT_TRUE(a.has_value()) << (a ? "" : a.error());

    auto r = register_external_pin(cpu, "lcore-0");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("already occupied"), std::string::npos);
    EXPECT_NE(r.error().find("app-thread"), std::string::npos)
        << "register_external_pin error should name the pin_thread owner: "
        << r.error();
}

TEST(PinThreadDup, UnregisterExternalThenPinThreadSucceeds) {
    reset_pin_registry_for_tests();
    int cpu = pick_non_isolated_cpu();
    if (cpu < 0) GTEST_SKIP() << "no non-isolated cpu available";

    ASSERT_TRUE(register_external_pin(cpu, "lcore-0").has_value());
    unregister_external_pin(cpu);

    auto r = pin_thread(cpu, "app-thread");
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error());
}

TEST(ExternalPin, ResetForTestsClearsBothMaps) {
    ASSERT_TRUE(register_external_pin(8, "lcore-2").has_value());
    EXPECT_TRUE(is_cpu_externally_pinned(8));

    reset_pin_registry_for_tests();

    EXPECT_FALSE(is_cpu_externally_pinned(8));
    // After reset, re-registering the same cpu must succeed (not flag
    // duplicate from a stale role-map entry).
    auto r = register_external_pin(8, "lcore-3");
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error());
}

// ──────────────────────────────────────────────────────────────────────
// PinGuard — RAII move/release semantics on top of the registry
// ──────────────────────────────────────────────────────────────────────

TEST(PinGuard, DefaultConstructedIsEmptyAndDestructsCleanly) {
    PinGuard g;
    EXPECT_TRUE(g.empty());
    EXPECT_LT(g.cpu(), 0);
    // Going out of scope: dtor must be a no-op (cpu_ == -1).
}

TEST(PinGuard, AdoptThenDestructUnregisters) {
    reset_pin_registry_for_tests();
    ASSERT_TRUE(register_external_pin(60, "x").has_value());
    EXPECT_TRUE(is_cpu_externally_pinned(60));
    {
        auto g = PinGuard::adopt(60);
        EXPECT_FALSE(g.empty());
        EXPECT_EQ(g.cpu(), 60);
    }  // dtor unregisters
    EXPECT_FALSE(is_cpu_externally_pinned(60));
}

TEST(PinGuard, MoveCtorTransfersOwnership) {
    reset_pin_registry_for_tests();
    ASSERT_TRUE(register_external_pin(61, "x").has_value());

    auto orig = PinGuard::adopt(61);
    {
        PinGuard moved = std::move(orig);
        EXPECT_TRUE(orig.empty());           // moved-from is empty
        EXPECT_EQ(moved.cpu(), 61);          // new owner has the cpu
        EXPECT_TRUE(is_cpu_externally_pinned(61));
    }  // moved destructs -> cpu released
    EXPECT_FALSE(is_cpu_externally_pinned(61));
}

TEST(PinGuard, MoveAssignReleasesPriorOwnership) {
    reset_pin_registry_for_tests();
    ASSERT_TRUE(register_external_pin(70, "a").has_value());
    ASSERT_TRUE(register_external_pin(71, "b").has_value());

    auto guard_a = PinGuard::adopt(70);
    auto guard_b = PinGuard::adopt(71);
    EXPECT_TRUE(is_cpu_externally_pinned(70));
    EXPECT_TRUE(is_cpu_externally_pinned(71));

    guard_a = std::move(guard_b);  // assigning over guard_a must release cpu 70
    EXPECT_FALSE(is_cpu_externally_pinned(70))
        << "move-assign must unregister the prior cpu";
    EXPECT_TRUE(is_cpu_externally_pinned(71))
        << "move-assign target now owns the moved-in cpu";
}

TEST(PinGuard, ReleaseSkipsUnregistration) {
    reset_pin_registry_for_tests();
    ASSERT_TRUE(register_external_pin(80, "x").has_value());
    {
        auto g = PinGuard::adopt(80);
        g.release();
        EXPECT_TRUE(g.empty());
        // After release, destruction must not unregister.
    }
    EXPECT_TRUE(is_cpu_externally_pinned(80))
        << "release() should leave registry untouched";

    // Cleanup so the next test isn't polluted.
    unregister_external_pin(80);
}

TEST(PinGuard, PinThreadReturnsLiveGuardThatUnregistersOnDrop) {
    reset_pin_registry_for_tests();
    int cpu = pick_non_isolated_cpu();
    if (cpu < 0) GTEST_SKIP() << "no non-isolated cpu available";
    {
        auto r = pin_thread(cpu, "scoped");
        ASSERT_TRUE(r.has_value()) << (r ? "" : r.error());
        EXPECT_EQ(r->cpu(), cpu);
        EXPECT_TRUE(is_cpu_externally_pinned(cpu));
    }  // r out of scope -> registry entry gone
    EXPECT_FALSE(is_cpu_externally_pinned(cpu));
}

// ---------------------------------------------------------------------------
// read_cpu_list_file boundary tests — sysfs is normally trusted, but the
// parse routine is also fed by /proc, container overlays, and any future
// caller that hands it a runtime-discovered path. A malformed line like
// "0-2147483647" would otherwise expand into INT_MAX std::set inserts and
// take the process down with memory exhaustion before the caller saw the
// result. Negative ids are also nonsensical for every consumer of the API.
// ---------------------------------------------------------------------------

#include <cstdio>
#include <fstream>

namespace {
std::string write_temp_cpu_list(std::string_view contents) {
    char tmpl[] = "/tmp/eph_cpu_list_test_XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd < 0) return {};
    ::close(fd);
    std::ofstream out(tmpl);
    out << contents;
    out.close();
    return std::string{tmpl};
}
} // namespace

TEST(ReadCpuListFile, RejectsRangeWithIntMaxAsHi) {
    auto path = write_temp_cpu_list("0-2147483647\n");
    ASSERT_FALSE(path.empty());
    auto out = detail::read_cpu_list_file(path);
    // Hi must have been clamped to kMaxCpuId-1 == 8191. So size <= 8192.
    EXPECT_LE(out.size(), 8192u)
        << "INT_MAX as range high MUST be clamped — otherwise this test "
           "would hang or OOM the runner";
    // Lo=0 must still be present (clamping only narrows the range).
    EXPECT_TRUE(out.contains(0));
    ::unlink(path.c_str());
}

TEST(ReadCpuListFile, NegativeStandaloneIdRejected) {
    // Singleton "-5" parses successfully via stoi (returns -5) but the
    // post-fix bound `id >= 0` drops it before insertion. Pre-fix the
    // negative would have been silently inserted as a -5 entry that
    // every downstream caller (pin / numa / queue resolution) would
    // misinterpret as either a missing CPU or a stoi-error sentinel.
    auto path = write_temp_cpu_list("-5,3,7\n");
    ASSERT_FALSE(path.empty());
    auto out = detail::read_cpu_list_file(path);
    EXPECT_EQ(out.size(), 2u);
    EXPECT_FALSE(out.contains(-5));
    EXPECT_TRUE(out.contains(3));
    EXPECT_TRUE(out.contains(7));
    ::unlink(path.c_str());
}

TEST(ReadCpuListFile, RejectsStandaloneOutOfRangeId) {
    auto path = write_temp_cpu_list("1,99999,3\n");
    ASSERT_FALSE(path.empty());
    auto out = detail::read_cpu_list_file(path);
    // 99999 > kMaxCpuId so it's dropped; 1 and 3 stay.
    EXPECT_EQ(out.size(), 2u);
    EXPECT_TRUE(out.contains(1));
    EXPECT_TRUE(out.contains(3));
    EXPECT_FALSE(out.contains(99999));
    ::unlink(path.c_str());
}
