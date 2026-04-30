/// @file test_dpdk_multiprocess_config.cpp
/// Unit tests for phase-1 / phase-2 multi-process scaffolding:
///   - PlatformConfig validation (Primary default + Secondary contract)
///   - build_eal_argv serialization
///   - rr_counter range algorithm `lo + fetch_add % (hi - lo)`
///
/// Pure unit tests — no DPDK runtime / NIC required. They run under the
/// shared `dpdk_test_env` that boots EAL in --no-pci --no-huge mode, so
/// they're safe on any host.

#include <atomic>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/platform.hpp"

using eph::dpdk::EalConfig;
using eph::dpdk::PlatformConfig;
using eph::dpdk::Platform;
using eph::dpdk::ProcType;
using eph::dpdk::MpTopology;
using eph::dpdk::ProcSpec;
using eph::dpdk::build_eal_argv;

// ─────────────────────────────────────────────────────────────────────────────
// PlatformConfig — secondary-mode contract validation
// ─────────────────────────────────────────────────────────────────────────────

namespace {
// Build a valid Secondary-mode config. Tests tweak one field at a time.
// nb_rx_queues = 4 lets us pick rx_queue_range = {2, 4} as a non-empty
// sub-range that passes validate_config.
PlatformConfig valid_secondary_cfg() {
    PlatformConfig cfg{};
    cfg.port_id        = 0;
    cfg.nb_rx_queues   = 4;
    cfg.proc_type      = ProcType::Secondary;
    cfg.file_prefix    = "eph_mp_test";
    cfg.rx_queue_range = {2, 4};
    return cfg;
}
} // namespace

TEST(CreateSecondaryValidation, ValidSecondaryConfigPassesContractLayer) {
    // The call will fail later (no primary is running, no shared mempool),
    // but the contract-layer checks must accept this cfg.
    auto cfg = valid_secondary_cfg();
    auto r = Platform::create_secondary(cfg);
    ASSERT_FALSE(r);
    // Error must NOT contain the secondary-contract reason strings.
    EXPECT_EQ(r.error().find("file_prefix must be non-empty"), std::string::npos)
        << r.error();
    EXPECT_EQ(r.error().find("rx_queue_range"),
              std::string::npos)
        << r.error();
}

TEST(CreateSecondaryValidation, EmptyFilePrefixRejected) {
    auto cfg = valid_secondary_cfg();
    cfg.file_prefix = "";
    auto r = Platform::create_secondary(cfg);
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().find("file_prefix must be non-empty"),
              std::string::npos)
        << "expected file_prefix rejection, got: " << r.error();
}

TEST(CreateSecondaryValidation, InvertedRxQueueRangeRejected) {
    // The structural validator now catches this for both Primary and
    // Secondary callers; verify create_secondary surfaces it correctly.
    auto cfg = valid_secondary_cfg();
    cfg.rx_queue_range = {4, 2};
    auto r = Platform::create_secondary(cfg);
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().find("rx_queue_range: lo must be < hi"),
              std::string::npos)
        << r.error();
}

TEST(CreateSecondaryValidation, FullRangeSentinelAcceptedAtValidator) {
    // {0, 0} is the sentinel meaning "full range"; must pass validation.
    auto cfg = valid_secondary_cfg();
    cfg.rx_queue_range = {0, 0};
    auto r = Platform::create_secondary(cfg);
    ASSERT_FALSE(r);  // will still fail later — but not on rx_queue_range
    EXPECT_EQ(r.error().find("rx_queue_range:"),
              std::string::npos)
        << r.error();
}

// ── M1 regression: validate_config now policies rx_queue_range ──
//
// Pre-M1, a primary-mode `cfg.rx_queue_range = {3, 3}` would slip past
// validation and land in `create_and_attach`'s round-robin selector,
// where the fallback ternary swallowed the bad input silently. The
// validator now rejects any non-sentinel range that is empty / inverted
// / out-of-bounds.

TEST(ValidateConfigRxQueueRange, EmptyRangeRejected) {
    PlatformConfig cfg{};
    cfg.nb_rx_queues   = 4;
    cfg.rx_queue_range = {3, 3};   // lo == hi, non-sentinel
    auto err = eph::dpdk::validate_config(cfg);
    EXPECT_NE(err.find("rx_queue_range: lo must be < hi"),
              std::string_view::npos)
        << "got: " << err;
}

TEST(ValidateConfigRxQueueRange, InvertedRangeRejected) {
    PlatformConfig cfg{};
    cfg.nb_rx_queues   = 4;
    cfg.rx_queue_range = {3, 1};
    auto err = eph::dpdk::validate_config(cfg);
    EXPECT_NE(err.find("rx_queue_range: lo must be < hi"),
              std::string_view::npos)
        << "got: " << err;
}

