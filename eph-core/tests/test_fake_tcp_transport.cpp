/// @file test_fake_tcp_transport.cpp
/// Unit tests for FakeTcpTransport mock class.

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "eph/core/fake_tcp_transport.hpp"

using namespace eph::net;
using namespace eph::net::testing;
using namespace std::chrono_literals;

// ============================================================================
// Concept satisfaction (compile-time)
// ============================================================================

static_assert(TcpTransport<FakeTcpTransport>,
    "FakeTcpTransport must satisfy TcpTransport concept");

// ============================================================================
// connect()
// ============================================================================

TEST(FakeTcpTransport, ConnectSucceedsByDefault) {
    FakeTcpTransport t;
    auto result = t.connect(100ms);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(t.state(), TcpState::Established);
    EXPECT_TRUE(t.is_established());
}

TEST(FakeTcpTransport, ConnectReturnsErrorWhenSet) {
    FakeTcpTransport t;
    t.set_connect_error("connection refused");
    auto result = t.connect(100ms);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "connection refused");
    // State should remain Closed on failure.
    EXPECT_EQ(t.state(), TcpState::Closed);
}

// ============================================================================
// send()
// ============================================================================

TEST(FakeTcpTransport, SendStoresDataInSentData) {
    FakeTcpTransport t;
    ASSERT_TRUE(t.connect(100ms).has_value());

    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    auto result = t.send(payload, sizeof(payload));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, sizeof(payload));

    ASSERT_EQ(t.sent_data().size(), 1u);
    EXPECT_EQ(t.sent_data()[0], (std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04}));
}

TEST(FakeTcpTransport, SendFailsWhenNotConnected) {
    FakeTcpTransport t;
    const uint8_t payload[] = {0x01};
    auto result = t.send(payload, sizeof(payload));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "not connected");
}

TEST(FakeTcpTransport, SendReturnsErrorWhenSet) {
    FakeTcpTransport t;
    ASSERT_TRUE(t.connect(100ms).has_value());

    t.set_send_error("write failed");
    const uint8_t payload[] = {0x01};
    auto result = t.send(payload, sizeof(payload));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "write failed");
    // Data should not be stored when error is injected.
    EXPECT_TRUE(t.sent_data().empty());
}

TEST(FakeTcpTransport, SendMultiplePayloads) {
    FakeTcpTransport t;
    ASSERT_TRUE(t.connect(100ms).has_value());

    const uint8_t a[] = {0xAA};
    const uint8_t b[] = {0xBB, 0xCC};
    ASSERT_TRUE(t.send(a, sizeof(a)).has_value());
    ASSERT_TRUE(t.send(b, sizeof(b)).has_value());

    ASSERT_EQ(t.sent_data().size(), 2u);
    EXPECT_EQ(t.sent_data()[0], (std::vector<uint8_t>{0xAA}));
    EXPECT_EQ(t.sent_data()[1], (std::vector<uint8_t>{0xBB, 0xCC}));
}

// ============================================================================
// poll_rx()
// ============================================================================

TEST(FakeTcpTransport, PollRxReturnsZeroWhenEmpty) {
    FakeTcpTransport t;
    auto result = t.poll_rx([](const uint8_t*, uint16_t) {
        FAIL() << "callback should not be invoked when queue is empty";
    });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0);
}

TEST(FakeTcpTransport, PollRxDeliversInjectedData) {
    FakeTcpTransport t;
    t.inject_rx(std::vector<uint8_t>{0x10, 0x20, 0x30});

    std::vector<uint8_t> received;
    auto result = t.poll_rx([&](const uint8_t* data, uint16_t len) {
        received.assign(data, data + len);
    });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1);
    EXPECT_EQ(received, (std::vector<uint8_t>{0x10, 0x20, 0x30}));

    // Queue should be drained after one poll.
    auto result2 = t.poll_rx([](const uint8_t*, uint16_t) {
        FAIL() << "should not be called";
    });
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(*result2, 0);
}

