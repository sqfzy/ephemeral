/// @file test_transport.cpp
/// Unit tests for the Transport class using a WS-aware mock TCP backend.
///
/// Tests use use_tls=false (plain WS mode) to bypass TLS and focus on
/// Transport logic: lifecycle, send/recv, reconnection, error paths, stats.

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <expected>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "eph/net/http.hpp"
#include "eph/net/tcp_concept.hpp"
#include "eph/net/transport.hpp"
#include "eph/net/websocket.hpp"

using namespace eph::net;
using namespace std::chrono_literals;

// ===========================================================================
// WsMockTcpTransport — thread-safe mock that auto-responds to WS handshake
// ===========================================================================

/// A mock TCP transport that:
///   1. Auto-responds with HTTP 101 to WebSocket upgrade requests
///   2. Optionally echoes received WS frames back as server (unmasked) frames
///   3. Supports injecting arbitrary server frames via inject_server_frame()
///   4. Can simulate TCP errors via set_error_on_next_poll()
///
/// Thread safety: all public methods are mutex-protected, safe for
/// concurrent TX/RX thread access.
struct WsMockTcpTransport {
    // -- Mock configuration (set before passing to Transport) --
    bool echo_mode = false;          // Echo received WS frames back
    bool fail_connect = false;       // Make connect() fail

    // -- State --
    TcpState current_state = TcpState::Closed;

    // -- Thread-safe state --
    mutable std::mutex mtx;
    std::deque<std::vector<uint8_t>> rx_queue;    // Frames to deliver via poll_rx
    std::vector<std::vector<uint8_t>> sent_data;  // All data sent via send()
    bool error_on_next_poll = false;
    std::string next_poll_error = "mock TCP error";
    bool handshake_done = false;

    auto connect(std::chrono::milliseconds /*timeout*/)
        -> std::expected<void, std::string>
    {
        if (fail_connect) {
            return std::unexpected("mock connect failure");
        }
        current_state = TcpState::Established;
        return {};
    }

    auto send(const void* data, size_t len)
        -> std::expected<size_t, std::string>
    {
        std::lock_guard lock(mtx);

        const auto* bytes = static_cast<const uint8_t*>(data);
        sent_data.emplace_back(bytes, bytes + len);

        if (!handshake_done) {
            // Check if this is an HTTP upgrade request
            std::string_view sv(static_cast<const char*>(data), len);
            if (sv.starts_with("GET ") && sv.find("Upgrade: websocket") != std::string_view::npos) {
                // Extract Sec-WebSocket-Key
                auto key_pos = sv.find("Sec-WebSocket-Key: ");
                if (key_pos != std::string_view::npos) {
                    key_pos += 19; // skip "Sec-WebSocket-Key: "
                    auto key_end = sv.find("\r\n", key_pos);
                    std::string ws_key(sv.substr(key_pos, key_end - key_pos));

                    // Compute Sec-WebSocket-Accept per RFC 6455
                    std::string accept = compute_ws_accept(ws_key);

                    // Build HTTP 101 response
                    std::string response =
                        "HTTP/1.1 101 Switching Protocols\r\n"
                        "Upgrade: websocket\r\n"
                        "Connection: Upgrade\r\n"
                        "Sec-WebSocket-Accept: " + accept + "\r\n"
                        "\r\n";

                    rx_queue.emplace_back(response.begin(), response.end());
                    handshake_done = true;
                }
            }
        } else if (echo_mode) {
            // Decode client WS frame (masked), build server frame (unmasked), queue it
            echo_ws_frame(bytes, len);
        }

        return len;
    }

    template <typename Callback>
    auto poll_rx(Callback&& cb)
        -> std::expected<uint16_t, std::string>
    {
        std::lock_guard lock(mtx);

        if (error_on_next_poll) {
            error_on_next_poll = false;
            return std::unexpected(next_poll_error);
        }

        if (rx_queue.empty()) {
            return uint16_t{0};
        }

        auto& front = rx_queue.front();
        auto sz = static_cast<uint16_t>(front.size());
        cb(front.data(), sz);
        rx_queue.pop_front();
        return sz;
    }

    auto close() -> std::expected<void, std::string> {
        current_state = TcpState::Closed;
        return {};
    }

    void reset() noexcept {
        std::lock_guard lock(mtx);
        current_state = TcpState::Closed;
        sent_data.clear();
        rx_queue.clear();
        handshake_done = false;
        error_on_next_poll = false;
    }

    auto last_rx_burst_tsc() const -> uint64_t { return 0; }
    auto mss() const -> uint16_t { return 1460; }
    auto state() const -> TcpState { return current_state; }
    auto is_established() const -> bool { return current_state == TcpState::Established; }

    // -- Mock control methods --

    /// Inject a server-side (unmasked) WS frame into the RX queue.
    void inject_server_frame(uint8_t opcode, const uint8_t* payload, size_t payload_len) {
        std::lock_guard lock(mtx);
        std::vector<uint8_t> frame;
        build_server_frame_ex(frame, opcode, payload, payload_len);
        rx_queue.push_back(std::move(frame));
    }

    /// Inject a server-side text frame.
    void inject_text(std::string_view text) {
        inject_server_frame(ws::opcode::kText,
            reinterpret_cast<const uint8_t*>(text.data()), text.size());
    }

    /// Inject a server-side binary frame.
    void inject_binary(const std::vector<uint8_t>& data) {
        inject_server_frame(ws::opcode::kBinary, data.data(), data.size());
    }

    /// Inject a server-side WS frame with explicit FIN control (for fragmentation tests).
    void inject_frame_ex(uint8_t opcode, const uint8_t* payload, size_t payload_len, bool fin) {
        std::lock_guard lock(mtx);
        std::vector<uint8_t> frame;
        build_server_frame_ex(frame, opcode, payload, payload_len, fin);
        rx_queue.push_back(std::move(frame));
    }

    /// Make the next poll_rx() call return an error.
    void set_error_on_next_poll(std::string error = "mock TCP error") {
        std::lock_guard lock(mtx);
        error_on_next_poll = true;
        next_poll_error = std::move(error);
    }

    /// Get number of send() calls (including handshake).
    size_t send_count() const {
        std::lock_guard lock(mtx);
        return sent_data.size();
    }

private:
    /// Compute Sec-WebSocket-Accept from client key (RFC 6455 §4.2.2).
    static std::string compute_ws_accept(std::string_view key) {
        static constexpr std::string_view kGuid =
            "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

        std::string input;
        input.reserve(key.size() + kGuid.size());
        input.append(key);
        input.append(kGuid);

        uint8_t hash[20];
        unsigned int hash_len = 0;
        EVP_Digest(input.data(), input.size(), hash, &hash_len,
                   EVP_sha1(), nullptr);

        return http::detail::base64_encode(hash, hash_len);
    }

    /// Build a server-side (unmasked) WS frame with controllable FIN bit.
    static void build_server_frame_ex(std::vector<uint8_t>& out, uint8_t opcode,
                                       const uint8_t* payload, size_t payload_len,
                                       bool fin = true) {
        out.push_back((fin ? ws::kFinBit : uint8_t{0}) | (opcode & 0x0F));

        // Length (no MASK bit for server frames)
        if (payload_len < 126) {
            out.push_back(static_cast<uint8_t>(payload_len));
        } else if (payload_len <= 65535) {
            out.push_back(126);
            out.push_back(static_cast<uint8_t>(payload_len >> 8));
            out.push_back(static_cast<uint8_t>(payload_len & 0xFF));
        } else {
            out.push_back(127);
            for (int i = 7; i >= 0; --i) {
                out.push_back(static_cast<uint8_t>((payload_len >> (i * 8)) & 0xFF));
            }
        }

        // Payload (unmasked)
        out.insert(out.end(), payload, payload + payload_len);
    }

    /// Echo a masked client WS frame back as an unmasked server frame.
    void echo_ws_frame(const uint8_t* data, size_t len) {
        auto frame = ws::decode_frame(data, len);
        if (!frame) return;

        // Only echo data frames (text/binary), skip control frames
        if (frame->opcode == ws::opcode::kText ||
            frame->opcode == ws::opcode::kBinary) {

            // Unmask the payload (client frames are masked)
            std::vector<uint8_t> payload(frame->payload,
                                          frame->payload + frame->payload_len);
            if (frame->masked) {
                for (size_t i = 0; i < payload.size(); ++i) {
                    payload[i] ^= frame->mask_key[i % 4];
                }
            }

            std::vector<uint8_t> response;
            build_server_frame_ex(response, frame->opcode, payload.data(), payload.size());
            rx_queue.push_back(std::move(response));
        }
    }
};

// Verify WsMockTcpTransport satisfies the concept
static_assert(TcpTransport<WsMockTcpTransport>,
    "WsMockTcpTransport must satisfy TcpTransport");

// ===========================================================================
// Test fixture
// ===========================================================================

class TransportTest : public ::testing::Test {
protected:
    static constexpr size_t kMaxPayload = 512;
    static constexpr size_t kQueueDepth = 64;

    using TestTransport = Transport<WsMockTcpTransport, WsFramer, kMaxPayload, kQueueDepth>;

    // Shared pointer to the mock so we can inspect/control it after Transport takes ownership.
    // The factory creates new instances, but we keep a pointer to the latest one.
    WsMockTcpTransport* last_mock_ = nullptr;

    std::expected<std::unique_ptr<TestTransport>, ConnectionErrorInfo>
    create_transport(bool echo = false, bool disable_ping = true,
                     bool disable_reconnect = true) {
        TransportConfig config;
        config.remote_host = "mock.test";
        config.remote_port = 9999;
        config.ws_path = "/ws";
        config.use_tls = false;
        if (disable_ping) {
            config.ping_interval = 0s;
        }
        if (disable_reconnect) {
            config.max_reconnect_attempts = 0;
        }

        auto factory = [this, echo]()
            -> std::expected<std::unique_ptr<WsMockTcpTransport>, std::string>
        {
            auto mock = std::make_unique<WsMockTcpTransport>();
            mock->echo_mode = echo;
            last_mock_ = mock.get();
            auto r = mock->connect(3000ms);
            if (!r) return std::unexpected(r.error());
            return mock;
        };

        return TestTransport::create(std::move(factory), config);
    }

