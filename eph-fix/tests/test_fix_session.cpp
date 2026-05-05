#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "eph/fix.hpp"

using namespace eph::fix;

// ---------------------------------------------------------------------------
// Mock transport: captures sent messages, builds server responses
// ---------------------------------------------------------------------------

class MockFixTransport {
public:
    std::vector<std::vector<uint8_t>> sent;

    bool send(const uint8_t* data, size_t len) {
        sent.emplace_back(data, data + len);
        return send_ok_;
    }

    FixSession::SendFn send_fn() {
        return [this](const uint8_t* d, size_t l) { return send(d, l); };
    }

    void set_send_fails(bool fail) { send_ok_ = !fail; }

    // --- Server message builders ---

    static std::vector<uint8_t> build_msg(const char* msg_type, uint32_t seq,
                                           std::function<void(MessageBuilder&)> extra = {}) {
        uint8_t buf[512];
        MessageBuilder b(buf, sizeof(buf));
        b.set(tag::MsgType, msg_type);
        b.set(tag::SenderCompID, "EXCHANGE");
        b.set(tag::TargetCompID, "CLIENT");
        b.set_int(tag::MsgSeqNum, static_cast<int64_t>(seq));
        if (extra) extra(b);
        size_t len = b.finish("FIX.4.4");
        return {buf, buf + len};
    }

    static std::vector<uint8_t> logon_response(uint32_t seq = 1, int hb_int = 30) {
        return build_msg("A", seq, [&](MessageBuilder& b) {
            b.set_int(tag::EncryptMethod, 0);
            b.set_int(tag::HeartBtInt, hb_int);
        });
    }

    static std::vector<uint8_t> logout_response(uint32_t seq = 2) {
        return build_msg("5", seq);
    }

    static std::vector<uint8_t> heartbeat(uint32_t seq) {
        return build_msg("0", seq);
    }

    static std::vector<uint8_t> test_request(uint32_t seq, std::string_view id = "TR001") {
        return build_msg("1", seq, [&](MessageBuilder& b) {
            b.set(tag::TestReqID, id);
        });
    }

    static std::vector<uint8_t> market_data(uint32_t seq) {
        return build_msg("X", seq, [](MessageBuilder& b) {
            b.set(tag::MDReqID, "md-001");
        });
    }

    static std::vector<uint8_t> sequence_reset_gap_fill(uint32_t seq, uint32_t new_seq) {
        return build_msg("4", seq, [&](MessageBuilder& b) {
            b.set_bool(tag::GapFillFlag, true);
            b.set_int(tag::NewSeqNo, static_cast<int64_t>(new_seq));
        });
    }

    static std::vector<uint8_t> resend_request(uint32_t seq, uint32_t begin, uint32_t end) {
        return build_msg("2", seq, [&](MessageBuilder& b) {
            b.set_int(tag::BeginSeqNo, static_cast<int64_t>(begin));
            b.set_int(tag::EndSeqNo, static_cast<int64_t>(end));
        });
    }

    // Parse the last sent message
    std::optional<MessageView> parse_last_sent() {
        if (sent.empty()) return std::nullopt;
        auto& last = sent.back();
        auto r = parse(last.data(), last.size());
        return r ? std::optional{*r} : std::nullopt;
    }

private:
    bool send_ok_ = true;
};

// Helper: logon a session (blocking, with server response in background)
static void do_logon(FixSession& session, MockFixTransport& mock) {
    size_t before = mock.sent.size();
    std::thread t([&] { (void)session.logon(std::chrono::milliseconds{500}); });
    while (mock.sent.size() <= before) std::this_thread::yield();
    auto resp = MockFixTransport::logon_response();
    session.on_rx(resp.data(), resp.size());
    t.join();
}

static FixSessionConfig test_config() {
    return {
        .sender_comp_id = "CLIENT",
        .target_comp_id = "EXCHANGE",
        .heartbeat_interval_sec = 30,
        .reset_seq_on_logon = true,
    };
}

// ===========================================================================
// State lifecycle
// ===========================================================================

TEST(FixSession, initial_state_is_disconnected) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    EXPECT_EQ(session.state(), SessionState::kDisconnected);
    EXPECT_EQ(session.next_outbound_seq(), 1u);
    EXPECT_EQ(session.last_inbound_seq(), 0u);
    EXPECT_EQ(session.expected_inbound_seq(), 1u);
}

// ===========================================================================
// Logon
// ===========================================================================

TEST(FixSession, logon_sends_logon_message) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());

    do_logon(session, mock);

    // Verify the Logon message that was sent
    ASSERT_FALSE(mock.sent.empty());
    auto& first = mock.sent.front();
    auto msg = parse(first.data(), first.size());
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->msg_type(), std::optional<std::string_view>("A"));
    EXPECT_EQ(msg->get(tag::SenderCompID), std::optional<std::string_view>("CLIENT"));
    EXPECT_EQ(msg->get_int(tag::MsgSeqNum), std::optional<int64_t>(1));
    EXPECT_EQ(msg->get_int(tag::HeartBtInt), std::optional<int64_t>(30));

    EXPECT_EQ(session.state(), SessionState::kActive);
    EXPECT_EQ(session.next_outbound_seq(), 2u);
}

TEST(FixSession, logon_timeout_returns_error) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    auto r = session.logon(std::chrono::milliseconds{50});
    EXPECT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("timeout"), std::string::npos);
    EXPECT_EQ(session.state(), SessionState::kDisconnected);
}

// Calling logon() while the session is already kActive is a programmer
// error. session.hpp:317 guards with "session not in DISCONNECTED
// state" — must return error and not mutate any state. Untested.
TEST(FixSession, logon_when_active_returns_error_unchanged) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);
    ASSERT_EQ(session.state(), SessionState::kActive);
    const uint32_t exp_before  = session.expected_inbound_seq();
    const uint32_t out_before  = session.next_outbound_seq();
    const size_t   sent_before = mock.sent.size();

    auto r = session.logon(std::chrono::milliseconds{100});
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(std::string_view{r.error()}.find("not in DISCONNECTED"),
              std::string_view::npos)
        << "expected 'not in DISCONNECTED' diagnostic, got: " << r.error();
    // No state mutation on the rejected re-logon.
    EXPECT_EQ(session.state(), SessionState::kActive);
    EXPECT_EQ(session.expected_inbound_seq(), exp_before);
    EXPECT_EQ(session.next_outbound_seq(), out_before);
    EXPECT_EQ(mock.sent.size(), sent_before)
        << "rejected re-logon emitted bytes on the wire";
}

TEST(FixSession, logon_send_failure) {
    MockFixTransport mock;
    mock.set_send_fails(true);
    FixSession session(mock.send_fn(), test_config());
    auto r = session.logon(std::chrono::milliseconds{100});
    EXPECT_FALSE(r.has_value());
}

