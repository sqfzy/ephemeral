#include <gtest/gtest.h>

#include "eph/net/gateway.hpp"

#include <atomic>
#include <format>
#include <string>
#include <thread>
#include <vector>

using namespace eph::net;

// Mock transport for testing
struct MockTransport {
    std::atomic<bool> running{false};
    std::atomic<int> start_count{0};
    std::atomic<int> stop_count{0};
    std::atomic<int> reconnect_count{0};

    void start_threads() noexcept {
        running.store(true, std::memory_order_release);
        start_count.fetch_add(1, std::memory_order_relaxed);
    }

    void stop() noexcept {
        running.store(false, std::memory_order_release);
        stop_count.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] bool is_running() const noexcept {
        return running.load(std::memory_order_acquire);
    }

    void reconnect_now() noexcept {
        reconnect_count.fetch_add(1, std::memory_order_relaxed);
    }
};

TEST(Gateway, InitiallyEmpty) {
    Gateway gw;
    EXPECT_EQ(gw.connection_count(), 0);
}

TEST(Gateway, AddConnection) {
    Gateway gw;
    MockTransport tp;
    auto id = gw.add("test-conn", &tp);
    EXPECT_EQ(id, 0);
    EXPECT_EQ(gw.connection_count(), 1);
    EXPECT_EQ(gw.tag(id), "test-conn");
}

TEST(Gateway, AddMultipleConnections) {
    Gateway gw;
    MockTransport tp1, tp2, tp3;
    auto id1 = gw.add("binance-btc", &tp1, 1);
    auto id2 = gw.add("binance-eth", &tp2, 2);
    auto id3 = gw.add("deribit-btc", &tp3, 3);
    EXPECT_EQ(gw.connection_count(), 3);
    EXPECT_EQ(id1, 0);
    EXPECT_EQ(id2, 1);
    EXPECT_EQ(id3, 2);
}

TEST(Gateway, AddNullTransportReturnsMaxSize) {
    Gateway gw;
    auto id = gw.add<MockTransport>("null", nullptr);
    EXPECT_EQ(id, SIZE_MAX);
    EXPECT_EQ(gw.connection_count(), 0);
}

TEST(Gateway, StartAllStartsStoppedConnections) {
    Gateway gw;
    MockTransport tp1, tp2;
    gw.add("t1", &tp1);
    gw.add("t2", &tp2);

    EXPECT_FALSE(tp1.is_running());
    EXPECT_FALSE(tp2.is_running());

    gw.start_all();

    EXPECT_TRUE(tp1.is_running());
    EXPECT_TRUE(tp2.is_running());
    EXPECT_EQ(tp1.start_count.load(), 1);
    EXPECT_EQ(tp2.start_count.load(), 1);
}

TEST(Gateway, StopAllStopsRunningConnections) {
    Gateway gw;
    MockTransport tp1, tp2;
    tp1.running.store(true);
    tp2.running.store(true);

    gw.add("t1", &tp1);
    gw.add("t2", &tp2);

    gw.stop_all();

    EXPECT_FALSE(tp1.is_running());
    EXPECT_FALSE(tp2.is_running());
}

TEST(Gateway, StopAllSkipsAlreadyStopped) {
    Gateway gw;
    MockTransport tp;
    gw.add("t1", &tp);
    // tp is stopped initially — stop_all should be a no-op
    gw.stop_all();
    EXPECT_EQ(tp.stop_count.load(), 0);
}

TEST(Gateway, ReconnectSpecificConnection) {
    Gateway gw;
    MockTransport tp1, tp2;
    tp1.running.store(true);
    tp2.running.store(true);

    gw.add("t1", &tp1);
    gw.add("t2", &tp2);

    gw.reconnect(0);
    EXPECT_EQ(tp1.reconnect_count.load(), 1);
    EXPECT_EQ(tp2.reconnect_count.load(), 0);
}