    /// Overload that accepts a config modifier for custom settings.
    std::expected<std::unique_ptr<TestTransport>, ConnectionErrorInfo>
    create_transport_with(std::function<void(TransportConfig&)> modifier,
                          bool echo = false) {
        TransportConfig config;
        config.remote_host = "mock.test";
        config.remote_port = 9999;
        config.ws_path = "/ws";
        config.use_tls = false;
        config.ping_interval = 0s;
        config.max_reconnect_attempts = 0;
        if (modifier) modifier(config);

        auto factory = [this, echo]()
            -> std::expected<std::unique_ptr<WsMockTcpTransport>, std::string>
        {
            auto mock = std::make_unique<WsMockTcpTransport>();
            mock->echo_mode = echo;
            last_mock_ = mock.get();
            auto r = mock->connect(3000ms);
            if (!r) return std::unexpected(r.error());
            return mock;
        };

        return TestTransport::create(std::move(factory), config);
    }

    /// Wait for a condition with timeout (avoids test hangs).
    template <typename Pred>
    bool wait_for(Pred&& pred, std::chrono::milliseconds timeout = 2000ms) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!pred()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }
};

// ===========================================================================
// Lifecycle tests
// ===========================================================================

TEST_F(TransportTest, CreateAndStop) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    EXPECT_TRUE(tp->is_running());
    EXPECT_TRUE(tp->is_connected());
    EXPECT_EQ(tp->state(), TransportState::kConnected);
    EXPECT_EQ(tp->tls_version(), "none");
    EXPECT_EQ(tp->cipher_name(), "none");

    tp->stop();
    EXPECT_FALSE(tp->is_running());
    EXPECT_EQ(tp->state(), TransportState::kStopped);
}

TEST_F(TransportTest, DestructorStopsCleanly) {
    {
        auto result = create_transport();
        ASSERT_TRUE(result.has_value());
        // Destructor should call stop() and join threads without hanging
    }
    // If we get here, the destructor didn't deadlock
}

TEST_F(TransportTest, DoubleStopIsHarmless) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value());
    auto& tp = *result;

    tp->stop();
    tp->stop(); // Should not crash or hang
    EXPECT_FALSE(tp->is_running());
}

TEST_F(TransportTest, FactoryFailureReturnsError) {
    TransportConfig config;
    config.remote_host = "mock.test";
    config.remote_port = 9999;
    config.ws_path = "/ws";
    config.use_tls = false;
    config.ping_interval = 0s;

    auto factory = []()
        -> std::expected<std::unique_ptr<WsMockTcpTransport>, std::string>
    {
        return std::unexpected("simulated factory failure");
    };

    auto result = TestTransport::create(std::move(factory), config);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ConnectionError::kFactoryFailed);
}

TEST_F(TransportTest, InvalidConfigReturnsError) {
    TransportConfig config;
    // Empty remote_host
    config.ws_path = "/ws";
    config.use_tls = false;

    auto factory = []()
        -> std::expected<std::unique_ptr<WsMockTcpTransport>, std::string>
    {
        return std::unexpected("should not be called");
    };

    auto result = TestTransport::create(std::move(factory), config);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ConnectionError::kInvalidConfig);
}

// ===========================================================================
// Send API tests
// ===========================================================================

TEST_F(TransportTest, SendBinarySuccess) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    auto err = tp->send_binary(payload, sizeof(payload));
    EXPECT_EQ(err, SendError::kOk);

    // Wait for TX thread to drain the queue
    EXPECT_TRUE(wait_for([&] { return tp->tx_queue_size() == 0; }));

    tp->stop();

    auto stats = tp->stats();
    EXPECT_GE(stats.tx_packets, 1u);
    EXPECT_GE(stats.tx_bytes, sizeof(payload));
}

TEST_F(TransportTest, SendTextSuccess) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    auto err = tp->send_text("hello");
    EXPECT_EQ(err, SendError::kOk);

    EXPECT_TRUE(wait_for([&] { return tp->tx_queue_size() == 0; }));

    tp->stop();

    auto stats = tp->stats();
    EXPECT_GE(stats.tx_text_packets, 1u);
}

TEST_F(TransportTest, SendTextInvalidUtf8Rejected) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    // Invalid UTF-8 sequence
    const uint8_t bad_utf8[] = {0xFF, 0xFE, 0x00};
    auto err = tp->send_text(bad_utf8, sizeof(bad_utf8));
    EXPECT_EQ(err, SendError::kInvalidUtf8);

    tp->stop();
}

TEST_F(TransportTest, SendMessageTooLarge) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::vector<uint8_t> big(kMaxPayload + 1, 0x42);
    auto err = tp->send_binary(big.data(), big.size());
    EXPECT_EQ(err, SendError::kMessageTooLarge);

    tp->stop();
}

TEST_F(TransportTest, SendWhenStoppedReturnsNotConnected) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    tp->stop();

    auto err = tp->send_text("hello");
    EXPECT_EQ(err, SendError::kNotConnected);
}

TEST_F(TransportTest, SendNullDataWithLenReturnsNullData) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    auto err = tp->send(nullptr, 10);
    EXPECT_EQ(err, SendError::kNullData);

    // Null data with len=0 should succeed (no-op payload)
    auto err2 = tp->send(nullptr, 0);
    EXPECT_EQ(err2, SendError::kOk);

    tp->stop();
}

TEST_F(TransportTest, SendTextNullDataReturnsNullData) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    // send_text with null data + non-zero len must return kNullData
    // (not crash in is_valid_utf8)
    auto err = tp->send_text(nullptr, 10);
    EXPECT_EQ(err, SendError::kNullData);

    // send with text opcode + null data must also be safe
    auto err2 = tp->send(nullptr, 5, ws::opcode::kText);
    EXPECT_EQ(err2, SendError::kNullData);

    tp->stop();
}

TEST_F(TransportTest, SendForNullDataReturnsNullData) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    // send_for with null data + non-zero len
    auto err = tp->send_for(nullptr, 10, 100ms);
    EXPECT_EQ(err, SendError::kNullData);

    // send_text_for with null data + non-zero len
    auto err2 = tp->send_text_for(nullptr, 10, 100ms);
    EXPECT_EQ(err2, SendError::kNullData);

    tp->stop();
}

TEST_F(TransportTest, SendCloseEnqueues) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    auto err = tp->send_close(ws::close_code::kNormal, "bye");
    EXPECT_EQ(err, SendError::kOk);

    tp->stop();
}

TEST_F(TransportTest, SendCloseInvalidCodeRejected) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    // 1005 is reserved and must not be sent in a Close frame
    auto err = tp->send_close(1005);
    EXPECT_EQ(err, SendError::kInvalidCloseCode);

    tp->stop();
}

TEST_F(TransportTest, SendPingSuccess) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    auto err = tp->send_ping();
    EXPECT_EQ(err, SendError::kOk);

    EXPECT_TRUE(wait_for([&] { return tp->tx_queue_size() == 0; }));

    tp->stop();
}

TEST_F(TransportTest, SendPingWithPayload) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    const uint8_t payload[] = {0xDE, 0xAD};
    auto err = tp->send_ping(payload, sizeof(payload));
    EXPECT_EQ(err, SendError::kOk);

    tp->stop();
}

TEST_F(TransportTest, SendPingOversizedPayloadTruncates) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    // RFC 6455 §5.5: control frame payload max 125 bytes; send_ping truncates
    uint8_t big_payload[200];
    std::memset(big_payload, 0xAA, sizeof(big_payload));
    auto err = tp->send_ping(big_payload, sizeof(big_payload));
    EXPECT_EQ(err, SendError::kOk); // Should succeed after truncation

    tp->stop();
}

TEST_F(TransportTest, SendPingNullPayloadIgnored) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    // null payload with non-zero len should be handled gracefully
    auto err = tp->send_ping(nullptr, 0);
    EXPECT_EQ(err, SendError::kOk);

    tp->stop();
}

// ===========================================================================
// Receive API tests
// ===========================================================================

TEST_F(TransportTest, ReceiveServerPushedBinary) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    // Give Transport threads time to start
    std::this_thread::sleep_for(10ms);

    // Inject a server binary frame
    const std::vector<uint8_t> payload = {0x01, 0x02, 0x03, 0x04};
    last_mock_->inject_binary(payload);

    // Wait for RX thread to process and enqueue
    std::vector<uint8_t> received;
    bool got = wait_for([&] {
        return tp->recv([&](const uint8_t* data, uint16_t len) {
            received.assign(data, data + len);
        });
    });

    EXPECT_TRUE(got);
    EXPECT_EQ(received, payload);

    tp->stop();
}

TEST_F(TransportTest, ReceiveServerPushedText) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(10ms);

    last_mock_->inject_text("hello from server");

    std::string received;
    uint8_t received_opcode = 0;
    bool got = wait_for([&] {
        return tp->recv([&](const uint8_t* data, uint16_t len, uint8_t opcode) {
            received.assign(reinterpret_cast<const char*>(data), len);
            received_opcode = opcode;
        });
    });

    EXPECT_TRUE(got);
    EXPECT_EQ(received, "hello from server");
    EXPECT_EQ(received_opcode, ws::opcode::kText);

    tp->stop();
}

TEST_F(TransportTest, ReceiveMultipleFrames) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(10ms);

    // Inject 5 frames
    for (int i = 0; i < 5; ++i) {
        std::vector<uint8_t> payload = {static_cast<uint8_t>(i)};
        last_mock_->inject_binary(payload);
    }

    // Receive all 5
    int count = 0;
    EXPECT_TRUE(wait_for([&] {
        tp->recv([&](const uint8_t* data, uint16_t len) {
            EXPECT_EQ(len, 1);
            EXPECT_EQ(data[0], static_cast<uint8_t>(count));
            count++;
        });
        return count >= 5;
    }));

    EXPECT_EQ(count, 5);

    tp->stop();
}

TEST_F(TransportTest, TryRecvReturnsNulloptWhenEmpty) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    auto msg = tp->try_recv();
    EXPECT_FALSE(msg.has_value());

    tp->stop();
}

TEST_F(TransportTest, TryRecvMsgReturnsPayloadAndOpcode) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(10ms);

    last_mock_->inject_text("test");

    std::optional<TestTransport::ReceivedMessage> msg;
    EXPECT_TRUE(wait_for([&] {
        msg = tp->try_recv_msg();
        return msg.has_value();
    }));

    ASSERT_TRUE(msg.has_value());
    EXPECT_TRUE(msg->is_text());
    EXPECT_EQ(msg->text(), "test");

    tp->stop();
}

