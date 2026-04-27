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

// ─────────────────────────────────────────────────────────────────────────────
// Error injection (round-46) — tests that the new `inject_send_error`
// API surfaces caller-supplied `core::ErrorInfo` exactly once on the
// next `send()` call. Lets downstream tests cover paths like "caller
// retries on `WouldBlock`" or "caller logs `BufferFull`" without
// reaching for a real backend.
// ─────────────────────────────────────────────────────────────────────────────

TEST(FakeStream, InjectSendErrorPopsOnceFifo) {
    ent::FakeStream fs;
    fs.set_attached(true);

    // Queue two distinct errors — they must drain FIFO.
    fs.inject_send_error(eph::core::Error::WouldBlock,  "wb-first");
    fs.inject_send_error(eph::core::Error::BufferFull,  "bf-second");
    EXPECT_EQ(fs.pending_send_errors(), 2u);

    const uint8_t payload[] = {0x01};
    auto r1 = fs.send(payload);
    ASSERT_FALSE(r1.has_value());
    EXPECT_EQ(r1.error().code, eph::core::Error::WouldBlock);
    EXPECT_STREQ(r1.error().detail, "wb-first");
    EXPECT_EQ(fs.pending_send_errors(), 1u);

    auto r2 = fs.send(payload);
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code, eph::core::Error::BufferFull);
    EXPECT_STREQ(r2.error().detail, "bf-second");
    EXPECT_EQ(fs.pending_send_errors(), 0u);

    // Queue drained — next send must succeed and accumulate to tx.
    auto r3 = fs.send(payload);
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(*r3, 1u);
    EXPECT_EQ(fs.collect_tx().size(), 1u);
}

TEST(FakeStream, InjectSendErrorDoesNotConsumeWhenNotAttached) {
    // NotAttached must take precedence over the injected-error queue
    // so tests cannot accidentally observe an error-injected path on a
    // detached stream (mirrors real-backend ordering: state guards
    // first, then the sendpath).
    ent::FakeStream fs;
    fs.inject_send_error(eph::core::Error::CodecBad, "should-stay-queued");
    EXPECT_EQ(fs.pending_send_errors(), 1u);

    const uint8_t payload[] = {0xCA, 0xFE};
    auto r = fs.send(payload);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::NotAttached);
    // Queue still has the IoError — not consumed because NotAttached
    // short-circuited before the queue was inspected.
    EXPECT_EQ(fs.pending_send_errors(), 1u);
}

TEST(FakeStream, InjectSendErrorDoesNotConsumeWhenDisconnected) {
    // Same precedence rule for `Disconnected`: tests on a closed
    // session must not pop an injected error.
    ent::FakeStream fs;
    fs.set_attached(true);
    fs.set_state(en::TcpState::Closed);
    fs.inject_send_error(eph::core::Error::Timeout, "should-stay-queued");

    const uint8_t payload[] = {0xAA};
    auto r = fs.send(payload);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::Disconnected);
    EXPECT_EQ(fs.pending_send_errors(), 1u);
}

TEST(FakeStream, InjectSendErrorRichOverloadCarriesDetail) {
    // Verify the `ErrorInfo` overload preserves both code and detail
    // verbatim — needed for tests that match on detail substrings.
    ent::FakeStream fs;
    fs.set_attached(true);
    fs.inject_send_error(eph::core::ErrorInfo{
        eph::core::Error::TlsHandshakeFailed,
        "tls record decrypt failed: bad mac"});

    const uint8_t payload[] = {0x00};
    auto r = fs.send(payload);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::TlsHandshakeFailed);
    EXPECT_STREQ(r.error().detail, "tls record decrypt failed: bad mac");
}
