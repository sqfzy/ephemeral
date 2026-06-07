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

// ─────────────────────────────────────────────────────────────────────────────
// PacketView trim_front / trim_back — production-parity
// surface that the FakeStream PacketView exposes so tests can compose a
// real codec over the mock. Until now no unit test pinned the clamp
// behaviour — a regression in `trim_front(n > length)` would silently
// underflow `length()` and any codec test would observe an enormous
// span.
// ─────────────────────────────────────────────────────────────────────────────

TEST(FakeStreamPacketView, TrimFrontClampsAtLength) {
    uint8_t buf[] = {0xAA, 0xBB, 0xCC, 0xDD};
    ent::FakeStream::PacketView v(buf, sizeof(buf));
    EXPECT_EQ(v.length(), 4u);
    v.trim_front(10);  // way more than 4
    EXPECT_EQ(v.length(), 0u);
    EXPECT_EQ(v.data(), buf + 4);
}

TEST(FakeStreamPacketView, TrimBackClampsAtLength) {
    uint8_t buf[] = {0x11, 0x22, 0x33};
    ent::FakeStream::PacketView v(buf, sizeof(buf));
    v.trim_back(99);
    EXPECT_EQ(v.length(), 0u);
    // tail_ pulled back to head_; data() still points at the start.
    EXPECT_EQ(v.data(), buf);
}

TEST(FakeStreamPacketView, TrimFrontThenTrimBackDoesNotUnderflow) {
    // After a partial trim_front, trim_back must clamp on the remaining
    // window — not on the original length. A pre-clamp version would
    // wrap `tail_ -= n` when `n` exceeded the residual window.
    uint8_t buf[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    ent::FakeStream::PacketView v(buf, sizeof(buf));
    v.trim_front(3);   // window now [3, 5) → length 2
    EXPECT_EQ(v.length(), 2u);
    v.trim_back(7);    // request more than 2 — must clamp
    EXPECT_EQ(v.length(), 0u);
}

TEST(FakeStreamPacketView, WritableDataMatchesData) {
    uint8_t buf[] = {0x10, 0x20, 0x30};
    ent::FakeStream::PacketView v(buf, sizeof(buf));
    EXPECT_EQ(v.writable_data(), v.data());
    v.trim_front(1);
    EXPECT_EQ(v.writable_data(), v.data());
    EXPECT_EQ(v.writable_data(), buf + 1);
}

// Re-entrancy contract: a callback that calls `inject_rx` on its own
// stream must NOT have those bytes silently dropped. FakeStream's
// poll_once_ used to do `on_message(rx_buf_); rx_buf_.clear();` —
// bytes injected mid-callback got cleared too.
//
// FakeDatagram explicitly drains its queue into a local before invoking
// the callback (see `FakeDatagram::poll_once_`); we expect the same
// invariant here. Without this test, a regression that re-introduces
// the post-callback `rx_buf_.clear()` bug would silently break echo
// patterns in test code.
TEST(FakeStream, OnMessageReentrantInjectSurvivesIntoNextPoll) {
    ent::FakeStream fs;
    fs.set_attached(true);
    int first_calls = 0;
    bool reentrant_done = false;
    const uint8_t late_bytes[] = {0xCA, 0xFE};
    fs.on_message = [&](std::span<const uint8_t> /*frame*/) {
        ++first_calls;
        if (!reentrant_done) {
            // Re-enter inject_rx during the callback (simulates an
            // echo-loop test where the callback queues a follow-up
            // frame for the next poll cycle).
            fs.inject_rx(late_bytes);
            reentrant_done = true;
        }
    };

    const uint8_t first[] = {0x01, 0x02};
    fs.inject_rx(first);
    EXPECT_EQ(fs.poll_once_(), 1u);
    EXPECT_EQ(first_calls, 1);
    EXPECT_TRUE(reentrant_done);

    // The re-injected bytes must still be drainable on the NEXT poll.
    std::vector<uint8_t> next_frame;
    fs.on_message = [&](std::span<const uint8_t> f) {
        next_frame.assign(f.begin(), f.end());
    };
    EXPECT_EQ(fs.poll_once_(), 1u);
    EXPECT_EQ(next_frame.size(), 2u);
    if (next_frame.size() == 2) {
        EXPECT_EQ(next_frame[0], 0xCA);
        EXPECT_EQ(next_frame[1], 0xFE);
    }
}

// FakeStream test-control recovery sequence: inject_disconnect flips
// state to Closed and clears rx_buf; tests can simulate reconnect by
// flipping state back to Established and injecting fresh data. Pin
// this flow so reconnect-orchestration tests have a stable contract
// for "simulate disconnect then reconnect on the same fake".
TEST(FakeStream, RecoveryAfterInjectDisconnectViaSetState) {
    ent::FakeStream fs;
    std::vector<uint8_t> captured;
    fs.on_message = [&](std::span<const uint8_t> app_frame) {
        captured.assign(app_frame.begin(), app_frame.end());
    };

    // 1. Active stream, deliver a frame.
    const uint8_t before[] = {0x01, 0x02};
    fs.inject_rx(before);
    EXPECT_EQ(fs.poll_once_(), 1u);
    ASSERT_EQ(captured, std::vector<uint8_t>(before, before + 2));

    // 2. Simulated disconnect — state Closed, rx cleared, queued data
    //    that hadn't been polled is dropped per the helper docstring.
    captured.clear();
    const uint8_t orphaned[] = {0xFF, 0xFE};
    fs.inject_rx(orphaned);
    fs.inject_disconnect();
    EXPECT_EQ(fs.state(), en::TcpState::Closed);
    EXPECT_EQ(fs.poll_once_(), 0u);
    EXPECT_TRUE(captured.empty());

    // 3. Recovery: flip state back to Established and inject a fresh
    //    payload. poll_once_ must dispatch the new data — the prior
    //    `orphaned` bytes stay dropped.
    fs.set_state(en::TcpState::Established);
    const uint8_t after[] = {0x77, 0x88};
    fs.inject_rx(after);
    EXPECT_EQ(fs.poll_once_(), 1u);
    EXPECT_EQ(captured, std::vector<uint8_t>(after, after + 2))
        << "post-recovery dispatch must deliver the fresh payload, "
           "not the pre-disconnect orphaned bytes";
}