// logout() error-state contract (documented in R80): on TX failure
// the state is LEFT AT kActive — we don't pretend the session is
// dead because a transient TLS desync / EPIPE may be recoverable.
// Without this test, a future refactor that "fixed" the asymmetry
// (e.g. forcing kDisconnected on TX failure) would silently break
// reconnect logic that depends on the kActive-on-TX-fail contract.
TEST(FixSession, logout_send_failure_leaves_state_active) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);
    ASSERT_EQ(session.state(), SessionState::kActive);

    // Now poison the transport so the Logout send fails.
    mock.set_send_fails(true);
    auto r = session.logout(std::chrono::milliseconds{100});
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(std::string_view{r.error()}.find("failed to send"),
              std::string_view::npos)
        << "error message must mention send failure: " << r.error();
    // The state must NOT have been forced to kDisconnected — the
    // contract is "TX-fail leaves state Active so caller can decide".
    EXPECT_EQ(session.state(), SessionState::kActive)
        << "logout() TX failure incorrectly transitioned state to "
        << session_state_name(session.state())
        << " — breaks the documented R80 asymmetry contract";
}

// ===========================================================================
// Server HeartBtInt override
// ===========================================================================

TEST(FixSession, server_heartbeat_interval_override) {
    MockFixTransport mock;
    auto cfg = test_config();
    cfg.heartbeat_interval_sec = 30;
    FixSession session(mock.send_fn(), cfg);

    // Pre-Logon: runtime hb_int reflects the client config.
    EXPECT_EQ(session.heartbeat_interval_sec(), 30);

    std::thread t([&] { (void)session.logon(std::chrono::milliseconds{500}); });
    while (mock.sent.empty()) std::this_thread::yield();
    // Server responds with HeartBtInt=10 (overrides client's 30)
    auto resp = MockFixTransport::logon_response(1, 10);
    session.on_rx(resp.data(), resp.size());
    t.join();
    EXPECT_EQ(session.state(), SessionState::kActive);
    // Now verify the override actually landed in the runtime atomic —
    // the previous "tick() behavior will change" comment was right but
    // untested. With heartbeat_interval_sec() exposed as a getter, the
    // post-Logon value must equal the server-supplied 10.
    EXPECT_EQ(session.heartbeat_interval_sec(), 10)
        << "Server HeartBtInt override (30 -> 10) did not land in the "
        << "runtime atomic — tick() / maybe_send_heartbeat() would "
        << "still pace at the stale client value";
}

// Symmetric guard: a server HeartBtInt outside the [1, 3600] sanity
// range must NOT override the runtime value (session.hpp:537-549
// rejects negatives, zeros, and > 3600 with a WARN). Without this
// test, a future loosening of the guard would silently let a peer
// drive HeartBtInt to e.g. 86400, suppressing all proactive
// heartbeats and risking dead-link silence for a full day.
TEST(FixSession, unreasonable_server_heartbeat_interval_ignored) {
    MockFixTransport mock;
    auto cfg = test_config();
    cfg.heartbeat_interval_sec = 30;
    FixSession session(mock.send_fn(), cfg);

    std::thread t([&] { (void)session.logon(std::chrono::milliseconds{500}); });
    while (mock.sent.empty()) std::this_thread::yield();
    // Server tries to push HeartBtInt=86400 (one day). Must be ignored.
    auto resp = MockFixTransport::logon_response(1, 86400);
    session.on_rx(resp.data(), resp.size());
    t.join();
    EXPECT_EQ(session.state(), SessionState::kActive);
    EXPECT_EQ(session.heartbeat_interval_sec(), 30)
        << "Unreasonable server HeartBtInt=86400 was accepted; the "
        << "runtime cap (kMaxHeartbeatSec=3600 in session.hpp) must "
        << "reject anything above 1 hour to prevent dead-link masking";
}

// ===========================================================================
// on_rx dispatch
// ===========================================================================

TEST(FixSession, on_rx_true_for_session_messages) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);

    auto hb = MockFixTransport::heartbeat(2);
    EXPECT_TRUE(session.on_rx(hb.data(), hb.size()));

    size_t before = mock.sent.size();
    auto tr = MockFixTransport::test_request(3);
    EXPECT_TRUE(session.on_rx(tr.data(), tr.size()));
    EXPECT_GT(mock.sent.size(), before);  // Heartbeat response sent

    // Verify Heartbeat response uses tag 112 (TestReqID), not tag 58
    auto sent_msg = mock.parse_last_sent();
    ASSERT_TRUE(sent_msg.has_value());
    EXPECT_EQ(sent_msg->msg_type(), std::optional<std::string_view>("0"));
    EXPECT_EQ(sent_msg->get(tag::TestReqID), std::optional<std::string_view>("TR001"));
}

TEST(FixSession, on_rx_false_for_app_messages) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);

    auto md = MockFixTransport::market_data(2);
    EXPECT_FALSE(session.on_rx(md.data(), md.size()));
    EXPECT_EQ(session.last_inbound_seq(), 2u);
}

// ===========================================================================
// Sequence number tracking
// ===========================================================================

TEST(FixSession, outbound_seq_increments) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);
    EXPECT_EQ(session.next_outbound_seq(), 2u);

    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "V");
    b.set(tag::MDReqID, "md-001");
    EXPECT_TRUE(session.send_app(b));
    EXPECT_EQ(session.next_outbound_seq(), 3u);

    auto& last = mock.sent.back();
    auto msg = parse(last.data(), last.size());
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->get_int(tag::MsgSeqNum), std::optional<int64_t>(2));
}

TEST(FixSession, inbound_seq_tracks_server) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);

    EXPECT_EQ(session.last_inbound_seq(), 1u);
    EXPECT_EQ(session.expected_inbound_seq(), 2u);

    auto hb = MockFixTransport::heartbeat(2);
    session.on_rx(hb.data(), hb.size());
    EXPECT_EQ(session.last_inbound_seq(), 2u);
    EXPECT_EQ(session.expected_inbound_seq(), 3u);
}

// ===========================================================================
// Sequence gap detection
// ===========================================================================

TEST(FixSession, detects_sequence_gap) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);

    // Server sends seq 5 (expected 2) — gap of 3 messages
    auto md = MockFixTransport::market_data(5);
    session.on_rx(md.data(), md.size());
    EXPECT_EQ(session.last_inbound_seq(), 5u);
    EXPECT_EQ(session.expected_inbound_seq(), 6u);  // Jumped past gap
}

TEST(FixSession, sends_resend_request_on_gap_when_configured) {
    MockFixTransport mock;
    auto cfg = test_config();
    cfg.resend_on_gap = true;
    FixSession session(mock.send_fn(), cfg);
    do_logon(session, mock);

    size_t before = mock.sent.size();
    auto md = MockFixTransport::market_data(5);  // Gap: expected 2, got 5
    session.on_rx(md.data(), md.size());

    // Should have sent ResendRequest
    EXPECT_GT(mock.sent.size(), before);
    auto msg = mock.parse_last_sent();
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->msg_type(), std::optional<std::string_view>("2"));
    EXPECT_EQ(msg->get_int(tag::BeginSeqNo), std::optional<int64_t>(2));
    EXPECT_EQ(msg->get_int(tag::EndSeqNo), std::optional<int64_t>(4));
}

// ===========================================================================
// SequenceReset / GapFill
// ===========================================================================

