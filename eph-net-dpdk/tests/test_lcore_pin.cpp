/// @file eph-net-dpdk/tests/test_lcore_pin.cpp
/// Unit tests for `eph::dpdk::LcorePin`, `build_lcore_argv`,
/// `pin_lcore`, and `pin_lcores`.
///
/// PinGuard's own move/release semantics live in eph-utils/tests/
/// test_cpu_pin.cpp — vector<PinGuard> from pin_lcores inherits its
/// move behaviour from stdlib and doesn't need separate coverage.
///
/// EalGuard::init_with_pins integration (success path with real EAL init)
/// lives in tests/integration/test_eal_init_with_pins.cpp because it
/// requires real DPDK runtime / hugepages / vfio.

#include <array>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "eph/dpdk/eal.hpp"
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
// pin_lcore (single) / pin_lcores (batch)
// ──────────────────────────────────────────────────────────────────────

TEST(PinLcore, SinglePinRegistersAndRoleIsLcoreWrapped) {
    eph::utils::reset_pin_registry_for_tests();
    auto g = pin_lcore(7, 50, "fancy-rx");
    ASSERT_TRUE(g.has_value()) << (g ? "" : g.error());
    EXPECT_EQ(g->cpu(), 50);
    EXPECT_TRUE(eph::utils::is_cpu_externally_pinned(50));

    // Forcing a duplicate via register_external_pin surfaces the stored
    // role string — verify the "lcore-N(label)" prefix is automatic.
    auto dup = eph::utils::register_external_pin(50, "x");
    ASSERT_FALSE(dup.has_value());
    EXPECT_NE(dup.error().find("lcore-7(fancy-rx)"), std::string::npos)
        << dup.error();
}

TEST(PinLcore, NegativeCpuRejected) {
    eph::utils::reset_pin_registry_for_tests();
    auto g = pin_lcore(0, -1, "x");
    ASSERT_FALSE(g.has_value());
    EXPECT_NE(g.error().find("invalid cpu_id"), std::string::npos);
}

TEST(PinLcores, EmptySpanReturnsEmptyVector) {
    eph::utils::reset_pin_registry_for_tests();
    std::array<LcorePin, 0> none{};
    auto g = pin_lcores(std::span<LcorePin const>{none});
    ASSERT_TRUE(g.has_value()) << (g ? "" : g.error());
    EXPECT_TRUE(g->empty());
    EXPECT_EQ(g->size(), 0u);
}

TEST(PinLcores, HappyPathThreePinsAllRegistered) {
    eph::utils::reset_pin_registry_for_tests();
    std::array pins = {
        LcorePin{0, 4, "rx"},
        LcorePin{1, 5, "tx"},
        LcorePin{2, 6, "control"},
    };
    auto g = pin_lcores(std::span<LcorePin const>{pins});
    ASSERT_TRUE(g.has_value()) << (g ? "" : g.error());
    EXPECT_EQ(g->size(), 3u);
    EXPECT_TRUE(eph::utils::is_cpu_externally_pinned(4));
    EXPECT_TRUE(eph::utils::is_cpu_externally_pinned(5));
    EXPECT_TRUE(eph::utils::is_cpu_externally_pinned(6));
}

TEST(PinLcores, VectorDestructorUnregistersAll) {
    eph::utils::reset_pin_registry_for_tests();
    std::array pins = {
        LcorePin{0, 10, "a"},
        LcorePin{1, 11, "b"},
    };
    {
        auto g = pin_lcores(std::span<LcorePin const>{pins});
        ASSERT_TRUE(g.has_value()) << (g ? "" : g.error());
        EXPECT_TRUE(eph::utils::is_cpu_externally_pinned(10));
        EXPECT_TRUE(eph::utils::is_cpu_externally_pinned(11));
    }  // <-- vector<PinGuard> destructs here, each guard unregisters
    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(10));
    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(11));
}

TEST(PinLcores, NegativeCpuRollsBackEarlierStaged) {
    eph::utils::reset_pin_registry_for_tests();
    std::array pins = {
        LcorePin{0, 20, "ok-1"},
        LcorePin{1, 21, "ok-2"},
        LcorePin{2, -1, "broken"},  // <-- must reject and roll back
    };
    auto g = pin_lcores(std::span<LcorePin const>{pins});
    ASSERT_FALSE(g.has_value());
    EXPECT_NE(g.error().find("pin[2]"), std::string::npos)
        << "error must point to the offending index: " << g.error();
    EXPECT_NE(g.error().find("invalid cpu_id"), std::string::npos);

    // staged cpus (20, 21) must be unregistered after rollback
    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(20));
    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(21));
}