TEST(Gateway, ReconnectInvalidIdIsNoop) {
    Gateway gw;
    gw.reconnect(999);  // should not crash
}

TEST(Gateway, HealthInitiallyStoppedForNewConnections) {
    Gateway gw;
    MockTransport tp;
    auto id = gw.add("t1", &tp);
    EXPECT_EQ(gw.health(id), ConnHealth::Stopped);
}

TEST(Gateway, HealthReflectsRunningState) {
    Gateway gw;
    MockTransport tp;
    tp.running.store(true);
    auto id = gw.add("t1", &tp);
    // After add with running=true, health should be Healthy
    EXPECT_EQ(gw.health(id), ConnHealth::Healthy);
}

TEST(Gateway, CheckHealthDetectsDisconnection) {
    Gateway gw;
    MockTransport tp;
    tp.running.store(true);
    auto id = gw.add("t1", &tp);

    // Simulate disconnection
    tp.running.store(false);
    gw.check_health();

    EXPECT_EQ(gw.health(id), ConnHealth::Disconnected);
}

TEST(Gateway, CheckHealthDetectsReconnection) {
    Gateway gw;
    MockTransport tp;
    auto id = gw.add("t1", &tp);
    gw.start_all();

    // Disconnect then reconnect
    tp.running.store(false);
    gw.check_health();
    EXPECT_EQ(gw.health(id), ConnHealth::Disconnected);

    tp.running.store(true);
    gw.check_health();
    EXPECT_EQ(gw.health(id), ConnHealth::Healthy);
}

TEST(Gateway, HealthChangeCallback) {
    int callback_count = 0;
    std::string last_tag;
    ConnHealth last_old{}, last_new{};

    Gateway gw({
        .on_health_change = [&](std::string_view tag, ConnHealth old_h, ConnHealth new_h) {
            ++callback_count;
            last_tag = std::string(tag);
            last_old = old_h;
            last_new = new_h;
        },
    });

    MockTransport tp;
    tp.running.store(true);
    gw.add("exchange-1", &tp);

    tp.running.store(false);
    gw.check_health();

    EXPECT_EQ(callback_count, 1);
    EXPECT_EQ(last_tag, "exchange-1");
    EXPECT_EQ(last_old, ConnHealth::Healthy);
    EXPECT_EQ(last_new, ConnHealth::Disconnected);
}

TEST(Gateway, DumpProducesOutput) {
    Gateway gw;
    MockTransport tp1, tp2;
    tp1.running.store(true);
    gw.add("binance-btc", &tp1, 1);
    gw.add("deribit-eth", &tp2, 5);

    std::string output = gw.dump();
    EXPECT_NE(output.find("binance-btc"), std::string::npos);
    EXPECT_NE(output.find("deribit-eth"), std::string::npos);
    EXPECT_NE(output.find("HEALTHY"), std::string::npos);
    EXPECT_NE(output.find("STOPPED"), std::string::npos);
}

TEST(Gateway, DestructorStopsAll) {
    MockTransport tp;
    tp.running.store(true);
    {
        Gateway gw;
        gw.add("t1", &tp);
    }  // destructor should stop
    EXPECT_FALSE(tp.is_running());
}

TEST(Gateway, MonitorStartStop) {
    Gateway gw({.health_check_interval = std::chrono::milliseconds{50}});
    MockTransport tp;
    tp.running.store(true);
    gw.add("t1", &tp);

    gw.start_monitor();
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    gw.stop_monitor();
    // Should not crash or hang
}

TEST(Gateway, HealthOutOfBoundsReturnsStopped) {
    Gateway gw;
    EXPECT_EQ(gw.health(999), ConnHealth::Stopped);
}

TEST(Gateway, TagOutOfBoundsReturnsEmpty) {
    Gateway gw;
    EXPECT_EQ(gw.tag(999), "");
}

