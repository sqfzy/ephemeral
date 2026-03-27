#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "eph/fix.hpp"

using namespace eph::fix;

// ---------------------------------------------------------------------------
// Test helper: capture sent messages and simulate server responses
// ---------------------------------------------------------------------------

class MockFixTransport {
public:
    // Messages sent by FixSession
    std::vector<std::vector<uint8_t>> sent;

    // Send callback for FixSession
    bool send(const uint8_t* data, size_t len) {
        sent.emplace_back(data, data + len);
        return send_result_;
    }

    FixSession::SendFn send_fn() {
        return [this](const uint8_t* d, size_t l) { return send(d, l); };
    }

    void set_send_fails(bool fail) { send_result_ = !fail; }

    // Build a server Logon response
    std::vector<uint8_t> build_logon_response(uint32_t seq = 1) {
        uint8_t buf[512];
        MessageBuilder b(buf, sizeof(buf));
        b.set(tag::MsgType, "A");  // Logon
        b.set(tag::SenderCompID, "EXCHANGE");
        b.set(tag::TargetCompID, "CLIENT");
        b.set_int(tag::MsgSeqNum, static_cast<int64_t>(seq));
        b.set_int(tag::EncryptMethod, 0);
        b.set_int(tag::HeartBtInt, 30);
        size_t len = b.finish("FIX.4.4");
        return {buf, buf + len};
    }

    // Build a server Logout response
    std::vector<uint8_t> build_logout_response(uint32_t seq = 2) {
        uint8_t buf[256];
        MessageBuilder b(buf, sizeof(buf));
        b.set(tag::MsgType, "5");  // Logout
        b.set(tag::SenderCompID, "EXCHANGE");
        b.set(tag::TargetCompID, "CLIENT");
        b.set_int(tag::MsgSeqNum, static_cast<int64_t>(seq));
        size_t len = b.finish("FIX.4.4");
        return {buf, buf + len};
    }

    // Build a TestRequest
    std::vector<uint8_t> build_test_request(uint32_t seq, std::string_view test_req_id = "TR001") {
        uint8_t buf[256];
        MessageBuilder b(buf, sizeof(buf));
        b.set(tag::MsgType, "1");  // TestRequest
        b.set(tag::SenderCompID, "EXCHANGE");
        b.set(tag::TargetCompID, "CLIENT");
        b.set_int(tag::MsgSeqNum, static_cast<int64_t>(seq));
        b.set(tag::Text, test_req_id);
        size_t len = b.finish("FIX.4.4");
        return {buf, buf + len};
    }

    // Build a Heartbeat from server
    std::vector<uint8_t> build_heartbeat(uint32_t seq) {
        uint8_t buf[256];
        MessageBuilder b(buf, sizeof(buf));
        b.set(tag::MsgType, "0");
        b.set(tag::SenderCompID, "EXCHANGE");
        b.set(tag::TargetCompID, "CLIENT");
        b.set_int(tag::MsgSeqNum, static_cast<int64_t>(seq));
        size_t len = b.finish("FIX.4.4");
        return {buf, buf + len};
    }

    // Build an application message (MarketDataIncrementalRefresh)
    std::vector<uint8_t> build_market_data(uint32_t seq) {
        uint8_t buf[512];
        MessageBuilder b(buf, sizeof(buf));
        b.set(tag::MsgType, "X");  // MarketDataIncRefresh
        b.set(tag::SenderCompID, "EXCHANGE");
        b.set(tag::TargetCompID, "CLIENT");
        b.set_int(tag::MsgSeqNum, static_cast<int64_t>(seq));
        b.set(tag::MDReqID, "md-001");
        size_t len = b.finish("FIX.4.4");
        return {buf, buf + len};
    }

    // Parse the last sent message
    std::optional<MessageView> parse_last_sent() {
        if (sent.empty()) return std::nullopt;
        auto& last = sent.back();
        auto result = parse(last.data(), last.size());
        if (result) return *result;
        return std::nullopt;
    }

private:
    bool send_result_ = true;
};

static FixSessionConfig test_config() {
    return {
        .sender_comp_id = "CLIENT",
        .target_comp_id = "EXCHANGE",
        .heartbeat_interval_sec = 30,
        .reset_seq_on_logon = true,
    };
}

// ---------------------------------------------------------------------------
// State tests
// ---------------------------------------------------------------------------

