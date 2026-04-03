#include <gtest/gtest.h>

#include "eph/net/kill_switch.hpp"

#include <atomic>
#include <thread>

using namespace eph::net;

// Mock transport for testing
struct MockTransport {
    std::atomic<bool> running{true};
    std::atomic<int> stop_count{0};

    void stop() noexcept {
        running.store(false, std::memory_order_release);
        stop_count.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] bool is_running() const noexcept {
        return running.load(std::memory_order_acquire);
    }
};

// Verify the Stoppable concept is satisfied by MockTransport
static_assert(Stoppable<MockTransport>,
    "MockTransport must satisfy the Stoppable concept");

// Verify non-conforming types are correctly rejected
struct NotStoppable { int x; };
static_assert(!Stoppable<NotStoppable>,
    "NotStoppable must NOT satisfy the Stoppable concept");

struct MissingIsRunning { void stop() {} };
static_assert(!Stoppable<MissingIsRunning>,
    "MissingIsRunning (no is_running()) must NOT satisfy Stoppable");

struct MissingStop { bool is_running() const { return false; } };
static_assert(!Stoppable<MissingStop>,
    "MissingStop (no stop()) must NOT satisfy Stoppable");

TEST(KillSwitch, InitialState) {
    KillSwitch ks;
    EXPECT_FALSE(ks.is_shutdown_requested());
    EXPECT_EQ(ks.transport_count(), 0);
}

TEST(KillSwitch, RegisterTransport) {
    KillSwitch ks;
    MockTransport tp;
    EXPECT_TRUE(ks.register_transport(&tp));
    EXPECT_EQ(ks.transport_count(), 1);
}

TEST(KillSwitch, RegisterNullptrRejected) {
    KillSwitch ks;
    EXPECT_FALSE(ks.register_transport<MockTransport>(nullptr));
    EXPECT_EQ(ks.transport_count(), 0);
}

TEST(KillSwitch, UnregisterTransport) {
    KillSwitch ks;
    MockTransport tp;
    ASSERT_TRUE(ks.register_transport(&tp));
    EXPECT_EQ(ks.transport_count(), 1);
    ks.unregister_transport(&tp);
    EXPECT_EQ(ks.transport_count(), 0);
}

TEST(KillSwitch, UnregisterNonexistentIsNoop) {
    KillSwitch ks;
    MockTransport tp1, tp2;
    ASSERT_TRUE(ks.register_transport(&tp1));
    ks.unregister_transport(&tp2);  // not registered
    EXPECT_EQ(ks.transport_count(), 1);
}

TEST(KillSwitch, RegisterMultipleTransports) {
    KillSwitch ks;
    MockTransport tp1, tp2, tp3;
    EXPECT_TRUE(ks.register_transport(&tp1));
    EXPECT_TRUE(ks.register_transport(&tp2));
    EXPECT_TRUE(ks.register_transport(&tp3));
    EXPECT_EQ(ks.transport_count(), 3);
}

TEST(KillSwitch, RegisterMaxTransports) {
    KillSwitch ks;
    std::array<MockTransport, kKillSwitchMaxTransports + 1> transports;
    for (size_t i = 0; i < kKillSwitchMaxTransports; ++i) {
        EXPECT_TRUE(ks.register_transport(&transports[i]));
    }
    EXPECT_EQ(ks.transport_count(), kKillSwitchMaxTransports);
    // One more should fail
    EXPECT_FALSE(ks.register_transport(&transports[kKillSwitchMaxTransports]));
}

TEST(KillSwitch, RequestShutdownSetsFlag) {
    KillSwitch ks;
    EXPECT_FALSE(ks.is_shutdown_requested());
    ks.request_shutdown();
    EXPECT_TRUE(ks.is_shutdown_requested());
}