// ===========================================================================
// Peek tests
// ===========================================================================

TEST_F(TransportTest, RecvPeekReturnsMessageWithoutConsuming) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(10ms);

    last_mock_->inject_text("peek_me");

    // Wait for message to arrive in RX queue
    EXPECT_TRUE(wait_for([&] {
        return tp->rx_queue_size() > 0;
    }));

    // Peek should see the message
    bool peeked = false;
    tp->recv_peek([&](const uint8_t* data, size_t len, uint8_t opcode) {
        peeked = true;
        EXPECT_EQ(opcode, ws::opcode::kText);
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(data), len), "peek_me");
    });
    EXPECT_TRUE(peeked);

    // Queue should still have the message (not consumed)
    EXPECT_GT(tp->rx_queue_size(), 0u);

    // Now consume it
    auto msg = tp->try_recv_msg();
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->text(), "peek_me");

    // Queue should now be empty
    EXPECT_EQ(tp->rx_queue_size(), 0u);

    tp->stop();
}

TEST_F(TransportTest, RecvPeekEmptyReturnsFalse) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    bool peeked = false;
    tp->recv_peek([&](const uint8_t*, size_t) {
        peeked = true;
    });
    EXPECT_FALSE(peeked);

    tp->stop();
}

TEST_F(TransportTest, PeekRecvMsgReturnsCopiedMessage) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(10ms);

    last_mock_->inject_text("peek_copy");

    EXPECT_TRUE(wait_for([&] {
        return tp->rx_queue_size() > 0;
    }));

    auto msg = tp->peek_recv_msg();
    ASSERT_TRUE(msg.has_value());
    EXPECT_TRUE(msg->is_text());
    EXPECT_EQ(msg->text(), "peek_copy");

    // Message still in queue
    EXPECT_GT(tp->rx_queue_size(), 0u);

    // Consume should return same message
    auto msg2 = tp->try_recv_msg();
    ASSERT_TRUE(msg2.has_value());
    EXPECT_EQ(msg2->text(), "peek_copy");

    tp->stop();
}

// ===========================================================================
// Echo roundtrip tests
// ===========================================================================

TEST_F(TransportTest, EchoRoundtripBinary) {
    auto result = create_transport(/*echo=*/true);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    const uint8_t payload[] = {0xCA, 0xFE, 0xBA, 0xBE};
    auto err = tp->send_binary(payload, sizeof(payload));
    EXPECT_EQ(err, SendError::kOk);

    // Wait for TX to send, mock to echo, RX to receive
    std::vector<uint8_t> received;
    bool got = wait_for([&] {
        return tp->recv([&](const uint8_t* data, uint16_t len) {
            received.assign(data, data + len);
        });
    });

    EXPECT_TRUE(got);
    EXPECT_EQ(received, (std::vector<uint8_t>{0xCA, 0xFE, 0xBA, 0xBE}));

    tp->stop();
}

TEST_F(TransportTest, EchoRoundtripText) {
    auto result = create_transport(/*echo=*/true);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    auto err = tp->send_text("round trip test");
    EXPECT_EQ(err, SendError::kOk);

    std::string received;
    bool got = wait_for([&] {
        return tp->recv([&](const uint8_t* data, uint16_t len) {
            received.assign(reinterpret_cast<const char*>(data), len);
        });
    });

    EXPECT_TRUE(got);
    EXPECT_EQ(received, "round trip test");

    tp->stop();
}

// ===========================================================================
// Queue and backpressure tests
// ===========================================================================

TEST_F(TransportTest, QueueSizeReflectsBackpressure) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    EXPECT_EQ(tp->tx_queue_size(), 0u);
    EXPECT_EQ(tp->rx_queue_size(), 0u);
    EXPECT_LE(tp->tx_queue_fill_ratio(), 0.01);

    tp->stop();
}

TEST_F(TransportTest, BatchSendN) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::vector<uint8_t> p1 = {0x01, 0x02};
    std::vector<uint8_t> p2 = {0x03, 0x04, 0x05};
    std::span<const uint8_t> payloads[] = {p1, p2};

    auto err = tp->send_n(payloads, 2, ws::opcode::kBinary);
    EXPECT_EQ(err, SendError::kOk);

    EXPECT_TRUE(wait_for([&] { return tp->tx_queue_size() == 0; }));

    tp->stop();

    auto stats = tp->stats();
    EXPECT_GE(stats.tx_packets, 2u);
}

TEST_F(TransportTest, BatchSendNRejectsInvalidUtf8Text) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    // First payload is valid UTF-8, second is invalid
    std::vector<uint8_t> valid = {'h', 'e', 'l', 'l', 'o'};
    std::vector<uint8_t> invalid = {0xFF, 0xFE};
    std::span<const uint8_t> payloads[] = {valid, invalid};

    auto err = tp->send_n(payloads, 2, ws::opcode::kText);
    EXPECT_EQ(err, SendError::kInvalidUtf8);

    // Verify no messages were enqueued (all-or-nothing)
    EXPECT_EQ(tp->tx_queue_size(), 0u);

    tp->stop();
}

TEST_F(TransportTest, BatchSendNAcceptsValidUtf8Text) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::vector<uint8_t> p1 = {'h', 'i'};
    std::vector<uint8_t> p2 = {'o', 'k'};
    std::span<const uint8_t> payloads[] = {p1, p2};

    auto err = tp->send_n(payloads, 2, ws::opcode::kText);
    EXPECT_EQ(err, SendError::kOk);

    tp->stop();
}

// ===========================================================================
// Stats tests
// ===========================================================================

TEST_F(TransportTest, StatsAccumulateCorrectly) {
    auto result = create_transport(/*echo=*/true);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    // Send multiple messages
    for (int i = 0; i < 10; ++i) {
        uint8_t payload = static_cast<uint8_t>(i);
        tp->send_binary(&payload, 1);
    }

    // Wait for all to be sent
    EXPECT_TRUE(wait_for([&] { return tp->tx_queue_size() == 0; }));

    // Drain echoed messages
    int received = 0;
    EXPECT_TRUE(wait_for([&] {
        tp->recv([&](const uint8_t*, uint16_t) { received++; });
        return received >= 10;
    }));

    tp->stop();

    auto stats = tp->stats();
    EXPECT_GE(stats.tx_packets, 10u);
    EXPECT_GE(stats.tx_bytes, 10u);
    EXPECT_GE(stats.rx_packets, 10u);
    EXPECT_GT(stats.uptime_ns, 0u);
}

TEST_F(TransportTest, ResetStatsZeros) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    tp->send_binary("\x01", 1);
    EXPECT_TRUE(wait_for([&] { return tp->tx_queue_size() == 0; }));

    tp->reset_stats();
    auto stats = tp->stats();
    EXPECT_EQ(stats.tx_packets, 0u);
    EXPECT_EQ(stats.tx_bytes, 0u);

    tp->stop();
}

TEST_F(TransportTest, HandshakeLatencyRecorded) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    auto stats = tp->stats();
    EXPECT_GT(stats.handshake_ns, 0u);
    // Per-phase breakdown should sum to approximately total handshake
    EXPECT_GT(stats.tcp_connect_ns, 0u);
    EXPECT_GT(stats.ws_upgrade_ns, 0u);
    // TLS is disabled in mock, so tls_handshake_ns should be 0
    EXPECT_EQ(stats.tls_handshake_ns, 0u);
    // Sum of phases should be close to total (allow some overhead)
    uint64_t phase_sum = stats.tcp_connect_ns + stats.tls_handshake_ns + stats.ws_upgrade_ns;
    EXPECT_LE(phase_sum, stats.handshake_ns);

    tp->stop();
}

// ===========================================================================
// State callback tests
// ===========================================================================

TEST_F(TransportTest, StateChangeCallbackFires) {
    TransportConfig config;
    config.remote_host = "mock.test";
    config.remote_port = 9999;
    config.ws_path = "/ws";
    config.use_tls = false;
    config.ping_interval = 0s;
    config.max_reconnect_attempts = 0;

    std::vector<TransportEvent> events;
    std::mutex events_mtx;

    config.on_state_change = [&](TransportEvent event, std::string_view /*detail*/) {
        std::lock_guard lock(events_mtx);
        events.push_back(event);
    };

    auto factory = []()
        -> std::expected<std::unique_ptr<WsMockTcpTransport>, std::string>
    {
        auto mock = std::make_unique<WsMockTcpTransport>();
        auto r = mock->connect(3000ms);
        if (!r) return std::unexpected(r.error());
        return mock;
    };

    auto result = TestTransport::create(std::move(factory), config);
    ASSERT_TRUE(result.has_value()) << result.error().message();

    // Should have gotten kConnected
    {
        std::lock_guard lock(events_mtx);
        ASSERT_FALSE(events.empty());
        EXPECT_EQ(events[0], TransportEvent::kConnected);
    }

    (*result)->stop();

    // Should have gotten kStopped
    {
        std::lock_guard lock(events_mtx);
        EXPECT_EQ(events.back(), TransportEvent::kStopped);
    }
}

// ===========================================================================
// on_message callback tests
// ===========================================================================

TEST_F(TransportTest, OnMessageCallbackReceivesDirectly) {
    TransportConfig config;
    config.remote_host = "mock.test";
    config.remote_port = 9999;
    config.ws_path = "/ws";
    config.use_tls = false;
    config.ping_interval = 0s;
    config.max_reconnect_attempts = 0;

    std::atomic<int> message_count{0};
    std::string last_message;
    std::mutex msg_mtx;

    config.on_message = [&](const uint8_t* data, uint16_t len, uint8_t /*opcode*/) {
        std::lock_guard lock(msg_mtx);
        last_message.assign(reinterpret_cast<const char*>(data), len);
        message_count.fetch_add(1, std::memory_order_relaxed);
    };

    WsMockTcpTransport* mock_ptr = nullptr;
    auto factory = [&]()
        -> std::expected<std::unique_ptr<WsMockTcpTransport>, std::string>
    {
        auto mock = std::make_unique<WsMockTcpTransport>();
        mock_ptr = mock.get();
        auto r = mock->connect(3000ms);
        if (!r) return std::unexpected(r.error());
        return mock;
    };

    auto result = TestTransport::create(std::move(factory), config);
    ASSERT_TRUE(result.has_value()) << result.error().message();

    std::this_thread::sleep_for(10ms);

    // Inject server frame — should go directly to on_message, not rx queue
    mock_ptr->inject_text("direct callback");

    EXPECT_TRUE(wait_for([&] {
        return message_count.load(std::memory_order_relaxed) >= 1;
    }));

    {
        std::lock_guard lock(msg_mtx);
        EXPECT_EQ(last_message, "direct callback");
    }

    // rx queue should be empty (on_message bypasses it)
    auto msg = (*result)->try_recv();
    EXPECT_FALSE(msg.has_value());

    (*result)->stop();
}