TEST(ValidateConfigRxQueueRange, RangeOutOfBoundsRejected) {
    PlatformConfig cfg{};
    cfg.nb_rx_queues   = 4;
    cfg.rx_queue_range = {0, 8};   // hi > nb_rx_queues
    auto err = eph::dpdk::validate_config(cfg);
    EXPECT_NE(err.find("rx_queue_range.hi must not exceed nb_rx_queues"),
              std::string_view::npos)
        << "got: " << err;
}

TEST(ValidateConfigRxQueueRange, FullRangeSentinelAccepted) {
    PlatformConfig cfg{};
    cfg.nb_rx_queues   = 4;
    cfg.rx_queue_range = {0, 0};   // sentinel
    auto err = eph::dpdk::validate_config(cfg);
    EXPECT_TRUE(err.empty()) << "sentinel should pass: " << err;
}

TEST(ValidateConfigRxQueueRange, ValidSubRangeAccepted) {
    PlatformConfig cfg{};
    cfg.nb_rx_queues   = 4;
    cfg.rx_queue_range = {2, 4};   // valid sub-range
    auto err = eph::dpdk::validate_config(cfg);
    EXPECT_TRUE(err.empty()) << "valid sub-range should pass: " << err;
}

// ─────────────────────────────────────────────────────────────────────────────
// PlatformConfig.mp_topology — auto-derived MP layout (reshape stage 3)
// ─────────────────────────────────────────────────────────────────────────────

TEST(ValidateConfigMpTopology, NoMpTopologyPreservesLegacyBehavior) {
    // Sanity: a config without mp_topology behaves byte-for-byte like
    // the pre-reshape baseline — both the {0,0} sentinel and a manual
    // sub-range are accepted unchanged.
    PlatformConfig cfg_sentinel{};
    cfg_sentinel.nb_rx_queues = 4;
    EXPECT_TRUE(eph::dpdk::validate_config(cfg_sentinel).empty());

    PlatformConfig cfg_manual{};
    cfg_manual.nb_rx_queues   = 4;
    cfg_manual.rx_queue_range = {2, 4};
    EXPECT_TRUE(eph::dpdk::validate_config(cfg_manual).empty());
}

TEST(ValidateConfigMpTopology, MpTopologyOnlyAccepted) {
    PlatformConfig cfg{};
    cfg.nb_rx_queues = 4;
    cfg.mp_topology  = MpTopology::uniform(/*self_index=*/0,
                                           /*total_procs=*/2,
                                           /*nb_rx_queues=*/4);
    // rx_queue_range left at default {0,0} — the recommended path.
    auto err = eph::dpdk::validate_config(cfg);
    EXPECT_TRUE(err.empty()) << "mp_topology + sentinel rx_queue_range should pass: " << err;
}

TEST(ValidateConfigMpTopology, MpTopologyAndManualRangeConflict) {
    PlatformConfig cfg{};
    cfg.nb_rx_queues   = 4;
    cfg.mp_topology    = MpTopology::uniform(0, 2, 4);
    cfg.rx_queue_range = {0, 2};   // forbidden combination
    auto err = eph::dpdk::validate_config(cfg);
    EXPECT_NE(err.find("mp_topology is set; rx_queue_range must remain {0,0}"),
              std::string_view::npos)
        << "got: " << err;
}

TEST(ValidateConfigMpTopology, MpTopologyInvalidRejected) {
    PlatformConfig cfg{};
    cfg.nb_rx_queues = 4;
    // Invalid: empty topology fails MpTopology::valid().
    cfg.mp_topology  = MpTopology{};
    auto err = eph::dpdk::validate_config(cfg);
    EXPECT_NE(err.find("mp_topology failed valid()"),
              std::string_view::npos)
        << "got: " << err;
}

TEST(ValidateConfigMpTopology, SelfQueueHiExceedsNbRxQueuesRejected) {
    PlatformConfig cfg{};
    cfg.nb_rx_queues = 2;
    // Topology declares queues up to 4, but cfg.nb_rx_queues only 2.
    cfg.mp_topology  = MpTopology::uniform(/*self_index=*/1,
                                           /*total_procs=*/2,
                                           /*nb_rx_queues=*/4);
    auto err = eph::dpdk::validate_config(cfg);
    EXPECT_NE(err.find("queue_hi exceeds nb_rx_queues"),
              std::string_view::npos)
        << "got: " << err;
}

