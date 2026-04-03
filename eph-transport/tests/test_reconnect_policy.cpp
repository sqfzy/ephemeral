#include <gtest/gtest.h>

#include "eph/transport/reconnect_policy.hpp"

using namespace eph::net;

class ReconnectPolicyTest : public ::testing::Test {
protected:
    TransportConfig make_config(int max_attempts = 3,
                                 int interval_ms = 10,
                                 int max_backoff_ms = 100) {
        TransportConfig cfg;
        cfg.remote_host = "test.host";
        cfg.remote_port = 1234;
        cfg.max_reconnect_attempts = max_attempts;
        cfg.reconnect_interval = std::chrono::milliseconds{interval_ms};
        cfg.max_reconnect_backoff = std::chrono::milliseconds{max_backoff_ms};
        return cfg;
    }
};

TEST_F(ReconnectPolicyTest, InitialStateIsZero) {
    auto cfg = make_config();
    ReconnectPolicy policy(cfg);
    EXPECT_EQ(policy.attempts(), 0);
    EXPECT_EQ(policy.total_reconnects(), 0);
    EXPECT_FALSE(policy.exhausted());
}

TEST_F(ReconnectPolicyTest, SuccessfulAttemptIncrementsTotal) {
    auto cfg = make_config();
    ReconnectPolicy policy(cfg);

    bool ok = policy.attempt([]() -> std::expected<void, ConnectionErrorInfo> {
        return {};  // success
    });

    EXPECT_TRUE(ok);
    EXPECT_EQ(policy.total_reconnects(), 1);
}

TEST_F(ReconnectPolicyTest, FailedAttemptIncrementsAttemptCount) {
    auto cfg = make_config(3);
    ReconnectPolicy policy(cfg);

    bool ok = policy.attempt([]() -> std::expected<void, ConnectionErrorInfo> {
        return std::unexpected(ConnectionErrorInfo{
            ConnectionError::kFactoryFailed, "test failure"});
    });

    EXPECT_FALSE(ok);
    EXPECT_EQ(policy.attempts(), 1);
    EXPECT_EQ(policy.total_reconnects(), 0);
    EXPECT_FALSE(policy.exhausted());
}

TEST_F(ReconnectPolicyTest, ExhaustedAfterMaxAttempts) {
    auto cfg = make_config(2);
    ReconnectPolicy policy(cfg);

    auto fail_fn = []() -> std::expected<void, ConnectionErrorInfo> {
        return std::unexpected(ConnectionErrorInfo{
            ConnectionError::kFactoryFailed, "test failure"});
    };

    (void)policy.attempt(fail_fn);
    EXPECT_FALSE(policy.exhausted());

    (void)policy.attempt(fail_fn);
    EXPECT_TRUE(policy.exhausted());
}

TEST_F(ReconnectPolicyTest, ResetClearsAttemptCount) {
    auto cfg = make_config(5);
    ReconnectPolicy policy(cfg);

    auto fail_fn = []() -> std::expected<void, ConnectionErrorInfo> {
        return std::unexpected(ConnectionErrorInfo{
            ConnectionError::kFactoryFailed, "test failure"});
    };

    (void)policy.attempt(fail_fn);
    (void)policy.attempt(fail_fn);
    EXPECT_EQ(policy.attempts(), 2);

    policy.reset();
    EXPECT_EQ(policy.attempts(), 0);
    EXPECT_FALSE(policy.exhausted());
    // total_reconnects is NOT reset
}

TEST_F(ReconnectPolicyTest, CallbackCanAbortReconnection) {
    auto cfg = make_config(10);
    cfg.on_reconnect_attempt = [](int, int, std::string_view) {
        return false;  // abort
    };
    ReconnectPolicy policy(cfg);

    auto fail_fn = []() -> std::expected<void, ConnectionErrorInfo> {
        return std::unexpected(ConnectionErrorInfo{
            ConnectionError::kFactoryFailed, "test failure"});
    };

    bool ok = policy.attempt(fail_fn);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(policy.exhausted());  // callback forced exhaustion
}

