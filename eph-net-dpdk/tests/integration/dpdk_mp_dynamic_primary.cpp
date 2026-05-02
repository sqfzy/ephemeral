/// @file dpdk_mp_dynamic_primary.cpp
/// Primary-role integration binary for the **autojoin** path
/// (`Platform::create_or_join`) — the recommended factory where the
/// caller passes only the PCI BDF and `nb_rx_queues`, and DPDK +
/// the eph registry decide who is primary, who is secondary, and
/// which queue / port slots each peer owns.
///
/// What this binary asserts:
///   1. `Platform::create_or_join` returns a valid Platform with the
///      EAL having auto-resolved this process to PRIMARY (because
///      this binary is started first by the orchestrator shell).
///   2. `is_secondary()` is false.
///   3. The auto-derived `rx_queue_range` is `[0, queues_per_proc)`
///      since the primary always claims slot 0.
///   4. `has_mp_topology()` is true (autojoin always populates one).
///
/// Started by `tests/integration/dpdk_mp_dynamic_e2e.sh`. Uses the
/// same env-var shape as the declarative-path MP integration
/// binaries; skips cleanly when any required env var is missing so
/// CI on a host without vfio-pci NIC reports SKIP.

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "eph/dpdk/eal.hpp"
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

TEST(DpdkMpDynamicPrimary, AutojoinResolvesAsPrimary) {
    const char* pci        = env_or_null("EPH_MP_ALLOWED_DEV");
    const char* ready_file = env_or_null("EPH_MP_READY_FILE");
    if (!pci || !ready_file) {
        GTEST_SKIP() << "missing EPH_MP_ALLOWED_DEV / EPH_MP_READY_FILE — "
                        "run via tests/integration/dpdk_mp_dynamic_e2e.sh";
    }

    const std::string nb_rx_queues_s = env_or("EPH_MP_NB_RX_QUEUES", "4");
    const std::string lcores         = env_or("EPH_MP_LCORES",       "0");
    const std::string hold_seconds_s = env_or("EPH_MP_HOLD_SECONDS", "30");

    const uint16_t nb_rx_queues =
        static_cast<uint16_t>(std::stoul(nb_rx_queues_s));
    const int hold_seconds = std::stoi(hold_seconds_s);
    ASSERT_GE(nb_rx_queues, 2u)
        << "autojoin test requires nb_rx_queues >= 2";

    // V3 autojoin: primary peer fills `nic` (only consulted
    // when this peer resolves to Primary). max_procs default 0 means
    // library auto-derives from nb_rx_queues / queues_per_proc.
    eph::dpdk::CreateOrJoinConfig cfg{};
    cfg.pci                            = pci;
    cfg.nic.nb_rx_queues               = nb_rx_queues;
    cfg.nic.nb_tx_queues               = nb_rx_queues;
    cfg.lcores                         = {lcores};

    auto plat_r = eph::dpdk::Platform::create_or_join(std::move(cfg));
    ASSERT_TRUE(plat_r) << "create_or_join failed: " << plat_r.error();
    auto platform = std::move(*plat_r);

    EXPECT_FALSE(platform.is_secondary())
        << "first peer should resolve as primary";
    EXPECT_TRUE(platform.has_mp_topology());

    // queues_per_proc=1 default + nb_rx_queues queues → max_procs =
    // nb_rx_queues. Primary owns slot 0 → queue range [0, 1).
    const auto qr = platform.effective_rx_queue_range();
    EXPECT_EQ(qr.first, 0);
    EXPECT_EQ(qr.second, 1);

    auto pr = platform.self_port_range();
    ASSERT_TRUE(pr.has_value()) << "self_port_range expected non-empty";
    EXPECT_EQ(pr->first, 32768u);

    {
        std::ofstream f(ready_file);
        f << "dynamic primary ready " << std::time(nullptr) << "\n";
    }

    std::this_thread::sleep_for(std::chrono::seconds(hold_seconds));
    // ~Platform at function exit owns EAL: it releases DPDK resources
    // and runs eal_cleanup itself. No nested-scope idiom needed.
}
