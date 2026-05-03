/// @file tests/unit/bench/test_mp_output_suffix.cpp
/// Pure-function coverage of `bench::mp_output_suffix`.
///
/// `mp_output_suffix` adds `_pid<N>` to the JSON output filename so
/// concurrently-launched peer processes do not collide on the same
/// output file. Post the 2026-05-02 daemon reshape, every `lat_*_dpdk`
/// binary is inherently a secondary that the orchestrator may launch
/// in parallel — the helper therefore appends the suffix
/// **unconditionally under `EPH_USE_DPDK` builds** and never under
/// kernel-only builds. Two overrides are honoured for back-compat:
///
///   * `EPH_LAT_FORCE_PID=0|false|no`   — force-disable
///   * `EPH_LAT_FORCE_PID=<anything else non-empty>` — force-enable
///   * `EPH_LAT_AUTOJOIN_MAX_PROCS=<2..64>` — legacy autojoin gate;
///     still enables the suffix for any out-of-tree script that still
///     exports it (strict-parse contract identical to the pre-reshape
///     `load_dpdk_env` predicate, commit `c7989efb`).
///
/// The test binary itself is built **without** `EPH_USE_DPDK` (see
/// `tests/unit/bench/xmake.lua`), so the build-mode default branch
/// returns the empty suffix — that is what the unset-env tests verify.
///
/// These tests do not touch DPDK at runtime.

#include <cstdlib>     // setenv / unsetenv / getpid via unistd
#include <string>
#include <unistd.h>    // getpid
#include <gtest/gtest.h>

#include "core/bench_ctx.hpp"

namespace {

// RAII guard that snapshots an env var on construction and restores it
// (or unsets if originally unset) on destruction. Per-test isolation.
class EnvGuard {
public:
    explicit EnvGuard(const char* name) : name_(name) {
        if (const char* v = std::getenv(name)) {
            had_value_ = true;
            saved_     = v;
        }
    }
    void set(const char* value) {
        ::setenv(name_, value, /*overwrite=*/1);
    }
    void unset() {
        ::unsetenv(name_);
    }
    ~EnvGuard() {
        if (had_value_) ::setenv(name_, saved_.c_str(), /*overwrite=*/1);
        else            ::unsetenv(name_);
    }
private:
    const char* name_;
    bool        had_value_ = false;
    std::string saved_;
};

constexpr const char* kEnv      = "EPH_LAT_AUTOJOIN_MAX_PROCS";
constexpr const char* kEnvForce = "EPH_LAT_FORCE_PID";

// Per-test fixture — clears BOTH override env vars at the start of
// every test so individual cases need only set what they want
// exercised. Without this a previous test's `EPH_LAT_FORCE_PID` set
// would leak across test boundaries and force a `_pid` suffix into a
// later "expect empty" assertion.
class MpOutputSuffix : public ::testing::Test {
protected:
    EnvGuard g_legacy_{kEnv};
    EnvGuard g_force_ {kEnvForce};

    void SetUp() override {
        g_legacy_.unset();
        g_force_.unset();
    }
};

}  // namespace

// ── Negative cases — no suffix (kernel build, no overrides) ────────────

TEST_F(MpOutputSuffix, UnsetEnv_ReturnsEmpty) {
    // No EPH_USE_DPDK on this test binary, no overrides → empty.
    EXPECT_EQ(bench::mp_output_suffix(), "");
}

TEST_F(MpOutputSuffix, EmptyEnv_ReturnsEmpty) {
    g_legacy_.set("");
    EXPECT_EQ(bench::mp_output_suffix(), "");
}

TEST_F(MpOutputSuffix, NonNumericEnv_ReturnsEmpty) {
    // Strict-parse contract: load_dpdk_env rejects "garbage" up-front.
    // The legacy-envvar branch must agree, otherwise a typo'd env var
    // would silently produce an unwanted `_pid<N>` suffix.
    g_legacy_.set("garbage");
    EXPECT_EQ(bench::mp_output_suffix(), "");
}

TEST_F(MpOutputSuffix, BelowMin_ReturnsEmpty) {
    // [2, 64] required by the legacy gate. 0 and 1 are not valid MP counts.
    g_legacy_.set("0");
    EXPECT_EQ(bench::mp_output_suffix(), "");
    g_legacy_.set("1");
    EXPECT_EQ(bench::mp_output_suffix(), "");
}

