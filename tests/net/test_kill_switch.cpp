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