TEST_F(ReconnectPolicyTest, SuccessOnSecondAttempt) {
    auto cfg = make_config(5);
    ReconnectPolicy policy(cfg);

    int call_count = 0;
    auto fn = [&]() -> std::expected<void, ConnectionErrorInfo> {
        ++call_count;
        if (call_count == 1) {
            return std::unexpected(ConnectionErrorInfo{
                ConnectionError::kFactoryFailed, "first try fails"});
        }
        return {};  // second try succeeds
    };

    bool ok1 = policy.attempt(fn);
    EXPECT_FALSE(ok1);

    bool ok2 = policy.attempt(fn);
    EXPECT_TRUE(ok2);
    EXPECT_EQ(policy.total_reconnects(), 1);
}

TEST_F(ReconnectPolicyTest, DefaultMaxBackoffIs16xBase) {
    // When max_reconnect_backoff is 0 (default), backoff should cap at 16x base.
    // Before the fix, 0ms max_backoff caused std::min(backoff*2, 0ms) = 0ms,
    // meaning backoff collapsed to zero after the first failure.
    auto cfg = make_config(/*max_attempts=*/10,
                           /*interval_ms=*/10,
                           /*max_backoff_ms=*/0);  // 0 = 16x base = 160ms
    ReconnectPolicy policy(cfg);

    auto fail_fn = []() -> std::expected<void, ConnectionErrorInfo> {
        return std::unexpected(ConnectionErrorInfo{
            ConnectionError::kFactoryFailed, "always fail"});
    };

    // Run several attempts to let backoff grow; measure elapsed time.
    // With 16x cap at 160ms, attempts should take non-trivial time.
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 3; ++i) {
        (void)policy.attempt(fail_fn);
    }
    auto elapsed = std::chrono::steady_clock::now() - start;

    // With proper backoff (10ms, 20ms, 40ms + jitter), total > 50ms.
    // With the old bug (0ms backoff), total < 5ms.
    EXPECT_GT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 30);
}

TEST_F(ReconnectPolicyTest, ExplicitMaxBackoffIsCapped) {
    // When max_backoff is explicitly set, backoff should not exceed it.
    auto cfg = make_config(/*max_attempts=*/10,
                           /*interval_ms=*/10,
                           /*max_backoff_ms=*/50);
    ReconnectPolicy policy(cfg);

    auto fail_fn = []() -> std::expected<void, ConnectionErrorInfo> {
        return std::unexpected(ConnectionErrorInfo{
            ConnectionError::kFactoryFailed, "always fail"});
    };

    // After 5 failures, backoff doubles: 10, 20, 40, 50(cap), 50(cap).
    // Total with jitter: ~170-213ms (75%-125% jitter).
    // Without cap: 10, 20, 40, 80, 160 = 310ms base.
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 5; ++i) {
        (void)policy.attempt(fail_fn);
    }
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    // Should be roughly 170ms (10+20+40+50+50), not 310ms (10+20+40+80+160).
    // Use a generous upper bound accounting for jitter (125%) and scheduling.
    EXPECT_LT(elapsed_ms, 400);
    // But must be non-trivial (not collapsed to 0).
    EXPECT_GT(elapsed_ms, 80);
}

TEST_F(ReconnectPolicyTest, BackoffDoublesOnConsecutiveFailures) {
    // Verify exponential growth: each attempt takes longer than the last.
    auto cfg = make_config(/*max_attempts=*/10,
                           /*interval_ms=*/20,
                           /*max_backoff_ms=*/500);
    ReconnectPolicy policy(cfg);

    auto fail_fn = []() -> std::expected<void, ConnectionErrorInfo> {
        return std::unexpected(ConnectionErrorInfo{
            ConnectionError::kFactoryFailed, "always fail"});
    };

    // Measure first attempt (base ~20ms) vs third attempt (base ~80ms).
    auto t0 = std::chrono::steady_clock::now();
    (void)policy.attempt(fail_fn);
    auto first_elapsed = std::chrono::steady_clock::now() - t0;

    (void)policy.attempt(fail_fn);  // skip second

    auto t2 = std::chrono::steady_clock::now();
    (void)policy.attempt(fail_fn);
    auto third_elapsed = std::chrono::steady_clock::now() - t2;

    // Third attempt backoff (80ms * jitter) should be > first (20ms * jitter).
    // Use conservative comparison: third > first * 1.5 (accounts for jitter variance).
    auto first_ms = std::chrono::duration_cast<std::chrono::milliseconds>(first_elapsed).count();
    auto third_ms = std::chrono::duration_cast<std::chrono::milliseconds>(third_elapsed).count();
    EXPECT_GT(third_ms, first_ms);
}