TEST_F(MpOutputSuffix, AboveMax_ReturnsEmpty) {
    g_legacy_.set("65");
    EXPECT_EQ(bench::mp_output_suffix(), "");
    g_legacy_.set("999999");
    EXPECT_EQ(bench::mp_output_suffix(), "");
}

TEST_F(MpOutputSuffix, NegativeEnv_ReturnsEmpty) {
    // strtoul wraps negatives to huge values; the `< 2` check would
    // miss this without the explicit string-form guard. Verify the
    // strict-parse rejects this case rather than producing a giant
    // mp_raw that satisfies `mp_raw <= 64` only by accident.
    g_legacy_.set("-1");
    EXPECT_EQ(bench::mp_output_suffix(), "");
}

TEST_F(MpOutputSuffix, TrailingGarbage_ReturnsEmpty) {
    // strtoul stops at the first non-digit; the `*end != '\0'` guard
    // is what catches "2x" / "3 " etc.
    g_legacy_.set("2x");
    EXPECT_EQ(bench::mp_output_suffix(), "");
    g_legacy_.set("3 ");
    EXPECT_EQ(bench::mp_output_suffix(), "");
}

// ── Positive cases — legacy envvar still honoured ──────────────────────

TEST_F(MpOutputSuffix, LegacyEnvvar_MinValid_ReturnsPidSuffix) {
    g_legacy_.set("2");
    const std::string expected =
        "_pid" + std::to_string(::getpid());
    EXPECT_EQ(bench::mp_output_suffix(), expected);
}

TEST_F(MpOutputSuffix, LegacyEnvvar_MaxValid_ReturnsPidSuffix) {
    g_legacy_.set("64");
    const std::string expected =
        "_pid" + std::to_string(::getpid());
    EXPECT_EQ(bench::mp_output_suffix(), expected);
}

TEST_F(MpOutputSuffix, LegacyEnvvar_MidValid_ReturnsPidSuffix) {
    g_legacy_.set("7");
    const std::string expected =
        "_pid" + std::to_string(::getpid());
    EXPECT_EQ(bench::mp_output_suffix(), expected);
}

// ── EPH_LAT_FORCE_PID override — wins over both build-mode default
//    and the legacy envvar.

TEST_F(MpOutputSuffix, ForcePid_NonemptyTruthy_EmitsSuffix) {
    // `1` / `yes` / arbitrary string — anything not in the disable
    // set — must force-enable the suffix even though no other gate
    // is engaged.
    const std::string expected =
        "_pid" + std::to_string(::getpid());
    for (const char* v : {"1", "true", "yes", "Y", "anything"}) {
        g_force_.set(v);
        EXPECT_EQ(bench::mp_output_suffix(), expected) << "v=" << v;
    }
}

TEST_F(MpOutputSuffix, ForcePid_DisableValues_EmitsEmpty) {
    // The disable set must cover the common falsy spellings so a user
    // typing `EPH_LAT_FORCE_PID=0` actually disables it. Under DPDK
    // builds this is the only way to opt out of the new default.
    for (const char* v : {"0", "false", "FALSE", "no", "NO"}) {
        g_force_.set(v);
        EXPECT_EQ(bench::mp_output_suffix(), "") << "v=" << v;
    }
}

TEST_F(MpOutputSuffix, ForcePid_OverridesLegacyEnvvar) {
    // EPH_LAT_FORCE_PID=0 must defeat a legacy `EPH_LAT_AUTOJOIN_MAX_PROCS=2`
    // — explicit wins over implicit. Otherwise an out-of-tree script
    // exporting both could not actually disable the suffix.
    g_legacy_.set("2");
    g_force_.set("0");
    EXPECT_EQ(bench::mp_output_suffix(), "");
}

TEST_F(MpOutputSuffix, ForcePid_EmptyValueFallsThroughToOtherGates) {
    // An empty `EPH_LAT_FORCE_PID=""` must NOT be treated as the
    // override — that would suppress the legacy envvar's effect by
    // accident on a shell that touched the var with no value.
    g_force_.set("");
    g_legacy_.set("3");
    const std::string expected =
        "_pid" + std::to_string(::getpid());
    EXPECT_EQ(bench::mp_output_suffix(), expected);
}
