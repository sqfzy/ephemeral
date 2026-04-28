/// @file eph-net-dpdk/tests/test_lcore_pin.cpp
/// Unit tests for `eph::dpdk::LcorePin`, `build_lcore_argv`,
/// `register_lcore_pins`, and `RegisteredLcoreGuard`.
///
/// EalGuard::init_with_pins integration (stage 6) lives in
/// tests/integration/test_eal_init_with_pins.cpp because it requires
/// real DPDK runtime / hugepages / vfio.

#include <array>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "eph/dpdk/lcore_pin.hpp"
#include "eph/utils/cpu.hpp"

using namespace eph::dpdk;

TEST(BuildLcoreArgv, EmptySpanProducesEmptyString) {
    std::vector<LcorePin> pins;
    EXPECT_EQ(build_lcore_argv(std::span<LcorePin const>{pins}), "");

    std::array<LcorePin, 0> empty_arr{};
    EXPECT_EQ(build_lcore_argv(std::span<LcorePin const>{empty_arr}), "");
}

TEST(BuildLcoreArgv, SinglePin) {
    std::array pins = { LcorePin{0, 4, "rx"} };
    EXPECT_EQ(build_lcore_argv(std::span<LcorePin const>{pins}),
              "--lcores=0@4");
}

TEST(BuildLcoreArgv, MultiplePinsCommaSeparated) {
    std::array pins = {
        LcorePin{0, 4, "rx"},
        LcorePin{1, 5, "tx"},
        LcorePin{2, 6, "control"},
    };
    EXPECT_EQ(build_lcore_argv(std::span<LcorePin const>{pins}),
              "--lcores=0@4,1@5,2@6");
}

TEST(BuildLcoreArgv, NonContiguousIdsPreserveOrder) {
    // EAL accepts any ordering as long as lcore_ids are unique. Output
    // must reflect input order (no reordering / sorting).
    std::array pins = {
        LcorePin{7, 12, "x"},
        LcorePin{1, 4,  "y"},
        LcorePin{3, 8,  "z"},
    };
    EXPECT_EQ(build_lcore_argv(std::span<LcorePin const>{pins}),
              "--lcores=7@12,1@4,3@8");
}

TEST(BuildLcoreArgv, RoleFieldDoesNotAffectOutput) {
    // role is purely diagnostic — must never leak into argv (DPDK would
    // reject any non-numeric / non-recognized syntax).
    std::array pins_with_role    = { LcorePin{0, 4, "very-long-role-name"} };
    std::array pins_without_role = { LcorePin{0, 4, ""} };
    EXPECT_EQ(build_lcore_argv(std::span<LcorePin const>{pins_with_role}),
              build_lcore_argv(std::span<LcorePin const>{pins_without_role}));
}

TEST(BuildLcoreArgv, RoleWithSpecialCharsStillIgnored) {
    // Even pathological role strings (commas, '@', spaces) must not
    // contaminate the argv — they are dropped silently.
    std::array pins = { LcorePin{0, 4, "evil,@ role"} };
    EXPECT_EQ(build_lcore_argv(std::span<LcorePin const>{pins}),
              "--lcores=0@4");
}

TEST(BuildLcoreArgv, AcceptsNonZeroStartingCpu) {
    // Realistic HFT layout: lcore 0 lands on a high-numbered cpu (NUMA
    // local to the NIC) rather than cpu 0.
    std::array pins = {
        LcorePin{0, 32, "rx"},
        LcorePin{1, 33, "tx"},
    };
    EXPECT_EQ(build_lcore_argv(std::span<LcorePin const>{pins}),
              "--lcores=0@32,1@33");
}

TEST(LcorePin, IsAggregateConstructible) {
    // LcorePin must remain an aggregate so callers can use brace-init
    // and std::array literal initialization. (Not constexpr because
    // role is std::string, which requires runtime allocation.)
    LcorePin p{0, 4, ""};
    EXPECT_EQ(p.lcore_id, 0);
    EXPECT_EQ(p.cpu_id, 4);
    EXPECT_TRUE(p.role.empty());
}

// ──────────────────────────────────────────────────────────────────────
// register_lcore_pins / RegisteredLcoreGuard
// ──────────────────────────────────────────────────────────────────────

TEST(RegisterLcorePins, EmptySpanReturnsEmptyGuard) {
    eph::utils::reset_pin_registry_for_tests();
    std::array<LcorePin, 0> none{};
    auto g = register_lcore_pins(std::span<LcorePin const>{none});
    ASSERT_TRUE(g.has_value()) << (g ? "" : g.error());
    EXPECT_TRUE(g->empty());
    EXPECT_EQ(g->size(), 0u);
}

TEST(RegisterLcorePins, HappyPathThreePinsAllRegistered) {
    eph::utils::reset_pin_registry_for_tests();
    std::array pins = {
        LcorePin{0, 4, "rx"},
        LcorePin{1, 5, "tx"},
        LcorePin{2, 6, "control"},
    };
    auto g = register_lcore_pins(std::span<LcorePin const>{pins});
    ASSERT_TRUE(g.has_value()) << (g ? "" : g.error());
    EXPECT_EQ(g->size(), 3u);
    EXPECT_TRUE(eph::utils::is_cpu_externally_pinned(4));
    EXPECT_TRUE(eph::utils::is_cpu_externally_pinned(5));
    EXPECT_TRUE(eph::utils::is_cpu_externally_pinned(6));
}