TEST(Gateway, StartAllSkipsAlreadyRunning) {
    Gateway gw;
    MockTransport tp;
    tp.running.store(true);  // pre-started externally
    gw.add("t1", &tp);

    // stop_all first to set health to Stopped, then start_all
    gw.stop_all();
    tp.running.store(true);  // simulate external start before gateway's start
    int before = tp.start_count.load();
    gw.start_all();  // should detect running and skip
    // start_threads_fn should NOT be called since transport is already running
    EXPECT_EQ(tp.start_count.load(), before);
}

TEST(Gateway, StopAllHandlesThrowingStopFn) {
    // A transport whose stop() throws should not prevent other transports
    // from being stopped (Gateway catches exceptions in stop_all).
    struct ThrowingTransport {
        std::atomic<bool> running{true};
        void start_threads() noexcept { running = true; }
        void stop() { throw std::runtime_error("stop failed"); }
        bool is_running() const noexcept { return running.load(); }
        void reconnect_now() noexcept {}
    };

    Gateway gw;
    ThrowingTransport tp1;
    MockTransport tp2;
    tp2.running.store(true);

    gw.add("throwing", &tp1);
    gw.add("normal", &tp2);

    // Should not throw; tp2 should still be stopped
    gw.stop_all();
    EXPECT_FALSE(tp2.is_running());
}

TEST(Gateway, CheckHealthSkipsStoppedConnections) {
    Gateway gw;
    MockTransport tp;
    gw.add("t1", &tp);

    // Health starts as Stopped; check_health should leave it as Stopped
    gw.check_health();
    EXPECT_EQ(gw.health(0), ConnHealth::Stopped);
}

TEST(Gateway, StartAllVerifiesActualStart) {
    // Transport that fails to start (start_threads called but running stays false)
    struct FailStartTransport {
        void start_threads() noexcept { /* deliberately does not set running */ }
        void stop() noexcept {}
        bool is_running() const noexcept { return false; }
        void reconnect_now() noexcept {}
    };

    Gateway gw;
    FailStartTransport tp;
    gw.add("fail-start", &tp);

    gw.start_all();
    // Should be Disconnected since is_running() returns false after start attempt
    EXPECT_EQ(gw.health(0), ConnHealth::Disconnected);
}

TEST(ConnHealth, NameCoversAllValues) {
    EXPECT_EQ(conn_health_name(ConnHealth::Healthy), "HEALTHY");
    EXPECT_EQ(conn_health_name(ConnHealth::Degraded), "DEGRADED");
    EXPECT_EQ(conn_health_name(ConnHealth::Disconnected), "DISCONNECTED");
    EXPECT_EQ(conn_health_name(ConnHealth::Stopped), "STOPPED");
}

TEST(ConnHealth, FormatterProducesOutput) {
    EXPECT_EQ(std::format("{}", ConnHealth::Healthy), "HEALTHY");
    EXPECT_EQ(std::format("{}", ConnHealth::Degraded), "DEGRADED");
    EXPECT_EQ(std::format("{}", ConnHealth::Disconnected), "DISCONNECTED");
    EXPECT_EQ(std::format("{}", ConnHealth::Stopped), "STOPPED");
}

TEST(ConnHealth, FormatterWorksInCompositeFormat) {
    auto s = std::format("health={} id={}", ConnHealth::Healthy, 42);
    EXPECT_EQ(s, "health=HEALTHY id=42");
}

// ── Gateway::Config::validate() ─────────────────────────────────────────────