TEST(FixSession, initial_state_is_disconnected) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());
    EXPECT_EQ(session.state(), SessionState::kDisconnected);
    EXPECT_EQ(session.next_outbound_seq(), 1u);
    EXPECT_EQ(session.last_inbound_seq(), 0u);
}

// ---------------------------------------------------------------------------
// Logon tests
// ---------------------------------------------------------------------------

TEST(FixSession, logon_sends_logon_message) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());

    // Start logon in background thread
    std::atomic<bool> logon_done{false};
    std::thread t([&] {
        auto result = session.logon(std::chrono::milliseconds{500});
        logon_done.store(true);
    });

    // Wait for Logon to be sent
    while (mock.sent.empty()) std::this_thread::yield();
    EXPECT_EQ(session.state(), SessionState::kLogonSent);

    // Verify sent Logon
    auto msg = mock.parse_last_sent();
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->msg_type(), std::optional<std::string_view>("A"));
    EXPECT_EQ(msg->get(tag::SenderCompID), std::optional<std::string_view>("CLIENT"));
    EXPECT_EQ(msg->get(tag::TargetCompID), std::optional<std::string_view>("EXCHANGE"));
    EXPECT_EQ(msg->get_int(tag::MsgSeqNum), std::optional<int64_t>(1));
    EXPECT_EQ(msg->get_int(tag::EncryptMethod), std::optional<int64_t>(0));
    EXPECT_EQ(msg->get_int(tag::HeartBtInt), std::optional<int64_t>(30));

    // Simulate server Logon response
    auto response = mock.build_logon_response();
    session.on_rx(response.data(), response.size());

    t.join();
    EXPECT_TRUE(logon_done.load());
    EXPECT_EQ(session.state(), SessionState::kActive);
    EXPECT_EQ(session.next_outbound_seq(), 2u);  // seq 1 used by Logon
    EXPECT_EQ(session.last_inbound_seq(), 1u);
}

TEST(FixSession, logon_timeout_returns_error) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());

    auto result = session.logon(std::chrono::milliseconds{50});
    EXPECT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().find("timeout") != std::string::npos);
    EXPECT_EQ(session.state(), SessionState::kDisconnected);
}

TEST(FixSession, logon_send_failure_returns_error) {
    MockFixTransport mock;
    mock.set_send_fails(true);
    FixSession session(mock.send_fn(), test_config());

    auto result = session.logon(std::chrono::milliseconds{100});
    EXPECT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().find("failed to send") != std::string::npos);
}

// ---------------------------------------------------------------------------
// on_rx dispatch tests
// ---------------------------------------------------------------------------

TEST(FixSession, on_rx_returns_true_for_session_messages) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());

    // Force to active state via logon
    std::thread t([&] { session.logon(std::chrono::milliseconds{500}); });
    while (mock.sent.empty()) std::this_thread::yield();
    auto logon_resp = mock.build_logon_response();
    session.on_rx(logon_resp.data(), logon_resp.size());
    t.join();

    // Heartbeat → session message → true
    auto hb = mock.build_heartbeat(2);
    EXPECT_TRUE(session.on_rx(hb.data(), hb.size()));

    // TestRequest → session message → true (and sends Heartbeat response)
    size_t sent_before = mock.sent.size();
    auto tr = mock.build_test_request(3);
    EXPECT_TRUE(session.on_rx(tr.data(), tr.size()));
    EXPECT_GT(mock.sent.size(), sent_before);  // Heartbeat response was sent
}

TEST(FixSession, on_rx_returns_false_for_app_messages) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());

    // Force active
    std::thread t([&] { session.logon(std::chrono::milliseconds{500}); });
    while (mock.sent.empty()) std::this_thread::yield();
    auto resp = mock.build_logon_response();
    session.on_rx(resp.data(), resp.size());
    t.join();

    // MarketData → application message → false
    auto md = mock.build_market_data(2);
    EXPECT_FALSE(session.on_rx(md.data(), md.size()));
    EXPECT_EQ(session.last_inbound_seq(), 2u);
}

// ---------------------------------------------------------------------------
// Sequence number tracking
// ---------------------------------------------------------------------------

