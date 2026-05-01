/// @file test_platform_v3_surface.cpp
/// Stage-1 surface tests for the v3 Platform API.
///
/// **Scope**: type / signature / translation logic only. Runtime
/// behavior of `Platform::create / attach / *_with_eal / join_dynamic`
/// is verified by:
///   - integration tests under `tests/integration/dpdk_mp_*` (stage 4)
///   - examples (stage 4e/4f)
/// Booting EAL inside a unit test would conflict with concurrent test
/// runs and require vfio/hugepages, neither of which is present in the
/// unit-test environment.
///
/// What this file pins down:
///   - v3 types (`PlatformConfig`, `PlatformAttachConfig`,
///     `JoinDynamicConfig`) compile and have expected fields.
///   - v3 entry-point function pointers have the documented signatures.
///   - The v3 `validate_config(PlatformConfig)` rejection set
///     (max_procs / file_prefix / queues-per-proc invariants).

#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "eph/dpdk/lcore_pin.hpp"
#include "eph/dpdk/platform.hpp"
#include "eph/dpdk/platform_attach.hpp"
#include "eph/utils/cpu.hpp"

using namespace eph::dpdk;

// ──────────────────────────────────────────────────────────────────────
// Type-shape tests
// ──────────────────────────────────────────────────────────────────────

TEST(PlatformConfig, DefaultsAreSingleProcess) {
    PlatformConfig cfg{};
    EXPECT_EQ(cfg.max_procs, 1);
    EXPECT_EQ(cfg.queues_per_proc, 0);    // 0 = auto
    EXPECT_EQ(cfg.port_id, 0);
    EXPECT_EQ(cfg.nb_rx_queues, 1);
    EXPECT_EQ(cfg.nb_tx_queues, 1);
    EXPECT_TRUE(cfg.file_prefix.empty());
}

TEST(PlatformConfig, NicPhysicalFieldsMirrorV2Defaults) {
    // Sanity: v3's NIC physical defaults match v2's so silent semantic
    // shift is impossible in stage 1.
    PlatformConfig v3{};
    LegacyPlatformConfig   v2{};
    EXPECT_EQ(v3.nb_rx_queues,    v2.nb_rx_queues);
    EXPECT_EQ(v3.nb_tx_queues,    v2.nb_tx_queues);
    EXPECT_EQ(v3.nb_rx_desc,      v2.nb_rx_desc);
    EXPECT_EQ(v3.nb_tx_desc,      v2.nb_tx_desc);
    EXPECT_EQ(v3.mbuf_pool_size,  v2.mbuf_pool_size);
    EXPECT_EQ(v3.mbuf_cache_size, v2.mbuf_cache_size);
    EXPECT_EQ(v3.link_timeout_ms, v2.link_timeout_ms);
    EXPECT_EQ(v3.enable_promiscuous,         v2.enable_promiscuous);
    EXPECT_EQ(v3.enable_rx_checksum_offload, v2.enable_rx_checksum_offload);
    EXPECT_EQ(v3.enable_strict_rx_checksum,  v2.enable_strict_rx_checksum);
}

TEST(PlatformAttachConfig, MinimalSurface) {
    // The whole point of v3: secondary input set is tiny.
    PlatformAttachConfig cfg{};
    EXPECT_TRUE(cfg.file_prefix.empty());
    EXPECT_EQ(cfg.port_id, 0);
    EXPECT_EQ(cfg.self_lcore_mask, 0);

    // Verify dump format.
    cfg.file_prefix = "demo";
    cfg.port_id     = 1;
    auto s = cfg.dump();
    EXPECT_NE(s.find("file_prefix='demo'"), std::string::npos);
    EXPECT_NE(s.find("port_id=1"),          std::string::npos);
}

TEST(JoinDynamicConfig, NoTopLevelConsensusFields) {
    // v3 zero-consensus: queues_per_proc / max_procs are NOT top-level
    // (compile-time enforcement: the names don't exist on the struct).
    // We can't test absence directly in C++; instead verify the fields
    // that DO exist match the v3 design.
    JoinDynamicConfig cfg{};
    EXPECT_TRUE(cfg.pci.empty());
    EXPECT_EQ(cfg.primary_config.max_procs, 1);
    EXPECT_EQ(cfg.primary_config.queues_per_proc, 0);
    EXPECT_EQ(cfg.self_lcore_mask, 0);
    EXPECT_TRUE(cfg.lcores.empty());
}

// ──────────────────────────────────────────────────────────────────────
// Function pointer / signature tests (compile-time)
// ──────────────────────────────────────────────────────────────────────