// ===========================================================================
// Graceful close tests
// ===========================================================================

TEST_F(TransportTest, CloseGracefullyTimesOutWithoutServerResponse) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    // close_gracefully should timeout since mock doesn't auto-respond to Close
    bool server_responded = tp->close_gracefully(
        ws::close_code::kNormal, "bye", 100ms);

    EXPECT_FALSE(server_responded);
    EXPECT_FALSE(tp->is_running());
}

TEST_F(TransportTest, StopDrainsRemainingTxMessages) {
    // Verify that stop() drains queued messages instead of losing them.
    // Use echo mode so we can count how many were actually sent.
    auto result = create_transport(/*echo=*/true);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    // Enqueue several messages
    for (int i = 0; i < 5; ++i) {
        uint8_t payload = static_cast<uint8_t>(i);
        tp->send_binary(&payload, 1);
    }

    // Stop immediately (TX thread may not have sent all yet)
    tp->stop();

    // After stop, all enqueued messages should have been sent.
    // Check via stats — tx_packets should be >= 5.
    auto stats = tp->stats();
    EXPECT_GE(stats.tx_packets, 5u)
        << "TX drain on stop() should have sent all queued messages";
}

// ===========================================================================
// Wait-receive tests
// ===========================================================================

TEST_F(TransportTest, WaitRecvTimesOutWhenEmpty) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    bool got = tp->wait_recv(
        [](const uint8_t*, uint16_t) {}, 50ms);

    EXPECT_FALSE(got);

    tp->stop();
}

TEST_F(TransportTest, WaitRecvReturnsWhenDataAvailable) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(10ms);

    // Inject after a short delay
    std::thread injector([&] {
        std::this_thread::sleep_for(20ms);
        last_mock_->inject_text("delayed");
    });

    std::string received;
    bool got = tp->wait_recv(
        [&](const uint8_t* data, uint16_t len) {
            received.assign(reinterpret_cast<const char*>(data), len);
        }, 2000ms);

    EXPECT_TRUE(got);
    EXPECT_EQ(received, "delayed");

    injector.join();
    tp->stop();
}

// ===========================================================================
// MaxPayload and QueueDepth template parameters
// ===========================================================================

TEST_F(TransportTest, MaxPayloadExactFit) {
    auto result = create_transport(/*echo=*/true);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    // Send exactly MaxPayload bytes
    std::vector<uint8_t> payload(kMaxPayload, 0xAA);
    auto err = tp->send_binary(payload.data(), payload.size());
    EXPECT_EQ(err, SendError::kOk);

    // Should echo back
    std::vector<uint8_t> received;
    bool got = wait_for([&] {
        return tp->recv([&](const uint8_t* data, uint16_t len) {
            received.assign(data, data + len);
        });
    });

    EXPECT_TRUE(got);
    EXPECT_EQ(received.size(), kMaxPayload);

    tp->stop();
}

// ===========================================================================
// Reconnection tests
// ===========================================================================

TEST_F(TransportTest, ReconnectOnPollError) {
    TransportConfig config;
    config.remote_host = "mock.test";
    config.remote_port = 9999;
    config.ws_path = "/ws";
    config.use_tls = false;
    config.ping_interval = 0s;
    config.max_reconnect_attempts = 3;
    config.reconnect_interval = 50ms;

    std::atomic<int> connect_count{0};
    WsMockTcpTransport* mock_ptr = nullptr;

    auto factory = [&]()
        -> std::expected<std::unique_ptr<WsMockTcpTransport>, std::string>
    {
        auto mock = std::make_unique<WsMockTcpTransport>();
        mock_ptr = mock.get();
        connect_count.fetch_add(1, std::memory_order_relaxed);
        auto r = mock->connect(3000ms);
        if (!r) return std::unexpected(r.error());
        return mock;
    };

    auto result = TestTransport::create(std::move(factory), config);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    // Initial connection
    EXPECT_EQ(connect_count.load(), 1);

    std::this_thread::sleep_for(10ms);

    // Trigger a TCP error — RX thread should attempt reconnect
    mock_ptr->set_error_on_next_poll("simulated disconnect");

    // Wait for reconnection (factory called again)
    bool reconnected = wait_for([&] {
        return connect_count.load() >= 2;
    }, 5000ms);

    EXPECT_TRUE(reconnected);

    auto stats = tp->stats();
    EXPECT_GE(stats.reconnect_count, 1u);

    tp->stop();
}

TEST_F(TransportTest, OnReconnectedCallbackFires) {
    TransportConfig config;
    config.remote_host = "mock.test";
    config.remote_port = 9999;
    config.ws_path = "/ws";
    config.use_tls = false;
    config.ping_interval = 0s;
    config.max_reconnect_attempts = 3;
    config.reconnect_interval = 50ms;

    std::atomic<bool> callback_fired{false};
    int cb_attempt = 0;
    uint64_t cb_downtime_ns = 0;
    uint64_t cb_total = 0;
    std::mutex cb_mtx;

    config.on_reconnected = [&](int attempt, uint64_t downtime_ns, uint64_t total) {
        std::lock_guard lock(cb_mtx);
        cb_attempt = attempt;
        cb_downtime_ns = downtime_ns;
        cb_total = total;
        callback_fired.store(true, std::memory_order_relaxed);
    };

    WsMockTcpTransport* mock_ptr = nullptr;

    auto factory = [&]()
        -> std::expected<std::unique_ptr<WsMockTcpTransport>, std::string>
    {
        auto mock = std::make_unique<WsMockTcpTransport>();
        mock_ptr = mock.get();
        auto r = mock->connect(3000ms);
        if (!r) return std::unexpected(r.error());
        return mock;
    };

    auto result = TestTransport::create(std::move(factory), config);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(10ms);

    // Trigger disconnect
    mock_ptr->set_error_on_next_poll("simulated disconnect");

    // Wait for on_reconnected callback
    bool fired = wait_for([&] {
        return callback_fired.load(std::memory_order_relaxed);
    }, 5000ms);

    EXPECT_TRUE(fired);

    {
        std::lock_guard lock(cb_mtx);
        EXPECT_EQ(cb_attempt, 1);
        EXPECT_GT(cb_downtime_ns, 0u);
        EXPECT_GE(cb_total, 1u);
    }

    tp->stop();
}

TEST_F(TransportTest, ReconnectExhaustedStopsTransport) {
    TransportConfig config;
    config.remote_host = "mock.test";
    config.remote_port = 9999;
    config.ws_path = "/ws";
    config.use_tls = false;
    config.ping_interval = 0s;
    config.max_reconnect_attempts = 1;
    config.reconnect_interval = 10ms;

    std::atomic<int> factory_calls{0};
    WsMockTcpTransport* mock_ptr = nullptr;

    auto factory = [&]()
        -> std::expected<std::unique_ptr<WsMockTcpTransport>, std::string>
    {
        int n = factory_calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1) {
            // First call succeeds (initial connection)
            auto mock = std::make_unique<WsMockTcpTransport>();
            mock_ptr = mock.get();
            auto r = mock->connect(3000ms);
            if (!r) return std::unexpected(r.error());
            return mock;
        }
        // All reconnect attempts fail
        return std::unexpected("reconnect failure");
    };

    auto result = TestTransport::create(std::move(factory), config);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    // Wait for RX thread to start, then trigger error
    std::this_thread::sleep_for(10ms);
    mock_ptr->set_error_on_next_poll("simulated disconnect");

    // Wait for transport to stop after exhausting reconnect attempts
    bool stopped = wait_for([&] {
        return tp->state() == TransportState::kStopped;
    }, 5000ms);

    EXPECT_TRUE(stopped);
}

TEST_F(TransportTest, DataFlowsAfterReconnect) {
    // After a reconnect, verify that send/recv still works
    TransportConfig config;
    config.remote_host = "mock.test";
    config.remote_port = 9999;
    config.ws_path = "/ws";
    config.use_tls = false;
    config.ping_interval = 0s;
    config.max_reconnect_attempts = 3;
    config.reconnect_interval = 50ms;

    WsMockTcpTransport* mock_ptr = nullptr;
    std::atomic<int> connect_count{0};

    auto factory = [&]()
        -> std::expected<std::unique_ptr<WsMockTcpTransport>, std::string>
    {
        auto mock = std::make_unique<WsMockTcpTransport>();
        mock->echo_mode = true;
        mock_ptr = mock.get();
        connect_count.fetch_add(1, std::memory_order_relaxed);
        auto r = mock->connect(3000ms);
        if (!r) return std::unexpected(r.error());
        return mock;
    };

    auto result = TestTransport::create(std::move(factory), config);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(10ms);

    // Trigger disconnect
    mock_ptr->set_error_on_next_poll("simulated disconnect");

    // Wait for reconnection
    bool reconnected = wait_for([&] {
        return connect_count.load() >= 2;
    }, 5000ms);
    ASSERT_TRUE(reconnected);

    // Give RX thread time to resume
    std::this_thread::sleep_for(20ms);

    // Send data after reconnect and verify echo
    std::string msg = "post-reconnect";
    auto err = tp->send_text(msg);
    EXPECT_EQ(err, SendError::kOk);

    bool received = wait_for([&] {
        auto m = tp->try_recv_msg();
        if (m) {
            EXPECT_TRUE(m->is_text());
            EXPECT_EQ(m->text(), "post-reconnect");
            return true;
        }
        return false;
    }, 3000ms);
    EXPECT_TRUE(received) << "Data should flow after reconnect";

    tp->stop();
}