TEST(CreateSecondaryValidation, PrimaryProcTypeInputDoesNotShortCircuitValidation) {
    // Caller passes Primary by mistake — factory must still run secondary
    // validation. We can't directly observe the proc_type coercion (no
    // public getter), but we can verify the secondary contract checks
    // still fire as if proc_type=Secondary.
    auto cfg = valid_secondary_cfg();
    cfg.proc_type = ProcType::Primary;
    cfg.file_prefix = "";   // would still pass *primary* path; must fail in secondary
    auto r = Platform::create_secondary(cfg);
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().find("file_prefix must be non-empty"),
              std::string::npos)
        << "create_secondary should still enforce file_prefix even when "
           "caller mis-set proc_type=Primary; got: " << r.error();
}

// ─────────────────────────────────────────────────────────────────────────────
// build_eal_argv — serialization correctness
// ─────────────────────────────────────────────────────────────────────────────

TEST(BuildEalArgv, DefaultsEmitOnlyProgramName) {
    EalConfig cfg{};
    auto argv = build_eal_argv(cfg);
    ASSERT_EQ(argv.size(), 1u);
    EXPECT_EQ(argv[0], "eph_app");
}

TEST(BuildEalArgv, PrimaryWithFilePrefix) {
    EalConfig cfg{};
    cfg.program_name  = "my_primary";
    cfg.proc_type     = ProcType::Primary;
    cfg.proc_type_set = true;
    cfg.file_prefix   = "eph_mp_demo";
    auto argv = build_eal_argv(cfg);
    ASSERT_EQ(argv.size(), 5u);
    EXPECT_EQ(argv[0], "my_primary");
    EXPECT_EQ(argv[1], "--proc-type");
    EXPECT_EQ(argv[2], "primary");
    EXPECT_EQ(argv[3], "--file-prefix");
    EXPECT_EQ(argv[4], "eph_mp_demo");
}

TEST(BuildEalArgv, SecondaryWithLcoresAndAllowedDevs) {
    EalConfig cfg{};
    cfg.program_name  = "my_secondary";
    cfg.proc_type     = ProcType::Secondary;
    cfg.proc_type_set = true;
    cfg.file_prefix   = "eph_mp_demo";
    cfg.lcores        = {"2-3"};
    cfg.allowed_devs  = {"0000:05:00.1"};
    auto argv = build_eal_argv(cfg);
    // program_name + 2(--proc-type) + 2(--file-prefix) + 2(-l) + 2(-a)
    ASSERT_EQ(argv.size(), 9u);
    EXPECT_EQ(argv[0], "my_secondary");
    EXPECT_EQ(argv[2], "secondary");
    EXPECT_EQ(argv[4], "eph_mp_demo");
    EXPECT_EQ(argv[5], "-l");
    EXPECT_EQ(argv[6], "2-3");
    EXPECT_EQ(argv[7], "-a");
    EXPECT_EQ(argv[8], "0000:05:00.1");
}

TEST(BuildEalArgv, ExtraArgsAppended) {
    EalConfig cfg{};
    cfg.extra_args = {"--no-pci", "--log-level=3"};
    auto argv = build_eal_argv(cfg);
    ASSERT_EQ(argv.size(), 3u);
    EXPECT_EQ(argv[1], "--no-pci");
    EXPECT_EQ(argv[2], "--log-level=3");
}