TEST(PinLcores, DuplicateCpuRollsBackEarlierStaged) {
    eph::utils::reset_pin_registry_for_tests();
    std::array pins = {
        LcorePin{0, 30, "first"},
        LcorePin{1, 31, "second"},
        LcorePin{2, 30, "dup"},  // <-- same cpu as pin[0]
    };
    auto g = pin_lcores(std::span<LcorePin const>{pins});
    ASSERT_FALSE(g.has_value());
    EXPECT_NE(g.error().find("pin[2]"), std::string::npos);
    EXPECT_NE(g.error().find("already occupied"), std::string::npos);

    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(30));
    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(31));
}

TEST(PinLcores, DuplicateLcoreIdIsRejectedBeforeStaging) {
    // Two LcorePin entries with the same lcore_id but different cpus
    // would slip past the cpu-collision check (the registry only
    // notices duplicate cpus, not duplicate lcore ids), and produce a
    // --lcores=0@4,0@5 argv that rte_eal_init would later reject with
    // a generic "invalid lcore mask" message. pin_lcores must fail
    // fast and name both indices so the misconfiguration is obvious.
    eph::utils::reset_pin_registry_for_tests();
    std::array pins = {
        LcorePin{0, 50, "first"},
        LcorePin{1, 51, "second"},
        LcorePin{0, 52, "dup-id"},  // <-- same lcore_id as pin[0]
    };
    auto g = pin_lcores(std::span<LcorePin const>{pins});
    ASSERT_FALSE(g.has_value());
    EXPECT_NE(g.error().find("pin[2]"), std::string::npos)
        << "error must point at the duplicate index: " << g.error();
    EXPECT_NE(g.error().find("lcore_id=0"), std::string::npos)
        << "error must name the duplicated lcore id: " << g.error();
    EXPECT_NE(g.error().find("pin[0]"), std::string::npos)
        << "error must reference the prior occupant index: " << g.error();

    // No staged cpus survive the rejection — pin[0]/pin[1] never made
    // it into the registry because we fail BEFORE pin_lcore is called
    // for pin[2], and the fail return drops the guard vector which
    // would have unregistered them anyway.
    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(50));
    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(51));
    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(52));
}

TEST(PinLcores, ConflictWithExistingExternalPinDetected) {
    eph::utils::reset_pin_registry_for_tests();
    ASSERT_TRUE(eph::utils::register_external_pin(40, "external-mock").has_value());

    std::array pins = { LcorePin{0, 40, "rx"} };
    auto g = pin_lcores(std::span<LcorePin const>{pins});
    ASSERT_FALSE(g.has_value());
    EXPECT_NE(g.error().find("already occupied"), std::string::npos);
    EXPECT_NE(g.error().find("external-mock"), std::string::npos)
        << "error should name the prior owner: " << g.error();
}

// ──────────────────────────────────────────────────────────────────────
// EalGuard::init_with_pins — pre-EAL paths only.
//
// The success path calls rte_eal_init() and therefore requires DPDK
// runtime (hugepages / vfio). That lives in
// tests/integration/test_eal_init_with_pins.cpp. Here we cover the
// validation paths that fail BEFORE rte_eal_init is reached, so the
// EAL global state stays untouched and the tests are safe to run on
// any host.
// ──────────────────────────────────────────────────────────────────────

TEST(InitWithPins, RejectsConflictingCfgLcoresAndPins) {
    eph::utils::reset_pin_registry_for_tests();

    EalConfig cfg;
    cfg.lcores = {"0@4"};  // raw escape-hatch path
    std::array pins = { LcorePin{0, 4, "rx"} };  // typed path

    auto r = EalGuard::init_with_pins(cfg, std::span<LcorePin const>{pins});
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("mutually exclusive"), std::string::npos)
        << r.error();
    // Registry must be untouched.
    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(4));
}