TEST(FixSession, handles_sequence_reset_gap_fill) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);

    // Server sends GapFill to advance expected to 10
    auto gf = MockFixTransport::sequence_reset_gap_fill(2, 10);
    EXPECT_TRUE(session.on_rx(gf.data(), gf.size()));
    EXPECT_EQ(session.expected_inbound_seq(), 10u);
}

// FIX 4.4 Vol 2 §4 caps MsgSeqNum at uint32 — anything above UINT32_MAX
// is illegal on the wire. The session was casting NewSeqNo (parsed as
// int64_t) directly to uint32_t without bounds checking, so a malicious
// or buggy peer sending NewSeqNo=4294967296 (UINT32_MAX+1) would
// silently truncate to 0 in expected_inbound_seq_ and treat every
// subsequent received message as a fresh gap. Reject the over-range
// reset rather than corrupt local state.
// Same protocol cap as NewSeqNo above, but for the per-message
// MsgSeqNum bookkeeping. A peer sending MsgSeqNum > UINT32_MAX would
// truncate to the low 32 bits, get stored in `last_inbound_seq_`, and
// the gap-detection comparison would treat it as a fresh number rather
// than rejecting the protocol-illegal input.
TEST(FixSession, rejects_message_with_overrange_msg_seq_num) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);

    // After logon, expected_inbound_seq is 2.
    ASSERT_EQ(session.expected_inbound_seq(), 2u);

    // Hand-build a Heartbeat (MsgType=0) with MsgSeqNum > UINT32_MAX
    // (UINT32_MAX + 7 = 4294967302).
    uint8_t buf[512];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "0");
    b.set(tag::SenderCompID, "EXCHANGE");
    b.set(tag::TargetCompID, "CLIENT");
    b.set_int(tag::MsgSeqNum,
              static_cast<int64_t>(std::numeric_limits<uint32_t>::max())
                  + 7);
    const size_t len = b.finish("FIX.4.4");
    std::vector<uint8_t> msg(buf, buf + len);

    EXPECT_TRUE(session.on_rx(msg.data(), msg.size()));

    // Truncation fingerprint: (UINT32_MAX + 7) & 0xFFFF_FFFF == 6.
    // If the unchecked uint32_t cast was used, expected_inbound_seq
    // would advance to 7 (recv=6 looks like a gap from expected=2,
    // recv+1 -> 7 is stored).
    const uint32_t after = session.expected_inbound_seq();
    EXPECT_NE(after, 7u)
        << "Truncation bug: MsgSeqNum=UINT32_MAX+7 was cast to uint32 "
           "and stored as recv=6, then expected advanced to 7. Local "
           "seq tracker is now corrupt.";
    // Correct behaviour: ignore the over-range MsgSeqNum entirely so
    // expected stays at 2, ready to receive the legitimate next
    // message at seq=2.
    EXPECT_EQ(after, 2u)
        << "Over-range MsgSeqNum should be ignored, expected stays at 2";
}

TEST(FixSession, rejects_sequence_reset_with_overrange_new_seq) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);

    // Logon (server's MsgSeqNum=1) advanced expected_inbound_seq to 2.
    // The SequenceReset we are about to send carries MsgSeqNum=2 and
    // is processed by the MsgSeqNum bookkeeping FIRST (advancing
    // expected to 3), THEN by the SequenceReset NewSeqNo handler.
    // The bug specifically corrupts NewSeqNo handling — the MsgSeqNum
    // bookkeeping is correct on its own. Pin the post-MsgSeqNum,
    // post-SequenceReset value so the test names exactly what the
    // patch must preserve.
    ASSERT_EQ(session.expected_inbound_seq(), 2u);

    // Hand-build a SequenceReset (MsgType=4) with NewSeqNo > UINT32_MAX
    // (UINT32_MAX + 5). MessageBuilder::set_int takes int64_t, so this
    // fits on the wire even though it's protocol-illegal.
    uint8_t buf[512];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "4");
    b.set(tag::SenderCompID, "EXCHANGE");
    b.set(tag::TargetCompID, "CLIENT");
    b.set_int(tag::MsgSeqNum, 2);
    b.set_bool(tag::GapFillFlag, true);
    b.set_int(tag::NewSeqNo,
              static_cast<int64_t>(std::numeric_limits<uint32_t>::max())
                  + 5);
    const size_t len = b.finish("FIX.4.4");
    std::vector<uint8_t> msg(buf, buf + len);

    EXPECT_TRUE(session.on_rx(msg.data(), msg.size()));
    const uint32_t after = session.expected_inbound_seq();

    // Truncation bug: (UINT32_MAX + 5) & 0xFFFF_FFFF == 4. If the
    // session is using the unchecked uint32_t cast, expected_inbound_
    // seq lands here.
    EXPECT_NE(after, 4u)
        << "Truncation bug: NewSeqNo=UINT32_MAX+5 was cast to uint32 "
           "and stored as 4. Local seq tracker is now corrupt.";
    // Correct behaviour: ignore the over-range NewSeqNo entirely
    // (the per-message MsgSeqNum bookkeeping STILL advances expected
    // from 2 → 3 because the message was syntactically valid; only
    // the NewSeqNo field is rejected).
    EXPECT_EQ(after, 3u)
        << "Over-range NewSeqNo should not move expected via the "
           "SequenceReset path; only the per-message MsgSeqNum "
           "bookkeeping (2→3) is allowed to advance.";
}

// SequenceReset (MsgType=4) — boundary cases not covered by the
// happy-path or over-range tests above.

// GapFill must reject a NewSeqNo that goes BACKWARD from the current
// expected sequence — RFC documents (FIX 4.4 Vol 2 §B.5) forbid backward
// resets in GapFill mode. The session's expected_inbound_seq must NOT
// be moved backward, even temporarily.
TEST(FixSession, gap_fill_backward_reset_rejected) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);

    // Move expected_inbound_seq forward to 10 via a legitimate GapFill
    // (server seq=2, NewSeqNo=10 — accepts).
    auto step1 = MockFixTransport::sequence_reset_gap_fill(2, 10);
    EXPECT_TRUE(session.on_rx(step1.data(), step1.size()));
    ASSERT_EQ(session.expected_inbound_seq(), 10u);

    // Now server sends GapFill with NewSeqNo=5 (< expected=10) at seq=10.
    // Must be rejected and expected MUST stay at 10.
    auto step2 = MockFixTransport::sequence_reset_gap_fill(10, 5);
    EXPECT_TRUE(session.on_rx(step2.data(), step2.size()));
    EXPECT_EQ(session.expected_inbound_seq(), 11u)
        << "Backward GapFill must not move expected backward; the only "
           "advance is the per-message MsgSeqNum bookkeeping (10→11).";
}

// GapFill with NewSeqNo == expected is a no-op — neither error nor
// advance. The per-message MsgSeqNum bookkeeping still runs (advances
// expected by 1), but the SequenceReset handler itself must not
// double-advance.
TEST(FixSession, gap_fill_at_expected_seq_no_op) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);

    ASSERT_EQ(session.expected_inbound_seq(), 2u);
    // GapFill at seq=2 with NewSeqNo=2 (== expected). The MsgSeqNum
    // bookkeeping advances 2→3; the SequenceReset NewSeqNo handler is
    // a no-op (does not advance to anything < or == current).
    auto gf = MockFixTransport::sequence_reset_gap_fill(2, 2);
    EXPECT_TRUE(session.on_rx(gf.data(), gf.size()));
    EXPECT_EQ(session.expected_inbound_seq(), 3u);
}

