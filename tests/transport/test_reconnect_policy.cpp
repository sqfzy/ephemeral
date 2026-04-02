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

    policy.attempt(fail_fn);
    policy.attempt(fail_fn);
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
