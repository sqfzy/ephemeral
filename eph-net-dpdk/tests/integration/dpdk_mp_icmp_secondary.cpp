/// @file dpdk_mp_icmp_secondary.cpp
/// Secondary-role binary for cross-process ICMP forwarding e2e.
///
/// Flow:
///   1. EAL secondary attach + Platform::create_secondary with mp_topology
///      (upper-half queue/port).
///   2. Look up the test tuple in the cross-proc IcmpDirectory — should
///      hit (primary registered it before secondary started).
///   3. Build an IcmpDispatchMsg simulating "router-fired Frag Needed
///      landed on this secondary's RX queue" and IPC-forward it to
///      primary via mp_ipc_send_oneway.
///   4. Sleep briefly for the IPC msg to be delivered, then exit
///      cleanly (secondary tears down BEFORE primary, per DPDK MP
///      contract).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/detail/icmp_directory.hpp"
#include "eph/dpdk/detail/mp_ipc.hpp"
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

TEST(DpdkMpIcmpSecondary, ForwardsIcmpToPrimary) {
    const char* file_prefix = env_or_null("EPH_MP_FILE_PREFIX");
    if (!file_prefix) {
        GTEST_SKIP() << "missing EPH_MP_FILE_PREFIX — run via "
                        "dpdk_mp_icmp_e2e.sh";
    }

    const std::string port_id_s      = env_or("EPH_MP_PORT_ID",      "0");
    const std::string nb_rx_queues_s = env_or("EPH_MP_NB_RX_QUEUES", "4");
    const std::string lcores         = env_or("EPH_MP_LCORES_SEC",   "1");
    const std::string allowed_dev    = env_or("EPH_MP_ALLOWED_DEV",  "");

    const uint16_t port_id      = static_cast<uint16_t>(std::stoul(port_id_s));
    const uint16_t nb_rx_queues = static_cast<uint16_t>(std::stoul(nb_rx_queues_s));
    ASSERT_GE(nb_rx_queues, 2u);

    eph::dpdk::EalConfig eal_cfg{};
    eal_cfg.program_name  = "dpdk_mp_icmp_secondary";
    eal_cfg.proc_type     = eph::dpdk::ProcType::Secondary;
    eal_cfg.proc_type_set = true;
    eal_cfg.file_prefix   = file_prefix;
    eal_cfg.lcores        = {lcores};
    if (!allowed_dev.empty()) eal_cfg.allowed_devs = {allowed_dev};

    auto argv_owned = eph::dpdk::build_eal_argv(eal_cfg);
    std::vector<char*> argv;
    for (auto& s : argv_owned) argv.push_back(s.data());
    auto eal_r = eph::dpdk::eal_init(static_cast<int>(argv.size()), argv.data());
    ASSERT_TRUE(eal_r) << "eal_init failed: " << eal_r.error();

    {
        eph::dpdk::PlatformConfig pcfg{};
        pcfg.port_id      = port_id;
        pcfg.nb_rx_queues = nb_rx_queues;
        pcfg.nb_tx_queues = nb_rx_queues;
        pcfg.proc_type    = eph::dpdk::ProcType::Secondary;
        pcfg.file_prefix  = file_prefix;
        pcfg.mp_topology  = eph::dpdk::MpTopology::uniform(1, 2, nb_rx_queues);

        auto plat_r = eph::dpdk::Platform::create_secondary(std::move(pcfg));
        ASSERT_TRUE(plat_r) << "create_secondary failed: " << plat_r.error();
        auto platform = std::move(*plat_r);

        ASSERT_TRUE(platform.has_mp_topology());

        // Look up the test tuple in the cross-proc directory. Primary
        // should have registered it before signaling ready.
        auto* dir = ::eph::dpdk::detail::g_active_icmp_directory.load(
            std::memory_order_acquire);
        ASSERT_NE(dir, nullptr) << "g_active_icmp_directory must be set";

        auto found = dir->lookup(test_tuple(), kProtoTcp);
        ASSERT_TRUE(found.has_value())
            << "lookup of primary's test tuple failed — directory empty?";
        EXPECT_EQ(found->owner_proc, 0)
            << "expected owner=primary (proc 0)";

        // Build a fake IcmpDispatchMsg simulating "router fired Frag
        // Needed at secondary's RX queue, parse_icmp extracted these
        // fields, local IcmpRegistry::dispatch_returns_hit returned
        // false, and the closure now forwards via IPC."
        ::eph::dpdk::net::ParsedIcmp fake_parsed{};
        fake_parsed.embedded_valid    = true;
        fake_parsed.embedded_proto    = kProtoTcp;
        fake_parsed.embedded_src_ip   = test_tuple().src_ip;
        fake_parsed.embedded_dst_ip   = test_tuple().dst_ip;
        fake_parsed.embedded_src_port = test_tuple().src_port;
        fake_parsed.embedded_dst_port = test_tuple().dst_port;
        fake_parsed.next_hop_mtu      = kInjectedMtu;

        auto msg = ::eph::dpdk::detail::make_icmp_dispatch_msg(
            fake_parsed, found->slot_idx, found->generation);

        auto ipc_r = ::eph::dpdk::detail::mp_ipc_send_oneway(
            ::eph::dpdk::detail::kIcmpDispatchActionName, msg);
        EXPECT_TRUE(ipc_r) << "mp_ipc_send_oneway failed: "
                            << (ipc_r ? "" : ipc_r.error().detail);

        if (ipc_r) {
            dir->header()->ipc_msgs_sent.fetch_add(
                1, std::memory_order_relaxed);
        }

        // Give the IPC msg time to traverse the EAL socket to primary.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    (void)eph::dpdk::eal_cleanup();
}