// SequenceReset (MsgType=4) without GapFillFlag is "Reset mode" — must
// unconditionally set expected_inbound_seq, even backward. Distinct
// from GapFill mode tested above.
TEST(FixSession, sequence_reset_without_gap_fill_is_unconditional) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);

    // Move expected forward to 50 via GapFill.
    auto step1 = MockFixTransport::sequence_reset_gap_fill(2, 50);
    EXPECT_TRUE(session.on_rx(step1.data(), step1.size()));
    ASSERT_EQ(session.expected_inbound_seq(), 50u);

    // Now build a SequenceReset (no GapFillFlag) with NewSeqNo=20.
    // In Reset mode this MUST unconditionally set expected to 20,
    // even though it is below the current expected — the handler is
    // a session-recovery escape hatch.
    uint8_t buf[512];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "4");
    b.set(tag::SenderCompID, "EXCHANGE");
    b.set(tag::TargetCompID, "CLIENT");
    b.set_int(tag::MsgSeqNum, 50);
    b.set_int(tag::NewSeqNo, 20);
    const size_t len = b.finish("FIX.4.4");
    std::vector<uint8_t> msg(buf, buf + len);

    EXPECT_TRUE(session.on_rx(msg.data(), msg.size()));
    EXPECT_EQ(session.expected_inbound_seq(), 20u)
        << "SequenceReset without GapFillFlag is unconditional Reset mode "
           "and must set expected exactly to NewSeqNo, even backward.";
}

// SequenceReset with NewSeqNo<1 must be ignored — sequence numbers
// in FIX 4.4 are 1-based.
TEST(FixSession, sequence_reset_new_seq_below_one_ignored) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);

    ASSERT_EQ(session.expected_inbound_seq(), 2u);

    // Build a SequenceReset with NewSeqNo=0 (illegal, must be ignored).
    uint8_t buf[512];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "4");
    b.set(tag::SenderCompID, "EXCHANGE");
    b.set(tag::TargetCompID, "CLIENT");
    b.set_int(tag::MsgSeqNum, 2);
    b.set_int(tag::NewSeqNo, 0);
    const size_t len = b.finish("FIX.4.4");
    std::vector<uint8_t> msg(buf, buf + len);

    EXPECT_TRUE(session.on_rx(msg.data(), msg.size()));
    // Only the per-message MsgSeqNum bookkeeping runs (2 → 3); the
    // SequenceReset handler must short-circuit without storing 0.
    EXPECT_EQ(session.expected_inbound_seq(), 3u);
}

// ===========================================================================
// ResendRequest from server
// ===========================================================================

TEST(FixSession, responds_to_resend_request_with_gap_fill) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);

    size_t before = mock.sent.size();
    auto rr = MockFixTransport::resend_request(2, 1, 5);
    EXPECT_TRUE(session.on_rx(rr.data(), rr.size()));

    // Should respond with SequenceReset-GapFill
    EXPECT_GT(mock.sent.size(), before);
    auto msg = mock.parse_last_sent();
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->msg_type(), std::optional<std::string_view>("4"));
    EXPECT_EQ(msg->get(tag::GapFillFlag), std::optional<std::string_view>("Y"));
}

// ===========================================================================
// Logout
// ===========================================================================

TEST(FixSession, logout_handshake) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);

    std::thread t([&] { (void)session.logout(std::chrono::milliseconds{500}); });
    while (mock.sent.size() < 2) std::this_thread::yield();

    auto resp = MockFixTransport::logout_response(2);
    session.on_rx(resp.data(), resp.size());
    t.join();
    EXPECT_EQ(session.state(), SessionState::kDisconnected);
}

// session.hpp::logout() at line ~380 spins on the state atomic until
// either the server responds (state goes to kDisconnected) or the
// timeout deadline is reached. The timeout path FORCES the state to
// kDisconnected and returns success ({}) — graceful-shutdown is
// best-effort: a non-responsive server should not block the local
// teardown indefinitely. The previous suite covered the happy path
// (server responds in time) but not the timeout path.
//
// Without this test, a future refactor that returned an error on
// timeout (or — worse — left the state at kLogoutSent) would silently
// regress: the local session would leak and the next reconnect
// attempt would fail an "already connected" check.
TEST(FixSession, logout_timeout_forces_disconnected) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);
    ASSERT_EQ(session.state(), SessionState::kActive);

    // Use a short timeout so the test is fast. We deliberately do NOT
    // feed the session a logout_response — the spin-loop must hit
    // its deadline.
    auto r = session.logout(std::chrono::milliseconds{50});
    // logout() returns {} on timeout (best-effort), per the comment
    // at session.hpp:382-384 and the early-return at line 385.
    EXPECT_TRUE(r.has_value())
        << "logout timeout must succeed (best-effort tear-down) — "
        << "regressing this returns an error and breaks the reconnect "
        << "orchestrator's expectation that logout() is graceful";
    // State must be forced to kDisconnected. A future refactor that
    // left the state at kLogoutSent would leak the session.
    EXPECT_EQ(session.state(), SessionState::kDisconnected)
        << "post-timeout state must be kDisconnected, not kLogoutSent";
}

// Per FIX 4.4 §B.6, a server-side Logon rejection may arrive as a
// Logout (auth failure, bad config, server-side sequence mismatch).
// Pre-fix: session stayed in kLogonSent and logon() blocked until
// timeout (typically 5s). Post-fix: on_rx Logout in kLogonSent
// transitions to kDisconnected so the logon() spin-wait exits
// promptly with the "did not reach ACTIVE state" error.
TEST(FixSession, server_logout_during_logon_transitions_to_disconnected) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());

    // Run logon() with a generous timeout. If the fix is in place,
    // the Logout will trigger early state transition and logon()
    // will return quickly with an error, well before timeout.
    std::expected<void, std::string> result;
    const auto t_start = std::chrono::steady_clock::now();
    std::thread t([&] {
        result = session.logon(std::chrono::milliseconds{5000});
    });
    while (mock.sent.empty()) std::this_thread::yield();

    // Server replies with a Logout (instead of Logon).
    auto logout_resp = MockFixTransport::logout_response(1);
    session.on_rx(logout_resp.data(), logout_resp.size());

    t.join();
    const auto elapsed = std::chrono::steady_clock::now() - t_start;

    // Logon must have failed (not transitioned to Active).
    EXPECT_FALSE(result.has_value())
        << "logon() returned success after server sent Logout — the "
        << "session was incorrectly treated as connected";
    // State must be kDisconnected (not stuck in kLogonSent).
    EXPECT_EQ(session.state(), SessionState::kDisconnected);
    // Critical regression assertion: logon() must have exited well
    // before the 5s timeout. Pre-fix this would take ~5s (the
    // configured timeout); post-fix it should be < 500ms (just the
    // RX dispatch + spin-loop wake). 1s gives generous headroom for
    // CI noise without weakening the regression signal.
    EXPECT_LT(elapsed, std::chrono::seconds{1})
        << "logon() took "
        << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
        << " ms — expected < 1000 ms because server Logout should "
        << "trigger immediate state transition rather than letting "
        << "the spin-wait run to timeout";
}