TEST(KillSwitch, ShutdownStopsAllTransports) {
    KillSwitch ks;
    MockTransport tp1, tp2, tp3;
    ASSERT_TRUE(ks.register_transport(&tp1));
    ASSERT_TRUE(ks.register_transport(&tp2));
    ASSERT_TRUE(ks.register_transport(&tp3));

    EXPECT_TRUE(tp1.is_running());
    EXPECT_TRUE(tp2.is_running());
    EXPECT_TRUE(tp3.is_running());

    ks.shutdown();

    EXPECT_FALSE(tp1.is_running());
    EXPECT_FALSE(tp2.is_running());
    EXPECT_FALSE(tp3.is_running());
    EXPECT_TRUE(ks.is_shutdown_requested());
}

TEST(KillSwitch, ShutdownIsIdempotent) {
    KillSwitch ks;
    MockTransport tp;
    ASSERT_TRUE(ks.register_transport(&tp));

    ks.shutdown();
    EXPECT_EQ(tp.stop_count.load(), 1);

    ks.shutdown();  // second call should be no-op
    EXPECT_EQ(tp.stop_count.load(), 1);
}

TEST(KillSwitch, ShutdownSkipsAlreadyStoppedTransports) {
    KillSwitch ks;
    MockTransport tp1, tp2;
    tp1.running.store(false);  // pre-stopped

    ASSERT_TRUE(ks.register_transport(&tp1));
    ASSERT_TRUE(ks.register_transport(&tp2));

    ks.shutdown();

    EXPECT_EQ(tp1.stop_count.load(), 0);  // was already stopped
    EXPECT_EQ(tp2.stop_count.load(), 1);
}

TEST(KillSwitch, KillSetsRequestWithoutBlocking) {
    KillSwitch ks;
    MockTransport tp;
    ASSERT_TRUE(ks.register_transport(&tp));

    ks.kill();  // non-blocking emergency kill

    EXPECT_TRUE(ks.is_shutdown_requested());
    // Transport is NOT stopped — kill() doesn't call stop()
    EXPECT_TRUE(tp.is_running());
}

TEST(KillSwitch, DestructorCallsShutdown) {
    MockTransport tp;
    {
        KillSwitch ks;
        ASSERT_TRUE(ks.register_transport(&tp));
        EXPECT_TRUE(tp.is_running());
    }  // destructor calls shutdown()
    EXPECT_FALSE(tp.is_running());
}

TEST(KillSwitch, ConcurrentRegistration) {
    KillSwitch ks;
    constexpr int N = 16;
    std::array<MockTransport, N> transports;
    std::vector<std::thread> threads;

    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&ks, &transports, i] {
            [[maybe_unused]] bool ok = ks.register_transport(&transports[i]);
        });
    }
    for (auto& t : threads) t.join();

    EXPECT_EQ(ks.transport_count(), N);
    ks.shutdown();
    for (int i = 0; i < N; ++i) {
        EXPECT_FALSE(transports[i].is_running());
    }
}

TEST(KillSwitch, UnregisterMiddleElementPreservesOthers) {
    KillSwitch ks;
    MockTransport tp1, tp2, tp3;
    ASSERT_TRUE(ks.register_transport(&tp1));
    ASSERT_TRUE(ks.register_transport(&tp2));
    ASSERT_TRUE(ks.register_transport(&tp3));

    ks.unregister_transport(&tp2);
    EXPECT_EQ(ks.transport_count(), 2);

    ks.shutdown();
    EXPECT_FALSE(tp1.is_running());
    EXPECT_TRUE(tp2.is_running());  // was unregistered
    EXPECT_FALSE(tp3.is_running());
}

TEST(KillSwitch, ShutdownContinuesAfterStopThrows) {
    // A transport whose stop() throws must not prevent other transports
    // from being stopped -- KillSwitch catches exceptions and continues.
    struct ThrowingTransport {
        std::atomic<bool> running{true};
        void stop() { throw std::runtime_error("stop failed"); }
        bool is_running() const noexcept { return running.load(); }
    };

    KillSwitch ks;
    ThrowingTransport tp_throw;
    MockTransport tp_normal;

    ASSERT_TRUE(ks.register_transport(&tp_throw));
    ASSERT_TRUE(ks.register_transport(&tp_normal));

    // shutdown() should not throw even though tp_throw.stop() does
    ks.shutdown();

    // tp_normal should still have been stopped despite tp_throw's exception
    EXPECT_FALSE(tp_normal.is_running());
    EXPECT_TRUE(ks.is_shutdown_requested());
}

