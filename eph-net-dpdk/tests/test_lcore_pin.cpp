/// @file eph-net-dpdk/tests/test_lcore_pin.cpp
/// Unit tests for `eph::dpdk::LcorePin` and `build_lcore_argv`.
///
/// Stage 4 scope only: type definition + pure-function argv builder. The
/// stateful pieces (`register_lcore_pins`, `RegisteredLcoreGuard`,
/// `EalGuard::init_with_pins`) land in stages 5 and 6 and bring their own
/// tests covering registry side effects, RAII rollback, and EAL integration.

#include <array>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "eph/dpdk/lcore_pin.hpp"

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