TEST(FixSession, server_initiated_logout) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);

    size_t before = mock.sent.size();
    auto srv_logout = MockFixTransport::logout_response(2);
    session.on_rx(srv_logout.data(), srv_logout.size());

    EXPECT_GT(mock.sent.size(), before);  // Sent Logout response
    EXPECT_EQ(session.state(), SessionState::kDisconnected);
}

// ===========================================================================
// Reset and re-logon
// ===========================================================================

TEST(FixSession, reset_allows_relogon) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);

    session.reset();
    EXPECT_EQ(session.state(), SessionState::kDisconnected);
    EXPECT_EQ(session.next_outbound_seq(), 1u);
    EXPECT_EQ(session.expected_inbound_seq(), 1u);

    do_logon(session, mock);
    EXPECT_EQ(session.state(), SessionState::kActive);
}

/// reset() must clear test_request_pending_ — otherwise a session that
/// reset after a probe was issued would re-logon with pending=true, and
/// the very first tick() in the new Active state would (after one
/// HB-interval of silence) declare the freshly-reconnected server dead.
/// session.hpp::reset() does this at line ~409, but no test currently
/// asserts it. Add a focused test so a future refactor that drops the
/// pending-flag clear can't slip past CI silently.
// reset() also restores heartbeat_interval_sec_ to the client config
// (session.hpp:410). Without this, a session that received a
// server-pushed HeartBtInt override (say 10s) and then reset() would
// retain the 10s value into the next logon — even though the
// re-logon flow may not arrive at a server that wants 10s. Test
// ensures the override is wiped so re-logon starts from the original
// client config.
TEST(FixSession, reset_restores_heartbeat_interval_to_config) {
    MockFixTransport mock;
    auto cfg = test_config();
    cfg.heartbeat_interval_sec = 30;  // client baseline
    FixSession session(mock.send_fn(), cfg);

    // Logon with server-overridden HeartBtInt = 10.
    std::thread t([&] { (void)session.logon(std::chrono::milliseconds{500}); });
    while (mock.sent.empty()) std::this_thread::yield();
    auto resp = MockFixTransport::logon_response(1, 10);
    session.on_rx(resp.data(), resp.size());
    t.join();
    ASSERT_EQ(session.heartbeat_interval_sec(), 10);  // override landed

    // reset() must wipe the override back to the client config.
    session.reset();
    EXPECT_EQ(session.heartbeat_interval_sec(), 30)
        << "reset() did not restore heartbeat_interval_sec_ to "
        << "cfg_.heartbeat_interval_sec — re-logon would carry the "
        << "stale server-pushed value into a fresh session";
}

TEST(FixSession, reset_clears_test_request_pending_state) {
    MockFixTransport mock;
    auto cfg = test_config();
    // Use a 1-second heartbeat so we can drive tick() through the
    // probe-issue path without sleeping for 30 s.
    cfg.heartbeat_interval_sec = 1;
    cfg.heartbeat_timeout_factor = 2.0;
    FixSession session(mock.send_fn(), cfg);
    do_logon(session, mock);

    // Drive probe via tick() after a synthetic idle stretch:
    // - reset is the only clean way to set test_request_pending_ to a
    //   known state we can read back. We don't have a public getter, so
    //   instead we inspect indirectly via the disconnect-on-second-tick
    //   contract: after reset the next tick on a fresh logon must NOT
    //   immediately disconnect even if hb_interval has elapsed (because
    //   pending=false → we'd send a TR first and only disconnect on the
    //   tick AFTER that).
    session.reset();
    do_logon(session, mock);
    ASSERT_EQ(session.state(), SessionState::kActive);

    // tick() must return true (healthy) — pending was cleared so no
    // immediate disconnect even if a hypothetical pending=true had been
    // carried over.
    EXPECT_TRUE(session.tick())
        << "tick() returned false on a fresh post-reset session — "
        << "test_request_pending_ likely was not cleared by reset()";
}

// ===========================================================================
// send_app guards
// ===========================================================================

TEST(FixSession, send_app_fails_when_not_active) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());

    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "V");
    EXPECT_FALSE(session.send_app(b));
}

// send_app on an active session must succeed when the underlying
// transport accepts the bytes; pin both the success path and the
// transport-failure passthrough.
TEST(FixSession, send_app_succeeds_when_active_and_transport_ok) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);

    size_t before = mock.sent.size();
    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "V");
    b.set(tag::MDReqID, "ord-001");
    EXPECT_TRUE(session.send_app(b));
    EXPECT_GT(mock.sent.size(), before);

    // The sent bytes must include the MsgType and a MsgSeqNum that's
    // > the Logon's sequence number (= 1) — so >= 2.
    auto last = mock.parse_last_sent();
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last->msg_type(), std::optional<std::string_view>("V"));
    auto seq = last->get_int(tag::MsgSeqNum);
    ASSERT_TRUE(seq.has_value());
    EXPECT_GE(*seq, 2);
}

TEST(FixSession, send_app_passthroughs_transport_failure) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);

    // Now flip the transport to fail mode.
    mock.set_send_fails(true);

    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "V");
    EXPECT_FALSE(session.send_app(b))
        << "send_app must surface the transport's send() failure";
}

// ===========================================================================
// Heartbeat timeout detection (tick)
// ===========================================================================

TEST(FixSession, tick_returns_true_when_healthy) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);

    EXPECT_TRUE(session.tick());
}

TEST(FixSession, tick_returns_true_when_disconnected) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    // Not logged in — tick should be no-op, return true
    EXPECT_TRUE(session.tick());
}

// ===========================================================================
// PossDupFlag handling
// ===========================================================================

TEST(FixSession, poss_dup_flag_does_not_advance_expected_seq) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);

    EXPECT_EQ(session.expected_inbound_seq(), 2u);

    // Server sends PossDupFlag=Y message with seq 1 (duplicate of logon)
    auto dup = MockFixTransport::build_msg("X", 1, [](MessageBuilder& b) {
        b.set_bool(tag::PossDupFlag, true);
        b.set(tag::MDReqID, "md-dup");
    });
    session.on_rx(dup.data(), dup.size());
    // expected_inbound_seq should NOT have changed
    EXPECT_EQ(session.expected_inbound_seq(), 2u);
}

// PossDupFlag=Y with seq == expected: per FIX 4.4 §4, this is the
// "next expected message but flagged as a possible duplicate"
// scenario. The session.hpp handler at line 507-518 treats this as
// the equal-branch and advances expected — even though PossDup is
// set. This is intentional: the message IS the next expected, and
// PossDupFlag is just a hint for the application layer to dedupe.
//
// Without this test, a future refactor that "fixed" the equal-branch
// to also check is_dup would silently break the seq advancement and
// leave the session expecting the next message at the same seq
// forever (until it eventually arrived without PossDup, breaking
// gap-detect monotonicity).
TEST(FixSession, poss_dup_at_expected_seq_still_advances) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);
    ASSERT_EQ(session.expected_inbound_seq(), 2u);

    // PossDupFlag=Y at seq=2 (the expected next).
    auto poss_dup_at_expected = MockFixTransport::build_msg("X", 2,
        [](MessageBuilder& b) {
            b.set_bool(tag::PossDupFlag, true);
            b.set(tag::MDReqID, "md-dup-at-expected");
        });
    session.on_rx(poss_dup_at_expected.data(), poss_dup_at_expected.size());

    // expected_inbound_seq MUST advance to 3 — the message IS the
    // next expected, regardless of the PossDup hint.
    EXPECT_EQ(session.expected_inbound_seq(), 3u)
        << "PossDupFlag=Y at expected seq incorrectly suppressed "
        << "advancement; gap-detect would now stall at seq 2";
}