TEST(KillSwitch, UnregisterNullIsNoop) {
    KillSwitch ks;
    ks.unregister_transport<MockTransport>(nullptr);
    EXPECT_EQ(ks.transport_count(), 0);
}

TEST(KillSwitch, ResetClearsShutdownState) {
    KillSwitch ks;
    ks.request_shutdown();
    EXPECT_TRUE(ks.is_shutdown_requested());
    ks.reset();
    EXPECT_FALSE(ks.is_shutdown_requested());
}

TEST(KillSwitch, ResetAllowsReShutdown) {
    KillSwitch ks;
    MockTransport tp1;
    ASSERT_TRUE(ks.register_transport(&tp1));
    ks.shutdown();
    EXPECT_FALSE(tp1.is_running());
    EXPECT_EQ(tp1.stop_count.load(), 1);

    // Reset and register a new transport
    ks.reset();
    MockTransport tp2;
    ASSERT_TRUE(ks.register_transport(&tp2));
    ks.shutdown();
    EXPECT_FALSE(tp2.is_running());
    EXPECT_EQ(tp2.stop_count.load(), 1);
}

TEST(KillSwitch, ResetIsIdempotent) {
    KillSwitch ks;
    ks.reset();  // no-op on fresh instance
    ks.reset();
    EXPECT_FALSE(ks.is_shutdown_requested());
}

// ── to_json() ────────────────────────────────────────────────────────────

TEST(KillSwitch, ToJsonInitialState) {
    KillSwitch ks;
    auto j = ks.to_json();
    EXPECT_EQ(j.front(), '{');
    EXPECT_EQ(j.back(), '}');
    EXPECT_NE(j.find("\"transport_count\":0"), std::string::npos);
    EXPECT_NE(j.find("\"shutdown_requested\":false"), std::string::npos);
    EXPECT_NE(j.find("\"shutdown_done\":false"), std::string::npos);
}

TEST(KillSwitch, ToJsonAfterRegister) {
    KillSwitch ks;
    MockTransport tp1, tp2;
    ASSERT_TRUE(ks.register_transport(&tp1));
    ASSERT_TRUE(ks.register_transport(&tp2));
    auto j = ks.to_json();
    EXPECT_NE(j.find("\"transport_count\":2"), std::string::npos);
}

TEST(KillSwitch, ToJsonAfterShutdown) {
    KillSwitch ks;
    MockTransport tp;
    ASSERT_TRUE(ks.register_transport(&tp));
    ks.shutdown();
    auto j = ks.to_json();
    EXPECT_NE(j.find("\"shutdown_requested\":true"), std::string::npos);
    EXPECT_NE(j.find("\"shutdown_done\":true"), std::string::npos);
}

TEST(KillSwitch, ToJsonSignalHandlersInstalled) {
    KillSwitch ks;
    auto j_before = ks.to_json();
    EXPECT_NE(j_before.find("\"signal_handlers_installed\":false"), std::string::npos);

    ks.install_signal_handlers();
    auto j_after = ks.to_json();
    EXPECT_NE(j_after.find("\"signal_handlers_installed\":true"), std::string::npos);
}

// ── Concurrent unregister during shutdown ───────────────────────────────────

