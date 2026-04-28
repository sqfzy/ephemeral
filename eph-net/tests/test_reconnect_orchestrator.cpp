/// @file test_reconnect_orchestrator.cpp
/// Stage-2 unit tests for `eph::net::ReconnectOrchestrator<S>`.
///
/// All tests use `FakeStream` (no syscalls, no NIC) and a synthetic
/// monotonic TSC counter so the state machine can be exercised without
/// touching real time. `TSC::init()` is still called once in the test
/// fixture so the orchestrator's `to_cycles` / `to_ns` helpers don't
/// fall back to the 3 GHz estimate.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "eph/core/error.hpp"
#include "eph/core/metrics_concept.hpp"
#include "eph/utils/time.hpp"

#include "eph/net/reconnect_orchestrator.hpp"
#include "eph/net/test/fake_stream.hpp"

namespace en  = eph::net;
namespace ent = eph::net::test;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// MetricsSink that records every push for later inspection.
struct RecordingSink {
    struct Counter { std::string name; int64_t value; };
    struct Gauge   { std::string name; double  value; };
    std::vector<Counter> counters;
    std::vector<Gauge>   gauges;

    void push_counter(std::string_view name, int64_t value,
                      std::span<const eph::core::MetricTag> /*tags*/ = {}) noexcept {
        counters.push_back({std::string(name), value});
    }
    void push_gauge(std::string_view name, double value,
                    std::span<const eph::core::MetricTag> /*tags*/ = {}) noexcept {
        gauges.push_back({std::string(name), value});
    }
    void push_histogram(std::string_view, double,
                        std::span<const eph::core::MetricTag> = {}) noexcept {}
    void flush() noexcept {}
};
static_assert(eph::core::MetricsSink<RecordingSink>);

// Convenience: orchestrator over FakeStream.
using Orch = en::ReconnectOrchestrator<ent::FakeStream>;
using StreamPtr = Orch::StreamPtr;

// Standard policy: 3 attempts, no jitter (deterministic).
en::ReconnectConfig kDeterministicConfig() {
    en::ReconnectConfig cfg{};
    cfg.policy.initial_backoff = 10ms;
    cfg.policy.max_backoff     = 40ms;
    cfg.policy.multiplier      = 2.0;
    cfg.policy.jitter_factor   = 0.0;
    cfg.policy.max_attempts    = 3;
    return cfg;
}

class ReconnectOrchestratorTest : public ::testing::Test {
public:
    static void SetUpTestSuite() {
        // Calibrate once so to_cycles / to_ns work for the whole suite.
        eph::utils::TSC::init();
    }
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: HappyReconnect — initial connect succeeds, peer disconnects, the
// orchestrator backs off, and the next factory call wins.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(ReconnectOrchestratorTest, HappyReconnect) {
    auto cfg = kDeterministicConfig();
    int factory_calls = 0;
    Orch orch{
        cfg,
        /*factory=*/[&]() -> std::expected<StreamPtr, eph::core::ErrorInfo> {
            ++factory_calls;
            auto s = ent::FakeStream::create();
            s->set_state(eph::net::TcpState::Established);
            s->set_attached(true);
            return s;
        },
    };

    ASSERT_TRUE(orch.start(eph::utils::TSC::now()).has_value());
    EXPECT_EQ(orch.state(), en::ReconnectState::Connected);
    EXPECT_EQ(factory_calls, 1);
    EXPECT_EQ(orch.reconnect_count(), 1u);  // initial connect counts as a "reconnect"
    EXPECT_EQ(orch.reconnect_failures(), 0u);
    ASSERT_NE(orch.current(), nullptr);

    // Peer drops the connection — flip the FakeStream state.
    orch.current()->set_state(eph::net::TcpState::Closed);

    // tick() detects the closed state and starts backoff.
    orch.tick(eph::utils::TSC::now());
    EXPECT_EQ(orch.state(), en::ReconnectState::Backoff);
    EXPECT_EQ(orch.current(), nullptr);

