/// @file dpdk_mp_secondary.cpp
/// Secondary-role integration binary for single-NIC multi-process e2e test.
///
/// Started by `tests/integration/dpdk_mp_e2e.sh` *after* the primary has
/// touched its ready file. Expects the same `EPH_MP_*` env vars as
/// `dpdk_mp_primary.cpp`.
///
/// Skips cleanly (GTEST_SKIP) when env vars are missing.

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

bool file_exists(const std::string& p) {
    std::ifstream f(p);
    return f.good();
}

} // namespace

TEST(DpdkMpSecondary, AttachAndVerify) {
    const char* file_prefix = env_or_null("EPH_MP_FILE_PREFIX");
    const char* ready_file  = env_or_null("EPH_MP_READY_FILE");

    if (!file_prefix || !ready_file) {
        GTEST_SKIP() << "missing EPH_MP_FILE_PREFIX / EPH_MP_READY_FILE "
                        "— run via tests/integration/dpdk_mp_e2e.sh";
    }

    // Wait up to 10 seconds for the primary's ready file.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!file_exists(ready_file) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!file_exists(ready_file)) {
        GTEST_SKIP() << "primary ready file never appeared — is dpdk_mp_e2e.sh "
                        "orchestrating this run?";
    }

    const std::string port_id_s      = env_or("EPH_MP_PORT_ID",      "0");
    const std::string nb_rx_queues_s = env_or("EPH_MP_NB_RX_QUEUES", "4");
    const std::string lcores         = env_or("EPH_MP_LCORES_SEC",   "2");
    const std::string allowed_dev    = env_or("EPH_MP_ALLOWED_DEV",  "");

    const uint16_t port_id      = static_cast<uint16_t>(std::stoul(port_id_s));
    const uint16_t nb_rx_queues = static_cast<uint16_t>(std::stoul(nb_rx_queues_s));

    // M4 guard: see dpdk_mp_primary.cpp — partition needs ≥ 2 queues
    // so primary [0, n/2) and secondary [n/2, n) are genuinely disjoint.
    ASSERT_GE(nb_rx_queues, 2u)
        << "MP test requires nb_rx_queues >= 2 (got " << nb_rx_queues
        << "); set EPH_MP_NB_RX_QUEUES to a value >= 2";

    eph::dpdk::EalConfig eal_cfg{};
    eal_cfg.program_name  = "dpdk_mp_secondary";
    eal_cfg.proc_type     = eph::dpdk::ProcType::Secondary;
    eal_cfg.proc_type_set = true;
    eal_cfg.file_prefix   = file_prefix;
    eal_cfg.lcores        = {lcores};
    if (!allowed_dev.empty()) eal_cfg.allowed_devs = {allowed_dev};

    eph::dpdk::PlatformConfig pcfg{};
    pcfg.port_id        = port_id;
    pcfg.nb_rx_queues   = nb_rx_queues;
    pcfg.nb_tx_queues   = nb_rx_queues;
    pcfg.proc_type      = eph::dpdk::ProcType::Secondary;
    pcfg.file_prefix    = file_prefix;
    pcfg.rx_queue_range = {static_cast<uint16_t>(nb_rx_queues / 2), nb_rx_queues};

    auto plat_r = eph::dpdk::Platform::create_with_eal(
        std::move(pcfg), std::move(eal_cfg),
        /*pins=*/{}, eph::utils::CpuPinPolicy{});
    ASSERT_TRUE(plat_r) << "create_with_eal (secondary) failed: " << plat_r.error();
    auto platform = std::move(*plat_r);

    EXPECT_TRUE(platform.is_running());
    EXPECT_EQ(platform.port_id(), port_id);
    EXPECT_NE(platform.mempool(), nullptr)
        << "secondary mempool lookup returned nullptr";

    const auto qr = platform.effective_rx_queue_range();
    EXPECT_EQ(qr.first,  nb_rx_queues / 2);
    EXPECT_EQ(qr.second, nb_rx_queues);
    // ~Platform handles teardown.
}
