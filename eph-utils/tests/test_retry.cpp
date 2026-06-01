/// @file test_retry.cpp
/// Unit tests for `eph::utils::retry`.
///
/// All tests use a recording sleeper that never actually sleeps, so the whole
/// suite runs in well under the 100ms budget regardless of the configured
/// backoff delays — proving the sleeper injection point works.

#include <chrono>
#include <expected>
#include <vector>
#include <gtest/gtest.h>

#include "eph/core/error.hpp"
#include "eph/utils/backoff.hpp"
#include "eph/utils/retry.hpp"

namespace eu = eph::utils;
using namespace std::chrono_literals;
using eph::core::Error;
using eph::core::ErrorInfo;
using Result = std::expected<int, ErrorInfo>;

namespace {

/// Sleeper that records requested delays instead of sleeping.
struct RecordingSleeper {
    std::vector<std::chrono::milliseconds>* log;
    void operator()(std::chrono::milliseconds d) const noexcept {
        log->push_back(d);
    }
};

/// Callable that fails `fail_n` times then succeeds, counting invocations.
struct Flaky {
    int fail_n;
    int* calls;
    Error err = Error::Disconnected;
    Result operator()() {
        ++*calls;
        if (*calls <= fail_n) return std::unexpected(ErrorInfo{err});
        return Result{42};
    }
};

}  // namespace

TEST(Retry, SucceedsFirstTryNoSleep) {
    int calls = 0;
    std::vector<std::chrono::milliseconds> slept;
    auto r = eu::retry(Flaky{.fail_n = 0, .calls = &calls},
                       eu::ExponentialBackoff{{.max_attempts = 5}},
                       eu::RetryAlways{}, RecordingSleeper{&slept});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 42);
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(slept.empty()) << "no backoff should be consumed on first success";
}

TEST(Retry, SucceedsAfterRetriesRecordsDelays) {
    int calls = 0;
    std::vector<std::chrono::milliseconds> slept;
    auto r = eu::retry(
        Flaky{.fail_n = 2, .calls = &calls},
        eu::ExponentialBackoff{{.initial_backoff = 100ms,
                                .max_backoff     = 10s,
                                .multiplier      = 2.0,
                                .jitter_factor   = 0.0,
                                .max_attempts    = 5}},
        eu::RetryAlways{}, RecordingSleeper{&slept});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 42);
    EXPECT_EQ(calls, 3);  // 2 failures + 1 success
    ASSERT_EQ(slept.size(), 2u);
    EXPECT_EQ(slept[0], 100ms);
    EXPECT_EQ(slept[1], 200ms);
}

TEST(Retry, ExhaustionReturnsLastError) {
    int calls = 0;
    std::vector<std::chrono::milliseconds> slept;
    auto r = eu::retry(
        Flaky{.fail_n = 100, .calls = &calls, .err = Error::Timeout},
        eu::ExponentialBackoff{{.initial_backoff = 10ms,
                                .max_backoff     = 10ms,
                                .jitter_factor   = 0.0,
                                .max_attempts    = 3}},
        eu::RetryAlways{}, RecordingSleeper{&slept});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::Timeout);
    EXPECT_EQ(calls, 4);          // initial + 3 retries
    EXPECT_EQ(slept.size(), 3u);  // 3 delays consumed
}

TEST(Retry, WhenPredicateStopsEarly) {
    int calls = 0;
    std::vector<std::chrono::milliseconds> slept;
    // Fails with InvalidConfig, which the predicate treats as non-retriable.
    auto r = eu::retry(
        Flaky{.fail_n = 100, .calls = &calls, .err = Error::InvalidConfig},
        eu::ExponentialBackoff{{.max_attempts = 5}},
        [](const ErrorInfo& e) { return e.code != Error::InvalidConfig; },
        RecordingSleeper{&slept});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
    EXPECT_EQ(calls, 1) << "non-retriable error must not be retried";
    EXPECT_TRUE(slept.empty());
}

TEST(Retry, ConstantBackoffDelays) {
    int calls = 0;
    std::vector<std::chrono::milliseconds> slept;
    auto r = eu::retry(Flaky{.fail_n = 3, .calls = &calls},
                       eu::ConstantBackoff{{.delay = 50ms, .max_attempts = 5}},
                       eu::RetryAlways{}, RecordingSleeper{&slept});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(calls, 4);
    ASSERT_EQ(slept.size(), 3u);
    for (auto d : slept) EXPECT_EQ(d, 50ms);
}

TEST(Retry, DefaultWhenRetriesAllErrors) {
    // With the default RetryAlways and a 1-retry budget, a persistent failure
    // still gets exactly one retry.
    int calls = 0;
    std::vector<std::chrono::milliseconds> slept;
    auto r = eu::retry(Flaky{.fail_n = 100, .calls = &calls},
                       eu::ConstantBackoff{{.delay = 1ms, .max_attempts = 1}},
                       eu::RetryAlways{}, RecordingSleeper{&slept});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(calls, 2);  // initial + 1 retry
    EXPECT_EQ(slept.size(), 1u);
}