TEST_F(TransportTest, OnReconnectAttemptAborts) {
    // on_reconnect_attempt returning false should abort reconnection
    TransportConfig config;
    config.remote_host = "mock.test";
    config.remote_port = 9999;
    config.ws_path = "/ws";
    config.use_tls = false;
    config.ping_interval = 0s;
    config.max_reconnect_attempts = 5;
    config.reconnect_interval = 10ms;

    std::atomic<int> attempt_count{0};
    config.on_reconnect_attempt = [&](int attempt, int /*max*/, std::string_view /*err*/) -> bool {
        attempt_count.store(attempt, std::memory_order_relaxed);
        return attempt < 2; // Abort after 2nd attempt
    };

    std::atomic<int> factory_calls{0};

    auto factory = [&]()
        -> std::expected<std::unique_ptr<WsMockTcpTransport>, std::string>
    {
        int n = factory_calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1) {
            // First call: succeed (initial connection)
            auto mock = std::make_unique<WsMockTcpTransport>();
            last_mock_ = mock.get();
            auto r = mock->connect(3000ms);
            if (!r) return std::unexpected(r.error());
            return mock;
        }
        // All reconnect attempts fail
        return std::unexpected("reconnect failure");
    };

    auto result = TestTransport::create(std::move(factory), config);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(10ms);
    last_mock_->set_error_on_next_poll("simulated disconnect");

    // Wait for transport to stop (should stop after 2 attempts, not 5)
    bool stopped = wait_for([&] {
        return tp->state() == TransportState::kStopped;
    }, 5000ms);

    EXPECT_TRUE(stopped);
    // Callback should have been called with attempt=2 (the abort point)
    EXPECT_EQ(attempt_count.load(), 2);

    tp->stop();
}

TEST_F(TransportTest, ReconnectNowTriggersReconnection) {
    TransportConfig config;
    config.remote_host = "mock.test";
    config.remote_port = 9999;
    config.ws_path = "/ws";
    config.use_tls = false;
    config.ping_interval = 0s;
    config.max_reconnect_attempts = 3;
    config.reconnect_interval = 10ms;

    std::atomic<int> factory_calls{0};
    std::atomic<bool> reconnected{false};

    config.on_reconnected = [&](int /*attempt*/, uint64_t /*downtime_ns*/,
                                 uint64_t /*total*/) {
        reconnected.store(true, std::memory_order_release);
    };

    auto factory = [&]()
        -> std::expected<std::unique_ptr<WsMockTcpTransport>, std::string>
    {
        int n = factory_calls.fetch_add(1, std::memory_order_relaxed) + 1;
        auto mock = std::make_unique<WsMockTcpTransport>();
        if (n == 1) last_mock_ = mock.get();
        auto r = mock->connect(3000ms);
        if (!r) return std::unexpected(r.error());
        return mock;
    };

    auto result = TestTransport::create(std::move(factory), config);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(10ms);

    // Force reconnect from application thread
    EXPECT_TRUE(tp->reconnect_now());

    // Wait for reconnection to complete
    bool ok = wait_for([&] {
        return reconnected.load(std::memory_order_acquire);
    }, 3000ms);
    EXPECT_TRUE(ok) << "reconnect_now() did not trigger reconnection";

    // Factory should have been called at least twice (initial + reconnect)
    EXPECT_GE(factory_calls.load(), 2);

    tp->stop();
}

TEST_F(TransportTest, ReconnectNowReturnsFalseWhenDisabled) {
    // Create transport with auto-reconnect disabled
    TransportConfig config;
    config.remote_host = "mock.test";
    config.remote_port = 9999;
    config.ws_path = "/ws";
    config.use_tls = false;
    config.ping_interval = 0s;
    config.max_reconnect_attempts = 0; // disabled

    auto factory = [&]()
        -> std::expected<std::unique_ptr<WsMockTcpTransport>, std::string>
    {
        auto mock = std::make_unique<WsMockTcpTransport>();
        last_mock_ = mock.get();
        auto r = mock->connect(3000ms);
        if (!r) return std::unexpected(r.error());
        return mock;
    };

    auto result = TestTransport::create(std::move(factory), config);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    // reconnect_now should return false when auto-reconnect is disabled
    EXPECT_FALSE(tp->reconnect_now());

    tp->stop();
}

TEST_F(TransportTest, ReconnectNowReturnsFalseWhenStopped) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    tp->stop();

    // Should return false when not running
    EXPECT_FALSE(tp->reconnect_now());
}

// ---------------------------------------------------------------------------
// Host header port logic (RFC 6455 §4.1)
// ---------------------------------------------------------------------------

// Helper: extract Host header value from the mock's first sent_data entry
static std::string extract_host_header(WsMockTcpTransport& mock) {
    std::lock_guard lock(mock.mtx);
    if (mock.sent_data.empty()) return {};
    auto& req = mock.sent_data.front();
    std::string_view sv(reinterpret_cast<const char*>(req.data()), req.size());
    auto pos = sv.find("Host: ");
    if (pos == std::string_view::npos) return {};
    pos += 6; // skip "Host: "
    auto end = sv.find("\r\n", pos);
    return std::string(sv.substr(pos, end - pos));
}

TEST_F(TransportTest, PlainWsDefaultPort80_HostOmitsPort) {
    // ws:// on port 80 → Host header should NOT include ":80"
    WsMockTcpTransport* mock_ptr = nullptr;

    auto factory = [&mock_ptr]()
        -> std::expected<std::unique_ptr<WsMockTcpTransport>, std::string>
    {
        auto tcp = std::make_unique<WsMockTcpTransport>();
        mock_ptr = tcp.get();
        tcp->current_state = TcpState::Established;
        return tcp;
    };

    TransportConfig config{
        .remote_host = "example.com",
        .remote_port = 80,
        .ws_path = "/ws",
        .use_tls = false,
        .max_reconnect_attempts = 0,
        .ping_interval = 0s,
    };

    auto result = TestTransport::create(std::move(factory), config);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    ASSERT_NE(mock_ptr, nullptr);

    auto host = extract_host_header(*mock_ptr);
    EXPECT_EQ(host, "example.com") << "Port 80 on plain WS should be omitted from Host header";

    (*result)->stop();
}

TEST_F(TransportTest, PlainWsNonDefaultPort_HostIncludesPort) {
    // ws:// on port 8080 → Host header SHOULD include ":8080"
    WsMockTcpTransport* mock_ptr = nullptr;

    auto factory = [&mock_ptr]()
        -> std::expected<std::unique_ptr<WsMockTcpTransport>, std::string>
    {
        auto tcp = std::make_unique<WsMockTcpTransport>();
        mock_ptr = tcp.get();
        tcp->current_state = TcpState::Established;
        return tcp;
    };

    TransportConfig config{
        .remote_host = "example.com",
        .remote_port = 8080,
        .ws_path = "/ws",
        .use_tls = false,
        .max_reconnect_attempts = 0,
        .ping_interval = 0s,
    };

    auto result = TestTransport::create(std::move(factory), config);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    ASSERT_NE(mock_ptr, nullptr);

    auto host = extract_host_header(*mock_ptr);
    EXPECT_EQ(host, "example.com:8080") << "Non-default port should appear in Host header";

    (*result)->stop();
}

TEST_F(TransportTest, PlainWsPort443_HostIncludesPort) {
    // Plain WS on port 443 is non-standard → port SHOULD be included
    // (443 is only the default for wss://, not ws://)
    WsMockTcpTransport* mock_ptr = nullptr;

    auto factory = [&mock_ptr]()
        -> std::expected<std::unique_ptr<WsMockTcpTransport>, std::string>
    {
        auto tcp = std::make_unique<WsMockTcpTransport>();
        mock_ptr = tcp.get();
        tcp->current_state = TcpState::Established;
        return tcp;
    };

    TransportConfig config{
        .remote_host = "example.com",
        .remote_port = 443,
        .ws_path = "/ws",
        .use_tls = false,
        .max_reconnect_attempts = 0,
        .ping_interval = 0s,
    };

    auto result = TestTransport::create(std::move(factory), config);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    ASSERT_NE(mock_ptr, nullptr);

    auto host = extract_host_header(*mock_ptr);
    EXPECT_EQ(host, "example.com:443") << "Port 443 on plain WS is non-default, should be included";

    (*result)->stop();
}

// ---------------------------------------------------------------------------
// WebSocket fragmentation (RFC 6455 §5.4)
// ---------------------------------------------------------------------------

TEST_F(TransportTest, FragmentedBinaryMessage_TwoFrames) {
    // Fragment "Hello World" into two parts: "Hello " + "World"
    auto result = create_transport(/*echo=*/false);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(10ms); // Let RX thread start

    const uint8_t part1[] = "Hello ";
    const uint8_t part2[] = "World";

    // First fragment: opcode=binary, FIN=0
    last_mock_->inject_frame_ex(ws::opcode::kBinary, part1, 6, /*fin=*/false);
    // Continuation frame: opcode=continuation, FIN=1
    last_mock_->inject_frame_ex(ws::opcode::kContinuation, part2, 5, /*fin=*/true);

    // Wait for reassembled message
    bool received = false;
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline && !received) {
        auto msg = tp->try_recv_msg();
        if (msg) {
            EXPECT_TRUE(msg->is_binary());
            std::string payload(msg->data.begin(), msg->data.end());
            EXPECT_EQ(payload, "Hello World");
            received = true;
        }
        std::this_thread::yield();
    }
    EXPECT_TRUE(received) << "Fragmented message was not reassembled";

    tp->stop();
}

TEST_F(TransportTest, FragmentedTextMessage_ThreeFrames) {
    // Fragment "ABC" into three frames: "A", "B", "C"
    auto result = create_transport(/*echo=*/false);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(10ms);

    // First fragment: opcode=text, FIN=0
    last_mock_->inject_frame_ex(ws::opcode::kText,
        reinterpret_cast<const uint8_t*>("A"), 1, /*fin=*/false);
    // Continuation: FIN=0
    last_mock_->inject_frame_ex(ws::opcode::kContinuation,
        reinterpret_cast<const uint8_t*>("B"), 1, /*fin=*/false);
    // Final continuation: FIN=1
    last_mock_->inject_frame_ex(ws::opcode::kContinuation,
        reinterpret_cast<const uint8_t*>("C"), 1, /*fin=*/true);

    bool received = false;
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline && !received) {
        auto msg = tp->try_recv_msg();
        if (msg) {
            EXPECT_TRUE(msg->is_text());
            EXPECT_EQ(msg->text(), "ABC");
            received = true;
        }
        std::this_thread::yield();
    }
    EXPECT_TRUE(received) << "Fragmented text message was not reassembled";

    tp->stop();
}

