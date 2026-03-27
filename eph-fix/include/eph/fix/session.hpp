#pragma once

/// @file session.hpp
/// FIX 4.4 Session management — Logon, Logout, Heartbeat, sequence numbers.
///
/// Minimal session layer for market data subscriptions. Sits on top of any
/// transport that can send/receive raw bytes (e.g., Transport<SocketTransport, FixFramer>).
///
/// Features:
///   - Logon/Logout handshake with configurable timeout
///   - Automatic Heartbeat sending and TestRequest response
///   - Bidirectional MsgSeqNum tracking (in-memory, reset on reconnect)
///   - Thread-safe state transitions (atomic SessionState for cross-thread logon wait)
///
/// Not implemented (by design):
///   - GapFill / ResendRequest (disconnect + reconnect instead)
///   - Sequence number persistence to disk
///   - PossDupFlag / PossResend handling
///   - FIX encryption (EncryptMethod always 0)
///
/// Usage with Transport:
/// @code
///   auto transport = Transport<SocketTransport, FixFramer>::create(factory, cfg);
///   FixSession session(
///       [&](const uint8_t* d, size_t l) { return tp->send(d, l) == SendError::kOk; },
///       {.sender_comp_id = "MY_ALGO", .target_comp_id = "EXCHANGE"});
///
///   // Register session's on_rx in transport's on_message callback
///   cfg.on_message = [&](const uint8_t* d, uint16_t l, uint8_t) {
///       if (!session.on_rx(d, l)) {
///           // Application message — process it
///           auto msg = eph::fix::parse(d, l);
///           // ...
///       }
///   };
///
///   session.logon();
///   // ... receive market data ...
///   session.logout();
/// @endcode

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "eph/fix/builder.hpp"
#include "eph/fix/parser.hpp"
#include "eph/fix/tags.hpp"

namespace eph::fix {

// ─────────────────────────────────────────────────────────────────────────────
// Session state
// ─────────────────────────────────────────────────────────────────────────────

enum class SessionState : uint8_t {
    kDisconnected,  ///< Initial or disconnected
    kLogonSent,     ///< Logon sent, waiting for server Logon response
    kActive,        ///< Session active — can send/receive application messages
    kLogoutSent,    ///< Logout sent, waiting for response or timeout
};

constexpr std::string_view session_state_name(SessionState s) noexcept {
    switch (s) {
    case SessionState::kDisconnected: return "DISCONNECTED";
    case SessionState::kLogonSent:    return "LOGON_SENT";
    case SessionState::kActive:       return "ACTIVE";
    case SessionState::kLogoutSent:   return "LOGOUT_SENT";
    }
    return "UNKNOWN";
}

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

struct FixSessionConfig {
    std::string sender_comp_id;
    std::string target_comp_id;
    int heartbeat_interval_sec = 30;     ///< HeartBtInt (tag 108)
    bool reset_seq_on_logon = true;      ///< Send ResetSeqNumFlag=Y on Logon
    std::string begin_string = "FIX.4.4";
};

// ─────────────────────────────────────────────────────────────────────────────
// Logger
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {
inline std::shared_ptr<spdlog::logger> fix_session_logger() {
    static auto l = [] {
        auto lg = spdlog::get("fix.session");
        if (!lg) lg = spdlog::stdout_color_mt("fix.session");
        return lg;
    }();
    return l;
}
} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// FixSession
// ─────────────────────────────────────────────────────────────────────────────

class FixSession {
public:
    /// Send callback: returns true if the message was sent successfully.
    /// The callback receives the complete FIX message (after finish()).
    using SendFn = std::function<bool(const uint8_t* data, size_t len)>;

    FixSession(SendFn send_fn, FixSessionConfig cfg)
        : send_(std::move(send_fn)), cfg_(std::move(cfg)) {
        SPDLOG_LOGGER_INFO(detail::fix_session_logger(),
            "FixSession created: {} → {}, HeartBtInt={}s, ResetSeq={}",
            cfg_.sender_comp_id, cfg_.target_comp_id,
            cfg_.heartbeat_interval_sec, cfg_.reset_seq_on_logon);
    }