    // Advance time past the backoff deadline. The 10ms initial backoff with
    // jitter=0 gives a deterministic 10ms wait; we add 50ms slack.
    auto deadline_tsc =
        eph::utils::TSC::now()
        + eph::utils::TSC::to_cycles(60ms).value_or(180'000'000ULL);
    orch.tick(deadline_tsc);
    EXPECT_EQ(orch.state(), en::ReconnectState::Connected);
    EXPECT_EQ(factory_calls, 2);
    EXPECT_EQ(orch.reconnect_count(), 2u);
    EXPECT_EQ(orch.reconnect_failures(), 0u);
    EXPECT_GT(orch.last_reconnect_duration_ns(), 0u);
    EXPECT_GT(orch.total_reconnect_duration_ns(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: MultipleFailuresThenSuccess — factory fails twice, third call wins.
// Verifies failures counter and that policy.attempts() advances per failure.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(ReconnectOrchestratorTest, MultipleFailuresThenSuccess) {
    auto cfg = kDeterministicConfig();
    cfg.policy.max_attempts = 0;  // unlimited so we can fail twice then succeed
    int factory_calls = 0;
    Orch orch{
        cfg,
        [&]() -> std::expected<StreamPtr, eph::core::ErrorInfo> {
            ++factory_calls;
            if (factory_calls < 3) {
                return std::unexpected(eph::core::ErrorInfo{
                    eph::core::Error::ConnectFailed, "synthetic"});
            }
            auto s = ent::FakeStream::create();
            s->set_state(eph::net::TcpState::Established);
            s->set_attached(true);
            return s;
        },
    };

    // Initial start fails — orchestrator goes to Backoff with attempt=1.
    auto vt = eph::utils::TSC::now();
    ASSERT_TRUE(orch.start(vt).has_value());
    EXPECT_EQ(orch.state(), en::ReconnectState::Backoff);
    EXPECT_EQ(orch.reconnect_failures(), 1u);

    // Drive forward by ratcheting virtual time past each backoff deadline.
    const auto step = eph::utils::TSC::to_cycles(1s).value_or(3'000'000'000ULL);
    for (int i = 0; i < 5 && orch.state() != en::ReconnectState::Connected; ++i) {
        vt += step;
        orch.tick(vt);
    }
    EXPECT_EQ(orch.state(), en::ReconnectState::Connected);
    EXPECT_EQ(factory_calls, 3);
    EXPECT_EQ(orch.reconnect_count(), 1u);
    EXPECT_EQ(orch.reconnect_failures(), 2u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: PolicyExhaustion — factory always fails; policy budget = 3.
// Final state is Failed; further ticks are no-ops.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(ReconnectOrchestratorTest, PolicyExhaustion) {
    auto cfg = kDeterministicConfig();
    cfg.policy.max_attempts = 3;
    int factory_calls = 0;
    Orch orch{
        cfg,
        [&]() -> std::expected<StreamPtr, eph::core::ErrorInfo> {
            ++factory_calls;
            return std::unexpected(eph::core::ErrorInfo{
                eph::core::Error::ConnectFailed, "always-fail"});
        },
    };

    auto vt = eph::utils::TSC::now();
    ASSERT_TRUE(orch.start(vt).has_value());
    // Drive forward until exhaustion — budget is 3 should_reconnect()s.
    const auto step = eph::utils::TSC::to_cycles(1s).value_or(3'000'000'000ULL);
    for (int i = 0; i < 10 && orch.state() == en::ReconnectState::Backoff; ++i) {
        vt += step;
        orch.tick(vt);
    }
    EXPECT_EQ(orch.state(), en::ReconnectState::Failed);
    EXPECT_EQ(orch.reconnect_count(), 0u);
    EXPECT_GE(orch.reconnect_failures(), 3u);  // at least the budget's worth

    // Further ticks must be no-ops.
    const auto fail_count_snapshot = orch.reconnect_failures();
    vt += step;
    orch.tick(vt);
    EXPECT_EQ(orch.state(), en::ReconnectState::Failed);
    EXPECT_EQ(orch.reconnect_failures(), fail_count_snapshot);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: ManualMarkDisconnected — Connected → user calls mark_disconnected
// → on_disconnect fires, state goes to Backoff, current() becomes null.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(ReconnectOrchestratorTest, ManualMarkDisconnected) {
    auto cfg = kDeterministicConfig();
    int disc_count = 0;
    eph::core::Error last_disc_code = eph::core::Error::Ok;
    Orch orch{
        cfg,
        [&]() -> std::expected<StreamPtr, eph::core::ErrorInfo> {
            auto s = ent::FakeStream::create();
            s->set_state(eph::net::TcpState::Established);
            s->set_attached(true);
            return s;
        },
        /*on_disconnect=*/[&](const eph::core::ErrorInfo& e) {
            ++disc_count;
            last_disc_code = e.code;
        },
    };

    ASSERT_TRUE(orch.start(eph::utils::TSC::now()).has_value());
    EXPECT_EQ(orch.state(), en::ReconnectState::Connected);
    orch.mark_disconnected({eph::core::Error::Timeout, "heartbeat"});
    EXPECT_EQ(orch.state(), en::ReconnectState::Backoff);
    EXPECT_EQ(orch.current(), nullptr);
    EXPECT_EQ(disc_count, 1);
    EXPECT_EQ(last_disc_code, eph::core::Error::Timeout);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5: StartFromNonIdle — second start() rejects with InvalidConfig.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(ReconnectOrchestratorTest, StartFromNonIdle) {
    auto cfg = kDeterministicConfig();
    Orch orch{
        cfg,
        []() -> std::expected<StreamPtr, eph::core::ErrorInfo> {
            auto s = ent::FakeStream::create();
            s->set_state(eph::net::TcpState::Established);
            s->set_attached(true);
            return s;
        },
    };

    ASSERT_TRUE(orch.start(eph::utils::TSC::now()).has_value());
    auto r2 = orch.start(eph::utils::TSC::now());
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code, eph::core::Error::InvalidConfig);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6: TickInFailedIsNoop — once Failed, no further state changes.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(ReconnectOrchestratorTest, TickInFailedIsNoop) {
    auto cfg = kDeterministicConfig();
    cfg.policy.max_attempts = 1;
    Orch orch{
        cfg,
        []() -> std::expected<StreamPtr, eph::core::ErrorInfo> {
            return std::unexpected(eph::core::ErrorInfo{
                eph::core::Error::ConnectFailed, "always-fail"});
        },
    };

    auto vt = eph::utils::TSC::now();
    ASSERT_TRUE(orch.start(vt).has_value());
    // Drive to exhaustion.
    const auto step = eph::utils::TSC::to_cycles(1s).value_or(3'000'000'000ULL);
    for (int i = 0; i < 5 && orch.state() != en::ReconnectState::Failed; ++i) {
        vt += step;
        orch.tick(vt);
    }
    ASSERT_EQ(orch.state(), en::ReconnectState::Failed);

    const auto count_snap   = orch.reconnect_count();
    const auto fail_snap    = orch.reconnect_failures();
    const auto last_dur     = orch.last_reconnect_duration_ns();

    for (int i = 0; i < 10; ++i) orch.tick(eph::utils::TSC::now());
    EXPECT_EQ(orch.state(), en::ReconnectState::Failed);
    EXPECT_EQ(orch.reconnect_count(), count_snap);
    EXPECT_EQ(orch.reconnect_failures(), fail_snap);
    EXPECT_EQ(orch.last_reconnect_duration_ns(), last_dur);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7: NullCallbacksOk — all optional callbacks default-constructed.
// State machine still works; no crash on tick / mark_disconnected.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(ReconnectOrchestratorTest, NullCallbacksOk) {
    auto cfg = kDeterministicConfig();
    Orch orch{
        cfg,
        []() -> std::expected<StreamPtr, eph::core::ErrorInfo> {
            auto s = ent::FakeStream::create();
            s->set_state(eph::net::TcpState::Established);
            s->set_attached(true);
            return s;
        },
        // on_disconnect, on_reconnect, attach, detach all defaulted (empty std::function)
    };

    ASSERT_TRUE(orch.start(eph::utils::TSC::now()).has_value());
    EXPECT_EQ(orch.state(), en::ReconnectState::Connected);
    orch.mark_disconnected({eph::core::Error::Disconnected, "test"});
    EXPECT_EQ(orch.state(), en::ReconnectState::Backoff);

    auto t = eph::utils::TSC::now()
             + eph::utils::TSC::to_cycles(100ms).value_or(300'000'000ULL);
    orch.tick(t);
    EXPECT_EQ(orch.state(), en::ReconnectState::Connected);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 8: MetricReadOutOfRange — querying a metric beyond the enum returns 0.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(ReconnectOrchestratorTest, MetricReadOutOfRange) {
    auto cfg = kDeterministicConfig();
    Orch orch{
        cfg,
        []() -> std::expected<StreamPtr, eph::core::ErrorInfo> {
            auto s = ent::FakeStream::create();
            s->set_state(eph::net::TcpState::Established);
            s->set_attached(true);
            return s;
        },
    };
    EXPECT_EQ(orch.metric(static_cast<en::ReconnectMetric>(99)), 0u);
    EXPECT_EQ(orch.metric(en::ReconnectMetric::kCount), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 9: PublishMetricsForwardsAll — all 4 metric values reach the sink with
// matching names; kLastReconnectDurationNs goes through push_gauge, the rest
// through push_counter.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(ReconnectOrchestratorTest, PublishMetricsForwardsAll) {
    auto cfg = kDeterministicConfig();
    Orch orch{
        cfg,
        []() -> std::expected<StreamPtr, eph::core::ErrorInfo> {
            auto s = ent::FakeStream::create();
            s->set_state(eph::net::TcpState::Established);
            s->set_attached(true);
            return s;
        },
    };
    ASSERT_TRUE(orch.start(eph::utils::TSC::now()).has_value());
    orch.mark_disconnected({eph::core::Error::Disconnected, "x"});
    auto t = eph::utils::TSC::now()
             + eph::utils::TSC::to_cycles(100ms).value_or(300'000'000ULL);
    orch.tick(t);
    ASSERT_EQ(orch.state(), en::ReconnectState::Connected);

    RecordingSink sink;
    en::publish_reconnect_metrics(orch, sink);

    ASSERT_EQ(sink.counters.size(), 3u);  // count + failures + duration_ns_total
    ASSERT_EQ(sink.gauges.size(), 1u);    // duration_ns.last

    EXPECT_EQ(sink.counters[0].name, "net.reconnect.count");
    EXPECT_EQ(sink.counters[0].value, static_cast<int64_t>(orch.reconnect_count()));
    EXPECT_EQ(sink.counters[1].name, "net.reconnect.failures");
    EXPECT_EQ(sink.counters[1].value, static_cast<int64_t>(orch.reconnect_failures()));
    EXPECT_EQ(sink.counters[2].name, "net.reconnect.duration_ns");
    EXPECT_EQ(sink.counters[2].value,
              static_cast<int64_t>(orch.total_reconnect_duration_ns()));
    EXPECT_EQ(sink.gauges[0].name, "net.reconnect.duration_ns.last");
    EXPECT_EQ(static_cast<uint64_t>(sink.gauges[0].value),
              orch.last_reconnect_duration_ns());
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 10: AttachFailureCountsAsAttempt — factory succeeds but attach hook
// rejects the stream; the orchestrator should bump failures, drop the
// fresh stream, and enter Backoff (not Connected).
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(ReconnectOrchestratorTest, AttachFailureCountsAsAttempt) {
    auto cfg = kDeterministicConfig();
    cfg.policy.max_attempts = 0;  // unlimited
    int attach_calls = 0;
    Orch orch{
        cfg,
        []() -> std::expected<StreamPtr, eph::core::ErrorInfo> {
            auto s = ent::FakeStream::create();
            s->set_state(eph::net::TcpState::Established);
            s->set_attached(true);
            return s;
        },
        /*on_disconnect=*/{},
        /*on_reconnect=*/{},
        /*attach=*/[&](ent::FakeStream*)
            -> std::expected<void, eph::core::ErrorInfo> {
            ++attach_calls;
            if (attach_calls == 1) {
                return std::unexpected(eph::core::ErrorInfo{
                    eph::core::Error::InvalidConfig, "synthetic-attach-fail"});
            }
            return {};
        },
    };

    ASSERT_TRUE(orch.start(eph::utils::TSC::now()).has_value());
    EXPECT_EQ(orch.state(), en::ReconnectState::Backoff);
    EXPECT_EQ(orch.reconnect_failures(), 1u);
    EXPECT_EQ(orch.reconnect_count(), 0u);
    EXPECT_EQ(orch.current(), nullptr);

    auto t = eph::utils::TSC::now()
             + eph::utils::TSC::to_cycles(100ms).value_or(300'000'000ULL);
    orch.tick(t);
    EXPECT_EQ(orch.state(), en::ReconnectState::Connected);
    EXPECT_EQ(orch.reconnect_count(), 1u);
    EXPECT_EQ(attach_calls, 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 11 (T2.14): EventsEmittedInOrder — the fine-grained event hook fires
// at every state transition. After connect → mark_disconnected → backoff →
// successful reconnect, only the events from the disconnect window forward
// are checked: [DisconnectDetected, BackoffScheduled, AttemptStarted,
// Connected]. Events from the initial start() are cleared first.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(ReconnectOrchestratorTest, EventsEmittedInOrder) {
    auto cfg = kDeterministicConfig();
    Orch orch{
        cfg,
        []() -> std::expected<StreamPtr, eph::core::ErrorInfo> {
            auto s = ent::FakeStream::create();
            s->set_state(eph::net::TcpState::Established);
            s->set_attached(true);
            return s;
        },
    };

    std::vector<en::ReconnectEventKind> events;
    orch.set_on_reconnect_event([&](const en::ReconnectEvent& ev) {
        events.push_back(ev.kind);
    });

    ASSERT_TRUE(orch.start(eph::utils::TSC::now()).has_value());
    ASSERT_EQ(orch.state(), en::ReconnectState::Connected);
    // Drop the start() sequence (AttemptStarted + Connected) so the
    // assertion below targets only the reconnect cycle.
    events.clear();

    orch.mark_disconnected({eph::core::Error::Disconnected, "x"});
    ASSERT_EQ(orch.state(), en::ReconnectState::Backoff);

    // Drive past the backoff deadline.
    auto t = eph::utils::TSC::now()
             + eph::utils::TSC::to_cycles(60ms).value_or(180'000'000ULL);
    orch.tick(t);
    ASSERT_EQ(orch.state(), en::ReconnectState::Connected);

    ASSERT_EQ(events.size(), 4u);
    EXPECT_EQ(events[0], en::ReconnectEventKind::DisconnectDetected);
    EXPECT_EQ(events[1], en::ReconnectEventKind::BackoffScheduled);
    EXPECT_EQ(events[2], en::ReconnectEventKind::AttemptStarted);
    EXPECT_EQ(events[3], en::ReconnectEventKind::Connected);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 12 (T2.14): AttemptFailedEventCarriesError — factory fails twice then
// succeeds. Verifies AttemptFailed events carry the underlying ErrorInfo and
// the full event sequence is observable for diagnostic correlation.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(ReconnectOrchestratorTest, AttemptFailedEventCarriesError) {
    auto cfg = kDeterministicConfig();
    cfg.policy.max_attempts = 0;  // unlimited
    int factory_calls = 0;
    Orch orch{
        cfg,
        [&]() -> std::expected<StreamPtr, eph::core::ErrorInfo> {
            ++factory_calls;
            if (factory_calls < 3) {
                return std::unexpected(eph::core::ErrorInfo{
                    eph::core::Error::ConnectFailed, "synthetic-fail"});
            }
            auto s = ent::FakeStream::create();
            s->set_state(eph::net::TcpState::Established);
            s->set_attached(true);
            return s;
        },
    };

    std::vector<en::ReconnectEvent> events;
    orch.set_on_reconnect_event([&](const en::ReconnectEvent& ev) {
        events.push_back(ev);
    });

    auto vt = eph::utils::TSC::now();
    ASSERT_TRUE(orch.start(vt).has_value());
    EXPECT_EQ(orch.state(), en::ReconnectState::Backoff);

    // Drive forward until success.
    const auto step = eph::utils::TSC::to_cycles(1s).value_or(3'000'000'000ULL);
    for (int i = 0; i < 5 && orch.state() != en::ReconnectState::Connected; ++i) {
        vt += step;
        orch.tick(vt);
    }
    ASSERT_EQ(orch.state(), en::ReconnectState::Connected);

    // Expected sequence:
    //   AttemptStarted, AttemptFailed, BackoffScheduled,   // attempt 1 fails
    //   AttemptStarted, AttemptFailed, BackoffScheduled,   // attempt 2 fails
    //   AttemptStarted, Connected                          // attempt 3 wins
    // (8 events)
    ASSERT_EQ(events.size(), 8u);
    EXPECT_EQ(events[0].kind, en::ReconnectEventKind::AttemptStarted);
    EXPECT_EQ(events[1].kind, en::ReconnectEventKind::AttemptFailed);
    EXPECT_EQ(events[1].error.code, eph::core::Error::ConnectFailed);
    EXPECT_STREQ(events[1].error.detail, "synthetic-fail");
    EXPECT_EQ(events[2].kind, en::ReconnectEventKind::BackoffScheduled);
    EXPECT_EQ(events[3].kind, en::ReconnectEventKind::AttemptStarted);
    EXPECT_EQ(events[4].kind, en::ReconnectEventKind::AttemptFailed);
    EXPECT_EQ(events[4].error.code, eph::core::Error::ConnectFailed);
    EXPECT_STREQ(events[4].error.detail, "synthetic-fail");
    EXPECT_EQ(events[5].kind, en::ReconnectEventKind::BackoffScheduled);
    EXPECT_EQ(events[6].kind, en::ReconnectEventKind::AttemptStarted);
    EXPECT_EQ(events[7].kind, en::ReconnectEventKind::Connected);

    // Connected event's duration_ns should be non-zero (real cycles elapsed
    // between start() and the third successful attempt).
    EXPECT_GT(events[7].duration_ns, 0u);

    // Non-failure events carry the sentinel {Ok, ""}.
    EXPECT_EQ(events[0].error.code, eph::core::Error::Ok);
    EXPECT_EQ(events[2].error.code, eph::core::Error::Ok);
    EXPECT_EQ(events[7].error.code, eph::core::Error::Ok);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 13 (T2.14): FailedEventOnExhaustion — max_attempts=2 with a factory
// that always fails; the terminal event must be `Failed`.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(ReconnectOrchestratorTest, FailedEventOnExhaustion) {
    auto cfg = kDeterministicConfig();
    cfg.policy.max_attempts = 2;
    Orch orch{
        cfg,
        []() -> std::expected<StreamPtr, eph::core::ErrorInfo> {
            return std::unexpected(eph::core::ErrorInfo{
                eph::core::Error::ConnectFailed, "always-fail"});
        },
    };

    std::vector<en::ReconnectEventKind> kinds;
    orch.set_on_reconnect_event([&](const en::ReconnectEvent& ev) {
        kinds.push_back(ev.kind);
    });

    auto vt = eph::utils::TSC::now();
    ASSERT_TRUE(orch.start(vt).has_value());
    const auto step = eph::utils::TSC::to_cycles(1s).value_or(3'000'000'000ULL);
    for (int i = 0; i < 5 && orch.state() != en::ReconnectState::Failed; ++i) {
        vt += step;
        orch.tick(vt);
    }
    ASSERT_EQ(orch.state(), en::ReconnectState::Failed);

    ASSERT_FALSE(kinds.empty());
    EXPECT_EQ(kinds.back(), en::ReconnectEventKind::Failed);
    // At least one AttemptFailed should appear before the terminal Failed.
    bool saw_attempt_failed = false;
    for (auto k : kinds) {
        if (k == en::ReconnectEventKind::AttemptFailed) saw_attempt_failed = true;
    }
    EXPECT_TRUE(saw_attempt_failed);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 14 (T2.14): NoEventCallbackIsOk — without any event hook installed,
// the state machine continues to function. Acts as a regression check that
// the new emit_event_ helper short-circuits cleanly when on_event_ is empty.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(ReconnectOrchestratorTest, NoEventCallbackIsOk) {
    auto cfg = kDeterministicConfig();
    Orch orch{
        cfg,
        []() -> std::expected<StreamPtr, eph::core::ErrorInfo> {
            auto s = ent::FakeStream::create();
            s->set_state(eph::net::TcpState::Established);
            s->set_attached(true);
            return s;
        },
    };
    // Deliberately do NOT call set_on_reconnect_event.

    ASSERT_TRUE(orch.start(eph::utils::TSC::now()).has_value());
    EXPECT_EQ(orch.state(), en::ReconnectState::Connected);

    orch.mark_disconnected({eph::core::Error::Disconnected, "x"});
    EXPECT_EQ(orch.state(), en::ReconnectState::Backoff);

    auto t = eph::utils::TSC::now()
             + eph::utils::TSC::to_cycles(60ms).value_or(180'000'000ULL);
    orch.tick(t);
    EXPECT_EQ(orch.state(), en::ReconnectState::Connected);
    EXPECT_EQ(orch.reconnect_count(), 2u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 15 (T2.14): EventDurationMatchesMetric — the Connected event's
// duration_ns matches `last_reconnect_duration_ns()` exactly. Both should
// reflect the same TSC-derived ns delta.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(ReconnectOrchestratorTest, EventDurationMatchesMetric) {
    auto cfg = kDeterministicConfig();
    Orch orch{
        cfg,
        []() -> std::expected<StreamPtr, eph::core::ErrorInfo> {
            auto s = ent::FakeStream::create();
            s->set_state(eph::net::TcpState::Established);
            s->set_attached(true);
            return s;
        },
    };

    uint64_t last_connected_duration_ns = 0;
    orch.set_on_reconnect_event([&](const en::ReconnectEvent& ev) {
        if (ev.kind == en::ReconnectEventKind::Connected) {
            last_connected_duration_ns = ev.duration_ns;
        }
    });

    ASSERT_TRUE(orch.start(eph::utils::TSC::now()).has_value());
    ASSERT_EQ(orch.state(), en::ReconnectState::Connected);
    // Initial start: disconnect_tsc_ == start tsc → duration is ~0.
    EXPECT_EQ(last_connected_duration_ns, orch.last_reconnect_duration_ns());

    // Now trigger a real reconnect with a measurable gap.
    orch.mark_disconnected({eph::core::Error::Disconnected, "x"});
    auto t = eph::utils::TSC::now()
             + eph::utils::TSC::to_cycles(100ms).value_or(300'000'000ULL);
    orch.tick(t);
    ASSERT_EQ(orch.state(), en::ReconnectState::Connected);

    // Both must match exactly — they're computed from the same ns_opt value
    // inside try_attempt_().
    EXPECT_EQ(last_connected_duration_ns, orch.last_reconnect_duration_ns());
    EXPECT_GT(last_connected_duration_ns, 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 16: StopHaltsTickProgress — stop() freezes the state machine even if
// the deadline elapses.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(ReconnectOrchestratorTest, StopHaltsTickProgress) {
    auto cfg = kDeterministicConfig();
    int factory_calls = 0;
    Orch orch{
        cfg,
        [&]() -> std::expected<StreamPtr, eph::core::ErrorInfo> {
            ++factory_calls;
            auto s = ent::FakeStream::create();
            s->set_state(eph::net::TcpState::Established);
            s->set_attached(true);
            return s;
        },
    };

    ASSERT_TRUE(orch.start(eph::utils::TSC::now()).has_value());
    orch.mark_disconnected({eph::core::Error::Disconnected, "x"});
    EXPECT_EQ(orch.state(), en::ReconnectState::Backoff);

    orch.stop();

    auto t = eph::utils::TSC::now()
             + eph::utils::TSC::to_cycles(1s).value_or(3'000'000'000ULL);
    orch.tick(t);
    // Still Backoff (not promoted to Connected) because stop() short-circuits.
    EXPECT_EQ(orch.state(), en::ReconnectState::Backoff);
    EXPECT_EQ(factory_calls, 1);  // only the initial start() call
}