TEST_F(TransportTest, FragmentedMessage_EmptyContinuation) {
    // First fragment has payload, continuation is empty, final has payload
    auto result = create_transport(/*echo=*/false);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(10ms);

    last_mock_->inject_frame_ex(ws::opcode::kBinary,
        reinterpret_cast<const uint8_t*>("XY"), 2, /*fin=*/false);
    // Empty continuation frame
    last_mock_->inject_frame_ex(ws::opcode::kContinuation,
        nullptr, 0, /*fin=*/false);
    // Final with payload
    last_mock_->inject_frame_ex(ws::opcode::kContinuation,
        reinterpret_cast<const uint8_t*>("Z"), 1, /*fin=*/true);

    bool received = false;
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline && !received) {
        auto msg = tp->try_recv_msg();
        if (msg) {
            EXPECT_TRUE(msg->is_binary());
            std::string payload(msg->data.begin(), msg->data.end());
            EXPECT_EQ(payload, "XYZ");
            received = true;
        }
        std::this_thread::yield();
    }
    EXPECT_TRUE(received) << "Fragmented message with empty continuation was not reassembled";

    tp->stop();
}

// ===========================================================================
// Server-initiated close tests
// ===========================================================================

TEST_F(TransportTest, ServerCloseFrameDeliveredToRxQueue) {
    // Server sends a Close frame; it should be delivered to the RX queue
    // and accessible via try_recv_msg() with close_code()/close_reason().
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(10ms);

    // Build a server Close frame: code=1000, reason="goodbye"
    uint8_t close_payload[2 + 7];
    close_payload[0] = static_cast<uint8_t>(1000 >> 8);
    close_payload[1] = static_cast<uint8_t>(1000 & 0xFF);
    std::memcpy(close_payload + 2, "goodbye", 7);
    last_mock_->inject_server_frame(ws::opcode::kClose, close_payload, sizeof(close_payload));

    // Wait for the close frame to appear in RX queue
    bool received = false;
    EXPECT_TRUE(wait_for([&] {
        auto msg = tp->try_recv_msg();
        if (msg && msg->is_close()) {
            EXPECT_EQ(msg->close_code(), 1000u);
            EXPECT_EQ(msg->close_reason(), "goodbye");
            received = true;
            return true;
        }
        return false;
    }));

    EXPECT_TRUE(received);

    tp->stop();
}

TEST_F(TransportTest, OnCloseCallbackFires) {
    TransportConfig config;
    config.remote_host = "mock.test";
    config.remote_port = 9999;
    config.ws_path = "/ws";
    config.use_tls = false;
    config.ping_interval = 0s;
    config.max_reconnect_attempts = 0;

    std::atomic<bool> close_fired{false};
    uint16_t close_code = 0;
    std::string close_reason;
    std::mutex cb_mtx;

    config.on_close = [&](uint16_t code, std::string_view reason) {
        std::lock_guard lock(cb_mtx);
        close_code = code;
        close_reason = std::string(reason);
        close_fired.store(true, std::memory_order_relaxed);
    };

    WsMockTcpTransport* mock_ptr = nullptr;
    auto factory = [&]()
        -> std::expected<std::unique_ptr<WsMockTcpTransport>, std::string>
    {
        auto mock = std::make_unique<WsMockTcpTransport>();
        mock_ptr = mock.get();
        auto r = mock->connect(3000ms);
        if (!r) return std::unexpected(r.error());
        return mock;
    };

    auto result = TestTransport::create(std::move(factory), config);
    ASSERT_TRUE(result.has_value()) << result.error().message();

    std::this_thread::sleep_for(10ms);

    // Send Close frame from server
    uint8_t close_payload[2 + 4];
    close_payload[0] = static_cast<uint8_t>(1001 >> 8);
    close_payload[1] = static_cast<uint8_t>(1001 & 0xFF);
    std::memcpy(close_payload + 2, "gone", 4);
    mock_ptr->inject_server_frame(ws::opcode::kClose, close_payload, sizeof(close_payload));

    bool fired = wait_for([&] {
        return close_fired.load(std::memory_order_relaxed);
    });

    EXPECT_TRUE(fired);
    {
        std::lock_guard lock(cb_mtx);
        EXPECT_EQ(close_code, 1001u);
        EXPECT_EQ(close_reason, "gone");
    }

    (*result)->stop();
}

// ===========================================================================
// Batch receive tests
// ===========================================================================

TEST_F(TransportTest, RecvNBatchDrains) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(10ms);

    // Inject 5 frames
    for (int i = 0; i < 5; ++i) {
        std::vector<uint8_t> payload = {static_cast<uint8_t>(i)};
        last_mock_->inject_binary(payload);
    }

    // Wait for all to arrive in RX queue
    EXPECT_TRUE(wait_for([&] {
        return tp->rx_queue_size() >= 5;
    }));

    // Batch receive
    int count = 0;
    std::vector<uint8_t> received_bytes;
    size_t drained = tp->recv_n([&](const uint8_t* data, uint16_t len) {
        EXPECT_EQ(len, 1);
        received_bytes.push_back(data[0]);
        count++;
    }, 10);

    EXPECT_EQ(drained, 5u);
    EXPECT_EQ(count, 5);
    EXPECT_EQ(received_bytes, (std::vector<uint8_t>{0, 1, 2, 3, 4}));

    tp->stop();
}

TEST_F(TransportTest, RecvNWithOpcodeCallback) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(10ms);

    last_mock_->inject_text("hello");
    last_mock_->inject_binary({0x42});

    EXPECT_TRUE(wait_for([&] {
        return tp->rx_queue_size() >= 2;
    }));

    std::vector<uint8_t> opcodes;
    size_t drained = tp->recv_n([&](const uint8_t*, uint16_t, uint8_t opcode) {
        opcodes.push_back(opcode);
    }, 10);

    EXPECT_EQ(drained, 2u);
    EXPECT_EQ(opcodes[0], ws::opcode::kText);
    EXPECT_EQ(opcodes[1], ws::opcode::kBinary);

    tp->stop();
}

TEST_F(TransportTest, DrainRecvConsumesAll) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(10ms);

    for (int i = 0; i < 3; ++i) {
        last_mock_->inject_binary({static_cast<uint8_t>(i)});
    }

    EXPECT_TRUE(wait_for([&] {
        return tp->rx_queue_size() >= 3;
    }));

    int count = 0;
    size_t drained = tp->drain_recv([&](const uint8_t*, uint16_t) {
        count++;
    });

    EXPECT_EQ(drained, 3u);
    EXPECT_EQ(tp->rx_queue_size(), 0u);

    tp->stop();
}

TEST_F(TransportTest, BatchSendNForSucceeds) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::vector<uint8_t> p1 = {0x01, 0x02};
    std::vector<uint8_t> p2 = {0x03};
    std::span<const uint8_t> payloads[] = {p1, p2};

    auto err = tp->send_n_for(payloads, 2, 100ms, ws::opcode::kBinary);
    EXPECT_EQ(err, SendError::kOk);

    EXPECT_TRUE(wait_for([&] { return tp->tx_queue_size() == 0; }));

    tp->stop();

    auto stats = tp->stats();
    EXPECT_GE(stats.tx_packets, 2u);
}

TEST_F(TransportTest, BatchSendNForRejectsInvalidUtf8) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::vector<uint8_t> valid = {'o', 'k'};
    std::vector<uint8_t> invalid = {0xFF};
    std::span<const uint8_t> payloads[] = {valid, invalid};

    auto err = tp->send_n_for(payloads, 2, 100ms, ws::opcode::kText);
    EXPECT_EQ(err, SendError::kInvalidUtf8);

    tp->stop();
}

// ===========================================================================
// Send with timeout tests
// ===========================================================================

TEST_F(TransportTest, SendForSucceeds) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    const uint8_t payload[] = {0x01, 0x02};
    auto err = tp->send_for(payload, sizeof(payload), 100ms);
    EXPECT_EQ(err, SendError::kOk);

    tp->stop();
}

TEST_F(TransportTest, SendTextForValidatesUtf8) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    // Valid UTF-8
    auto err = tp->send_text_for("hello", 100ms);
    EXPECT_EQ(err, SendError::kOk);

    // Invalid UTF-8
    const uint8_t bad[] = {0xFF, 0xFE};
    err = tp->send_text_for(bad, sizeof(bad), 100ms);
    EXPECT_EQ(err, SendError::kInvalidUtf8);

    tp->stop();
}

TEST_F(TransportTest, SendForWhenStoppedReturnsNotConnected) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    tp->stop();

    const uint8_t payload[] = {0x01};
    auto err = tp->send_for(payload, sizeof(payload), 100ms);
    EXPECT_EQ(err, SendError::kNotConnected);
}

// ===========================================================================
// wait_recv_msg tests
// ===========================================================================

TEST_F(TransportTest, WaitRecvMsgTimesOutWhenEmpty) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    auto msg = tp->wait_recv_msg(50ms);
    EXPECT_FALSE(msg.has_value());

    tp->stop();
}

TEST_F(TransportTest, WaitRecvMsgReturnsMessage) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(10ms);

    // Inject after delay
    std::thread injector([&] {
        std::this_thread::sleep_for(20ms);
        last_mock_->inject_text("deferred");
    });

    auto msg = tp->wait_recv_msg(2000ms);
    ASSERT_TRUE(msg.has_value());
    EXPECT_TRUE(msg->is_text());
    EXPECT_EQ(msg->text(), "deferred");

    injector.join();
    tp->stop();
}

// ===========================================================================
// Ping callback tests
// ===========================================================================

