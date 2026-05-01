/// @file dpdk_mp_topology_secondary.cpp
/// Secondary-role integration binary for the **MpTopology** path of the
/// single-NIC multi-process e2e test. Mirror of `dpdk_mp_topology_primary.
/// cpp`: caller supplies only `MpTopology::uniform(self_index=1,
/// total_procs=2, nb_rx_queues)` — the registry attach + cross-validate
/// happens inside `Platform::create_secondary`.
///
/// Started by `tests/integration/dpdk_mp_topology_e2e.sh` after the
/// primary signals ready. Skips cleanly when env vars are missing so the
/// build target is verifiable on any host.

#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/mp_topology.hpp"
#include "eph/dpdk/platform.hpp"

namespace {
const char* env_or_null(const char* k) {
    const char* v = std::getenv(k);
    return (v && *v) ? v : nullptr;
}
std::string env_or(const char* k, const char* fallback) {
    const char* v = env_or_null(k);
    return v ? v : fallback;
}
} // namespace

TEST(DpdkMpTopologySecondary, AttachAndVerifyDerivedRanges) {
    const char* file_prefix = env_or_null("EPH_MP_FILE_PREFIX");
    if (!file_prefix) {
        GTEST_SKIP()
            << "missing EPH_MP_FILE_PREFIX — run via "
               "tests/integration/dpdk_mp_topology_e2e.sh";
    }

    const std::string port_id_s      = env_or("EPH_MP_PORT_ID",       "0");
    const std::string nb_rx_queues_s = env_or("EPH_MP_NB_RX_QUEUES",  "4");
    const std::string lcores         = env_or("EPH_MP_LCORES_SEC",    "1");
    const std::string allowed_dev    = env_or("EPH_MP_ALLOWED_DEV",   "");

    const uint16_t port_id      = static_cast<uint16_t>(std::stoul(port_id_s));
    const uint16_t nb_rx_queues = static_cast<uint16_t>(std::stoul(nb_rx_queues_s));
    ASSERT_GE(nb_rx_queues, 2u);

    eph::dpdk::EalConfig eal_cfg{};
    eal_cfg.program_name = "dpdk_mp_topology_secondary";
    eal_cfg.lcores       = {lcores};
    if (!allowed_dev.empty()) eal_cfg.allowed_devs = {allowed_dev};

    // V3 zero-consensus secondary attach — no nb_rx_queues, no
    // mp_topology, no proc_type. Library reads slot table from
    // primary's registry and queries live NIC for queue counts.
    eph::dpdk::PlatformAttachConfig acfg{};
    acfg.file_prefix = file_prefix;
    acfg.port_id     = port_id;
    (void)nb_rx_queues;  // env value preserved for assertions below
    auto plat_r = eph::dpdk::Platform::attach_with_eal(
        std::move(acfg), std::move(eal_cfg),
        /*pins=*/{}, eph::utils::CpuPinPolicy{});
    ASSERT_TRUE(plat_r) << "create_with_eal failed: " << plat_r.error();
    auto platform = std::move(*plat_r);

    EXPECT_TRUE(platform.has_mp_topology());
    const auto qr = platform.effective_rx_queue_range();
    EXPECT_EQ(qr.first,  nb_rx_queues / 2);
    EXPECT_EQ(qr.second, nb_rx_queues);

    auto pr = platform.self_port_range();
    ASSERT_TRUE(pr.has_value());
    EXPECT_EQ(pr->first,  32768u + 16384u);
    EXPECT_EQ(pr->second, 65536u);
    // ~Platform handles teardown.
}
