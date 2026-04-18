/// @file test_fake_stream.cpp
/// Unit tests for `eph::net::test::FakeStream`.
///
/// Focuses on:
///   - inject/collect round-trip through poll_once_()
///   - NotAttached error when send() is called before attach
///   - state / attach flag lifecycle
///   - graceful close

#include <span>

#include <gtest/gtest.h>

#include "eph/net/test/fake_stream.hpp"

namespace en  = eph::net;
namespace ent = eph::net::test;

TEST(FakeStream, InitialStateEstablishedAndDetached) {
    ent::FakeStream fs;
    EXPECT_EQ(fs.state(), en::TcpState::Established);
    EXPECT_FALSE(fs.is_attached());
}

TEST(FakeStream, SendBeforeAttachReturnsNotAttached) {
    ent::FakeStream fs;
    const uint8_t data[] = {1, 2, 3};
    auto r = fs.send(data);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::NotAttached);
}

TEST(FakeStream, SendAfterAttachAccumulatesTx) {
    ent::FakeStream fs;
    fs.set_attached(true);
    const uint8_t first[]  = {0x01, 0x02};
    const uint8_t second[] = {0x03, 0x04, 0x05};
    ASSERT_TRUE(fs.send(first).has_value());
    ASSERT_TRUE(fs.send(second).has_value());
    auto tx = fs.collect_tx();
    ASSERT_EQ(tx.size(), 5u);
    EXPECT_EQ(tx[0], 0x01);
    EXPECT_EQ(tx[4], 0x05);
}

TEST(FakeStream, ClearTxEmptiesBufferOnly) {
    ent::FakeStream fs;
    fs.set_attached(true);
    const uint8_t data[] = {1, 2, 3};
    ASSERT_TRUE(fs.send(data).has_value());
    fs.clear_tx();
    EXPECT_TRUE(fs.collect_tx().empty());
    // State should NOT change.
    EXPECT_EQ(fs.state(), en::TcpState::Established);
    EXPECT_TRUE(fs.is_attached());
}

TEST(FakeStream, InjectRxThenPollOnceFiresOnMessage) {
    ent::FakeStream fs;
    std::vector<uint8_t> captured;
    fs.on_message = [&](std::span<const uint8_t> app_frame) {
        captured.assign(app_frame.begin(), app_frame.end());
    };

    const uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    fs.inject_rx(data);
    EXPECT_EQ(fs.poll_once_(), 1u);
    ASSERT_EQ(captured.size(), 4u);
    EXPECT_EQ(captured[0], 0xDE);
    EXPECT_EQ(captured[3], 0xEF);

    // Rx buffer consumed — second poll returns 0.
    EXPECT_EQ(fs.poll_once_(), 0u);
}

TEST(FakeStream, PollOnceWithNoCallbackDoesNotCrash) {
    ent::FakeStream fs;
    const uint8_t data[] = {1, 2, 3};
    fs.inject_rx(data);
    // Callback not set — poll must still drain the buffer without crashing.
    EXPECT_EQ(fs.poll_once_(), 1u);
    EXPECT_EQ(fs.poll_once_(), 0u);
}

TEST(FakeStream, CloseGracefullyFlipsState) {
    ent::FakeStream fs;
    EXPECT_FALSE(fs.closed_gracefully());
    auto r = fs.close_gracefully();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(fs.state(), en::TcpState::Closed);
    EXPECT_TRUE(fs.closed_gracefully());
}

TEST(FakeStream, SetStateOverride) {
    ent::FakeStream fs;
    fs.set_state(en::TcpState::CloseWait);
    EXPECT_EQ(fs.state(), en::TcpState::CloseWait);
}

TEST(FakeStream, NativeHandleIsStablePointer) {
    ent::FakeStream fs;
    EXPECT_EQ(fs.native_handle(), static_cast<void*>(&fs));
}

// ───────────────────────────────────────────────────────────────────────
// batch3-round3 MEDIUM-1/2 coverage: state-aware poll_once_ + send,
// inject_disconnect helper.
// ───────────────────────────────────────────────────────────────────────

TEST(FakeStream, PollOnceDoesNotDispatchWhenStateClosed) {
    ent::FakeStream fs;
    std::vector<uint8_t> captured;
    fs.on_message = [&](std::span<const uint8_t> app_frame) {
        captured.assign(app_frame.begin(), app_frame.end());
    };
    const uint8_t data[] = {0x01, 0x02};
    fs.inject_rx(data);
    fs.set_state(en::TcpState::Closed);
    // Real backends early-return 0 when state != Established. The mock
    // must match so reconnect tests don't observe bogus frames.
    EXPECT_EQ(fs.poll_once_(), 0u);
    EXPECT_TRUE(captured.empty());
}

TEST(FakeStream, SendAfterCloseReturnsDisconnected) {
    ent::FakeStream fs;
    fs.set_attached(true);
    ASSERT_TRUE(fs.close_gracefully().has_value());
    const uint8_t data[] = {0xAA};
    auto r = fs.send(data);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::Disconnected);
}

TEST(FakeStream, InjectDisconnectFlipsStateAndDropsRx) {
    ent::FakeStream fs;
    std::vector<uint8_t> captured;
    fs.on_message = [&](std::span<const uint8_t> app_frame) {
        captured.assign(app_frame.begin(), app_frame.end());
    };
    const uint8_t data[] = {0xDE, 0xAD};
    fs.inject_rx(data);
    fs.inject_disconnect();
    EXPECT_EQ(fs.state(), en::TcpState::Closed);
    // Rx buffer was dropped by the disconnect — poll_once_ returns 0.
    EXPECT_EQ(fs.poll_once_(), 0u);
    EXPECT_TRUE(captured.empty());
}