TEST(PlatformV3Surface, EntryPointsHaveDocumentedSignatures) {
    using CreateFn = std::expected<Platform, std::string> (*)(PlatformConfig);
    using AttachFn = std::expected<Platform, std::string> (*)(PlatformAttachConfig);
    using CreateWithEalFn = std::expected<Platform, std::string> (*)(
        PlatformConfig, EalConfig,
        std::span<LcorePin const>, eph::utils::CpuPinPolicy);
    using AttachWithEalFn = std::expected<Platform, std::string> (*)(
        PlatformAttachConfig, EalConfig,
        std::span<LcorePin const>, eph::utils::CpuPinPolicy);
    using JoinDynV3Fn = std::expected<Platform, std::string> (*)(
        JoinDynamicConfig);

    [[maybe_unused]] CreateFn        f1 = &Platform::create;
    [[maybe_unused]] AttachFn        f2 = &Platform::attach;
    [[maybe_unused]] CreateWithEalFn f3 = static_cast<CreateWithEalFn>(
        &Platform::create_with_eal);
    [[maybe_unused]] AttachWithEalFn f4 = &Platform::attach_with_eal;
    [[maybe_unused]] JoinDynV3Fn     f5 = static_cast<JoinDynV3Fn>(
        &Platform::join_dynamic);
    SUCCEED();
}

// ──────────────────────────────────────────────────────────────────────
// v3 PlatformConfig structural validator
// ──────────────────────────────────────────────────────────────────────
//
// `validate_config(PlatformConfig)` is the v3 free function consumed by
// `Platform::create` (cold path) and `MultiPortPlatform::create`'s
// pre-pass. The rejection set tests below pin down behavior the
// per-config validator owns; per-port validation rules already shared
// with v2 (per_lcore_pools, RSS rx/tx queue rule) are exercised under
// `test_dpdk_platform_mempool.cpp::PlatformMempoolConfig`.

TEST(PlatformConfigValidator, SingleProcessHappyPathAccepts) {
    PlatformConfig v3{};
    v3.port_id          = 1;
    v3.nb_rx_queues     = 4;
    v3.nb_tx_queues     = 4;
    v3.nb_rx_desc       = 1024;
    v3.nb_tx_desc       = 2048;
    v3.mbuf_pool_size   = 8191;
    v3.mbuf_cache_size  = 512;
    v3.enable_promiscuous = true;
    v3.link_timeout_ms  = 5000;
    v3.per_lcore_pools  = 2;
    // max_procs stays at default 1 → single-process; file_prefix may stay empty.
    EXPECT_TRUE(validate_config(v3).empty()) << validate_config(v3);
    EXPECT_TRUE(config_ok(v3));
}

TEST(PlatformConfigValidator, MpPrimaryRejectsMissingFilePrefix) {
    // max_procs > 1 mandates file_prefix (used as registry memzone name).
    PlatformConfig v3{};
    v3.nb_rx_queues = 4;
    v3.nb_tx_queues = 4;
    v3.max_procs    = 2;
    // file_prefix intentionally left empty
    auto err = validate_config(v3);
    ASSERT_FALSE(err.empty());
    EXPECT_NE(err.find("file_prefix"), std::string_view::npos)
        << "actual error: " << err;
}

TEST(PlatformConfigValidator, MaxProcsExceedingNbRxQueuesRejected) {
    // max_procs > nb_rx_queues → queues_per_proc auto-derives to 0,
    // which the validator rejects up front.
    PlatformConfig v3{};
    v3.nb_rx_queues = 2;
    v3.nb_tx_queues = 2;
    v3.max_procs    = 4;
    v3.file_prefix  = "demo";
    auto err = validate_config(v3);
    EXPECT_FALSE(err.empty());
}

TEST(PlatformConfigValidator, ZeroMaxProcsRejected) {
    PlatformConfig v3{};
    v3.max_procs = 0;
    auto err = validate_config(v3);
    ASSERT_FALSE(err.empty());
    EXPECT_NE(err.find("max_procs"), std::string_view::npos)
        << "actual error: " << err;
}

// ──────────────────────────────────────────────────────────────────────
// Pre-EAL validation (PlatformAttachConfig::file_prefix must be non-empty)
// ──────────────────────────────────────────────────────────────────────

TEST(PlatformAttach, EmptyFilePrefixRejectedPreEal) {
    PlatformAttachConfig cfg{};
    // file_prefix left empty
    auto r = Platform::attach(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("file_prefix"), std::string::npos)
        << "actual error: " << r.error();
    EXPECT_NE(r.error().find("non-empty"), std::string::npos)
        << "actual error: " << r.error();
}

// ──────────────────────────────────────────────────────────────────────
// Pre-EAL validation in Platform::join_dynamic
//
// Pin down the error-context contract added in commit 620f429e — the
// user-visible error std::string for each pre-EAL fail path must
// include the offending value, so a programmatic caller can diagnose
// without log-scraping. These tests cover the three fail paths
// reachable WITHOUT eal_init (which the function only invokes after
// argv assembly).
// ──────────────────────────────────────────────────────────────────────

TEST(JoinDynamicPreEal, MalformedPciIncludesValueInError) {
    // file_prefix is always auto-derived from pci in v3 — a malformed
    // BDF makes `sanitize_bdf_for_file_prefix` fail, and the wrapped
    // error must name the actual cfg.pci value the caller supplied.
    JoinDynamicConfig cfg{};
    cfg.pci = "not_a_bdf";  // valid string_view but not a BDF
    cfg.primary_config.nb_rx_queues = 4;
    cfg.primary_config.queues_per_proc = 1;

    auto r = Platform::join_dynamic(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("pci='not_a_bdf'"), std::string::npos)
        << "actual error: " << r.error();
    EXPECT_NE(r.error().find("file_prefix"), std::string::npos)
        << "actual error: " << r.error();
}

