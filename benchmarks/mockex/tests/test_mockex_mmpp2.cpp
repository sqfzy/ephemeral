/// @file tests/test_mockex_mmpp2.cpp
/// Statistical self-consistency tests for Mmpp2Sampler.
///
/// Rather than assert on specific PRNG outputs (brittle across libc++
/// versions), we drive the sampler for many events and check that the
/// *aggregate* behaviour matches the programmed parameters:
///
///   1. Seed reproducibility: two instances with the same seed emit
///      the same sequence of (interval, size) pairs.
///   2. Busy-state duration: with p_qb high and p_bq low, the sampler
///      spends most of its time in busy; assertion is on the fraction
///      of calls where `in_busy_regime()` is true.
///   3. Average rate: over N events with a fixed regime, the mean
///      interval is close to 1/λ. Confidence: 3σ of the gamma mean
///      for N samples of exp(λ).

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

#include "mockex/mmpp2.hpp"

namespace {

/// A trivial set of params sufficient for deterministic tests. Real
/// bench fixtures come from the offline fitter.
mockex::Mmpp2Params make_test_params(double lq = 100.0,
                                     double lb = 10'000.0,
                                     double pqb = 0.01,
                                     double pbq = 0.5) {
    mockex::Mmpp2Params p;
    p.lambda_quiet_hz    = lq;
    p.lambda_busy_hz     = lb;
    p.p_quiet_to_busy    = pqb;
    p.p_busy_to_quiet    = pbq;
    p.size_kde_bandwidth = 5.0;
    p.size_kde_anchors   = {200, 260, 320};
    p.size_kde_weights   = {0.5, 0.3, 0.2};
    p.seed_default       = 42;
    return p;
}

} // namespace

TEST(MmppSampler, SeedReproducibility) {
    auto p = make_test_params();
    mockex::Mmpp2Sampler<> a(p, /*seed=*/12345);
    mockex::Mmpp2Sampler<> b(p, /*seed=*/12345);
    for (int i = 0; i < 1000; ++i) {
        ASSERT_EQ(a.next_interval_ns(), b.next_interval_ns()) << i;
        ASSERT_EQ(a.size_hint_bytes(), b.size_hint_bytes()) << i;
    }
}

TEST(MmppSampler, DifferentSeedsDiverge) {
    auto p = make_test_params();
    mockex::Mmpp2Sampler<> a(p, /*seed=*/1);
    mockex::Mmpp2Sampler<> b(p, /*seed=*/2);
    bool any_differ = false;
    for (int i = 0; i < 200; ++i) {
        if (a.next_interval_ns() != b.next_interval_ns()) any_differ = true;
    }
    EXPECT_TRUE(any_differ)
        << "seed 1 vs seed 2 must produce at least one differing sample";
}

TEST(MmppSampler, RateMatchesQuietRegime) {
    // Force regime to quiet for the entire sweep. With p_qb=0 the
    // sampler should never leave quiet, giving a clean 1/λ_q check.
    auto p = make_test_params();
    p.p_quiet_to_busy = 0.0;  // pin to quiet
    mockex::Mmpp2Sampler<> s(p, /*seed=*/7);
    s.force_regime_for_test(/*busy=*/false);

    constexpr int N = 50'000;
    uint64_t total_ns = 0;
    for (int i = 0; i < N; ++i) total_ns += s.next_interval_ns();

    // Expected mean interval = 1/λ = 1e7 ns for λ=100 Hz → total ≈ N/λ s.
    const double expected_total_ns =
        static_cast<double>(N) / p.lambda_quiet_hz * 1e9;
    const double observed = static_cast<double>(total_ns);
    const double rel_err = std::abs(observed - expected_total_ns) /
                           expected_total_ns;
    // 3σ for exponential-sum: σ/mean = 1/sqrt(N). For N=50k, 3σ ≈ 1.3%.
    EXPECT_LT(rel_err, 0.03)
        << "expected_total=" << expected_total_ns
        << " observed=" << observed << " rel_err=" << rel_err;
}

TEST(MmppSampler, BusyFractionMatchesSteadyState) {
    // With the chain parameters programmed in make_test_params,
    // stationary π_busy = p_qb / (p_qb + p_bq) = 0.01 / (0.01+0.5) ≈ 0.0196.
    // Drive many events and check that observed busy fraction lands
    // close to stationary (within 3σ for a Bernoulli).
    auto p = make_test_params();
    mockex::Mmpp2Sampler<> s(p, /*seed=*/99);

    constexpr int N = 100'000;
    int busy_count = 0;
    for (int i = 0; i < N; ++i) {
        (void)s.next_interval_ns();
        if (s.in_busy_regime()) ++busy_count;
    }

    const double pi_busy_expected =
        p.p_quiet_to_busy / (p.p_quiet_to_busy + p.p_busy_to_quiet);
    const double observed = static_cast<double>(busy_count) / N;
    // σ_Bernoulli = sqrt(p(1-p)/N). For p≈0.02, N=100k, 3σ ≈ 0.0042.
    EXPECT_LT(std::abs(observed - pi_busy_expected), 0.01)
        << "expected≈" << pi_busy_expected
        << " observed=" << observed;
}

TEST(MmppSampler, SizeHintRespectsAnchorRange) {
    auto p = make_test_params();
    mockex::Mmpp2Sampler<> s(p, /*seed=*/5);
    for (int i = 0; i < 1000; ++i) {
        const size_t sz = s.size_hint_bytes();
        EXPECT_GE(sz, 1u);
        EXPECT_LE(sz, 65535u);
        // Jitter is ~σ=5, anchors 200..320 → 99% of samples in [180, 340].
        EXPECT_GE(sz, 150u);
        EXPECT_LE(sz, 400u);
    }
}
