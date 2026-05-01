/// @file dpdk_mp_icmp_primary.cpp
/// Primary-role binary for the cross-process ICMP forwarding e2e.
///
/// Flow:
///   1. EAL primary init + Platform::create_primary with mp_topology
///      (lower-half queue/port).
///   2. Register a fake ICMP target (tuple, proto=TCP) directly in
///      Platform's IcmpRegistry/IcmpDirectory — no actual stream
///      attach needed for this test, the callback flips a local atomic.
///   3. Touch ready-file, sleep for HOLD_SECONDS (during which the
///      secondary IPC-forwards a fake Frag Needed message).
///   4. After hold, assert: callback fired ≥ 1 time AND directory
///      ipc_msgs_received ≥ 1.
///
/// Skip semantics same as dpdk_mp_primary.cpp.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/detail/icmp_directory.hpp"
#include "eph/dpdk/mp_topology.hpp"
#include "eph/dpdk/packet_core.hpp"
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

// Test-stream callback. Its `stream` arg points at this counter.
std::atomic<uint16_t> g_observed_mtu{0};
std::atomic<uint32_t> g_callback_count{0};
void icmp_test_cb(void* /*stream*/, uint16_t next_hop_mtu) noexcept {
    g_observed_mtu.store(next_hop_mtu, std::memory_order_relaxed);
    g_callback_count.fetch_add(1, std::memory_order_relaxed);
}

constexpr uint8_t  kProtoTcp = 6;
constexpr uint16_t kInjectedMtu = 1280;

eph::dpdk::net::ConnectionTuple test_tuple() {
    return eph::dpdk::net::ConnectionTuple{
        .src_ip   = 0x0A000001,
        .dst_ip   = 0x0A000002,
        .src_port = 50000,
        .dst_port = 51000,
    };
}

} // namespace

TEST(DpdkMpIcmpPrimary, ReceivesForwardedIcmp) {
    const char* file_prefix = env_or_null("EPH_MP_FILE_PREFIX");
    const char* ready_file  = env_or_null("EPH_MP_READY_FILE");
    if (!file_prefix || !ready_file) {
        GTEST_SKIP()
            << "missing EPH_MP_FILE_PREFIX / EPH_MP_READY_FILE — run "
               "via dpdk_mp_icmp_e2e.sh";
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
    eal_cfg.program_name = "dpdk_mp_icmp_primary";
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

    auto handle = platform.register_icmp_target(
        test_tuple(), kProtoTcp,
        /*stream=*/&g_observed_mtu,   // any non-null pointer
        &icmp_test_cb);
    ASSERT_TRUE(handle) << handle.error();
    EXPECT_TRUE(handle->engaged());

    {
        std::ofstream f(ready_file);
        f << "icmp primary ready " << std::time(nullptr) << "\n";
    }

    std::this_thread::sleep_for(std::chrono::seconds(hold_seconds));

    EXPECT_GE(g_callback_count.load(std::memory_order_acquire), 1u)
        << "callback never fired — IPC didn't reach IcmpRegistry::dispatch";
    EXPECT_EQ(g_observed_mtu.load(std::memory_order_acquire),
              kInjectedMtu)
        << "callback fired but MTU value didn't match injected";

    auto* dir =
        ::eph::dpdk::detail::g_active_icmp_directory.load(
            std::memory_order_acquire);
    ASSERT_NE(dir, nullptr) << "g_active_icmp_directory must be set";
    EXPECT_GE(dir->header()->ipc_msgs_received.load(
                  std::memory_order_acquire),
              1u);
    // ~Platform handles teardown.
}
