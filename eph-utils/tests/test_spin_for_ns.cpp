/// @file eph-utils/tests/test_spin_for_ns.cpp
/// Verify `eph::utils::spin_for_ns(N)` busy-waits ≈ N ns measured via TSC.
/// The band is deliberately loose (30%) because unit tests run on shared
/// CI hardware where occasional scheduler hiccups make a tight bound flaky.
/// Tighter accuracy is validated end-to-end by the latency bench.

#include <cstdint>

#include <gtest/gtest.h>

#include "eph/utils/cpu.hpp"
#include "eph/utils/time.hpp"

namespace {

struct TscFixture : ::testing::Test {
    void SetUp() override {
        ASSERT_TRUE(eph::utils::TSC::init());
    }
};

uint64_t measured_ns(long target_ns) {
    auto t0 = eph::utils::TSC::now();
    eph::utils::spin_for_ns(target_ns);
    auto t1 = eph::utils::TSC::now();
    return static_cast<uint64_t>(
        eph::utils::TSC::to_ns(t1 - t0).value_or(0.0));
}

} // namespace

TEST_F(TscFixture, ZeroIsNoop) {
    EXPECT_LT(measured_ns(0), 100u);
    EXPECT_LT(measured_ns(-1), 100u);
}

TEST_F(TscFixture, OneMicrosecond) {
    (void)measured_ns(1000); // warm caches
    auto m = measured_ns(1000);
    EXPECT_GE(m, 700u);
    EXPECT_LE(m, 1500u);
}

TEST_F(TscFixture, TenMicroseconds) {
    (void)measured_ns(10'000);
    auto m = measured_ns(10'000);
    EXPECT_GE(m,  9'000u);
    EXPECT_LE(m, 13'000u);
}

TEST_F(TscFixture, OneHundredMicroseconds) {
    (void)measured_ns(100'000);
    auto m = measured_ns(100'000);
    EXPECT_GE(m,  90'000u);
    EXPECT_LE(m, 130'000u);
}
