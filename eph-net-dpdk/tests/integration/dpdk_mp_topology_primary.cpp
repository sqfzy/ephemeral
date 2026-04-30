/// @file dpdk_mp_topology_primary.cpp
/// Primary-role integration binary for the **MpTopology** path of the
/// single-NIC multi-process e2e test. Companion to `dpdk_mp_primary.cpp`
/// (which exercises the legacy hand-partitioned `rx_queue_range`); this
/// binary drives the recommended path: caller supplies only
/// `MpTopology::uniform(self_index=0, total_procs=2, nb_rx_queues)` and
/// the library auto-derives queue + src_port segments via the shared
/// hugepage registry.
///
/// Started by `tests/integration/dpdk_mp_topology_e2e.sh`. Same env-var
/// shape as `dpdk_mp_primary.cpp`; skips cleanly when any required env
/// var is missing so CI on a host without vfio-pci NIC reports SKIP.

#include <chrono>
#include <cstdlib>
#include <fstream>
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

TEST(DpdkMpTopologyPrimary, BringUpHoldAndCleanup) {
    const char* file_prefix = env_or_null("EPH_MP_FILE_PREFIX");
    const char* ready_file  = env_or_null("EPH_MP_READY_FILE");
    if (!file_prefix || !ready_file) {
        GTEST_SKIP()
            << "missing EPH_MP_FILE_PREFIX / EPH_MP_READY_FILE — run "
               "via tests/integration/dpdk_mp_topology_e2e.sh";
    }

    const std::string port_id_s      = env_or("EPH_MP_PORT_ID",      "0");
    const std::string nb_rx_queues_s = env_or("EPH_MP_NB_RX_QUEUES", "4");
    const std::string lcores         = env_or("EPH_MP_LCORES",       "0");
    const std::string allowed_dev    = env_or("EPH_MP_ALLOWED_DEV",  "");
    const std::string hold_seconds_s = env_or("EPH_MP_HOLD_SECONDS", "30");

    const uint16_t port_id      = static_cast<uint16_t>(std::stoul(port_id_s));
    const uint16_t nb_rx_queues = static_cast<uint16_t>(std::stoul(nb_rx_queues_s));
    const int      hold_seconds = std::stoi(hold_seconds_s);
    ASSERT_GE(nb_rx_queues, 2u)
        << "MpTopology test requires nb_rx_queues >= 2 (got "
        << nb_rx_queues << ")";

    eph::dpdk::EalConfig eal_cfg{};
    eal_cfg.program_name  = "dpdk_mp_topology_primary";
    eal_cfg.proc_type     = eph::dpdk::ProcType::Primary;
    eal_cfg.proc_type_set = true;
    eal_cfg.file_prefix   = file_prefix;
    eal_cfg.lcores        = {lcores};
    if (!allowed_dev.empty()) eal_cfg.allowed_devs = {allowed_dev};

    auto argv_owned = eph::dpdk::build_eal_argv(eal_cfg);
    std::vector<char*> argv;
    for (auto& s : argv_owned) argv.push_back(s.data());
    auto eal_r = eph::dpdk::eal_init(static_cast<int>(argv.size()), argv.data());
    ASSERT_TRUE(eal_r) << "eal_init failed: " << eal_r.error();

    eph::dpdk::PlatformConfig pcfg{};
    pcfg.port_id      = port_id;
    pcfg.nb_rx_queues = nb_rx_queues;
    pcfg.nb_tx_queues = nb_rx_queues;
    pcfg.proc_type    = eph::dpdk::ProcType::Primary;
    pcfg.file_prefix  = file_prefix;
    // The whole point of this test: NO manual rx_queue_range. Just
    // declare the topology and let the library auto-derive.
    pcfg.mp_topology  = eph::dpdk::MpTopology::uniform(
        /*self_index=*/0, /*total_procs=*/2, nb_rx_queues);

    // Nested scope is load-bearing — same teardown-order rule as
    // dpdk_mp_primary.cpp. ~Platform must fire before eal_cleanup
    // because Impl::cleanup() calls rte_eth_dev_stop / close /
    // rte_mempool_free, all illegal after EAL is torn down.
    {
        auto plat_r = eph::dpdk::Platform::create_primary(std::move(pcfg));
        ASSERT_TRUE(plat_r) << "create_primary failed: " << plat_r.error();
        auto platform = std::move(*plat_r);

        // Library should auto-derive lower-half queues and lower-half ports.
        EXPECT_TRUE(platform.has_mp_topology());
        const auto qr = platform.effective_rx_queue_range();
        EXPECT_EQ(qr.first,  0);
        EXPECT_EQ(qr.second, nb_rx_queues / 2);

        auto pr = platform.self_port_range();
        ASSERT_TRUE(pr.has_value()) << "self_port_range expected non-empty";
        EXPECT_EQ(pr->first,  32768u);
        EXPECT_EQ(pr->second, 32768u + 16384u);

        {
            std::ofstream f(ready_file);
            f << "topology primary ready " << std::time(nullptr) << "\n";
        }

        std::this_thread::sleep_for(std::chrono::seconds(hold_seconds));
    }  // ← ~Platform fires here, while EAL is still alive

    (void)eph::dpdk::eal_cleanup();
}