    // ─────────────────────────────────────────────────────────────────────
    // Lifecycle
    // ─────────────────────────────────────────────────────────────────────

    /// Send Logon and wait for server Logon response.
    /// Blocks the calling thread (spin-wait on atomic state).
    /// RX thread must call on_rx() concurrently to receive the response.
    [[nodiscard]] std::expected<void, std::string>
    logon(std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) {
        if (state_.load(std::memory_order_acquire) != SessionState::kDisconnected) {
            return std::unexpected("logon: session not in DISCONNECTED state");
        }

        // Reset sequence numbers on new session
        if (cfg_.reset_seq_on_logon) {
            outbound_seq_ = 1;
            inbound_seq_ = 0;
        }

        // Build and send Logon message
        if (!send_logon()) {
            return std::unexpected("logon: failed to send Logon message");
        }
        state_.store(SessionState::kLogonSent, std::memory_order_release);
        SPDLOG_LOGGER_INFO(detail::fix_session_logger(),
            "Logon sent (MsgSeqNum=1), waiting for response...");

        // Spin-wait for RX thread to set state to kActive (via on_rx)
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (state_.load(std::memory_order_acquire) == SessionState::kLogonSent) {
            if (std::chrono::steady_clock::now() >= deadline) {
                state_.store(SessionState::kDisconnected, std::memory_order_release);
                return std::unexpected("logon: timeout waiting for server Logon response");
            }
            std::this_thread::yield();
        }

        if (state_.load(std::memory_order_acquire) != SessionState::kActive) {
            return std::unexpected("logon: session did not reach ACTIVE state");
        }

        SPDLOG_LOGGER_INFO(detail::fix_session_logger(),
            "Session active (server Logon received)");
        last_sent_time_ = std::chrono::steady_clock::now();
        last_recv_time_ = last_sent_time_;
        return {};
    }

    /// Send Logout and wait for response.
    [[nodiscard]] std::expected<void, std::string>
    logout(std::chrono::milliseconds timeout = std::chrono::milliseconds{3000}) {
        auto cur = state_.load(std::memory_order_acquire);
        if (cur != SessionState::kActive) {
            return std::unexpected("logout: session not ACTIVE");
        }

        if (!send_logout()) {
            return std::unexpected("logout: failed to send Logout message");
        }
        state_.store(SessionState::kLogoutSent, std::memory_order_release);
        SPDLOG_LOGGER_INFO(detail::fix_session_logger(), "Logout sent");

        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (state_.load(std::memory_order_acquire) == SessionState::kLogoutSent) {
            if (std::chrono::steady_clock::now() >= deadline) {
                state_.store(SessionState::kDisconnected, std::memory_order_release);
                SPDLOG_LOGGER_WARN(detail::fix_session_logger(),
                    "Logout timeout — forcing disconnect");
                return {};  // Timeout is acceptable for logout
            }
            std::this_thread::yield();
        }

        SPDLOG_LOGGER_INFO(detail::fix_session_logger(), "Logout complete");
        return {};
    }

    /// Reset session state (call before re-logon after disconnect).
    void reset() noexcept {
        state_.store(SessionState::kDisconnected, std::memory_order_release);
        outbound_seq_ = 1;
        inbound_seq_ = 0;
        SPDLOG_LOGGER_DEBUG(detail::fix_session_logger(), "Session reset");
    }

    // ─────────────────────────────────────────────────────────────────────
    // RX path (called from RX thread via on_message callback)
    // ─────────────────────────────────────────────────────────────────────