TEST(JoinDynamicPreEal, MaxProcsOutOfRangeIncludesAllInputs) {
    // primary_config.max_procs > kMaxProcs (=64) → the OOR check fires.
    // Error must report the offending max_procs, the kMaxProcs cap, and
    // the inputs the auto-derive would have used (caller cfg.max_procs
    // / nb_rx_queues / queues_per_proc).
    JoinDynamicConfig cfg{};
    cfg.pci = "0000:28:00.0";
    cfg.primary_config.nb_rx_queues = 4;
    cfg.primary_config.queues_per_proc = 1;
    cfg.primary_config.max_procs = 200;  // > kMaxProcs

    auto r = Platform::join_dynamic(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("max_procs=200"), std::string::npos)
        << "actual error: " << r.error();
    EXPECT_NE(r.error().find("kMaxProcs"), std::string::npos)
        << "actual error: " << r.error();
    EXPECT_NE(r.error().find("nb_rx_queues=4"), std::string::npos)
        << "actual error: " << r.error();
    EXPECT_NE(r.error().find("queues_per_proc=1"), std::string::npos)
        << "actual error: " << r.error();
}

TEST(JoinDynamicPreEal, PinsAndLcoresMutexIncludesSizes) {
    // cfg.pins and cfg.lcores are mutually exclusive — error must
    // report both sizes so the caller knows which side has stuff.
    JoinDynamicConfig cfg{};
    cfg.pci = "0000:28:00.0";
    cfg.primary_config.nb_rx_queues = 2;
    cfg.primary_config.queues_per_proc = 1;
    cfg.primary_config.max_procs = 2;
    cfg.primary_config.file_prefix = "demo";  // required by validate_config(PlatformConfig)
    static const std::vector<LcorePin> kPins(
        3, LcorePin{.lcore_id=0, .cpu_id=0, .role=""});
    cfg.pins   = std::span<const LcorePin>{kPins};
    cfg.lcores = {"0", "1"};

    auto r = Platform::join_dynamic(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("cfg.pins"), std::string::npos)
        << "actual error: " << r.error();
    EXPECT_NE(r.error().find("cfg.lcores"), std::string::npos)
        << "actual error: " << r.error();
    EXPECT_NE(r.error().find("size=3"), std::string::npos)
        << "actual error: " << r.error();
    EXPECT_NE(r.error().find("size=2"), std::string::npos)
        << "actual error: " << r.error();
}

// ──────────────────────────────────────────────────────────────────────
// Pre-EAL validation in Platform::attach_with_eal
//
// Fail-fast contract: attach_with_eal previously deferred the
// file_prefix check to Platform::attach() AFTER eal_init had run,
// which wasted hugepage init for an obvious caller error. The check
// was lifted into attach_with_eal itself so the rejection happens
// pre-EAL. Plus the pins / eal_cfg.lcores mutex check (round 2,
// commit c003efca) needs a regression test for its size-context
// format the same way join_dynamic's does above.
// ──────────────────────────────────────────────────────────────────────

TEST(AttachWithEalPreEal, EmptyFilePrefixRejectedFailFast) {
    PlatformAttachConfig cfg{};
    // file_prefix left empty
    EalConfig eal_cfg{};

    auto r = Platform::attach_with_eal(cfg, std::move(eal_cfg),
                                       std::span<const LcorePin>{},
                                       eph::utils::CpuPinPolicy{});
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("file_prefix"), std::string::npos)
        << "actual error: " << r.error();
    EXPECT_NE(r.error().find("pre-EAL"), std::string::npos)
        << "actual error: " << r.error();
}

TEST(AttachWithEalPreEal, PinsAndLcoresMutexIncludesSizes) {
    PlatformAttachConfig cfg{};
    cfg.file_prefix = "test_prefix";  // non-empty so we get past file_prefix check

    EalConfig eal_cfg{};
    eal_cfg.lcores = {"0", "1", "2"};

    std::vector<LcorePin> pins{
        LcorePin{.lcore_id=0, .cpu_id=0, .role=""},
        LcorePin{.lcore_id=1, .cpu_id=1, .role=""},
    };

    auto r = Platform::attach_with_eal(cfg, std::move(eal_cfg),
                                       std::span<const LcorePin>{pins},
                                       eph::utils::CpuPinPolicy{});
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("attach_with_eal"), std::string::npos)
        << "actual error: " << r.error();
    EXPECT_NE(r.error().find("size=2"), std::string::npos)
        << "actual error: " << r.error();
    EXPECT_NE(r.error().find("size=3"), std::string::npos)
        << "actual error: " << r.error();
    EXPECT_NE(r.error().find("mutually exclusive"), std::string::npos)
        << "actual error: " << r.error();
}
