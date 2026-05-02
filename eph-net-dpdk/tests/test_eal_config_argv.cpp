/// @file test_eal_config_argv.cpp
/// Pure-logic tests for `eph::dpdk::build_eal_argv` — assembles the EAL
/// command-line argv from a typed `EalConfig`.
///
/// These tests deliberately do NOT include `dpdk_test_env.hpp` and never
/// call `rte_eal_init`: `build_eal_argv` is a cold-path helper that
/// transforms a struct into a `std::vector<std::string>`, with no DPDK
/// runtime side-effects. Running them safely coexists with another DPDK
/// process holding hugepages on the host.
///
/// The pre-existing tests for the EAL plumbing
/// (`test_eal_init_with_pins`) all require a real EAL bring-up and so are
/// gated behind hugepage / vfio-pci availability. This file fills the
/// gap by pinning the argv-shape contract that those tests depend on.

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/proc_type.hpp"

using eph::dpdk::EalConfig;
using eph::dpdk::ProcType;
using eph::dpdk::build_eal_argv;

namespace {

// Convenience: does `argv` contain `s` (exact match)?
bool contains(const std::vector<std::string>& argv, std::string_view s) {
    return std::find(argv.begin(), argv.end(), s) != argv.end();
}

// Find the index of `s` in `argv`, or -1.
int index_of(const std::vector<std::string>& argv, std::string_view s) {
    auto it = std::find(argv.begin(), argv.end(), s);
    return it == argv.end() ? -1 : static_cast<int>(it - argv.begin());
}

} // namespace

TEST(BuildEalArgv, DefaultEmitsOnlyProgramName) {
    // A default-constructed EalConfig should serialize to exactly one
    // argv entry — the program name. No --proc-type, no --file-prefix,
    // no -l, no -a. This is the minimal EAL bring-up: a single primary
    // with no NIC pinning.
    EalConfig cfg{};
    auto argv = build_eal_argv(cfg);
    ASSERT_EQ(argv.size(), 1u);
    EXPECT_EQ(argv[0], "eph_app");
}

TEST(BuildEalArgv, CustomProgramName) {
    EalConfig cfg{};
    cfg.program_name = "my_trader";
    auto argv = build_eal_argv(cfg);
    ASSERT_GE(argv.size(), 1u);
    EXPECT_EQ(argv[0], "my_trader");
}

TEST(BuildEalArgv, ProcTypePrimaryEmitted) {
    // Setting proc_type_set=true for Primary must emit "--proc-type primary"
    // — explicit primary is occasionally needed when the default is being
    // overridden via env var.
    EalConfig cfg{};
    cfg.proc_type     = ProcType::Primary;
    cfg.proc_type_set = true;
    auto argv = build_eal_argv(cfg);

    int idx = index_of(argv, "--proc-type");
    ASSERT_GE(idx, 0) << "--proc-type missing from argv";
    ASSERT_LT(idx + 1, static_cast<int>(argv.size()));
    EXPECT_EQ(argv[idx + 1], "primary");
}

TEST(BuildEalArgv, ProcTypeSecondaryEmitted) {
    EalConfig cfg{};
    cfg.proc_type     = ProcType::Secondary;
    cfg.proc_type_set = true;
    auto argv = build_eal_argv(cfg);

    int idx = index_of(argv, "--proc-type");
    ASSERT_GE(idx, 0);
    EXPECT_EQ(argv[idx + 1], "secondary");
}

TEST(BuildEalArgv, ProcTypeAutoEmitted) {
    // `--proc-type=auto` is no longer used by the public Platform
    // factories (post-daemon-reshape: applications go through
    // `Platform::create` with proc_type=Secondary, the daemon goes
    // through `Platform::serve_nic` with proc_type=Primary). The
    // enum value and serializer remain for any caller that drives
    // EAL by hand. A regression that dropped Auto from the
    // to_eal_string switch (returning "primary" via the
    // conservative-default fallback) would silently break those
    // callers.
    EalConfig cfg{};
    cfg.proc_type     = ProcType::Auto;
    cfg.proc_type_set = true;
    auto argv = build_eal_argv(cfg);

    int idx = index_of(argv, "--proc-type");
    ASSERT_GE(idx, 0);
    EXPECT_EQ(argv[idx + 1], "auto");
}

TEST(BuildEalArgv, ProcTypeSuppressedWhenNotSet) {
    // Default-constructed: proc_type_set=false. No --proc-type token
    // should appear, even though `proc_type` defaults to Primary.
    EalConfig cfg{};
    cfg.proc_type = ProcType::Primary;  // proc_type_set deliberately left false
    auto argv = build_eal_argv(cfg);
    EXPECT_FALSE(contains(argv, "--proc-type"));
}

