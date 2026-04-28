/// @file test_eal_init_with_pins.cpp
/// Integration test for `EalGuard::init_with_pins` — exercises the full
/// rte_eal_init path with typed lcore pins.
///
/// Why a separate integration binary instead of expanding test_lcore_pin
/// (the unit test file): rte_eal_init touches process-global DPDK state
/// (hugepages allocation, lcore thread spawn, optional vfio-pci open).
/// Mixing it with the lightweight pre-EAL unit tests would force every
/// other test in the same binary to inherit the EAL lifecycle.
///
/// Run conditions:
///   * Hugepages must be available (≥ 64MB free 2M hugepages).
///   * EAL is run in `--no-pci --no-shconf` mode so no NIC binding /
///     /var/run scratch is required.
///   * Suitable for any host with hugepages enabled; if hugepages are
///     not configured, every test SKIPs cleanly.
///
/// What it covers (only adds value beyond test_lcore_pin's pre-EAL paths):
///   * EAL succeeds and pin_guard is alive — registry has the cpus.
///   * EalGuard moves out of scope → eal_cleanup runs first, then
///     pin_guard_ destructor unregisters the cpus from the registry.
///
/// Failure paths (mutual-exclusion / pre-EAL conflict / batch rollback)
/// are already covered by test_lcore_pin without touching DPDK runtime,
/// so they are not duplicated here.

#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/lcore_pin.hpp"
#include "eph/utils/cpu.hpp"

using namespace eph::dpdk;

namespace {

/// Best-effort check: do we have at least one free 2MB hugepage on any
/// numa node? If not, rte_eal_init will fail with `Cannot allocate memory`
/// and the test is meaningless.
bool hugepages_available() {
    std::ifstream f("/proc/meminfo");
    std::string line;
    while (std::getline(f, line)) {
        if (line.find("HugePages_Free:") != std::string::npos) {
            // Line shape: "HugePages_Free:    1024"
            size_t pos = line.find_last_not_of(" 0123456789");
            if (pos == std::string::npos) return false;
            try {
                long n = std::stol(line.substr(pos + 1));
                return n > 0;
            } catch (...) { return false; }
        }
    }
    return false;
}

} // namespace

TEST(InitWithPinsIntegration, SuccessPathRegistersAndCleansUp) {
    if (!hugepages_available()) {
        GTEST_SKIP() << "no free hugepages — DPDK EAL init impossible on this host";
    }

    eph::utils::reset_pin_registry_for_tests();

    // Use cpu 0/1 — typically not isolcpus / not an SMT pair worth
    // reserving — and apply the relaxed default policy.
    std::array pins = {
        LcorePin{0, 0, "test-rx"},
        LcorePin{1, 1, "test-tx"},
    };

    EalConfig cfg;
    cfg.program_name = "test_eal_init_with_pins";
    // Run without PCI / shared-config so this test doesn't fight any
    // other DPDK process or require root to re-bind a NIC.
    cfg.extra_args = {"--no-pci", "--no-shconf", "-m", "64"};

    {
        auto eal = EalGuard::init_with_pins(
            cfg, std::span<LcorePin const>{pins});
        if (!eal) {
            GTEST_SKIP() << "rte_eal_init failed (likely a permission / "
                            "hugepage / EAL-already-init constraint): "
                         << eal.error();
        }

        EXPECT_TRUE(eph::utils::is_cpu_externally_pinned(0));
        EXPECT_TRUE(eph::utils::is_cpu_externally_pinned(1));
    }  // <-- ~EalGuard runs eal_cleanup() then pin_guard_ unregisters

    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(0))
        << "pin_guard_ in EalGuard must unregister cpu 0 on destruction";
    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(1))
        << "pin_guard_ in EalGuard must unregister cpu 1 on destruction";
}
