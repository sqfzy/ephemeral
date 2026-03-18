#pragma once

/// @file dpdk_test_env.hpp
/// Google Test environment that boots EAL once per test binary.

#include <vector>

#include <gtest/gtest.h>

#include <rte_eal.h>
#include <rte_errno.h>

namespace eph::dpdk::test {

class DpdkTestEnv : public ::testing::Environment {
public:
    void SetUp() override {
        std::vector<const char*> raw = {
            "dpdk_test",
            "--no-huge",
            "--no-pci",
            "--vdev=net_null0",
            "--log-level=1",
        };

        int argc = static_cast<int>(raw.size());
        std::vector<char*> argv;
        argv.reserve(raw.size());
        for (auto a : raw) argv.push_back(const_cast<char*>(a));

        int ret = rte_eal_init(argc, argv.data());
        ASSERT_GE(ret, 0)
            << "EAL init failed (rte_errno=" << rte_errno
            << "): " << rte_strerror(rte_errno)
            << "\nHint: on Linux, ensure CONFIG_VFIO or run with --no-huge";
    }

    void TearDown() override {
        rte_eal_cleanup();
    }
};

namespace detail {
    [[maybe_unused]]
    static const auto* dpdk_env_reg =
        ::testing::AddGlobalTestEnvironment(new DpdkTestEnv());
}

} // namespace eph::dpdk::test