TEST(KillSwitch, ConcurrentUnregisterDuringShutdown) {
    // Verify that unregistering transports from one thread while another
    // thread runs shutdown() does not crash or corrupt state.
    constexpr int N = 16;
    std::array<MockTransport, N> transports;

    KillSwitch ks;
    for (int i = 0; i < N; ++i) {
        ASSERT_TRUE(ks.register_transport(&transports[i]));
    }

    std::thread t1([&] { ks.shutdown(); });
    std::thread t2([&] {
        // Unregister even-indexed transports concurrently with shutdown
        for (int i = 0; i < N; i += 2) {
            ks.unregister_transport(&transports[i]);
        }
    });

    t1.join();
    t2.join();

    // After both threads complete, the system should be in a consistent state.
    EXPECT_TRUE(ks.is_shutdown_requested());
}

TEST(KillSwitch, ConcurrentRegisterAndShutdown) {
    // Register transports on one thread while shutdown runs on another.
    KillSwitch ks;
    constexpr int N = 16;
    std::array<MockTransport, N> transports;

    std::thread t1([&] {
        for (int i = 0; i < N; ++i) {
            ks.register_transport(&transports[i]);
        }
    });
    std::thread t2([&] { ks.shutdown(); });

    t1.join();
    t2.join();

    EXPECT_TRUE(ks.is_shutdown_requested());
}

TEST(KillSwitch, ShutdownAfterResetAndReRegister) {
    // Test the full cycle: register -> shutdown -> reset -> re-register -> shutdown
    KillSwitch ks;
    MockTransport tp1, tp2;
    ASSERT_TRUE(ks.register_transport(&tp1));

    ks.shutdown();
    EXPECT_FALSE(tp1.is_running());
    EXPECT_EQ(tp1.stop_count.load(), 1);

    // Reset and do it again with a different transport
    ks.reset();
    tp2.running.store(true);
    ASSERT_TRUE(ks.register_transport(&tp2));

    ks.shutdown();
    EXPECT_FALSE(tp2.is_running());
    EXPECT_EQ(tp2.stop_count.load(), 1);
    // tp1 was not re-registered, so its stop count should not change
    EXPECT_EQ(tp1.stop_count.load(), 1);
}

// ── running_count() ──────────────────────────────────────────────────────

TEST(KillSwitch, RunningCountReflectsState) {
    KillSwitch ks;
    MockTransport tp1, tp2, tp3;
    tp1.running.store(true);
    tp2.running.store(true);
    tp3.running.store(false);

    ASSERT_TRUE(ks.register_transport(&tp1));
    ASSERT_TRUE(ks.register_transport(&tp2));
    ASSERT_TRUE(ks.register_transport(&tp3));

    EXPECT_EQ(ks.running_count(), 2);
    EXPECT_EQ(ks.transport_count(), 3);
}

TEST(KillSwitch, RunningCountZeroAfterShutdown) {
    KillSwitch ks;
    MockTransport tp1, tp2;
    tp1.running.store(true);
    tp2.running.store(true);

    ASSERT_TRUE(ks.register_transport(&tp1));
    ASSERT_TRUE(ks.register_transport(&tp2));

    EXPECT_EQ(ks.running_count(), 2);
    ks.shutdown();
    EXPECT_EQ(ks.running_count(), 0);
}

TEST(KillSwitch, RunningCountZeroWhenEmpty) {
    KillSwitch ks;
    EXPECT_EQ(ks.running_count(), 0);
}

TEST(KillSwitch, KillThenShutdownIsIdempotent) {
    // kill() sets the flag; a subsequent shutdown() should still be a no-op
    // because shutdown_done_ is already set... wait, kill() does NOT set
    // shutdown_done_. So shutdown() after kill() should execute.
    KillSwitch ks;
    MockTransport tp;
    tp.running.store(true);
    ASSERT_TRUE(ks.register_transport(&tp));

    ks.kill(); // sets shutdown_requested but not shutdown_done
    EXPECT_TRUE(ks.is_shutdown_requested());
    EXPECT_TRUE(tp.is_running()); // kill() doesn't call stop()

    ks.shutdown(); // should now stop the transport
    EXPECT_FALSE(tp.is_running());
    EXPECT_EQ(tp.stop_count.load(), 1);
}