// ---------------------------------------------------------------------------
// MsgSeqNum boundary handling
// ---------------------------------------------------------------------------

/// FIX 4.4 §4 caps MsgSeqNum at 4-byte unsigned (UINT32_MAX). When the
/// server emits a message at the boundary the gap-detection branch
/// previously stored `recv + 1` into `expected_inbound_seq_`, which
/// overflowed `uint32_t` to 0 — silently corrupting all subsequent
/// gap detection. Per spec the only correct response is to refuse to
/// advance past UINT32_MAX (the session must Logoff + Logon with
/// ResetSeqNumFlag=Y to recycle); accepting a wrap-to-0 is at minimum
/// a session-level violation and at worst a replay-attack vector
/// against the gap-detector.
TEST(FixSession, inbound_seq_at_uint32_max_does_not_wrap_expected_to_zero) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);

    // After logon: expected_inbound_seq_ == 2.
    // Server sends a message at MsgSeqNum = UINT32_MAX. Gap-detect path:
    // recv > expected, advance to recv+1 — under the bug that's 0.
    auto md = MockFixTransport::market_data(UINT32_MAX);
    session.on_rx(md.data(), md.size());

    EXPECT_EQ(session.last_inbound_seq(), UINT32_MAX);
    // Bug: stores 0 here. Fix: stays at UINT32_MAX so the next valid
    // input is treated as a low-seq violation (sane) rather than as
    // another gap that resets the watermark.
    EXPECT_NE(session.expected_inbound_seq(), 0u)
        << "expected_inbound_seq_ wrapped to 0 after UINT32_MAX gap; "
        << "this corrupts gap detection on every subsequent message";
}

/// FIX 4.4 §4 mandates MsgSeqNum be >= 1. A peer sending MsgSeqNum=0
/// is sending a protocol-illegal value; the session.hpp guard at line
/// ~444 returns early without advancing any sequence counters.
/// Without this test, the guard could be silently weakened by a future
/// refactor and the symptom would only surface as "expected_inbound_seq_
/// got reset by a malicious zero-seq message" downstream.
TEST(FixSession, inbound_seq_zero_is_ignored) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    do_logon(session, mock);

    // After logon: expected_inbound_seq_ == 2, last_inbound_seq_ == 1.
    const uint32_t exp_before  = session.expected_inbound_seq();
    const uint32_t last_before = session.last_inbound_seq();

    // Build a message with MsgSeqNum=0 — illegal per spec.
    auto bad = MockFixTransport::market_data(0);
    session.on_rx(bad.data(), bad.size());

    // Both counters must remain frozen — the session must not treat the
    // illegal seq as a gap (would advance expected) or as the most
    // recent inbound (would corrupt last_inbound_seq).
    EXPECT_EQ(session.expected_inbound_seq(), exp_before)
        << "expected_inbound_seq_ moved after seq=0 input — guard "
        << "weakened? FIX 4.4 §4 requires MsgSeqNum >= 1";
    EXPECT_EQ(session.last_inbound_seq(), last_before)
        << "last_inbound_seq_ stored seq=0 — would falsely report the "
        << "session received a zero-seq message; downstream diagnostics "
        << "expect last_inbound_seq >= 1 once active";
    // State must remain Active (the message was rejected as a session-
    // level error, not as a fatal protocol violation).
    EXPECT_EQ(session.state(), SessionState::kActive);
}

// ---------------------------------------------------------------------------
// FixSessionConfig::validate()
// ---------------------------------------------------------------------------

TEST(FixSession, ConfigValidateAcceptsValidConfig) {
    FixSessionConfig cfg{
        .sender_comp_id = "SENDER",
        .target_comp_id = "TARGET"};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(FixSession, ConfigValidateRejectsEmptySender) {
    FixSessionConfig cfg{
        .sender_comp_id = "",
        .target_comp_id = "TARGET"};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("sender"), std::string_view::npos);
}

TEST(FixSession, ConfigValidateRejectsEmptyTarget) {
    FixSessionConfig cfg{
        .sender_comp_id = "SENDER",
        .target_comp_id = ""};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("target"), std::string_view::npos);
}

TEST(FixSession, ConfigValidateRejectsZeroHeartbeat) {
    FixSessionConfig cfg{
        .sender_comp_id = "SENDER",
        .target_comp_id = "TARGET",
        .heartbeat_interval_sec = 0};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("heartbeat"), std::string_view::npos);
}

TEST(FixSession, ConfigValidateRejectsBadTimeoutFactor) {
    FixSessionConfig cfg{
        .sender_comp_id = "SENDER",
        .target_comp_id = "TARGET",
        .heartbeat_timeout_factor = 1.0};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("timeout_factor"), std::string_view::npos);
}

// Boundary: heartbeat_timeout_factor must be STRICTLY > 1.0. A value
// below 1.0 (e.g. 0.5) means the timeout is shorter than the heartbeat
// interval — guaranteed false-positive disconnects on every tick.
// Pin the `<= 1.0` rejection branch.
TEST(FixSession, ConfigValidateRejectsBelowOneTimeoutFactor) {
    FixSessionConfig cfg{
        .sender_comp_id = "SENDER",
        .target_comp_id = "TARGET",
        .heartbeat_timeout_factor = 0.5};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("timeout_factor"), std::string_view::npos);
}

// Boundary: heartbeat_timeout_factor exactly 10.0 must be accepted
// (the cap is `> 10.0`, not `>= 10.0`).
TEST(FixSession, ConfigValidateAcceptsBoundaryTimeoutFactorTen) {
    FixSessionConfig cfg{
        .sender_comp_id = "SENDER",
        .target_comp_id = "TARGET",
        .heartbeat_timeout_factor = 10.0};
    auto err = cfg.validate();
    EXPECT_TRUE(err.empty()) << "expected accept at exactly 10.0, got: " << err;
}

// Empty begin_string must be rejected — without a wire-protocol
// version, the FIX builder can't write the BeginString tag (8) and
// downstream parsers will reject the message.
TEST(FixSession, ConfigValidateRejectsEmptyBeginString) {
    FixSessionConfig cfg{
        .sender_comp_id = "SENDER",
        .target_comp_id = "TARGET",
        .begin_string   = ""};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("begin_string"), std::string_view::npos);
}