TEST(FixSession, outbound_seq_increments_on_send) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());

    // Logon
    std::thread t([&] { session.logon(std::chrono::milliseconds{500}); });
    while (mock.sent.empty()) std::this_thread::yield();
    auto resp = mock.build_logon_response();
    session.on_rx(resp.data(), resp.size());
    t.join();

    EXPECT_EQ(session.next_outbound_seq(), 2u);  // Logon used seq 1

    // Send an app message
    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "V");  // MarketDataRequest
    b.set(tag::MDReqID, "md-001");
    bool ok = session.send_app(b);
    EXPECT_TRUE(ok);
    EXPECT_EQ(session.next_outbound_seq(), 3u);  // App msg used seq 2

    // Verify MsgSeqNum in sent message
    auto& last = mock.sent.back();
    auto msg = parse(last.data(), last.size());
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->get_int(tag::MsgSeqNum), std::optional<int64_t>(2));
}

TEST(FixSession, inbound_seq_tracks_server_messages) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());

    // Logon
    std::thread t([&] { session.logon(std::chrono::milliseconds{500}); });
    while (mock.sent.empty()) std::this_thread::yield();
    auto resp = mock.build_logon_response(1);
    session.on_rx(resp.data(), resp.size());
    t.join();

    EXPECT_EQ(session.last_inbound_seq(), 1u);

    auto hb = mock.build_heartbeat(2);
    session.on_rx(hb.data(), hb.size());
    EXPECT_EQ(session.last_inbound_seq(), 2u);

    auto md = mock.build_market_data(3);
    session.on_rx(md.data(), md.size());
    EXPECT_EQ(session.last_inbound_seq(), 3u);
}

// ---------------------------------------------------------------------------
// Logout tests
// ---------------------------------------------------------------------------

TEST(FixSession, logout_sends_logout_message) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());

    // Logon first
    std::thread t([&] { session.logon(std::chrono::milliseconds{500}); });
    while (mock.sent.empty()) std::this_thread::yield();
    session.on_rx(mock.build_logon_response().data(), mock.build_logon_response().size());
    t.join();

    // Logout
    std::thread t2([&] { session.logout(std::chrono::milliseconds{500}); });
    // Wait for Logout to be sent
    while (mock.sent.size() < 2) std::this_thread::yield();

    auto logout_resp = mock.build_logout_response(2);
    session.on_rx(logout_resp.data(), logout_resp.size());
    t2.join();

    EXPECT_EQ(session.state(), SessionState::kDisconnected);
}

TEST(FixSession, server_initiated_logout_sends_response) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());

    // Logon
    std::thread t([&] { session.logon(std::chrono::milliseconds{500}); });
    while (mock.sent.empty()) std::this_thread::yield();
    session.on_rx(mock.build_logon_response().data(), mock.build_logon_response().size());
    t.join();

    // Server sends Logout (unsolicited)
    size_t sent_before = mock.sent.size();
    auto srv_logout = mock.build_logout_response(2);
    session.on_rx(srv_logout.data(), srv_logout.size());

    // Session should have sent Logout response and moved to disconnected
    EXPECT_GT(mock.sent.size(), sent_before);
    EXPECT_EQ(session.state(), SessionState::kDisconnected);
}

// ---------------------------------------------------------------------------
// Reset tests
// ---------------------------------------------------------------------------

TEST(FixSession, reset_allows_relogon) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());

    // First logon
    std::thread t1([&] { session.logon(std::chrono::milliseconds{500}); });
    while (mock.sent.empty()) std::this_thread::yield();
    session.on_rx(mock.build_logon_response().data(), mock.build_logon_response().size());
    t1.join();
    EXPECT_EQ(session.state(), SessionState::kActive);

    // Simulate disconnect
    session.reset();
    EXPECT_EQ(session.state(), SessionState::kDisconnected);
    EXPECT_EQ(session.next_outbound_seq(), 1u);

    // Second logon
    std::thread t2([&] { session.logon(std::chrono::milliseconds{500}); });
    while (mock.sent.size() < 2) std::this_thread::yield();
    session.on_rx(mock.build_logon_response().data(), mock.build_logon_response().size());
    t2.join();
    EXPECT_EQ(session.state(), SessionState::kActive);
}

// ---------------------------------------------------------------------------
// send_app rejects when not active
// ---------------------------------------------------------------------------

TEST(FixSession, send_app_fails_when_not_active) {
    MockFixTransport mock;
    FixSession session(mock.send_fn(), test_config());

    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "V");
    EXPECT_FALSE(session.send_app(b));
}