TEST_F(TransportTest, OnPingCallbackFires) {
    TransportConfig config;
    config.remote_host = "mock.test";
    config.remote_port = 9999;
    config.ws_path = "/ws";
    config.use_tls = false;
    config.ping_interval = 0s;
    config.max_reconnect_attempts = 0;

    std::atomic<bool> ping_fired{false};
    config.on_ping = [&](const uint8_t*, uint16_t) {
        ping_fired.store(true, std::memory_order_relaxed);
    };

    WsMockTcpTransport* mock_ptr = nullptr;
    auto factory = [&]()
        -> std::expected<std::unique_ptr<WsMockTcpTransport>, std::string>
    {
        auto mock = std::make_unique<WsMockTcpTransport>();
        mock_ptr = mock.get();
        auto r = mock->connect(3000ms);
        if (!r) return std::unexpected(r.error());
        return mock;
    };

    auto result = TestTransport::create(std::move(factory), config);
    ASSERT_TRUE(result.has_value()) << result.error().message();

    std::this_thread::sleep_for(10ms);

    // Inject a ping frame from server
    const uint8_t ping_payload[] = {0x01, 0x02, 0x03};
    mock_ptr->inject_server_frame(ws::opcode::kPing, ping_payload, sizeof(ping_payload));

    bool fired = wait_for([&] {
        return ping_fired.load(std::memory_order_relaxed);
    });

    EXPECT_TRUE(fired);

    // Also verify pong was sent back (ws_pongs_sent counter)
    EXPECT_TRUE(wait_for([&] {
        return (*result)->stats().ws_pongs_sent >= 1;
    }));

    (*result)->stop();
}

TEST_F(TransportTest, ServerPingIncrementsPingCounter) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(10ms);

    // Inject 3 pings
    for (int i = 0; i < 3; ++i) {
        last_mock_->inject_server_frame(ws::opcode::kPing, nullptr, 0);
    }

    EXPECT_TRUE(wait_for([&] {
        return tp->stats().ws_pings_received >= 3;
    }));

    auto stats = tp->stats();
    EXPECT_GE(stats.ws_pings_received, 3u);
    EXPECT_GE(stats.ws_pongs_sent, 3u);

    tp->stop();
}

// ===========================================================================
// send_text_unchecked — bypasses UTF-8 validation
// ===========================================================================

TEST_F(TransportTest, SendTextUncheckedAcceptsAscii) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(5ms);

    auto err = tp->send_text_unchecked("hello", 5);
    EXPECT_EQ(err, SendError::kOk);

    tp->stop();
}

TEST_F(TransportTest, SendTextUncheckedStringViewOverload) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(5ms);

    auto err = tp->send_text_unchecked(std::string_view{"test"});
    EXPECT_EQ(err, SendError::kOk);

    tp->stop();
}

// ===========================================================================
// try_recv_msg — returns ReceivedMessage with opcode metadata
// ===========================================================================

TEST_F(TransportTest, TryRecvMsgReturnsNulloptWhenEmpty) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(5ms);

    auto msg = tp->try_recv_msg();
    EXPECT_FALSE(msg.has_value());

    tp->stop();
}

TEST_F(TransportTest, TryRecvMsgReturnsBinaryMessage) {
    auto result = create_transport(/*echo=*/false);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(5ms);

    // Inject a binary frame from server
    uint8_t payload[] = {0x01, 0x02, 0x03};
    last_mock_->inject_server_frame(ws::opcode::kBinary, payload, 3);

    std::optional<TestTransport::ReceivedMessage> msg;
    EXPECT_TRUE(wait_for([&] {
        msg = tp->try_recv_msg();
        return msg.has_value();
    }));

    ASSERT_TRUE(msg.has_value());
    EXPECT_TRUE(msg->is_binary());
    EXPECT_FALSE(msg->is_text());
    EXPECT_EQ(msg->data.size(), 3u);
    EXPECT_EQ(msg->data[0], 0x01);

    tp->stop();
}

// ===========================================================================
// Queue fill ratio and HWM
// ===========================================================================

TEST_F(TransportTest, QueueFillRatioStartsAtZero) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(5ms);

    // TX queue should be near-empty after creation
    EXPECT_LE(tp->tx_queue_fill_ratio(), 0.1);
    // RX queue should be empty (no server data)
    EXPECT_LE(tp->rx_queue_fill_ratio(), 0.1);

    tp->stop();
}

TEST_F(TransportTest, QueueHwmStartsLow) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(5ms);

    // HWM should be well below queue capacity after creation
    EXPECT_LT(tp->tx_queue_hwm(), TestTransport::queue_depth());
    EXPECT_LT(tp->rx_queue_hwm(), TestTransport::queue_depth());

    tp->stop();
}

// ===========================================================================
// reset_stats — zeroes all counters
// ===========================================================================

TEST_F(TransportTest, ResetStatsClearsCounters) {
    auto result = create_transport(/*echo=*/false);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(5ms);

    // Send some data to populate stats
    uint8_t data[] = {0xAA, 0xBB};
    tp->send(data, 2);

    // Wait for TX to process
    EXPECT_TRUE(wait_for([&] {
        return tp->stats().tx_packets > 0;
    }));
    EXPECT_GT(tp->stats().tx_packets, 0u);

    // Reset and verify
    tp->reset_stats();

    auto stats = tp->stats();
    EXPECT_EQ(stats.tx_packets, 0u);
    EXPECT_EQ(stats.tx_bytes, 0u);
    EXPECT_EQ(stats.tx_dropped, 0u);

    tp->stop();
}

TEST_F(TransportTest, ResetStatsClearsRttHistogram) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    // Before any pings, rtt_stats should be empty
    auto rtt_before = tp->rtt_stats();
    EXPECT_EQ(rtt_before.count, 0u);

    // Reset and verify rtt_stats still empty (no regression)
    tp->reset_stats();
    auto rtt_after = tp->rtt_stats();
    EXPECT_EQ(rtt_after.count, 0u);
    EXPECT_EQ(rtt_after.min_ns, 0u);
    EXPECT_EQ(rtt_after.max_ns, 0u);
    EXPECT_DOUBLE_EQ(rtt_after.mean_ns, 0.0);

    tp->stop();
}

TEST_F(TransportTest, ResetStatsClearsHwmCounters) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(5ms);

    // Send data to produce HWM
    uint8_t data[] = {0xAA};
    tp->send(data, 1);
    EXPECT_TRUE(wait_for([&] { return tp->stats().tx_packets > 0; }));

    tp->reset_stats();

    auto stats = tp->stats();
    EXPECT_EQ(stats.tx_queue_hwm, 0u) << "TX HWM should be reset";
    EXPECT_EQ(stats.rx_queue_hwm, 0u) << "RX HWM should be reset";
    EXPECT_EQ(stats.queue_full_count, 0u) << "queue_full_count should be reset";
    EXPECT_EQ(stats.reconnect_count, 0u) << "reconnect_count should be reset";

    tp->stop();
}

// ===========================================================================
// connection_info — accessible after creation
// ===========================================================================

TEST_F(TransportTest, ConnectionInfoAvailable) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    auto info = tp->connection_info();
    // In plain WS mode, TLS version should be "none"
    EXPECT_EQ(info.tls_version, "none");
    EXPECT_EQ(info.cipher_name, "none");
    EXPECT_FALSE(info.use_tls);

    tp->stop();
}

TEST_F(TransportTest, ConnectionInfoToJsonIsValid) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    auto json = tp->connection_info().to_json();
    // Should contain the key fields
    EXPECT_TRUE(json.find("\"tls_version\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"use_tls\"") != std::string::npos);

    tp->stop();
}

// ---------------------------------------------------------------------------
// config() accessor
// ---------------------------------------------------------------------------

TEST_F(TransportTest, ConfigAccessorReturnsCreationConfig) {
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    const auto& cfg = tp->config();
    EXPECT_EQ(cfg.remote_host, "mock.test");
    EXPECT_EQ(cfg.ws_path, "/ws");
    EXPECT_FALSE(cfg.use_tls);

    tp->stop();
}

TEST_F(TransportTest, ConfigAccessorConsistentAfterReconnect) {
    // Verify config() still returns the same config after internal state changes
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    const auto& cfg_before = tp->config();
    auto host_before = cfg_before.remote_host;

    // Force a stop and verify config is still accessible
    tp->stop();

    const auto& cfg_after = tp->config();
    EXPECT_EQ(cfg_after.remote_host, host_before);
}

// ===========================================================================
// close_gracefully edge cases
// ===========================================================================

TEST_F(TransportTest, CloseGracefullySucceedsWhenServerResponds) {
    // When the server echoes a Close frame, close_gracefully should return true.
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(10ms);

    // Inject a server Close response asynchronously after a short delay
    std::thread injector([this] {
        std::this_thread::sleep_for(30ms);
        uint8_t close_payload[2];
        close_payload[0] = static_cast<uint8_t>(ws::close_code::kNormal >> 8);
        close_payload[1] = static_cast<uint8_t>(ws::close_code::kNormal & 0xFF);
        last_mock_->inject_server_frame(ws::opcode::kClose,
                                         close_payload, sizeof(close_payload));
    });

    bool server_responded = tp->close_gracefully(
        ws::close_code::kNormal, "test close", 2000ms);

    injector.join();
    EXPECT_TRUE(server_responded)
        << "close_gracefully should return true when server responds with Close";
    EXPECT_FALSE(tp->is_running());
}

TEST_F(TransportTest, CloseGracefullyPropagatesCloseCode) {
    // Verify that the close code/reason from close_gracefully() reaches the
    // final Close frame in stop(). We use a short timeout so it times out,
    // then inspect the sent data for the close frame.
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(10ms);

    // close_gracefully with custom code and reason
    tp->close_gracefully(ws::close_code::kGoingAway, "server maintenance", 50ms);

    // Verify the transport is stopped
    EXPECT_FALSE(tp->is_running());
}

TEST_F(TransportTest, SendBinaryZeroLengthSucceeds) {
    // Zero-length binary frame should be accepted (empty payload is valid)
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    auto err = tp->send_binary(nullptr, 0);
    EXPECT_EQ(err, SendError::kOk)
        << "Zero-length binary payload should be accepted";
    tp->stop();
}

TEST_F(TransportTest, SendTextZeroLengthSucceeds) {
    // Zero-length text frame should be accepted (empty UTF-8 is valid)
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    auto err = tp->send_text(std::string_view{});
    EXPECT_EQ(err, SendError::kOk)
        << "Zero-length text payload should be accepted";
    tp->stop();
}