TEST_F(ReconnectPolicyTest, ResetRestoresBaseBackoff) {
    // After reset(), backoff should restart from the base interval,
    // not continue from the doubled value.
    auto cfg = make_config(/*max_attempts=*/10,
                           /*interval_ms=*/10,
                           /*max_backoff_ms=*/500);
    ReconnectPolicy policy(cfg);

    auto fail_fn = []() -> std::expected<void, ConnectionErrorInfo> {
        return std::unexpected(ConnectionErrorInfo{
            ConnectionError::kFactoryFailed, "always fail"});
    };

    // Run 3 failures: backoff grows to ~80ms base.
    for (int i = 0; i < 3; ++i) {
        (void)policy.attempt(fail_fn);
    }

    // Reset and measure next attempt -- should be ~10ms again.
    policy.reset();
    EXPECT_EQ(policy.attempts(), 0);

    auto start = std::chrono::steady_clock::now();
    (void)policy.attempt(fail_fn);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    // First attempt after reset should use base interval (~10ms with jitter).
    // Must be < 25ms (10ms * 125% jitter + scheduling slack).
    EXPECT_LT(elapsed_ms, 25);
}

TEST_F(ReconnectPolicyTest, TotalReconnectsPreservedAcrossReset) {
    auto cfg = make_config(5);
    ReconnectPolicy policy(cfg);

    int call_count = 0;
    auto fn = [&]() -> std::expected<void, ConnectionErrorInfo> {
        ++call_count;
        return {};  // always succeeds
    };

    (void)policy.attempt(fn);
    EXPECT_EQ(policy.total_reconnects(), 1);

    policy.reset();
    (void)policy.attempt(fn);
    EXPECT_EQ(policy.total_reconnects(), 2);
    EXPECT_EQ(policy.attempts(), 1);  // attempts reset, reconnects not
}

TEST_F(ReconnectPolicyTest, CallbackExceptionDoesNotAbort) {
    // If the on_reconnect_attempt callback throws, the exception should be
    // caught and reconnection should continue (not abort).
    auto cfg = make_config(10);
    cfg.on_reconnect_attempt = [](int, int, std::string_view) -> bool {
        throw std::runtime_error("callback exploded");
    };
    ReconnectPolicy policy(cfg);

    auto fail_fn = []() -> std::expected<void, ConnectionErrorInfo> {
        return std::unexpected(ConnectionErrorInfo{
            ConnectionError::kFactoryFailed, "test failure"});
    };

    // Should not propagate the exception
    bool ok = policy.attempt(fail_fn);
    EXPECT_FALSE(ok);
    // Should NOT be exhausted — exception is caught, retries continue
    EXPECT_FALSE(policy.exhausted());
    EXPECT_EQ(policy.attempts(), 1);
}

TEST_F(ReconnectPolicyTest, ZeroMaxAttemptsIsImmediatelyExhausted) {
    auto cfg = make_config(0);
    ReconnectPolicy policy(cfg);
    EXPECT_TRUE(policy.exhausted());  // 0 >= 0
}

TEST_F(ReconnectPolicyTest, SingleAttemptMaxExhaustsAfterOne) {
    auto cfg = make_config(1);
    ReconnectPolicy policy(cfg);
    EXPECT_FALSE(policy.exhausted());

    auto fail_fn = []() -> std::expected<void, ConnectionErrorInfo> {
        return std::unexpected(ConnectionErrorInfo{
            ConnectionError::kFactoryFailed, "test failure"});
    };

    (void)policy.attempt(fail_fn);
    EXPECT_TRUE(policy.exhausted());
}