TEST(BuildEalArgv, ProcTypeNotSetOmitsFlag) {
    // Even if proc_type is Secondary by value, if proc_type_set=false we
    // don't emit --proc-type (lets the caller rely on DPDK default = auto).
    EalConfig cfg{};
    cfg.proc_type = ProcType::Secondary;
    // proc_type_set left false
    auto argv = build_eal_argv(cfg);
    for (const auto& s : argv) {
        EXPECT_NE(s, "--proc-type") << "should not emit --proc-type when not set";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// rr_counter range algorithm
// ─────────────────────────────────────────────────────────────────────────────
//
// The algorithm applied at two call sites (tcp_stream.hpp / udp_socket.hpp):
//   target_qid = base + (rr_counter.fetch_add(1, relaxed) % range);
// with (base, range) derived from Platform::effective_rx_queue_range()
// (falling back to `{0, nb_q}` when the config sentinel `{0, 0}` is used).
//
// We test the algorithm in isolation against an identical local counter.

namespace {
uint16_t rr_step(std::atomic<uint16_t>& rr, uint16_t base, uint16_t range) {
    return static_cast<uint16_t>(
        base + (rr.fetch_add(1, std::memory_order_relaxed) % range));
}
} // namespace

TEST(RrCounterRange, FullRangeFallbackEquivalentToModNbQ) {
    // When (qlo, qhi) == (0, 0), the callers fall back to (base=0, range=nb_q).
    std::atomic<uint16_t> rr{0};
    constexpr uint16_t nb_q = 4;
    for (uint16_t i = 0; i < 4 * nb_q; ++i) {
        auto q = rr_step(rr, /*base=*/0, /*range=*/nb_q);
        EXPECT_EQ(q, i % nb_q);
    }
}

TEST(RrCounterRange, PartitionedRangeStaysWithinBounds) {
    // Secondary owns queues [2, 4).
    std::atomic<uint16_t> rr{0};
    constexpr uint16_t lo = 2, hi = 4;
    const uint16_t range = hi - lo;
    std::set<uint16_t> seen;
    for (uint16_t i = 0; i < 100; ++i) {
        auto q = rr_step(rr, lo, range);
        EXPECT_GE(q, lo);
        EXPECT_LT(q, hi);
        seen.insert(q);
    }
    // Over 100 iterations we must hit every queue in the range.
    EXPECT_EQ(seen.size(), range);
}

TEST(RrCounterRange, SingleQueueRangeReturnsConstant) {
    // Edge case: range = 1 queue.
    std::atomic<uint16_t> rr{0};
    constexpr uint16_t lo = 5;
    const uint16_t range = 1;
    for (uint16_t i = 0; i < 8; ++i) {
        auto q = rr_step(rr, lo, range);
        EXPECT_EQ(q, lo);
    }
}

TEST(RrCounterRange, DistinctRangesDoNotCollide) {
    // Simulate primary [0, 2) and secondary [2, 4). Separate counters
    // (one per process in production); target_qid sets must be disjoint.
    std::atomic<uint16_t> rr_p{0};
    std::atomic<uint16_t> rr_s{0};
    std::set<uint16_t> primary_qs, secondary_qs;
    for (uint16_t i = 0; i < 16; ++i) {
        primary_qs.insert(rr_step(rr_p, /*base=*/0, /*range=*/2));
        secondary_qs.insert(rr_step(rr_s, /*base=*/2, /*range=*/2));
    }
    for (auto q : primary_qs) EXPECT_LT(q, 2);
    for (auto q : secondary_qs) {
        EXPECT_GE(q, 2);
        EXPECT_LT(q, 4);
    }
    // No intersection.
    for (auto q : primary_qs) EXPECT_EQ(secondary_qs.count(q), 0u);
}

// ── rr_counter wrap-around ──────────────────────────────────────────────────
//
// `target_qid` is computed via `rr.fetch_add(1, relaxed) % range`. The
// counter is u16 so it wraps at 65535→0 in production after enough
// `create_and_attach` calls. The bound invariant `[lo, hi)` must hold
// across the wrap; the only risk is mishandling u16 arithmetic.

TEST(RrCounterRange, WrapAroundFromMaxU16PreservesBounds) {
    // Seed rr at 65530 so we hit the wrap inside this loop.
    std::atomic<uint16_t> rr{65530};
    constexpr uint16_t lo = 4;
    constexpr uint16_t range = 3;  // queues [4, 7)
    bool saw_wrap = false;
    uint16_t prev = rr.load();
    for (uint16_t i = 0; i < 20; ++i) {
        const uint16_t cur_pre = rr.load();
        auto q = rr_step(rr, lo, range);
        EXPECT_GE(q, lo);
        EXPECT_LT(q, lo + range)
            << "wrap broke the [lo, lo+range) invariant at iter=" << i
            << " rr_pre=" << cur_pre;
        const uint16_t cur_post = rr.load();
        if (cur_post < prev) saw_wrap = true;
        prev = cur_post;
        (void)prev;
    }
    EXPECT_TRUE(saw_wrap)
        << "test seed must have walked through 65535→0 wrap to be meaningful";
}

TEST(RrCounterRange, WrapAroundWithNonPowerOfTwoRange) {
    // A range that doesn't divide 2^16 evenly slightly biases the
    // distribution at the wrap, but every produced qid still falls in
    // [lo, lo+range). 7 doesn't divide 65536; we just need the bound.
    std::atomic<uint16_t> rr{65530};
    constexpr uint16_t lo = 0;
    constexpr uint16_t range = 7;
    for (uint16_t i = 0; i < 30; ++i) {
        auto q = rr_step(rr, lo, range);
        EXPECT_GE(q, lo);
        EXPECT_LT(q, lo + range);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Platform getters — type-level contract. The actual nullptr-safe branch
// of `effective_rx_queue_range` is exercised whenever a moved-from Platform
// is used post-move; covered by integration tests where a real port exists.
// Here we only assert the symbol exists with the documented signature so
// downstream code compiles against a stable interface.
// ─────────────────────────────────────────────────────────────────────────────

TEST(PlatformEffectiveGetters, GetterTypeContract) {
    using GettersT = std::pair<uint16_t, uint16_t> (Platform::*)() const noexcept;
    GettersT g1 = &Platform::effective_rx_queue_range;
    (void)g1;
    SUCCEED();
}