    /// Process an incoming FIX message. Called from the RX thread.
    ///
    /// @return true if this was a session-level message (handled internally).
    ///         false if this is an application message (caller should deliver).
    bool on_rx(const uint8_t* data, size_t len) noexcept {
        last_recv_time_ = std::chrono::steady_clock::now();

        // Parse to extract MsgType and MsgSeqNum
        auto result = parse(data, len);
        if (!result) {
            SPDLOG_LOGGER_WARN(detail::fix_session_logger(),
                "on_rx: failed to parse FIX message ({} bytes): {}",
                len, parse_error_name(result.error()));
            return false;  // Let application decide what to do with unparseable data
        }

        auto& msg = *result;
        auto msg_type = msg.msg_type();
        if (!msg_type) {
            SPDLOG_LOGGER_WARN(detail::fix_session_logger(),
                "on_rx: message has no MsgType field");
            return false;
        }

        // Track inbound sequence number
        if (auto seq = msg.get_int(tag::MsgSeqNum); seq) {
            inbound_seq_ = static_cast<uint32_t>(*seq);
        }

        auto type = *msg_type;
        auto cur_state = state_.load(std::memory_order_acquire);

        // ── Logon response ──
        if (type == std::string_view(&tag::msg_type::Logon, 1)) {
            SPDLOG_LOGGER_INFO(detail::fix_session_logger(),
                "Received Logon response (MsgSeqNum={})", inbound_seq_);
            if (cur_state == SessionState::kLogonSent) {
                state_.store(SessionState::kActive, std::memory_order_release);
            }
            return true;
        }

        // ── Logout ──
        if (type == std::string_view(&tag::msg_type::Logout, 1)) {
            auto text = msg.get(tag::Text);
            SPDLOG_LOGGER_INFO(detail::fix_session_logger(),
                "Received Logout{}", text ? std::string(": ").append(*text) : "");
            if (cur_state == SessionState::kLogoutSent) {
                // Expected response to our Logout
                state_.store(SessionState::kDisconnected, std::memory_order_release);
            } else if (cur_state == SessionState::kActive) {
                // Server-initiated Logout — send Logout response and disconnect
                send_logout();
                state_.store(SessionState::kDisconnected, std::memory_order_release);
            }
            return true;
        }

        // ── Heartbeat ──
        if (type == std::string_view(&tag::msg_type::Heartbeat, 1)) {
            SPDLOG_LOGGER_DEBUG(detail::fix_session_logger(),
                "Received Heartbeat (MsgSeqNum={})", inbound_seq_);
            return true;
        }

        // ── TestRequest → respond with Heartbeat ──
        if (type == std::string_view(&tag::msg_type::TestRequest, 1)) {
            auto test_req_id = msg.get(tag::Text);  // TestReqID is in tag 112, but some use 58
            SPDLOG_LOGGER_DEBUG(detail::fix_session_logger(),
                "Received TestRequest, sending Heartbeat response");
            // Respond with Heartbeat containing TestReqID
            send_heartbeat(test_req_id.value_or(""));
            return true;
        }

        // ── Application message ──
        return false;
    }

    // ─────────────────────────────────────────────────────────────────────
    // Heartbeat timer (fallback for idle periods)
    // ─────────────────────────────────────────────────────────────────────

    /// Check heartbeat timing and send if needed.
    /// Call periodically (e.g., every second) from application thread,
    /// or rely on on_rx()/send_app() to check opportunistically.
    void tick() noexcept {
        if (state_.load(std::memory_order_relaxed) != SessionState::kActive) return;
        maybe_send_heartbeat();
    }

    // ─────────────────────────────────────────────────────────────────────
    // TX path (application messages)
    // ─────────────────────────────────────────────────────────────────────