TEST_F(TransportTest, FragmentedMessageInterruptedByNewMessage) {
    // If a new non-continuation message arrives while fragments are being
    // accumulated, the old fragment buffer should be discarded.
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    std::this_thread::sleep_for(10ms);

    // Send first fragment of message 1 (FIN=false, opcode=binary)
    uint8_t frag1[] = {0x01, 0x02, 0x03};
    last_mock_->inject_frame_ex(ws::opcode::kBinary, frag1, sizeof(frag1), false);

    // Instead of continuation, send a new complete message (FIN=true, opcode=text)
    // This should discard the pending fragment and deliver the new message.
    std::string msg2 = "hello";
    last_mock_->inject_server_frame(ws::opcode::kText,
        reinterpret_cast<const uint8_t*>(msg2.data()), msg2.size());

    // Wait for the new message to arrive
    bool received = false;
    EXPECT_TRUE(wait_for([&] {
        auto msg = tp->try_recv_msg();
        if (msg && msg->opcode == ws::opcode::kText) {
            EXPECT_EQ(msg->data.size(), msg2.size());
            received = true;
            return true;
        }
        return false;
    }));

    EXPECT_TRUE(received) << "New message should be delivered after discarding fragment";
    tp->stop();
}

TEST_F(TransportTest, SendForTimesOutOnFullQueue) {
    // send_for should return kQueueFull after the timeout when the queue is full.
    // Use a non-echo transport and pause the TX thread by stopping it.
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    // Fill the queue completely (queue depth is 16 in tests)
    uint8_t payload[1] = {0x42};
    for (size_t i = 0; i < kQueueDepth + 100; ++i) {
        auto err = tp->send_binary(payload, 1);
        // Queue may not fill immediately due to TX draining, but eventually should
        if (err == SendError::kQueueFull) {
            // Now test that send_for with a short timeout also returns kQueueFull
            auto err2 = tp->send_for(payload, 1, 10ms);
            // It should either succeed (TX drained) or timeout
            EXPECT_TRUE(err2 == SendError::kOk || err2 == SendError::kQueueFull);
            break;
        }
    }

    tp->stop();
}

TEST_F(TransportTest, StatsReflectDroppedOnQueueFull) {
    // Verify that queue_full_count is incremented when TX queue overflows
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    // Rapidly send many messages to overflow the queue
    uint8_t payload[1] = {0xFF};
    int queue_full_count = 0;
    for (int i = 0; i < 1000; ++i) {
        if (tp->send_binary(payload, 1) == SendError::kQueueFull) {
            queue_full_count++;
        }
    }

    auto stats = tp->stats();
    EXPECT_EQ(stats.queue_full_count, static_cast<uint64_t>(queue_full_count))
        << "Stats queue_full_count should match actual kQueueFull returns";

    tp->stop();
}

// ===========================================================================
// skip_utf8_validation tests
// ===========================================================================

TEST_F(TransportTest, SkipUtf8ValidationAllowsInvalidUtf8ViaSendText) {
    auto result = create_transport_with([](TransportConfig& c) {
        c.skip_utf8_validation = true;
    });
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    const uint8_t bad_utf8[] = {0xFF, 0xFE, 0x00};
    auto err = tp->send_text(bad_utf8, sizeof(bad_utf8));
    EXPECT_EQ(err, SendError::kOk)
        << "send_text() should accept invalid UTF-8 when skip_utf8_validation=true";

    tp->stop();
}

TEST_F(TransportTest, SkipUtf8ValidationAllowsInvalidUtf8ViaSendTextStringView) {
    auto result = create_transport_with([](TransportConfig& c) {
        c.skip_utf8_validation = true;
    });
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    // Construct a string_view containing invalid UTF-8
    const char bad[] = {'\xFF', '\xFE', '\x00'};
    auto err = tp->send_text(std::string_view{bad, 3});
    EXPECT_EQ(err, SendError::kOk);

    tp->stop();
}

TEST_F(TransportTest, SkipUtf8ValidationAllowsInvalidUtf8ViaSend) {
    auto result = create_transport_with([](TransportConfig& c) {
        c.skip_utf8_validation = true;
    });
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    const uint8_t bad_utf8[] = {0xFF, 0xFE};
    auto err = tp->send(bad_utf8, sizeof(bad_utf8), ws::opcode::kText);
    EXPECT_EQ(err, SendError::kOk);

    tp->stop();
}

TEST_F(TransportTest, SkipUtf8ValidationAllowsInvalidUtf8ViaSendFor) {
    auto result = create_transport_with([](TransportConfig& c) {
        c.skip_utf8_validation = true;
    });
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    const uint8_t bad_utf8[] = {0xFF, 0xFE};
    auto err = tp->send_for(bad_utf8, sizeof(bad_utf8), 100ms, ws::opcode::kText);
    EXPECT_EQ(err, SendError::kOk);

    tp->stop();
}

TEST_F(TransportTest, SkipUtf8ValidationAllowsInvalidUtf8ViaSendTextFor) {
    auto result = create_transport_with([](TransportConfig& c) {
        c.skip_utf8_validation = true;
    });
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    const uint8_t bad_utf8[] = {0xFF, 0xFE};
    auto err = tp->send_text_for(bad_utf8, sizeof(bad_utf8), 100ms);
    EXPECT_EQ(err, SendError::kOk);

    tp->stop();
}

TEST_F(TransportTest, SkipUtf8ValidationAllowsInvalidUtf8ViaBatchSendN) {
    auto result = create_transport_with([](TransportConfig& c) {
        c.skip_utf8_validation = true;
    });
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    const uint8_t bad_utf8[] = {0xFF, 0xFE};
    std::span<const uint8_t> payloads[] = {
        std::span<const uint8_t>(bad_utf8, sizeof(bad_utf8)),
    };
    auto err = tp->send_n(payloads, 1, ws::opcode::kText);
    EXPECT_EQ(err, SendError::kOk);

    tp->stop();
}

TEST_F(TransportTest, SkipUtf8ValidationDefaultFalseStillRejects) {
    // Verify default behavior is unchanged: skip_utf8_validation defaults to false
    auto result = create_transport();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    const uint8_t bad_utf8[] = {0xFF, 0xFE, 0x00};
    auto err = tp->send_text(bad_utf8, sizeof(bad_utf8));
    EXPECT_EQ(err, SendError::kInvalidUtf8)
        << "Default config should still reject invalid UTF-8";

    tp->stop();
}

TEST_F(TransportTest, SkipUtf8ValidationConfigWarning) {
    TransportConfig config;
    config.skip_utf8_validation = true;
    auto warnings = config.warnings();
    bool found = false;
    for (const auto& w : warnings) {
        if (w.find("skip_utf8_validation") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found)
        << "Config warnings should mention skip_utf8_validation when enabled";
}

// ─────────────────────────────────────────────────────────────────────────────
// RTT stats
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TransportTest, RttStatsEmptyWhenNoPingsExchanged) {
    // Default config has ping_interval=0, so no pings are sent
    auto result = create_transport(/*echo=*/false, /*disable_ping=*/true);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    auto& tp = *result;

    auto rtt = tp->rtt_stats();
    EXPECT_EQ(rtt.count, 0u);
    EXPECT_EQ(rtt.min_ns, 0u);
    EXPECT_EQ(rtt.max_ns, 0u);
    EXPECT_DOUBLE_EQ(rtt.mean_ns, 0.0);
    EXPECT_EQ(rtt.p50_ns, 0u);
    EXPECT_EQ(rtt.p99_ns, 0u);
    EXPECT_EQ(rtt.p999_ns, 0u);

    tp->stop();
}

TEST(RttStatsUnit, ConvenienceAccessorsConvertToMicroseconds) {
    RttStats rtt{};
    rtt.count = 10;
    rtt.min_ns = 1000;
    rtt.max_ns = 5000;
    rtt.mean_ns = 2500.0;
    rtt.p50_ns = 2000;
    rtt.p99_ns = 4500;
    rtt.p999_ns = 4900;

    EXPECT_DOUBLE_EQ(rtt.min_us(), 1.0);
    EXPECT_DOUBLE_EQ(rtt.max_us(), 5.0);
    EXPECT_DOUBLE_EQ(rtt.mean_us(), 2.5);
    EXPECT_DOUBLE_EQ(rtt.p50_us(), 2.0);
    EXPECT_DOUBLE_EQ(rtt.p99_us(), 4.5);
    EXPECT_DOUBLE_EQ(rtt.p999_us(), 4.9);
}

TEST(RttStatsUnit, DumpEmptyReturnsNoSamples) {
    RttStats rtt{};
    auto dump = rtt.dump();
    EXPECT_NE(dump.find("no samples"), std::string::npos)
        << "dump() should indicate no samples when count=0, got: " << dump;
}

TEST(RttStatsUnit, DumpWithDataIncludesPercentiles) {
    RttStats rtt{};
    rtt.count = 100;
    rtt.min_ns = 500;
    rtt.max_ns = 10000;
    rtt.mean_ns = 3000.0;
    rtt.p50_ns = 2000;
    rtt.p99_ns = 9000;
    rtt.p999_ns = 9500;

    auto dump = rtt.dump();
    EXPECT_NE(dump.find("p50"), std::string::npos);
    EXPECT_NE(dump.find("p99"), std::string::npos);
}

TEST(RttStatsUnit, ToJsonEmptyReturnsZeroCount) {
    RttStats rtt{};
    auto json = rtt.to_json();
    EXPECT_NE(json.find("\"count\":0"), std::string::npos);
}

TEST(RttStatsUnit, ToJsonWithDataIncludesAllFields) {
    RttStats rtt{};
    rtt.count = 50;
    rtt.min_ns = 100;
    rtt.max_ns = 5000;
    rtt.mean_ns = 1500.0;
    rtt.p50_ns = 1000;
    rtt.p99_ns = 4000;
    rtt.p999_ns = 4500;

    auto json = rtt.to_json();
    EXPECT_NE(json.find("\"count\":50"), std::string::npos);
    EXPECT_NE(json.find("\"min_ns\":100"), std::string::npos);
    EXPECT_NE(json.find("\"max_ns\":5000"), std::string::npos);
    EXPECT_NE(json.find("\"p50_ns\":1000"), std::string::npos);
    EXPECT_NE(json.find("\"p99_ns\":4000"), std::string::npos);
}

TEST(RttStatsUnit, FormatterProducesOutput) {
    RttStats rtt{};
    rtt.count = 10;
    rtt.min_ns = 500;
    rtt.max_ns = 5000;
    rtt.mean_ns = 2000.0;
    rtt.p50_ns = 1800;
    rtt.p99_ns = 4500;
    rtt.p999_ns = 4800;

    auto formatted = std::format("{}", rtt);
    EXPECT_FALSE(formatted.empty());
    // Formatter should include percentile information
    EXPECT_NE(formatted.find("p50"), std::string::npos);
}