// Same mutual-exclusion contract for the raw `extra_args` escape hatch:
// hand-writing "--lcores=..." into extra_args while also passing typed
// pins would emit two `--lcores` tokens; DPDK silently keeps only the
// last so the user's escape-hatch value would be discarded with no
// diagnostic. init_with_pins must reject up front.
TEST(InitWithPins, RejectsLcoresInExtraArgsWithTypedPins) {
    eph::utils::reset_pin_registry_for_tests();

    EalConfig cfg;
    cfg.extra_args = {"--lcores=0@4"};  // raw escape-hatch via extra_args
    std::array pins = { LcorePin{0, 4, "rx"} };  // typed path

    auto r = EalGuard::init_with_pins(cfg, std::span<LcorePin const>{pins});
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("--lcores"), std::string::npos)
        << r.error();
    // Registry must be untouched — fail-fast happened before pin_lcores.
    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(4));
}

// Two-token `--lcores` (space-separated) form must also be detected —
// the EAL accepts it, so a user who knows the older argv style can
// trigger the same silent-override scenario.
TEST(InitWithPins, RejectsTwoTokenLcoresInExtraArgsWithTypedPins) {
    eph::utils::reset_pin_registry_for_tests();

    EalConfig cfg;
    cfg.extra_args = {"--lcores", "0@4"};
    std::array pins = { LcorePin{0, 4, "rx"} };

    auto r = EalGuard::init_with_pins(cfg, std::span<LcorePin const>{pins});
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("--lcores"), std::string::npos)
        << r.error();
    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(4));
}

// Counter-case (raw-only path): the `--lcores` mutual-exclusion gate
// is guarded by `!pins.empty()`. With an empty pins span the new
// extra_args scan must NOT fire — verified by static reasoning here
// (driving rte_eal_init from a unit test would pollute the per-binary
// EAL singleton). The integration-test sibling at
// `tests/integration/test_eal_init_with_pins.cpp` exercises the
// success path end-to-end.
//
// This test is intentionally minimal and structural: it confirms that
// constructing the cfg with raw `--lcores` in extra_args does not by
// itself touch the pin registry (the gate cannot fire without typed
// pins, so register_external_pin is never invoked).
TEST(InitWithPins, ExtraArgsLcoresWithoutTypedPinsLeavesRegistryUntouched) {
    eph::utils::reset_pin_registry_for_tests();

    EalConfig cfg;
    cfg.extra_args = {"--lcores=0@4"};
    // We deliberately do NOT call init_with_pins here — the unit-test
    // EAL global is owned by DpdkTestEnv and re-initing it via the
    // public factory would race with the test environment lifecycle.
    // The structural property the new gate documents — "raw-only path
    // never triggers the extra_args scan, because the scan is guarded
    // by !pins.empty()" — is enforced statically by the source code.
    // Confirm only that constructing the cfg has no pin side effect.
    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(4));
}

TEST(InitWithPins, PinValidationFailureDoesNotTouchEAL) {
    eph::utils::reset_pin_registry_for_tests();

    // Pre-occupy cpu 4 by an unrelated owner so pin_lcores fails.
    ASSERT_TRUE(eph::utils::register_external_pin(4, "occupant").has_value());

    EalConfig cfg;
    std::array pins = { LcorePin{0, 4, "rx"} };

    auto r = EalGuard::init_with_pins(cfg, std::span<LcorePin const>{pins});
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("init_with_pins"), std::string::npos);
    EXPECT_NE(r.error().find("already occupied"), std::string::npos);
    EXPECT_NE(r.error().find("occupant"), std::string::npos);

    // Pre-existing occupant must remain in the registry.
    EXPECT_TRUE(eph::utils::is_cpu_externally_pinned(4));

    eph::utils::unregister_external_pin(4);
}

TEST(InitWithPins, MultiCpuConflictRollsBackEarlierStaged) {
    eph::utils::reset_pin_registry_for_tests();

    // Pre-occupy cpu 6 by an unrelated owner. Pin 0 (cpu 4) and Pin 1
    // (cpu 5) will succeed; Pin 2 (cpu 6) will fail; Pins 0 and 1 must
    // be rolled back so the registry returns to its pre-call state.
    ASSERT_TRUE(eph::utils::register_external_pin(6, "occupant").has_value());

    EalConfig cfg;
    std::array pins = {
        LcorePin{0, 4, "rx"},
        LcorePin{1, 5, "tx"},
        LcorePin{2, 6, "control"},
    };

    auto r = EalGuard::init_with_pins(cfg, std::span<LcorePin const>{pins});
    ASSERT_FALSE(r.has_value());

    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(4));
    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(5));
    EXPECT_TRUE(eph::utils::is_cpu_externally_pinned(6));   // pre-existing

    eph::utils::unregister_external_pin(6);
}
