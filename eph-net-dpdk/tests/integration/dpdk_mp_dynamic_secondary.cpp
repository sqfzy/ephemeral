/// @file dpdk_mp_dynamic_secondary.cpp
/// Secondary-role integration binary for the autojoin path
/// (`Platform::join_dynamic`). Started by
/// `tests/integration/dpdk_mp_dynamic_e2e.sh` after the primary
/// has touched its ready-file.
///
/// What this binary asserts:
///   1. `Platform::join_dynamic` returns a valid Platform that has
///      auto-resolved this process to SECONDARY (DPDK lockfile
///      semantics: the primary already owns the file_prefix).
///   2. `is_secondary()` is true.
///   3. The auto-claimed slot is index 1 (the lowest free slot
///      after the primary took 0). Verified via
///      `effective_rx_queue_range()` — secondary owns
///      `[queues_per_proc, 2*queues_per_proc)` ⇒ for the default
///      `queues_per_proc=1`, range is `[1, 2)`.
///   4. The secondary's queue range is disjoint from the primary's.

#include <cstdlib>
#include <string>
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

TEST(DpdkMpDynamicSecondary, AutojoinResolvesAsSecondaryClaimsSlotOne) {
    const char* pci = env_or_null("EPH_MP_ALLOWED_DEV");
    if (!pci) {
        GTEST_SKIP() << "missing EPH_MP_ALLOWED_DEV — run via "
                        "tests/integration/dpdk_mp_dynamic_e2e.sh";
    }

    const std::string nb_rx_queues_s = env_or("EPH_MP_NB_RX_QUEUES", "4");
    const std::string lcores         = env_or("EPH_MP_LCORES_SEC",   "1");

    const uint16_t nb_rx_queues =
        static_cast<uint16_t>(std::stoul(nb_rx_queues_s));
    ASSERT_GE(nb_rx_queues, 2u);

    eph::dpdk::JoinDynamicConfig cfg{};
    cfg.pci                          = pci;
    cfg.pcfg_template.nb_rx_queues   = nb_rx_queues;
    cfg.lcores                       = {lcores};

    auto plat_r = eph::dpdk::Platform::join_dynamic(std::move(cfg));
    ASSERT_TRUE(plat_r)
        << "join_dynamic failed: " << plat_r.error();
    auto platform = std::move(*plat_r);

    EXPECT_TRUE(platform.is_secondary())
        << "second peer must resolve as secondary";
    EXPECT_TRUE(platform.has_mp_topology());

    // queues_per_proc=1 default → secondary should land on slot 1
    // (lowest free), owning queue range [1, 2).
    const auto qr = platform.effective_rx_queue_range();
    EXPECT_EQ(qr.first, 1)
        << "expected secondary to claim slot 1 (lowest free)";
    EXPECT_EQ(qr.second, 2);

    EXPECT_GT(qr.first, 0)
        << "secondary queue range must not overlap primary's [0, 1)";

    auto pr = platform.self_port_range();
    ASSERT_TRUE(pr.has_value());
    EXPECT_GT(pr->first, 32768u)
        << "secondary's port range must start above the primary's";

    // ~Platform at function exit owns EAL: it releases DPDK resources
    // and runs eal_cleanup itself. No nested-scope idiom needed.
}
