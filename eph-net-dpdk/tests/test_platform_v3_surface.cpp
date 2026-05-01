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
///   - v3 types (`PlatformConfigV3`, `PlatformAttachConfig`,
///     `JoinDynamicConfigV3`) compile and have expected fields.
///   - v3 entry-point function pointers have the documented signatures.
///   - The internal v3→v2 translator (`detail::v3_to_v2_primary`)
///     correctly maps fields.
///
/// Stage 3 will add behavior-level tests when v3 is no longer a wrapper
/// over v2 (and the A1 assumption — secondary's `rte_eth_dev_info_get`
/// returning primary-configured nb_rx_queues — gets a dedicated check).

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

TEST(PlatformConfigV3, DefaultsAreSingleProcess) {
    PlatformConfigV3 cfg{};
    EXPECT_EQ(cfg.max_procs, 1);
    EXPECT_EQ(cfg.queues_per_proc, 0);    // 0 = auto
    EXPECT_EQ(cfg.port_id, 0);
    EXPECT_EQ(cfg.nb_rx_queues, 1);
    EXPECT_EQ(cfg.nb_tx_queues, 1);
    EXPECT_TRUE(cfg.file_prefix.empty());
}

TEST(PlatformConfigV3, NicPhysicalFieldsMirrorV2Defaults) {
    // Sanity: v3's NIC physical defaults match v2's so silent semantic
    // shift is impossible in stage 1.
    PlatformConfigV3 v3{};
    PlatformConfig   v2{};
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

TEST(JoinDynamicConfigV3, NoTopLevelConsensusFields) {
    // v3 zero-consensus: queues_per_proc / max_procs are NOT top-level
    // (compile-time enforcement: the names don't exist on the struct).
    // We can't test absence directly in C++; instead verify the fields
    // that DO exist match the v3 design.
    JoinDynamicConfigV3 cfg{};
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
    using CreateFn = std::expected<Platform, std::string> (*)(PlatformConfigV3);
    using AttachFn = std::expected<Platform, std::string> (*)(PlatformAttachConfig);
    using CreateWithEalFn = std::expected<Platform, std::string> (*)(
        PlatformConfigV3, EalConfig,
        std::span<LcorePin const>, eph::utils::CpuPinPolicy);
    using AttachWithEalFn = std::expected<Platform, std::string> (*)(
        PlatformAttachConfig, EalConfig,
        std::span<LcorePin const>, eph::utils::CpuPinPolicy);
    using JoinDynV3Fn = std::expected<Platform, std::string> (*)(
        JoinDynamicConfigV3);

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
// Translation logic (detail::v3_to_v2_primary)
// ──────────────────────────────────────────────────────────────────────

TEST(V3ToV2Primary, SingleProcessIdentityMapping) {
    PlatformConfigV3 v3{};
    v3.port_id          = 1;
    v3.file_prefix      = "demo";
    v3.nb_rx_queues     = 4;
    v3.nb_tx_queues     = 4;
    v3.nb_rx_desc       = 1024;
    v3.nb_tx_desc       = 2048;
    v3.mbuf_pool_size   = 8191;
    v3.mbuf_cache_size  = 512;
    v3.enable_promiscuous = true;
    v3.link_timeout_ms  = 5000;
    v3.per_lcore_pools  = 2;
    // max_procs stays at default 1 → single-process

    auto v2 = detail::v3_to_v2_primary(v3);
    EXPECT_EQ(v2.port_id,          v3.port_id);
    EXPECT_EQ(v2.file_prefix,      v3.file_prefix);
    EXPECT_EQ(v2.nb_rx_queues,     v3.nb_rx_queues);
    EXPECT_EQ(v2.nb_tx_queues,     v3.nb_tx_queues);
    EXPECT_EQ(v2.nb_rx_desc,       v3.nb_rx_desc);
    EXPECT_EQ(v2.nb_tx_desc,       v3.nb_tx_desc);
    EXPECT_EQ(v2.mbuf_pool_size,   v3.mbuf_pool_size);
    EXPECT_EQ(v2.mbuf_cache_size,  v3.mbuf_cache_size);
    EXPECT_EQ(v2.link_timeout_ms,  v3.link_timeout_ms);
    EXPECT_EQ(v2.per_lcore_pools,  v3.per_lcore_pools);
    EXPECT_TRUE(v2.enable_promiscuous);
    EXPECT_EQ(v2.proc_type,        ProcType::Primary);

    // Single-process: no MpTopology synthesized.
    EXPECT_FALSE(v2.mp_topology.has_value());
}

TEST(V3ToV2Primary, MpPrimarySynthesizesTopology) {
    PlatformConfigV3 v3{};
    v3.nb_rx_queues = 4;
    v3.nb_tx_queues = 4;
    v3.max_procs    = 2;
    v3.file_prefix  = "mp_demo";

    auto v2 = detail::v3_to_v2_primary(v3);
    ASSERT_TRUE(v2.mp_topology.has_value());
    EXPECT_EQ(v2.mp_topology->self_index, 0);
    EXPECT_EQ(v2.mp_topology->total_procs, 2);
    EXPECT_EQ(v2.proc_type, ProcType::Primary);
}

TEST(V3ToV2Primary, SelfLcoreMaskPropagates) {
    PlatformConfigV3 v3{};
    v3.nb_rx_queues     = 2;
    v3.max_procs        = 2;
    v3.self_lcore_mask  = 0b1100ULL;  // lcores 2,3

    auto v2 = detail::v3_to_v2_primary(v3);
    ASSERT_TRUE(v2.mp_topology.has_value());
    EXPECT_EQ(v2.mp_topology->procs[0].lcore_mask, 0b1100ULL);
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
