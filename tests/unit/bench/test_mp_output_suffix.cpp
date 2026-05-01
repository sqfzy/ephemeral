/// @file tests/unit/bench/test_mp_output_suffix.cpp
/// Pure-function coverage of `bench::mp_output_suffix`.
///
/// `mp_output_suffix` adds `_pid<N>` to the JSON output filename when
/// the bench is running in MP autojoin mode (so two peers don't collide
/// on the same output file). The detection predicate must stay in
/// lockstep with `load_dpdk_env`'s strict-parse contract for
/// `EPH_LAT_AUTOJOIN_MAX_PROCS` (commit `c7989efb`); otherwise a typo'd
/// envvar would silently produce an unwanted `_pid` suffix on a
/// kernel-mode bench run that never engages DPDK.
///
/// These tests do not touch DPDK at runtime and do not depend on
/// EPH_USE_DPDK — `mp_output_suffix` is pure stdlib + getenv.

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

constexpr const char* kEnv = "EPH_LAT_AUTOJOIN_MAX_PROCS";

}  // namespace

// ── Negative cases — no suffix ─────────────────────────────────────────

TEST(MpOutputSuffix, UnsetEnv_ReturnsEmpty) {
    EnvGuard g(kEnv);
    g.unset();
    EXPECT_EQ(bench::mp_output_suffix(), "");
}

TEST(MpOutputSuffix, EmptyEnv_ReturnsEmpty) {
    EnvGuard g(kEnv);
    g.set("");
    EXPECT_EQ(bench::mp_output_suffix(), "");
}

TEST(MpOutputSuffix, NonNumericEnv_ReturnsEmpty) {
    // Strict-parse contract: load_dpdk_env rejects "garbage" up-front.
    // The suffix predicate must agree, otherwise a typo'd env var on a
    // kernel-mode run silently produces an unwanted `_pid<N>` suffix.
    EnvGuard g(kEnv);
    g.set("garbage");
    EXPECT_EQ(bench::mp_output_suffix(), "");
}

TEST(MpOutputSuffix, BelowMin_ReturnsEmpty) {
    // load_dpdk_env requires [2, 64]. 0 and 1 are not valid MP counts.
    EnvGuard g(kEnv);
    g.set("0");
    EXPECT_EQ(bench::mp_output_suffix(), "");
    g.set("1");
    EXPECT_EQ(bench::mp_output_suffix(), "");
}

TEST(MpOutputSuffix, AboveMax_ReturnsEmpty) {
    EnvGuard g(kEnv);
    g.set("65");
    EXPECT_EQ(bench::mp_output_suffix(), "");
    g.set("999999");
    EXPECT_EQ(bench::mp_output_suffix(), "");
}

TEST(MpOutputSuffix, NegativeEnv_ReturnsEmpty) {
    // strtoul wraps negatives to huge values; the `< 2` check would
    // miss this without the explicit string-form guard. Verify the
    // strict-parse rejects this case rather than producing a giant
    // mp_raw that satisfies `mp_raw <= 64` only by accident.
    EnvGuard g(kEnv);
    g.set("-1");
    EXPECT_EQ(bench::mp_output_suffix(), "");
}

TEST(MpOutputSuffix, TrailingGarbage_ReturnsEmpty) {
    // strtoul stops at the first non-digit; the `*end != '\0'` guard
    // is what catches "2x" / "3 " etc.
    EnvGuard g(kEnv);
    g.set("2x");
    EXPECT_EQ(bench::mp_output_suffix(), "");
    g.set("3 ");
    EXPECT_EQ(bench::mp_output_suffix(), "");
}

// ── Positive cases — suffix emitted ────────────────────────────────────

TEST(MpOutputSuffix, MinValid_ReturnsPidSuffix) {
    EnvGuard g(kEnv);
    g.set("2");
    const std::string expected =
        "_pid" + std::to_string(::getpid());
    EXPECT_EQ(bench::mp_output_suffix(), expected);
}

TEST(MpOutputSuffix, MaxValid_ReturnsPidSuffix) {
    EnvGuard g(kEnv);
    g.set("64");
    const std::string expected =
        "_pid" + std::to_string(::getpid());
    EXPECT_EQ(bench::mp_output_suffix(), expected);
}

TEST(MpOutputSuffix, MidValid_ReturnsPidSuffix) {
    EnvGuard g(kEnv);
    g.set("7");
    const std::string expected =
        "_pid" + std::to_string(::getpid());
    EXPECT_EQ(bench::mp_output_suffix(), expected);
}
