/// @file dpdk_mp_v3_secondary.cpp
/// V3 secondary-role integration binary — minimal "how to find primary"
/// surface.
///
/// Companion to `dpdk_mp_secondary.cpp` (v2 declarative path) — proves
/// `Platform::attach_with_eal(PlatformAttachConfig{file_prefix})` reaches
/// the same end state with **only file_prefix as input**. Empirically
/// validates plan assumption A1 (`rte_eth_dev_info_get` returns
/// primary-configured nb_rx_queues from a secondary process).
///
/// Started by `tests/integration/dpdk_mp_v3_e2e.sh` after primary's
/// ready file. Skips when env vars are missing or NIC unavailable.
///
/// Compare to v2 secondary (dpdk_mp_secondary.cpp): the v2 version
/// fills 5 PlatformConfig fields including nb_rx_queues / nb_tx_queues /
/// rx_queue_range / proc_type. The v3 version fills 1.

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
#include "eph/dpdk/platform_attach.hpp"

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

TEST(DpdkMpV3Secondary, AttachWithFilePrefixOnly) {
    const char* file_prefix = env_or_null("EPH_MP_FILE_PREFIX");
    const char* ready_file  = env_or_null("EPH_MP_READY_FILE");

    if (!file_prefix || !ready_file) {
        GTEST_SKIP() << "missing EPH_MP_FILE_PREFIX / EPH_MP_READY_FILE "
                        "— run via tests/integration/dpdk_mp_v3_e2e.sh";
    }

    // Wait up to 10 seconds for the primary's ready file.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!file_exists(ready_file) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!file_exists(ready_file)) {
        GTEST_SKIP() << "primary ready file never appeared — is the e2e "
                        "script orchestrating this run?";
    }

    const std::string port_id_s   = env_or("EPH_MP_PORT_ID",     "0");
    const std::string lcores      = env_or("EPH_MP_LCORES_SEC",  "2");
    const std::string allowed_dev = env_or("EPH_MP_ALLOWED_DEV", "");
    const uint16_t port_id = static_cast<uint16_t>(std::stoul(port_id_s));

    // EalConfig: caller does NOT set proc_type / file_prefix —
    // attach_with_eal injects both from the v3 PlatformAttachConfig.
    eph::dpdk::EalConfig eal_cfg{};
    eal_cfg.program_name = "dpdk_mp_v3_secondary";
    eal_cfg.lcores       = {lcores};
    if (!allowed_dev.empty()) eal_cfg.allowed_devs = {allowed_dev};

    // V3 secondary input: only what's needed to find primary.
    // Notable: NO nb_rx_queues, NO nb_tx_queues, NO rx_queue_range,
    // NO mp_topology, NO proc_type. The library reads the topology
    // from the registry and the NIC physical state via the live port.
    eph::dpdk::PlatformAttachConfig acfg{};
    acfg.file_prefix = file_prefix;
    acfg.port_id     = port_id;

    auto plat_r = eph::dpdk::Platform::attach_with_eal(
        std::move(acfg), std::move(eal_cfg),
        /*pins=*/{}, eph::utils::CpuPinPolicy{});
    ASSERT_TRUE(plat_r) << "attach_with_eal failed: " << plat_r.error();
    auto platform = std::move(*plat_r);

    EXPECT_TRUE(platform.is_running());
    EXPECT_EQ(platform.port_id(), port_id);
    EXPECT_NE(platform.mempool(), nullptr)
        << "secondary mempool lookup returned nullptr";

    // The library is supposed to derive rx_queue_range from the
    // registry's slot table — secondary slot is the lowest free at
    // claim time. Primary owns slot 0 = queues [0, n/2); secondary
    // claims slot 1 = queues [n/2, n).
    const auto qr = platform.effective_rx_queue_range();
    EXPECT_GT(qr.second, qr.first)
        << "expected non-empty rx_queue_range from registry slot";
    // ~Platform handles teardown.
}
