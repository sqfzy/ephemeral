/// @file dpdk_mp_primary.cpp
/// Primary-role integration binary for single-NIC multi-process e2e test.
///
/// Started by `tests/integration/dpdk_mp_e2e.sh`. Expects the following
/// environment variables:
///   EPH_MP_FILE_PREFIX   DPDK --file-prefix (must match secondary's)
///   EPH_MP_PORT_ID       DPDK port ID to initialize
///   EPH_MP_LCORES        -l argument (e.g. "0,1")
///   EPH_MP_ALLOWED_DEV   -a argument (e.g. "0000:05:00.1")
///   EPH_MP_NB_RX_QUEUES  total RX queues to configure on the port
///   EPH_MP_READY_FILE    absolute path to a file this primary will touch
///                        once Platform is ready
///   EPH_MP_HOLD_SECONDS  how many seconds to stay alive after ready (lets
///                        the secondary attach, test, exit)
///
/// Skips cleanly (GTEST_SKIP) when any env var is missing — no NIC needed
/// for CI.

#include <chrono>
#include <cstdio>
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

TEST(DpdkMpPrimary, BringUpHoldAndCleanup) {
    const char* file_prefix  = env_or_null("EPH_MP_FILE_PREFIX");
    const char* ready_file   = env_or_null("EPH_MP_READY_FILE");

    if (!file_prefix || !ready_file) {
        GTEST_SKIP() << "missing EPH_MP_FILE_PREFIX / EPH_MP_READY_FILE "
                        "— run via tests/integration/dpdk_mp_e2e.sh";
    }

    const std::string port_id_s      = env_or("EPH_MP_PORT_ID",      "0");
    const std::string nb_rx_queues_s = env_or("EPH_MP_NB_RX_QUEUES", "4");
    const std::string lcores         = env_or("EPH_MP_LCORES",       "0");
    const std::string allowed_dev    = env_or("EPH_MP_ALLOWED_DEV",  "");
    const std::string hold_seconds_s = env_or("EPH_MP_HOLD_SECONDS", "30");

    const uint16_t port_id      = static_cast<uint16_t>(std::stoul(port_id_s));
    const uint16_t nb_rx_queues = static_cast<uint16_t>(std::stoul(nb_rx_queues_s));
    const int      hold_seconds = std::stoi(hold_seconds_s);

    // M4 guard: this test partitions queues 50/50 between primary and
    // secondary. With nb_rx_queues == 1, primary's rx_queue_range
    // collapses to {0, 0} which is the "full range" sentinel — primary
    // would then own queue 0 and secondary's {0, 1} would also touch
    // queue 0, racing on mbufs. Require ≥ 2 queues so the partition is
    // genuinely disjoint.
    ASSERT_GE(nb_rx_queues, 2u)
        << "MP test requires nb_rx_queues >= 2 (got " << nb_rx_queues
        << "); set EPH_MP_NB_RX_QUEUES to a value >= 2";

    // Build EAL argv via the helper (phase-2 deliverable).
    eph::dpdk::EalConfig eal_cfg{};
    eal_cfg.program_name  = "dpdk_mp_primary";
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
    // Primary owns the lower half of queues. Source-port partitioning is
    // the caller's responsibility (eph-net-dpdk does not auto-allocate
    // src_port); this test does not need to drive the wire so we leave
    // that to whoever calls create_and_attach in production.
    pcfg.rx_queue_range = {0, static_cast<uint16_t>(nb_rx_queues / 2)};

    // Nested scope is load-bearing: `Platform`'s destructor runs
    // `Impl::cleanup()` which calls `rte_eth_dev_stop` / `close` /
    // `rte_mempool_free`. Those are illegal after `rte_eal_cleanup`,
    // so the Platform MUST go out of scope before the explicit
    // `eal_cleanup()` below — without the inner block, the test
    // function's stack-unwind order is `eal_cleanup` first, then
    // ~Platform → SIGSEGV against a dead EAL.
    {
        auto plat_r = eph::dpdk::Platform::create_primary(std::move(pcfg));
        ASSERT_TRUE(plat_r) << "create_primary failed: " << plat_r.error();
        auto platform = std::move(*plat_r);

        EXPECT_TRUE(platform.is_running());
        EXPECT_EQ(platform.port_id(), port_id);
        EXPECT_EQ(platform.nb_rx_queues(), nb_rx_queues);

        const auto qr = platform.effective_rx_queue_range();
        EXPECT_EQ(qr.first,  0);
        EXPECT_EQ(qr.second, nb_rx_queues / 2);

        // Signal readiness to the orchestrator.
        {
            std::ofstream f(ready_file);
            f << "primary ready " << std::time(nullptr) << "\n";
        }

        // Stay alive long enough for the secondary to attach + test + exit.
        std::this_thread::sleep_for(std::chrono::seconds(hold_seconds));
    }  // ← ~Platform fires here, while EAL is still alive

    (void)eph::dpdk::eal_cleanup();
}