TEST(BuildEalArgv, FilePrefixEmitted) {
    EalConfig cfg{};
    cfg.file_prefix = "eph_0000_28_00_0";
    auto argv = build_eal_argv(cfg);

    int idx = index_of(argv, "--file-prefix");
    ASSERT_GE(idx, 0);
    ASSERT_LT(idx + 1, static_cast<int>(argv.size()));
    EXPECT_EQ(argv[idx + 1], "eph_0000_28_00_0");
}

TEST(BuildEalArgv, FilePrefixSuppressedWhenEmpty) {
    EalConfig cfg{};
    cfg.file_prefix = std::string_view{};
    auto argv = build_eal_argv(cfg);
    EXPECT_FALSE(contains(argv, "--file-prefix"));
}

TEST(BuildEalArgv, SingleLcoreEntryEmitsOneDashL) {
    // A single lcores entry (typical use: range syntax "0-3" so DPDK
    // gets all four lcores from one -l) must serialize to exactly one
    // -l flag.
    EalConfig cfg{};
    cfg.lcores = {"0-3"};
    auto argv = build_eal_argv(cfg);

    auto count = std::count(argv.begin(), argv.end(), std::string{"-l"});
    EXPECT_EQ(count, 1u);

    int idx = index_of(argv, "-l");
    ASSERT_GE(idx, 0);
    EXPECT_EQ(argv[idx + 1], "0-3");
}

TEST(BuildEalArgv, MultipleLcoreEntriesEachGetDashL) {
    // Pin the DPDK footgun: each lcores entry produces a separate -l
    // flag. Three entries → three -l tokens. Per the doc comment this
    // is a footgun (DPDK only honours the LAST -l), but the typed path
    // still has to emit each one for the legacy escape hatch — so the
    // contract is that `lcores.size()` -l flags appear, in order.
    EalConfig cfg{};
    cfg.lcores = {"0", "1", "2"};
    auto argv = build_eal_argv(cfg);

    auto count = std::count(argv.begin(), argv.end(), std::string{"-l"});
    EXPECT_EQ(count, 3u);

    // Ordering: the entries must follow each -l in input order, so the
    // i-th `-l` slot's next token equals the i-th `lcores` entry.
    std::vector<std::string> expected_seq{"-l", "0", "-l", "1", "-l", "2"};
    auto pos = std::search(argv.begin(), argv.end(),
                           expected_seq.begin(), expected_seq.end());
    EXPECT_NE(pos, argv.end())
        << "expected -l 0 -l 1 -l 2 sub-sequence in argv";
}

TEST(BuildEalArgv, AllowedDevsEachGetDashA) {
    EalConfig cfg{};
    cfg.allowed_devs = {"0000:28:00.0", "0000:28:00.1"};
    auto argv = build_eal_argv(cfg);

    auto count = std::count(argv.begin(), argv.end(), std::string{"-a"});
    EXPECT_EQ(count, 2u);

    std::vector<std::string> expected_seq{"-a", "0000:28:00.0",
                                           "-a", "0000:28:00.1"};
    auto pos = std::search(argv.begin(), argv.end(),
                           expected_seq.begin(), expected_seq.end());
    EXPECT_NE(pos, argv.end());
}

TEST(BuildEalArgv, ExtraArgsAppendedRaw) {
    // The escape hatch: extra_args are appended verbatim with no
    // wrapping. Used for --lcores=... and similar typed-path inputs
    // that don't fit the lcores/allowed_devs structured slots.
    EalConfig cfg{};
    cfg.extra_args = {"--lcores=0@1,1@2", "--in-memory"};
    auto argv = build_eal_argv(cfg);

    EXPECT_TRUE(contains(argv, "--lcores=0@1,1@2"));
    EXPECT_TRUE(contains(argv, "--in-memory"));
}

TEST(BuildEalArgv, ProgramNameAlwaysFirst) {
    // argv[0] is the program name regardless of which other fields
    // are populated. EAL parsers depend on this convention.
    EalConfig cfg{};
    cfg.program_name  = "lat_tcp_dpdk";
    cfg.proc_type     = ProcType::Secondary;
    cfg.proc_type_set = true;
    cfg.file_prefix   = "eph_test";
    cfg.lcores        = {"4"};
    cfg.allowed_devs  = {"0000:00:06.0"};
    cfg.extra_args    = {"--in-memory"};

    auto argv = build_eal_argv(cfg);
    ASSERT_FALSE(argv.empty());
    EXPECT_EQ(argv[0], "lat_tcp_dpdk");
}

