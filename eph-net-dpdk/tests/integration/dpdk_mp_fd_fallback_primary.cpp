/// @file dpdk_mp_fd_fallback_primary.cpp
/// Primary-role binary for FlowDir secondary-fallback e2e (reshape
/// mp-icmp-flowdir milestone B stage 7).
///
/// Flow:
///   1. EAL primary + Platform::create_primary with mp_topology.
///   2. Hand off to ready file. Wait for secondary to invoke
///      try_install_flow_rule_via_ipc (which lands on our
///      `on_fd_install_thunk`) AND the matching destroy.
///   3. After hold, assert remote_flow_rules.size_for_test() == 0
///      (secondary's RAII fired eph_fd_destroy, our handler ran,
///      map is empty).

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/mp_topology.hpp"
#include "eph/dpdk/platform.hpp"
#include "eph/net/dpdk/flow_steering.hpp"

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

TEST(DpdkMpFdFallbackPrimary, ReceivesIpcInstallAndDestroy) {
    const char* file_prefix = env_or_null("EPH_MP_FILE_PREFIX");
    const char* ready_file  = env_or_null("EPH_MP_READY_FILE");
    if (!file_prefix || !ready_file) {
        GTEST_SKIP()
            << "missing EPH_MP_FILE_PREFIX / EPH_MP_READY_FILE — run "
               "via dpdk_mp_fd_fallback_e2e.sh";
    }

    const std::string port_id_s      = env_or("EPH_MP_PORT_ID",      "0");
    const std::string nb_rx_queues_s = env_or("EPH_MP_NB_RX_QUEUES", "4");
    const std::string lcores         = env_or("EPH_MP_LCORES",       "0");
    const std::string allowed_dev    = env_or("EPH_MP_ALLOWED_DEV",  "");
    const std::string hold_seconds_s = env_or("EPH_MP_HOLD_SECONDS", "10");

    const uint16_t port_id      = static_cast<uint16_t>(std::stoul(port_id_s));
    const uint16_t nb_rx_queues = static_cast<uint16_t>(std::stoul(nb_rx_queues_s));
    const int      hold_seconds = std::stoi(hold_seconds_s);
    ASSERT_GE(nb_rx_queues, 2u);

    eph::dpdk::EalConfig eal_cfg{};
    eal_cfg.program_name = "dpdk_mp_fd_fallback_primary";
    eal_cfg.lcores       = {lcores};
    if (!allowed_dev.empty()) eal_cfg.allowed_devs = {allowed_dev};

    eph::dpdk::PlatformConfigV3 pcfg{};
    pcfg.port_id      = port_id;
    pcfg.nb_rx_queues = nb_rx_queues;
    pcfg.nb_tx_queues = nb_rx_queues;
    pcfg.file_prefix  = file_prefix;
    pcfg.max_procs    = 2;

    auto plat_r = eph::dpdk::Platform::create_with_eal(
        std::move(pcfg), std::move(eal_cfg),
        /*pins=*/{}, eph::utils::CpuPinPolicy{});
    ASSERT_TRUE(plat_r) << "create_with_eal failed: " << plat_r.error();
    auto platform = std::move(*plat_r);
    ASSERT_TRUE(platform.has_mp_topology());

    auto* rules =
        ::eph::net::dpdk::detail::g_active_remote_flow_rules.load(
            std::memory_order_acquire);
    ASSERT_NE(rules, nullptr);
    EXPECT_EQ(rules->size_for_test(), 0u);

    {
        std::ofstream f(ready_file);
        f << "fd_fallback primary ready " << std::time(nullptr) << "\n";
    }

    std::this_thread::sleep_for(std::chrono::seconds(hold_seconds));

    EXPECT_EQ(rules->size_for_test(), 0u)
        << "remote_flow_rules has stale entries after secondary exit";
    // ~Platform handles teardown.
}