TEST(FakeTcpTransport, PollRxDeliversMultiplePacketsInOrder) {
    FakeTcpTransport t;
    t.inject_rx(std::vector<uint8_t>{0x01});
    t.inject_rx(std::vector<uint8_t>{0x02});

    std::vector<uint8_t> first;
    auto r1 = t.poll_rx([&](const uint8_t* data, uint16_t len) {
        first.assign(data, data + len);
    });
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(first, (std::vector<uint8_t>{0x01}));

    std::vector<uint8_t> second;
    auto r2 = t.poll_rx([&](const uint8_t* data, uint16_t len) {
        second.assign(data, data + len);
    });
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(second, (std::vector<uint8_t>{0x02}));
}

TEST(FakeTcpTransport, PollRxReturnsErrorWhenSet) {
    FakeTcpTransport t;
    t.inject_rx(std::vector<uint8_t>{0x01});
    t.set_rx_error("read failed");

    auto result = t.poll_rx([](const uint8_t*, uint16_t) {
        FAIL() << "callback should not be invoked on error";
    });
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "read failed");
}

TEST(FakeTcpTransport, InjectRxFromRawPointer) {
    FakeTcpTransport t;
    const uint8_t raw[] = {0xDE, 0xAD};
    t.inject_rx(raw, sizeof(raw));

    std::vector<uint8_t> received;
    auto result = t.poll_rx([&](const uint8_t* data, uint16_t len) {
        received.assign(data, data + len);
    });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(received, (std::vector<uint8_t>{0xDE, 0xAD}));
}

// ============================================================================
// inject_disconnect()
// ============================================================================

TEST(FakeTcpTransport, InjectDisconnectChangesStateToClosed) {
    FakeTcpTransport t;
    ASSERT_TRUE(t.connect(100ms).has_value());
    EXPECT_TRUE(t.is_established());

    t.inject_disconnect();
    EXPECT_EQ(t.state(), TcpState::Closed);
    EXPECT_FALSE(t.is_established());
}

// ============================================================================
// close() and reset()
// ============================================================================

TEST(FakeTcpTransport, CloseTransitionsStateToClosed) {
    FakeTcpTransport t;
    ASSERT_TRUE(t.connect(100ms).has_value());
    auto result = t.close();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(t.state(), TcpState::Closed);
}

TEST(FakeTcpTransport, ResetTransitionsStateToClosed) {
    FakeTcpTransport t;
    ASSERT_TRUE(t.connect(100ms).has_value());
    t.reset();
    EXPECT_EQ(t.state(), TcpState::Closed);
}

// ============================================================================
// clear_errors()
// ============================================================================

TEST(FakeTcpTransport, ClearErrorsResetsAllInjectedErrors) {
    FakeTcpTransport t;

    t.set_connect_error("fail");
    t.set_send_error("fail");
    t.set_rx_error("fail");
    t.clear_errors();

    // connect should now succeed.
    auto conn = t.connect(100ms);
    ASSERT_TRUE(conn.has_value());

    // send should now succeed.
    const uint8_t payload[] = {0x01};
    auto send = t.send(payload, sizeof(payload));
    ASSERT_TRUE(send.has_value());

    // poll_rx should now succeed (returns 0 for empty queue).
    auto rx = t.poll_rx([](const uint8_t*, uint16_t) {});
    ASSERT_TRUE(rx.has_value());
}

// ============================================================================
// clear_sent_data()
// ============================================================================

TEST(FakeTcpTransport, ClearSentDataEmptiesBuffer) {
    FakeTcpTransport t;
    ASSERT_TRUE(t.connect(100ms).has_value());

    const uint8_t payload[] = {0x01};
    ASSERT_TRUE(t.send(payload, sizeof(payload)).has_value());
    ASSERT_FALSE(t.sent_data().empty());

    t.clear_sent_data();
    EXPECT_TRUE(t.sent_data().empty());
}

// ============================================================================
// Default state queries
// ============================================================================

TEST(FakeTcpTransport, DefaultStateIsClosed) {
    FakeTcpTransport t;
    EXPECT_EQ(t.state(), TcpState::Closed);
    EXPECT_FALSE(t.is_established());
}

TEST(FakeTcpTransport, MssReturns1460) {
    FakeTcpTransport t;
    EXPECT_EQ(t.mss(), 1460);
}

TEST(FakeTcpTransport, LastRxBurstTscReturnsZero) {
    FakeTcpTransport t;
    EXPECT_EQ(t.last_rx_burst_tsc(), 0u);
}
