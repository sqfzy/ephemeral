/// @file test_platform_v3_surface.cpp
/// Stage-1 surface tests for the v3 Platform API.
///
/// **Scope**: type / signature / translation logic only. Runtime
/// behavior of `Platform::create / *_with_eal / create_or_join` is
/// verified by:
///   - integration tests under `tests/integration/dpdk_mp_dynamic_*`
///   - examples (`examples/dpdk_mp_demo.cpp`)
/// Booting EAL inside a unit test would conflict with concurrent test
/// runs and require vfio/hugepages, neither of which is present in the
/// unit-test environment.
///
/// What this file pins down:
///   - v3 types (`PlatformConfig`, `CreateOrJoinConfig`) compile and
///     have expected fields.
///   - v3 entry-point function pointers have the documented signatures.
///   - The v3 `validate_config(PlatformConfig)` rejection set
///     (max_procs / file_prefix / queues-per-proc invariants).
///   - `Platform::create` / `launch` reject `max_procs > 1`
///     (cooperative-MP path was removed; use `create_or_join` for MP).

#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "eph/dpdk/lcore_pin.hpp"
#include "eph/dpdk/platform.hpp"
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

TEST(CreateOrJoinConfig, NoTopLevelConsensusFields) {
    // v3 zero-consensus: queues_per_proc / max_procs are NOT top-level
    // (compile-time enforcement: the names don't exist on the struct).
    // We can't test absence directly in C++; instead verify the fields
    // that DO exist match the v3 design.
    CreateOrJoinConfig cfg{};
    EXPECT_TRUE(cfg.pci.empty());
    EXPECT_EQ(cfg.nic.max_procs, 1);
    EXPECT_EQ(cfg.nic.queues_per_proc, 0);
    EXPECT_EQ(cfg.self_lcore_mask, 0);
    EXPECT_TRUE(cfg.lcores.empty());
}

// ──────────────────────────────────────────────────────────────────────
// Function pointer / signature tests (compile-time)
// ──────────────────────────────────────────────────────────────────────

TEST(PlatformV3Surface, EntryPointsHaveDocumentedSignatures) {
    using CreateFn = std::expected<Platform, std::string> (*)(PlatformConfig);
    using CreateWithEalFn = std::expected<Platform, std::string> (*)(
        PlatformConfig, EalConfig,
        std::span<LcorePin const>, eph::utils::CpuPinPolicy);
    using CreateOrJoinFn = std::expected<Platform, std::string> (*)(
        CreateOrJoinConfig);

    [[maybe_unused]] CreateFn        f1 = &Platform::create;
    [[maybe_unused]] CreateWithEalFn f2 = static_cast<CreateWithEalFn>(
        &Platform::launch);
    [[maybe_unused]] CreateOrJoinFn     f3 = static_cast<CreateOrJoinFn>(
        &Platform::create_or_join);
    SUCCEED();
}

// `Platform::create` / `launch` are single-process only after
// the cooperative-MP removal. `cfg.max_procs > 1` must be rejected with
// a clear message pointing the caller at `Platform::create_or_join`.

TEST(PlatformCreate, RejectsMaxProcsGreaterThanOne) {
    PlatformConfig cfg{};
    cfg.nb_rx_queues = 4;
    cfg.nb_tx_queues = 4;
    cfg.max_procs    = 2;
    cfg.file_prefix  = "demo";  // satisfies validate_config

    auto r = Platform::create(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("max_procs"), std::string::npos)
        << "actual error: " << r.error();
    EXPECT_NE(r.error().find("create_or_join"), std::string::npos)
        << "actual error: " << r.error();
}

TEST(PlatformCreateWithEal, RejectsMaxProcsGreaterThanOne) {
    PlatformConfig cfg{};
    cfg.nb_rx_queues = 4;
    cfg.nb_tx_queues = 4;
    cfg.max_procs    = 2;
    cfg.file_prefix  = "demo";
    EalConfig eal_cfg{};

    auto r = Platform::launch(cfg, std::move(eal_cfg),
                                       std::span<const LcorePin>{},
                                       eph::utils::CpuPinPolicy{});
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("max_procs"), std::string::npos)
        << "actual error: " << r.error();
    EXPECT_NE(r.error().find("create_or_join"), std::string::npos)
        << "actual error: " << r.error();
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
// Pre-EAL validation in Platform::create_or_join
//
// Pin down the error-context contract added in commit 620f429e — the
// user-visible error std::string for each pre-EAL fail path must
// include the offending value, so a programmatic caller can diagnose
// without log-scraping. These tests cover the three fail paths
// reachable WITHOUT eal_init (which the function only invokes after
// argv assembly).
// ──────────────────────────────────────────────────────────────────────

TEST(CreateOrJoinPreEal, MalformedPciIncludesValueInError) {
    // file_prefix is always auto-derived from pci in v3 — a malformed
    // BDF makes `sanitize_bdf_for_file_prefix` fail, and the wrapped
    // error must name the actual cfg.pci value the caller supplied.
    CreateOrJoinConfig cfg{};
    cfg.pci = "not_a_bdf";  // valid string_view but not a BDF
    cfg.nic.nb_rx_queues = 4;
    cfg.nic.queues_per_proc = 1;

    auto r = Platform::create_or_join(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("pci='not_a_bdf'"), std::string::npos)
        << "actual error: " << r.error();
    EXPECT_NE(r.error().find("file_prefix"), std::string::npos)
        << "actual error: " << r.error();
}

TEST(CreateOrJoinPreEal, MaxProcsOutOfRangeIncludesAllInputs) {
    // nic.max_procs > kMaxProcs (=64) → the OOR check fires.
    // Error must report the offending max_procs, the kMaxProcs cap, and
    // the inputs the auto-derive would have used (caller cfg.max_procs
    // / nb_rx_queues / queues_per_proc).
    CreateOrJoinConfig cfg{};
    cfg.pci = "0000:28:00.0";
    cfg.nic.nb_rx_queues = 4;
    cfg.nic.queues_per_proc = 1;
    cfg.nic.max_procs = 200;  // > kMaxProcs

    auto r = Platform::create_or_join(cfg);
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

TEST(CreateOrJoinPreEal, PinsAndLcoresMutexIncludesSizes) {
    // cfg.pins and cfg.lcores are mutually exclusive — error must
    // report both sizes so the caller knows which side has stuff.
    CreateOrJoinConfig cfg{};
    cfg.pci = "0000:28:00.0";
    cfg.nic.nb_rx_queues = 2;
    cfg.nic.queues_per_proc = 1;
    cfg.nic.max_procs = 2;
    cfg.nic.file_prefix = "demo";  // required by validate_config(PlatformConfig)
    static const std::vector<LcorePin> kPins(
        3, LcorePin{.lcore_id=0, .cpu_id=0, .role=""});
    cfg.pins   = std::span<const LcorePin>{kPins};
    cfg.lcores = {"0", "1"};

    auto r = Platform::create_or_join(cfg);
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