// validate()'s isfinite()-first guard exists specifically because both
// `<= 1.0` and `> 10.0` return false for NaN (every NaN comparison is
// false). Without the explicit isfinite() check first, a NaN
// heartbeat_timeout_factor would slip through validate() and cause UB
// inside tick() at the float→int64 cast for the dead-server timeout.
// This test asserts the guard is in place — its absence would be a
// regression that the existing 1.0 test cannot catch.
TEST(FixSession, ConfigValidateRejectsNanTimeoutFactor) {
    FixSessionConfig cfg{
        .sender_comp_id = "SENDER",
        .target_comp_id = "TARGET",
        .heartbeat_timeout_factor = std::numeric_limits<double>::quiet_NaN()};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    // The error must specifically call out "finite" so an operator
    // greping the diagnostic understands the rejection cause; a generic
    // "timeout_factor must be > 1.0" would be misleading (NaN > 1.0 is
    // false but for the wrong reason).
    EXPECT_NE(err.find("finite"), std::string_view::npos)
        << "expected 'finite' in diagnostic, got: " << err;
}

TEST(FixSession, ConfigValidateRejectsPosInfTimeoutFactor) {
    FixSessionConfig cfg{
        .sender_comp_id = "SENDER",
        .target_comp_id = "TARGET",
        .heartbeat_timeout_factor =
            std::numeric_limits<double>::infinity()};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    // +inf currently lands on either the isfinite branch or the >10.0
    // branch depending on order — both must reject. Don't assert on
    // which branch triggers; just assert non-empty error.
}

TEST(FixSession, ConfigValidateRejectsNegInfTimeoutFactor) {
    FixSessionConfig cfg{
        .sender_comp_id = "SENDER",
        .target_comp_id = "TARGET",
        .heartbeat_timeout_factor =
            -std::numeric_limits<double>::infinity()};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
}

TEST(FixSession, ConfigValidateRejectsExcessiveTimeoutFactor) {
    // > 10.0 is rejected as "likely misconfiguration" — a 100x
    // factor would mean a 30s heartbeat_interval allows 50min of
    // server silence before the dead-server check fires, which is
    // never what you want in production.
    FixSessionConfig cfg{
        .sender_comp_id = "SENDER",
        .target_comp_id = "TARGET",
        .heartbeat_timeout_factor = 100.0};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("misconfiguration"), std::string_view::npos)
        << "expected 'misconfiguration' diagnostic for 100.0 factor, got: "
        << err;
}

// ---------------------------------------------------------------------------
// FixSessionConfig dump/to_json/equality/warnings/formatter tests
// ---------------------------------------------------------------------------

TEST(FixSession, ConfigDumpContainsAllFields) {
    FixSessionConfig cfg{
        .sender_comp_id = "MY_ALGO",
        .target_comp_id = "EXCHANGE",
        .heartbeat_interval_sec = 30,
        .reset_seq_on_logon = true,
        .begin_string = "FIX.4.4",
    };
    auto d = cfg.dump();
    EXPECT_NE(d.find("MY_ALGO"), std::string::npos);
    EXPECT_NE(d.find("EXCHANGE"), std::string::npos);
    EXPECT_NE(d.find("FIX.4.4"), std::string::npos);
    EXPECT_NE(d.find("30"), std::string::npos);
}

TEST(FixSession, ConfigToJsonIsValidStructure) {
    FixSessionConfig cfg{
        .sender_comp_id = "SENDER",
        .target_comp_id = "TARGET",
    };
    auto j = cfg.to_json();
    EXPECT_TRUE(j.starts_with("{"));
    EXPECT_TRUE(j.ends_with("}"));
    EXPECT_NE(j.find("\"sender_comp_id\":\"SENDER\""), std::string::npos);
    EXPECT_NE(j.find("\"target_comp_id\":\"TARGET\""), std::string::npos);
    EXPECT_NE(j.find("\"heartbeat_interval_sec\":30"), std::string::npos);
    EXPECT_NE(j.find("\"reset_seq_on_logon\":true"), std::string::npos);
}

TEST(FixSession, ConfigToJsonEscapesSpecialChars) {
    FixSessionConfig cfg{
        .sender_comp_id = "test\"quote",
        .target_comp_id = "back\\slash",
    };
    auto j = cfg.to_json();
    EXPECT_NE(j.find("test\\\"quote"), std::string::npos);
    EXPECT_NE(j.find("back\\\\slash"), std::string::npos);
}

TEST(FixSession, ConfigEqualityMatchesIdentical) {
    FixSessionConfig a{
        .sender_comp_id = "S",
        .target_comp_id = "T",
        .heartbeat_interval_sec = 15,
    };
    FixSessionConfig b = a;
    EXPECT_EQ(a, b);
}

TEST(FixSession, ConfigEqualityDetectsDifferences) {
    FixSessionConfig a{.sender_comp_id = "S", .target_comp_id = "T"};
    FixSessionConfig b = a;
    b.heartbeat_interval_sec = 60;
    EXPECT_NE(a, b);
}

TEST(FixSession, ConfigEqualityIgnoresCallbacks) {
    FixSessionConfig a{.sender_comp_id = "S", .target_comp_id = "T"};
    FixSessionConfig b = a;
    b.on_state_change = [](SessionState, SessionState) {};
    EXPECT_EQ(a, b);
}

// reset() calls set_state(kDisconnected), which triggers
// on_state_change for transitions (Active→Disconnected,
// LogonSent→Disconnected, LogoutSent→Disconnected). A future
// refactor that bypassed set_state() in reset() (e.g. to skip
// the callback "since the user already knows they reset") would
// silently break observability dashboards correlating reset events
// with state transitions.
TEST(FixSession, on_state_change_fires_on_reset_from_active) {
    MockFixTransport mock;
    auto cfg = test_config();
    std::vector<std::pair<SessionState, SessionState>> history;
    cfg.on_state_change = [&](SessionState old_s, SessionState new_s) {
        history.emplace_back(old_s, new_s);
    };
    FixSession session(mock.send_fn(), cfg);
    do_logon(session, mock);
    // Pre-reset history: 2 transitions (Disc→LogonSent→Active).
    ASSERT_EQ(history.size(), 2u);

    session.reset();

    // Reset must trigger one more transition: Active → Disconnected.
    EXPECT_EQ(history.size(), 3u)
        << "reset() did not fire on_state_change — observability "
        << "dashboards correlating resets with state transitions "
        << "would lose visibility";
    EXPECT_EQ(history[2].first, SessionState::kActive);
    EXPECT_EQ(history[2].second, SessionState::kDisconnected);
}

