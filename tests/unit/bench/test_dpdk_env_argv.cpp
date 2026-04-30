/// @file tests/unit/bench/test_dpdk_env_argv.cpp
/// Unit tests: pure-function coverage of `bench::synthesize_eal_argv`.
///
/// Rationale (plan D-5): `bench::load_dpdk_env` is a thin glue layer on
/// top of `DpdkBenchEnv::create_full`, which requires real EAL init and
/// cannot run on a CI host without DPDK. The only piece of real logic
/// worth unit-testing is the string-level argv synthesis: given a
/// comma-separated core list and an optional PCI BDF, does it produce
/// the expected argv array?
///
/// These tests do NOT touch DPDK at runtime — they only compile-gate on
/// `EPH_USE_DPDK` because the header itself is `#ifdef EPH_USE_DPDK`.

#ifdef EPH_USE_DPDK

#  include <string>
#  include <vector>

#  include <gtest/gtest.h>

#  include "core/dpdk_env.hpp"

// Helper: render the synthesized argv as a single space-joined string
// for easy `ASSERT_EQ` comparisons. This is a test-only convenience —
// production code walks the vector directly.
namespace {
std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i > 0) out.push_back(' ');
        out += v[i];
    }
    return out;
}
}  // namespace

// ── T1: single-core, realistic PCI BDF ─────────────────────────────
//
// Exercises the canonical "small bench box" layout with EAL on lcore 0
// and a single PCI device allow-listed. Verifies the full argv shape
// including the `--proc-type=auto`, `--log-level=lib.eal:warning`, and
// terminating `--` that `DpdkBenchEnv::create_full` expects.
TEST(DpdkEnvArgv, SingleCoreWithPci) {
    auto argv = bench::synthesize_eal_argv("0", "0000:28:00.0");

    ASSERT_EQ(argv.size(), 8u);
    EXPECT_EQ(argv[0], "lat_bench");
    EXPECT_EQ(argv[1], "-l");
    EXPECT_EQ(argv[2], "0");
    EXPECT_EQ(argv[3], "-a");
    EXPECT_EQ(argv[4], "0000:28:00.0");
    EXPECT_EQ(argv[5], "--proc-type=auto");
    EXPECT_EQ(argv[6], "--log-level=lib.eal:warning");
    EXPECT_EQ(argv[7], "--");

    // Joined form makes a regression-proof golden for the whole line.
    EXPECT_EQ(join(argv),
              "lat_bench -l 0 -a 0000:28:00.0 "
              "--proc-type=auto --log-level=lib.eal:warning --");
}

// ── T2: multi-core bench layout, different PCI BDF ────────────────
//
// Two points: (a) the core-list is passed through verbatim (no splitting,
// no normalising), and (b) a different BDF format string is preserved
// character-for-character so we cannot accidentally strip a leading `0000:`.
TEST(DpdkEnvArgv, MultiCoreWithDifferentPci) {
    auto argv = bench::synthesize_eal_argv("2,3,4", "0000:5e:00.1");

    ASSERT_EQ(argv.size(), 8u);
    EXPECT_EQ(argv[2], "2,3,4");
    EXPECT_EQ(argv[4], "0000:5e:00.1");

    // Full line comparison guards against any positional reshuffling.
    EXPECT_EQ(join(argv),
              "lat_bench -l 2,3,4 -a 0000:5e:00.1 "
              "--proc-type=auto --log-level=lib.eal:warning --");
}

// ── T3: empty PCI BDF → `-a` pair is omitted ──────────────────────
//
// Used by the single-NIC smoke-test path (future) where the user wants
// the EAL to auto-detect all devices. The argv must be shorter by two
// entries; `-l` + core list and the three trailer flags stay.
TEST(DpdkEnvArgv, EmptyPciOmitsAllowList) {
    auto argv = bench::synthesize_eal_argv("0,1", "");

    ASSERT_EQ(argv.size(), 6u);
    EXPECT_EQ(argv[0], "lat_bench");
    EXPECT_EQ(argv[1], "-l");
    EXPECT_EQ(argv[2], "0,1");
    EXPECT_EQ(argv[3], "--proc-type=auto");
    EXPECT_EQ(argv[4], "--log-level=lib.eal:warning");
    EXPECT_EQ(argv[5], "--");

    // Confirm `-a` never appears when pci_bdf is empty.
    for (const auto& s : argv) {
        EXPECT_NE(s, "-a") << "empty pci_bdf must not emit -a";
    }
}

// ── parse_eal_cores_csv — typed CSV → LcorePin list ──────────────────
//
// The typed pin path (`bench::parse_eal_cores_csv`) is what production
// bench bring-up uses (replaces the legacy `-l <csv>` argv lowering).
// Pre-fix, atoi-based parsing silently coerced "abc" → 0 and stacked
// multiple lcores onto cpu 0 — directly breaking the project rule that
// every bench thread pin to its own cpu. These tests pin the strict
// strtol-based parser.

TEST(DpdkEnvArgv, ParseCsv_HappyPath) {
    auto r = bench::parse_eal_cores_csv("0,4,5");
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->size(), 3u);
    EXPECT_EQ((*r)[0].lcore_id, 0);
    EXPECT_EQ((*r)[0].cpu_id, 0);
    EXPECT_EQ((*r)[1].lcore_id, 1);
    EXPECT_EQ((*r)[1].cpu_id, 4);
    EXPECT_EQ((*r)[2].lcore_id, 2);
    EXPECT_EQ((*r)[2].cpu_id, 5);
}

TEST(DpdkEnvArgv, ParseCsv_TrimsWhitespace) {
    auto r = bench::parse_eal_cores_csv(" 0 , 4 , 5 ");
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->size(), 3u);
    EXPECT_EQ((*r)[1].cpu_id, 4);
}

TEST(DpdkEnvArgv, ParseCsv_RejectsNonDigitToken) {
    // Pre-fix, atoi("abc") returned 0 and silently created a pin on cpu 0.
    auto r = bench::parse_eal_cores_csv("0,abc,5");
    EXPECT_FALSE(r.has_value())
        << "non-digit token 'abc' must be rejected";
}

TEST(DpdkEnvArgv, ParseCsv_RejectsTrailingGarbage) {
    // "5x" is exactly the kind of typo that atoi swallowed (parses '5'
    // and ignores 'x'). strtol's `end` pointer flags the unconsumed 'x'.
    auto r = bench::parse_eal_cores_csv("0,5x");
    EXPECT_FALSE(r.has_value())
        << "trailing garbage in '5x' must be rejected";
}

TEST(DpdkEnvArgv, ParseCsv_RejectsNegative) {
    auto r = bench::parse_eal_cores_csv("0,-3");
    EXPECT_FALSE(r.has_value())
        << "negative cpu must be rejected";
}

TEST(DpdkEnvArgv, ParseCsv_SkipsEmptyTokens) {
    // ",,3" — leading + double comma. The whitespace-stripping path
    // collapses these to nothing; only "3" produces a pin.
    auto r = bench::parse_eal_cores_csv(",,3");
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].cpu_id, 3);
    EXPECT_EQ((*r)[0].lcore_id, 0);
}

#else

// Compile-time fallback on non-DPDK builds so the test target still
// builds cleanly (the `eph-test` rule links gtest_main which provides
// the entry point). We emit a single SKIP-style test so the binary is
// non-empty and the gtest runner reports the expected count.
#  include <gtest/gtest.h>
TEST(DpdkEnvArgv, SkipOnNonDpdkBuild) {
    GTEST_SKIP() << "EPH_USE_DPDK not defined — argv helper compiled out";
}

#endif  // EPH_USE_DPDK