TEST(RegisterLcorePins, GuardDestructorUnregistersAll) {
    eph::utils::reset_pin_registry_for_tests();
    std::array pins = {
        LcorePin{0, 10, "a"},
        LcorePin{1, 11, "b"},
    };
    {
        auto g = register_lcore_pins(std::span<LcorePin const>{pins});
        ASSERT_TRUE(g.has_value()) << (g ? "" : g.error());
        EXPECT_TRUE(eph::utils::is_cpu_externally_pinned(10));
        EXPECT_TRUE(eph::utils::is_cpu_externally_pinned(11));
    }  // <-- guard destructs here
    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(10));
    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(11));
}

TEST(RegisterLcorePins, NegativeCpuRollsBackEarlierStaged) {
    eph::utils::reset_pin_registry_for_tests();
    std::array pins = {
        LcorePin{0, 20, "ok-1"},
        LcorePin{1, 21, "ok-2"},
        LcorePin{2, -1, "broken"},  // <-- must reject and roll back
    };
    auto g = register_lcore_pins(std::span<LcorePin const>{pins});
    ASSERT_FALSE(g.has_value());
    EXPECT_NE(g.error().find("pin[2]"), std::string::npos)
        << "error must point to the offending index: " << g.error();
    EXPECT_NE(g.error().find("invalid cpu_id"), std::string::npos);

    // staged cpus (20, 21) must be unregistered after rollback
    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(20));
    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(21));
}

TEST(RegisterLcorePins, DuplicateCpuRollsBackEarlierStaged) {
    eph::utils::reset_pin_registry_for_tests();
    std::array pins = {
        LcorePin{0, 30, "first"},
        LcorePin{1, 31, "second"},
        LcorePin{2, 30, "dup"},  // <-- same cpu as pin[0]
    };
    auto g = register_lcore_pins(std::span<LcorePin const>{pins});
    ASSERT_FALSE(g.has_value());
    EXPECT_NE(g.error().find("pin[2]"), std::string::npos);
    EXPECT_NE(g.error().find("already occupied"), std::string::npos);

    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(30));
    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(31));
}

TEST(RegisterLcorePins, ConflictWithExistingPinThreadDetected) {
    eph::utils::reset_pin_registry_for_tests();
    ASSERT_TRUE(eph::utils::register_external_pin(40, "external-mock").has_value());

    std::array pins = { LcorePin{0, 40, "rx"} };
    auto g = register_lcore_pins(std::span<LcorePin const>{pins});
    ASSERT_FALSE(g.has_value());
    EXPECT_NE(g.error().find("already occupied"), std::string::npos);
    EXPECT_NE(g.error().find("external-mock"), std::string::npos)
        << "error should name the prior owner: " << g.error();
}

TEST(RegisterLcorePins, RoleFormattedAsLcoreIdAndUserLabel) {
    eph::utils::reset_pin_registry_for_tests();
    std::array pins = { LcorePin{7, 50, "fancy-rx"} };
    auto g = register_lcore_pins(std::span<LcorePin const>{pins});
    ASSERT_TRUE(g.has_value()) << (g ? "" : g.error());

    // Force a duplicate to surface the stored role string in the error.
    auto dup = eph::utils::register_external_pin(50, "x");
    ASSERT_FALSE(dup.has_value());
    EXPECT_NE(dup.error().find("lcore-7(fancy-rx)"), std::string::npos)
        << dup.error();
}

TEST(RegisteredLcoreGuard, MoveCtorTransfersOwnership) {
    eph::utils::reset_pin_registry_for_tests();
    std::array pins = { LcorePin{0, 60, "x"} };
    auto orig = register_lcore_pins(std::span<LcorePin const>{pins});
    ASSERT_TRUE(orig.has_value()) << (orig ? "" : orig.error());

    {
        RegisteredLcoreGuard moved = std::move(*orig);
        EXPECT_TRUE(orig->empty());        // moved-from is empty
        EXPECT_EQ(moved.size(), 1u);       // new owner has the cpu
        EXPECT_TRUE(eph::utils::is_cpu_externally_pinned(60));
    }  // moved destructs -> cpu released

    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(60));

    // Original guard going out of scope here is a no-op (already moved).
}

TEST(RegisteredLcoreGuard, MoveAssignReleasesPriorOwnership) {
    eph::utils::reset_pin_registry_for_tests();
    std::array pins_a = { LcorePin{0, 70, "a"} };
    std::array pins_b = { LcorePin{1, 71, "b"} };

    auto guard_a = register_lcore_pins(std::span<LcorePin const>{pins_a});
    auto guard_b = register_lcore_pins(std::span<LcorePin const>{pins_b});
    ASSERT_TRUE(guard_a.has_value() && guard_b.has_value());
    EXPECT_TRUE(eph::utils::is_cpu_externally_pinned(70));
    EXPECT_TRUE(eph::utils::is_cpu_externally_pinned(71));

    *guard_a = std::move(*guard_b);  // assigning over guard_a must release cpu 70

    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(70))
        << "move-assign must unregister the prior cpu";
    EXPECT_TRUE(eph::utils::is_cpu_externally_pinned(71))
        << "move-assign target now owns the moved-in cpu";
}

TEST(RegisteredLcoreGuard, ReleaseSkipsUnregistration) {
    eph::utils::reset_pin_registry_for_tests();
    std::array pins = { LcorePin{0, 80, "x"} };
    {
        auto g = register_lcore_pins(std::span<LcorePin const>{pins});
        ASSERT_TRUE(g.has_value());
        g->release();
        // After release, destruction must not unregister.
    }
    EXPECT_TRUE(eph::utils::is_cpu_externally_pinned(80))
        << "release() should leave registry untouched";

    // Cleanup so the next test isn't polluted.
    eph::utils::unregister_external_pin(80);
}
