#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
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

TEST(FixSession, logon_send_failure) {
    MockFixTransport mock;
    mock.set_send_fails(true);
    FixSession session(mock.send_fn(), test_config());
    auto r = session.logon(std::chrono::milliseconds{100});
    EXPECT_FALSE(r.has_value());
}

// ===========================================================================
// Server HeartBtInt override
// ===========================================================================

TEST(FixSession, server_heartbeat_interval_override) {
    MockFixTransport mock;
    auto cfg = test_config();
    cfg.heartbeat_interval_sec = 30;
    FixSession session(mock.send_fn(), cfg);

    std::thread t([&] { (void)session.logon(std::chrono::milliseconds{500}); });
    while (mock.sent.empty()) std::this_thread::yield();
    // Server responds with HeartBtInt=10 (overrides client's 30)
    auto resp = MockFixTransport::logon_response(1, 10);
    session.on_rx(resp.data(), resp.size());
    t.join();
    EXPECT_EQ(session.state(), SessionState::kActive);
    // Can't directly read cfg_.heartbeat_interval_sec, but tick() behavior will change
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
