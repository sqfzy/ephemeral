/// @file test_eal_init_with_pins.cpp
/// Integration test for `EalGuard::init` (typed-pin overload) —
/// exercises the full rte_eal_init path with typed lcore pins.
///
/// Why a separate integration binary instead of expanding test_lcore_pin
/// (the unit test file): rte_eal_init touches process-global DPDK state
/// (hugepages allocation, lcore thread spawn, optional vfio-pci open).
/// Mixing it with the lightweight pre-EAL unit tests would force every
/// other test in the same binary to inherit the EAL lifecycle.
///
/// Run conditions:
///   * **Must run as root** (or with `CAP_IPC_LOCK` + `/dev/hugepages`
///     write access). EAL needs to mlock huge pages and create the
///     IPC socket under `/var/run/dpdk/<file_prefix>/`. NIC binding
///     is NOT required — the test runs `--no-pci`, so a vfio-pci
///     binding is irrelevant.
///   * Hugepages must be available (test allocates 64MB of 2MB pages).
///   * Uses a unique `--file-prefix=eph_test_<pid>` so it does not
///     collide with any other DPDK process running on the same host
///     (e.g. a production producer using the default `rte` prefix).
///
/// SKIPS cleanly when EAL init fails for any reason (no root / no
/// hugepages / unrelated DPDK init failure). Run with:
///   sudo $(xmake show -t test_eal_init_with_pins | grep targetfile | awk '{print $2}')
/// or:
///   sudo xmake run -P <project> test_eal_init_with_pins
///
/// What it covers (only adds value beyond test_lcore_pin's pre-EAL paths):
///   * EAL succeeds and pin_guard is alive — registry has the cpus.
///   * EalGuard moves out of scope → eal_cleanup runs first, then
///     pin_guards_ destructor unregisters the cpus from the registry.
///
/// Failure paths (mutual-exclusion / pre-EAL conflict / batch rollback)
/// are already covered by test_lcore_pin without touching DPDK runtime,
/// so they are not duplicated here.

#include <fstream>
#include <string>
#include <unistd.h>  // getpid()

#include <gtest/gtest.h>

#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/lcore_pin.hpp"
#include "eph/utils/cpu.hpp"

using namespace eph::dpdk;

namespace {

/// Best-effort check: do we have at least one free 2MB hugepage on any
/// numa node? If not, rte_eal_init will fail with `Cannot allocate memory`
/// and the test is meaningless. Note: EAL init can also fail with the
/// same errno when run as a non-root user (no permission to mlock pages
/// or write to /var/run/dpdk), so this check is necessary but not
/// sufficient — `EalGuard::init()` returning unexpected → SKIP covers
/// the rest.
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
    // Per-pid file_prefix so this test never collides with another DPDK
    // process on the same host (production producer / second test run
    // / leftover from a crashed run). rte_eal_init creates a directory
    // under /var/run/dpdk/<prefix>/ for its IPC sockets; the default
    // prefix `rte` is shared by every DPDK process unless overridden.
    std::string file_prefix = "eph_test_" + std::to_string(::getpid());
    cfg.file_prefix = file_prefix;
    // Run without PCI / shared-config so this test doesn't depend on a
    // vfio-pci NIC binding. We're verifying the lcore-pin × EAL plumbing,
    // not real NIC traffic.
    cfg.extra_args = {"--no-pci", "--no-shconf", "-m", "64"};

    {
        auto eal = EalGuard::init(
            cfg, std::span<LcorePin const>{pins});
        if (!eal) {
            GTEST_SKIP() << "rte_eal_init failed — most often this means the "
                            "test is running without root (EAL needs CAP_IPC_LOCK "
                            "to mlock hugepages and write access to /var/run/dpdk/). "
                            "Re-run with `sudo xmake run test_eal_init_with_pins`. "
                            "EAL detail: "
                         << eal.error();
        }

        EXPECT_TRUE(eph::utils::is_cpu_externally_pinned(0));
        EXPECT_TRUE(eph::utils::is_cpu_externally_pinned(1));
    }  // <-- ~EalGuard runs eal_cleanup() then pin_guards_ unregisters

    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(0))
        << "pin_guards_ in EalGuard must unregister cpu 0 on destruction";
    EXPECT_FALSE(eph::utils::is_cpu_externally_pinned(1))
        << "pin_guards_ in EalGuard must unregister cpu 1 on destruction";
}