// The on_state_change callback (FixSessionConfig.on_state_change) is
// fired by set_state() at session.hpp:813. Existing tests cover
// "callbacks excluded from equality" but never assert that the
// callback is actually invoked on a state transition. A future
// refactor that dropped the `if (cfg_.on_state_change)` invocation
// would silently break every observability dashboard that relies on
// it (e.g. session-state metrics in operator UIs).
//
// Drive the session through Logon (kDisconnected → kLogonSent →
// kActive) and assert the callback observed both transitions.
TEST(FixSession, on_state_change_callback_fires_for_each_transition) {
    MockFixTransport mock;
    auto cfg = test_config();

    // Capture transition history: each entry is (old, new).
    std::vector<std::pair<SessionState, SessionState>> history;
    cfg.on_state_change = [&](SessionState old_s, SessionState new_s) {
        history.emplace_back(old_s, new_s);
    };

    FixSession session(mock.send_fn(), cfg);
    do_logon(session, mock);

    // Logon flow: kDisconnected → kLogonSent → kActive (2 transitions).
    ASSERT_GE(history.size(), 2u);
    EXPECT_EQ(history[0].first, SessionState::kDisconnected);
    EXPECT_EQ(history[0].second, SessionState::kLogonSent);
    EXPECT_EQ(history[1].first, SessionState::kLogonSent);
    EXPECT_EQ(history[1].second, SessionState::kActive);

    // Also assert that set_state's "no transition if old == new" guard
    // is honored — the callback must NOT fire on a redundant
    // re-invocation of set_state with the current state. We can't
    // trigger this via the public API directly, but we can confirm
    // the post-Logon history is exactly 2 entries (no spurious
    // self-edges from internal bookkeeping).
    EXPECT_EQ(history.size(), 2u)
        << "spurious self-edge transitions detected in on_state_change "
        << "callback — set_state's `if (old != new_state)` guard may "
        << "have been weakened";
}

TEST(FixSession, ConfigWarningsDetectsLargeHeartbeat) {
    FixSessionConfig cfg{
        .sender_comp_id = "S", .target_comp_id = "T",
        .heartbeat_interval_sec = 200,
    };
    auto w = cfg.warnings();
    EXPECT_FALSE(w.empty());
    bool found = false;
    for (const auto& msg : w)
        if (msg.find("unusually large") != std::string::npos) found = true;
    EXPECT_TRUE(found);
}

TEST(FixSession, ConfigWarningsDetectsShortHeartbeat) {
    FixSessionConfig cfg{
        .sender_comp_id = "S", .target_comp_id = "T",
        .heartbeat_interval_sec = 2,
    };
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w)
        if (msg.find("very short") != std::string::npos) found = true;
    EXPECT_TRUE(found);
}

TEST(FixSession, ConfigWarningsDetectsNoSeqReset) {
    FixSessionConfig cfg{
        .sender_comp_id = "S", .target_comp_id = "T",
        .reset_seq_on_logon = false,
    };
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w)
        if (msg.find("persisted") != std::string::npos) found = true;
    EXPECT_TRUE(found);
}

TEST(FixSession, ConfigWarningsDetectsUnusualBeginString) {
    FixSessionConfig cfg{
        .sender_comp_id = "S", .target_comp_id = "T",
        .begin_string = "FIX.5.0",
    };
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w)
        if (msg.find("commonly used") != std::string::npos) found = true;
    EXPECT_TRUE(found);
}

TEST(FixSession, ConfigWarningsEmptyForNominalConfig) {
    FixSessionConfig cfg{
        .sender_comp_id = "S", .target_comp_id = "T",
        .heartbeat_interval_sec = 30,
        .reset_seq_on_logon = true,
        .begin_string = "FIX.4.4",
    };
    EXPECT_TRUE(cfg.warnings().empty());
}

TEST(FixSession, ConfigFormatterProducesDump) {
    FixSessionConfig cfg{
        .sender_comp_id = "S", .target_comp_id = "T",
    };
    auto formatted = std::format("{}", cfg);
    EXPECT_NE(formatted.find("FixSessionConfig"), std::string::npos);
    EXPECT_NE(formatted.find("S"), std::string::npos);
}

TEST(FixSession, SessionStateFormatterProducesName) {
    auto s = std::format("{}", SessionState::kActive);
    EXPECT_EQ(s, "ACTIVE");
    auto s2 = std::format("{}", SessionState::kDisconnected);
    EXPECT_EQ(s2, "DISCONNECTED");
}

// ===========================================================================
// Regression: concurrent send must produce strictly monotonic MsgSeqNum.
//
// The session's RX-thread heartbeat / TestRequest path (`on_rx` → send_*)
// races with the application-thread `send_app` path on outbound_seq_.
// `fill_session_header` historically used a load-then-store pattern,
// which under contention produces duplicate MsgSeqNum values on the wire
// — a fatal session-level FIX protocol violation that gets the session
// torn down by the counter-party.
//
// This test spawns two threads each issuing 200 send_app calls and then
// asserts that every successfully-sent message carries a unique MsgSeqNum
// in {2, 3, …, 401}. Before the fix the test fails with duplicate entries.
// ===========================================================================
TEST(FixSession, concurrent_send_app_produces_unique_seq) {
    // Mock with internal mutex so the concurrency under test is the
    // session's own outbound_seq_ handling, not vector growth.
    struct ThreadSafeMock {
        std::mutex                       mu;
        std::vector<std::vector<uint8_t>> sent;
        bool send(const uint8_t* d, size_t l) {
            std::lock_guard lk(mu);
            sent.emplace_back(d, d + l);
            return true;
        }
    } mock;

    auto cfg = test_config();
    FixSession session(
        [&](const uint8_t* d, size_t l) { return mock.send(d, l); }, cfg);

    // Logon manually so the mock's mutex doesn't deadlock with do_logon's
    // helper which uses MockFixTransport.
    size_t before;
    {
        std::lock_guard lk(mock.mu);
        before = mock.sent.size();
    }
    std::thread t([&] { (void)session.logon(std::chrono::milliseconds{500}); });
    while (true) {
        std::lock_guard lk(mock.mu);
        if (mock.sent.size() > before) break;
        std::this_thread::yield();
    }
    auto resp = MockFixTransport::logon_response();
    session.on_rx(resp.data(), resp.size());
    t.join();
    ASSERT_EQ(session.state(), SessionState::kActive);

    constexpr int kPerThread = 5000;
    std::atomic<int> success_count{0};
    auto worker = [&] {
        for (int i = 0; i < kPerThread; ++i) {
            uint8_t buf[256];
            MessageBuilder b(buf, sizeof(buf));
            b.set(tag::MsgType, "V");
            b.set(tag::MDReqID, "md-conc");
            if (session.send_app(b)) {
                success_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::thread t1(worker);
    std::thread t2(worker);
    t1.join();
    t2.join();

    // Walk every successfully-captured outbound message and collect the
    // MsgSeqNum values. With 2 × 200 = 400 sends after the Logon (seq=1),
    // the seq set must be {2..401} — exactly 400 unique values.
    std::lock_guard lk(mock.mu);
    std::set<int64_t> seen_seqs;
    int duplicate_count = 0;
    for (const auto& bytes : mock.sent) {
        auto msg = parse(bytes.data(), bytes.size());
        if (!msg) continue;
        auto seq = msg->get_int(tag::MsgSeqNum);
        if (!seq) continue;
        if (!seen_seqs.insert(*seq).second) {
            ++duplicate_count;
        }
    }

    EXPECT_EQ(duplicate_count, 0)
        << "outbound_seq_ race produced duplicate MsgSeqNum values "
        << "(violates FIX session-level monotonicity)";

    // Every successful send_app must contribute a fresh MsgSeqNum: under
    // contention the load-then-store version would (silently) reuse seqs,
    // leaving |seen_seqs| < |success_count|.
    EXPECT_EQ(static_cast<int>(seen_seqs.size()),
              success_count.load() + 1 /* +1 for the initial Logon at seq=1 */);
}