TEST(BuildEalArgv, FullCompositionOrderingPreserved) {
    // Pin the documented argv layout: program_name, --proc-type,
    // --file-prefix, then -l groups, then -a groups, then extra_args.
    // A reordering in `build_eal_argv` would break callers that
    // post-parse the argv (e.g. logging or unit-testing the structure).
    EalConfig cfg{};
    cfg.program_name  = "app";
    cfg.proc_type     = ProcType::Secondary;
    cfg.proc_type_set = true;
    cfg.file_prefix   = "fp";
    cfg.lcores        = {"0-3"};
    cfg.allowed_devs  = {"00:01.0"};
    cfg.extra_args    = {"--in-memory"};

    auto argv = build_eal_argv(cfg);

    // Find index of every fixture token; assert monotonic ordering.
    int i_proc = index_of(argv, "--proc-type");
    int i_fp   = index_of(argv, "--file-prefix");
    int i_l    = index_of(argv, "-l");
    int i_a    = index_of(argv, "-a");
    int i_ex   = index_of(argv, "--in-memory");

    EXPECT_EQ(index_of(argv, "app"), 0);
    ASSERT_GE(i_proc, 0);
    ASSERT_GE(i_fp,   0);
    ASSERT_GE(i_l,    0);
    ASSERT_GE(i_a,    0);
    ASSERT_GE(i_ex,   0);

    EXPECT_LT(i_proc, i_fp);
    EXPECT_LT(i_fp,   i_l);
    EXPECT_LT(i_l,    i_a);
    EXPECT_LT(i_a,    i_ex);
}

// ─────────────────────────────────────────────────────────────────────────────
// Boundary / negative cases.
// ─────────────────────────────────────────────────────────────────────────────

TEST(BuildEalArgv, EmptyLcoresAndAllowedDevsEmitNoPrefixes) {
    // Empty containers must not emit dangling `-l` / `-a` flags.
    // build_eal_argv loops `for (... : cfg.lcores)` and the same for
    // allowed_devs, so a stray default-constructed entry would surface
    // as a `-l ""` (a literal empty value EAL would reject as bad
    // syntax). This pins that the default container is genuinely empty.
    EalConfig cfg{};
    cfg.program_name = "x";
    auto argv = build_eal_argv(cfg);

    EXPECT_FALSE(contains(argv, "-l"));
    EXPECT_FALSE(contains(argv, "-a"));
    EXPECT_FALSE(contains(argv, ""));
}

TEST(BuildEalArgv, FilePrefixPreservesSpecialCharactersVerbatim) {
    // EAL --file-prefix is an opaque memzone-name fragment from DPDK's
    // POV; the typed config layer must not normalize / sanitize it.
    // Verify three classes of "special" content survive untouched:
    //   1) embedded '/' (path-like) — DPDK rejects, but it's the user's
    //      responsibility to provide a valid prefix; build_eal_argv
    //      isn't a validator.
    //   2) embedded space — DPDK's argv splitter tokenizes on argv
    //      boundaries (NOT internal whitespace) so this round-trips.
    //   3) embedded '=' — distinct from `--file-prefix=foo` because
    //      build_eal_argv emits the two-token form unconditionally.
    for (const std::string fp : {"path/with/slash",
                                  "name with space",
                                  "k=v=k"}) {
        EalConfig cfg{};
        cfg.program_name = "x";
        cfg.file_prefix  = fp;
        auto argv = build_eal_argv(cfg);

        int idx = index_of(argv, "--file-prefix");
        ASSERT_GE(idx, 0) << "fp='" << fp << "' missing --file-prefix flag";
        ASSERT_LT(idx + 1, static_cast<int>(argv.size()));
        EXPECT_EQ(argv[idx + 1], fp)
            << "fp='" << fp << "' was not preserved verbatim";
    }
}

TEST(BuildEalArgv, ExtraArgsAppendedInGivenOrderAfterAllStructuredFlags) {
    // Multiple extra_args entries must appear in user-supplied order
    // (a downstream EAL parser may treat first-wins or last-wins for
    // duplicate flags; either way, the typed layer's contract is to
    // not silently reorder).
    EalConfig cfg{};
    cfg.program_name = "x";
    cfg.lcores       = {"0"};
    cfg.allowed_devs = {"00:01.0"};
    cfg.extra_args   = {"--first", "--second", "--third"};
    auto argv = build_eal_argv(cfg);

    int i_first  = index_of(argv, "--first");
    int i_second = index_of(argv, "--second");
    int i_third  = index_of(argv, "--third");
    ASSERT_GE(i_first,  0);
    ASSERT_GE(i_second, 0);
    ASSERT_GE(i_third,  0);
    EXPECT_LT(i_first, i_second);
    EXPECT_LT(i_second, i_third);

    // And every extra_arg must be after the structured `-l` / `-a` block.
    int i_l = index_of(argv, "-l");
    int i_a = index_of(argv, "-a");
    ASSERT_GE(i_l, 0);
    ASSERT_GE(i_a, 0);
    EXPECT_LT(i_l, i_first);
    EXPECT_LT(i_a, i_first);
}