    /// Send an application-level FIX message.
    /// Automatically fills session header fields:
    ///   SenderCompID, TargetCompID, MsgSeqNum, SendingTime.
    ///
    /// @param builder  MessageBuilder with MsgType and application fields already set.
    ///                 Must NOT have called finish() yet.
    /// @return true if sent successfully
    bool send_app(MessageBuilder& builder) noexcept {
        if (state_.load(std::memory_order_acquire) != SessionState::kActive) {
            SPDLOG_LOGGER_WARN(detail::fix_session_logger(),
                "send_app: session not active (state={})",
                session_state_name(state_.load(std::memory_order_relaxed)));
            return false;
        }

        fill_session_header(builder);
        size_t len = builder.finish(cfg_.begin_string);
        if (len == 0) {
            SPDLOG_LOGGER_ERROR(detail::fix_session_logger(),
                "send_app: MessageBuilder::finish() failed (overflow?)");
            return false;
        }

        auto span = builder.as_span();
        bool ok = send_(span.data(), span.size());
        if (ok) {
            last_sent_time_ = std::chrono::steady_clock::now();
            // Opportunistic heartbeat check on send path
            maybe_send_heartbeat();
        }
        return ok;
    }

    // ─────────────────────────────────────────────────────────────────────
    // State queries
    // ─────────────────────────────────────────────────────────────────────

    [[nodiscard]] SessionState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint32_t next_outbound_seq() const noexcept { return outbound_seq_; }
    [[nodiscard]] uint32_t last_inbound_seq() const noexcept { return inbound_seq_; }

private:
    // ─────────────────────────────────────────────────────────────────────
    // Internal message builders
    // ─────────────────────────────────────────────────────────────────────

    void fill_session_header(MessageBuilder& b) noexcept {
        b.set(tag::SenderCompID, cfg_.sender_comp_id);
        b.set(tag::TargetCompID, cfg_.target_comp_id);
        b.set_int(tag::MsgSeqNum, outbound_seq_++);

        uint64_t now_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        b.set_timestamp(tag::SendingTime, now_ns);
    }

    bool send_message(MessageBuilder& b) noexcept {
        size_t len = b.finish(cfg_.begin_string);
        if (len == 0) return false;
        auto span = b.as_span();
        bool ok = send_(span.data(), span.size());
        if (ok) last_sent_time_ = std::chrono::steady_clock::now();
        return ok;
    }

    bool send_logon() noexcept {
        uint8_t buf[512];
        MessageBuilder b(buf, sizeof(buf));
        b.set(tag::MsgType, "A");  // Logon
        fill_session_header(b);
        b.set_int(tag::EncryptMethod, 0);  // No encryption
        b.set_int(tag::HeartBtInt, cfg_.heartbeat_interval_sec);
        if (cfg_.reset_seq_on_logon) {
            b.set_bool(tag::ResetSeqNumFlag, true);
        }
        return send_message(b);
    }

    bool send_logout() noexcept {
        uint8_t buf[256];
        MessageBuilder b(buf, sizeof(buf));
        b.set(tag::MsgType, "5");  // Logout
        fill_session_header(b);
        return send_message(b);
    }

    bool send_heartbeat(std::string_view test_req_id = {}) noexcept {
        uint8_t buf[256];
        MessageBuilder b(buf, sizeof(buf));
        b.set(tag::MsgType, "0");  // Heartbeat
        fill_session_header(b);
        if (!test_req_id.empty()) {
            b.set(tag::Text, test_req_id);  // Echo TestReqID
        }
        SPDLOG_LOGGER_DEBUG(detail::fix_session_logger(),
            "Sending Heartbeat (MsgSeqNum={})", outbound_seq_ - 1);
        return send_message(b);
    }

    void maybe_send_heartbeat() noexcept {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_sent_time_).count();
        if (elapsed >= cfg_.heartbeat_interval_sec) {
            send_heartbeat();
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    // State
    // ─────────────────────────────────────────────────────────────────────

    SendFn send_;
    FixSessionConfig cfg_;

    std::atomic<SessionState> state_{SessionState::kDisconnected};
    uint32_t outbound_seq_ = 1;  // Next outbound MsgSeqNum (written by app thread)
    uint32_t inbound_seq_ = 0;   // Last received MsgSeqNum (written by RX thread)

    std::chrono::steady_clock::time_point last_sent_time_{};
    std::chrono::steady_clock::time_point last_recv_time_{};
};

} // namespace eph::fix