TEST(GatewayConfig, DefaultConfigIsValid) {
    Gateway::Config cfg;
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(GatewayConfig, NegativeHealthCheckIntervalIsInvalid) {
    Gateway::Config cfg;
    cfg.health_check_interval = std::chrono::milliseconds{-1};
    EXPECT_FALSE(cfg.validate().empty());
    EXPECT_NE(cfg.validate().find("health_check_interval"), std::string_view::npos);
}

TEST(GatewayConfig, ZeroHealthCheckIntervalIsValid) {
    // 0 = disabled monitoring
    Gateway::Config cfg;
    cfg.health_check_interval = std::chrono::milliseconds{0};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(GatewayConfig, NegativeDegradedThresholdIsInvalid) {
    Gateway::Config cfg;
    cfg.degraded_threshold = std::chrono::milliseconds{-1};
    EXPECT_FALSE(cfg.validate().empty());
    EXPECT_NE(cfg.validate().find("degraded_threshold"), std::string_view::npos);
}

TEST(GatewayConfig, DegradedThresholdMustExceedHealthCheckInterval) {
    Gateway::Config cfg;
    cfg.health_check_interval = std::chrono::milliseconds{5000};
    cfg.degraded_threshold = std::chrono::milliseconds{5000};  // equal, not greater
    EXPECT_FALSE(cfg.validate().empty());
    EXPECT_NE(cfg.validate().find("degraded_threshold must be greater"), std::string_view::npos);
}

TEST(GatewayConfig, DegradedThresholdGreaterThanIntervalIsValid) {
    Gateway::Config cfg;
    cfg.health_check_interval = std::chrono::milliseconds{5000};
    cfg.degraded_threshold = std::chrono::milliseconds{5001};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(GatewayConfig, ZeroDegradedThresholdSkipsIntervalCheck) {
    // 0 degraded_threshold means disabled, so no comparison with interval needed
    Gateway::Config cfg;
    cfg.health_check_interval = std::chrono::milliseconds{5000};
    cfg.degraded_threshold = std::chrono::milliseconds{0};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(GatewayConfig, BothZeroIsValid) {
    Gateway::Config cfg;
    cfg.health_check_interval = std::chrono::milliseconds{0};
    cfg.degraded_threshold = std::chrono::milliseconds{0};
    EXPECT_TRUE(cfg.validate().empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// Gateway::Config dump, to_json, formatter
// ─────────────────────────────────────────────────────────────────────────────

TEST(GatewayConfig, DumpContainsKeyFields) {
    Gateway::Config cfg;
    cfg.health_check_interval = std::chrono::milliseconds{3000};
    cfg.degraded_threshold = std::chrono::milliseconds{15000};
    auto d = cfg.dump();
    EXPECT_NE(d.find("3000ms"), std::string::npos);
    EXPECT_NE(d.find("15000ms"), std::string::npos);
    EXPECT_NE(d.find("unset"), std::string::npos); // no callback set
}

TEST(GatewayConfig, DumpShowsCallbackSet) {
    Gateway::Config cfg;
    cfg.on_health_change = [](std::string_view, ConnHealth, ConnHealth) {};
    auto d = cfg.dump();
    EXPECT_NE(d.find("set"), std::string::npos);
}

TEST(GatewayConfig, ToJsonValidStructure) {
    Gateway::Config cfg;
    cfg.health_check_interval = std::chrono::milliseconds{5000};
    cfg.degraded_threshold = std::chrono::milliseconds{30000};
    auto j = cfg.to_json();
    EXPECT_EQ(j.front(), '{');
    EXPECT_EQ(j.back(), '}');
    EXPECT_NE(j.find("\"health_check_interval_ms\":5000"), std::string::npos);
    EXPECT_NE(j.find("\"degraded_threshold_ms\":30000"), std::string::npos);
}

TEST(GatewayConfig, FormatterContainsKeyFields) {
    Gateway::Config cfg;
    cfg.health_check_interval = std::chrono::milliseconds{5000};
    cfg.degraded_threshold = std::chrono::milliseconds{30000};
    auto s = std::format("{}", cfg);
    EXPECT_NE(s.find("Gateway"), std::string::npos);
    EXPECT_NE(s.find("health=5000ms"), std::string::npos);
    EXPECT_NE(s.find("degraded=30000ms"), std::string::npos);
}
