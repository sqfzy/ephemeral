#pragma once

/// @file session.hpp
/// FIX 4.4 Session layer — complete implementation.
///
/// Provides session management on top of any transport that can send/receive
/// raw bytes (e.g., Transport<SocketTransport, FixFramer>).
///
/// Features:
///   - Logon/Logout handshake with configurable timeout
///   - Automatic Heartbeat sending and TestRequest response (tag 112)
///   - Heartbeat timeout detection (server gone silent)
///   - Proactive TestRequest when server is idle
///   - Bidirectional MsgSeqNum tracking with gap detection
///   - SequenceReset/GapFill (MsgType=4) handling
///   - ResendRequest (MsgType=2) on sequence gaps
///   - Server HeartBtInt override from Logon response
///   - Thread-safe: atomic state, atomic sequence numbers, atomic timestamps
///
/// Not implemented (by design):
///   - Sequence number persistence to disk (reset on reconnect)
///   - FIX encryption (EncryptMethod always 0)
///   - Full PossDupFlag deduplication (logged but not filtered)
///
/// Usage with Transport:
/// @code
///   auto tp = Transport<SocketTransport, FixFramer>::create(factory, cfg);
///   FixSession session(
///       [&](const uint8_t* d, size_t l) { return tp->send(d, l) == SendError::kOk; },
///       {.sender_comp_id = "MY_ALGO", .target_comp_id = "EXCHANGE"});
///
///   cfg.on_message = [&](const uint8_t* d, uint16_t l, uint8_t) {
///       if (!session.on_rx(d, l)) {
///           // Application message — process it
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

// CPU spin-wait hint — included directly to avoid pulling in eph-utils
// (which would add eph-utils as a dependency and risk pulling in aws-lc).
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#  include <immintrin.h>
#endif

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
    int heartbeat_interval_sec = 30;        ///< HeartBtInt (tag 108), may be overridden by server
    bool reset_seq_on_logon = true;         ///< Send ResetSeqNumFlag=Y on Logon
    std::string begin_string = "FIX.4.4";

    /// Tolerance factor for heartbeat timeout detection.
    /// Server is considered dead if no message received within
    /// heartbeat_interval_sec * heartbeat_timeout_factor seconds.
    /// FIX 4.4 spec recommends 1.5x (i.e., HeartBtInt + 50%).
    double heartbeat_timeout_factor = 1.5;

    /// Whether to send ResendRequest on sequence gap.
    /// If false, gaps are logged but session continues (suitable for
    /// market data where missing ticks are acceptable).
    bool resend_on_gap = false;

    /// Callback when session state changes (optional, called from any thread).
    std::function<void(SessionState old_state, SessionState new_state)> on_state_change{};

    /// Validate configuration, returning an error description or empty string on success.
    [[nodiscard]] constexpr std::string_view validate() const noexcept {
        if (sender_comp_id.empty())
            return "sender_comp_id must not be empty";
        if (target_comp_id.empty())
            return "target_comp_id must not be empty";
        if (heartbeat_interval_sec <= 0)
            return "heartbeat_interval_sec must be positive";
        if (begin_string.empty())
            return "begin_string must not be empty";
        if (heartbeat_timeout_factor <= 1.0)
            return "heartbeat_timeout_factor must be > 1.0";
        if (heartbeat_timeout_factor > 10.0)
            return "heartbeat_timeout_factor must be <= 10.0 (likely misconfiguration)";
        return {};
    }
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
    using SendFn = std::function<bool(const uint8_t* data, size_t len)>;

    FixSession(SendFn send_fn, FixSessionConfig cfg)
        : send_(std::move(send_fn)), cfg_(std::move(cfg)),
          heartbeat_interval_sec_(cfg_.heartbeat_interval_sec) {
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
    [[nodiscard]] std::expected<void, std::string>
    logon(std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) {
        if (state_.load(std::memory_order_acquire) != SessionState::kDisconnected) {
            return std::unexpected("logon: session not in DISCONNECTED state");
        }

        if (cfg_.reset_seq_on_logon) {
            outbound_seq_.store(1, std::memory_order_relaxed);
            expected_inbound_seq_.store(1, std::memory_order_relaxed);
            last_inbound_seq_.store(0, std::memory_order_relaxed);
        }

        if (!send_logon()) {
            return std::unexpected("logon: failed to send Logon message");
        }
        set_state(SessionState::kLogonSent);
        SPDLOG_LOGGER_INFO(detail::fix_session_logger(),
            "Logon sent (MsgSeqNum=1), waiting for response...");

        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (state_.load(std::memory_order_acquire) == SessionState::kLogonSent) {
            if (std::chrono::steady_clock::now() >= deadline) {
                set_state(SessionState::kDisconnected);
                return std::unexpected("logon: timeout waiting for server Logon response");
            }
            // Spin-wait pause hint: reduces power usage and avoids pipeline stalls
            // on x86 (PAUSE) and ARM64 (YIELD) versus a bare spin loop.
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
            _mm_pause();
#elif defined(__aarch64__)
            asm volatile("yield");
#else
            std::this_thread::yield();
#endif
        }

        if (state_.load(std::memory_order_acquire) != SessionState::kActive) {
            return std::unexpected("logon: session did not reach ACTIVE state");
        }

        SPDLOG_LOGGER_INFO(detail::fix_session_logger(),
            "Session active (HeartBtInt={}s)",
            heartbeat_interval_sec_.load(std::memory_order_relaxed));
        auto now = std::chrono::steady_clock::now();
        last_sent_time_.store(now.time_since_epoch().count(), std::memory_order_release);
        last_recv_time_.store(now.time_since_epoch().count(), std::memory_order_release);
        test_request_pending_.store(false, std::memory_order_relaxed);
        return {};
    }

    /// Send Logout and wait for response.
    [[nodiscard]] std::expected<void, std::string>
    logout(std::chrono::milliseconds timeout = std::chrono::milliseconds{3000}) {
        if (state_.load(std::memory_order_acquire) != SessionState::kActive) {
            return std::unexpected("logout: session not ACTIVE");
        }

        if (!send_logout()) {
            return std::unexpected("logout: failed to send Logout message");
        }
        set_state(SessionState::kLogoutSent);
        SPDLOG_LOGGER_INFO(detail::fix_session_logger(), "Logout sent");

        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (state_.load(std::memory_order_acquire) == SessionState::kLogoutSent) {
            if (std::chrono::steady_clock::now() >= deadline) {
                set_state(SessionState::kDisconnected);
                SPDLOG_LOGGER_WARN(detail::fix_session_logger(),
                    "Logout timeout — forcing disconnect");
                return {};
            }
            // Spin-wait pause hint: reduces power usage and avoids pipeline stalls
            // on x86 (PAUSE) and ARM64 (YIELD) versus a bare spin loop.
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
            _mm_pause();
#elif defined(__aarch64__)
            asm volatile("yield");
#else
            std::this_thread::yield();
#endif
        }

        SPDLOG_LOGGER_INFO(detail::fix_session_logger(), "Logout complete");
        return {};
    }

    /// Reset session state (call before re-logon after disconnect).
    void reset() noexcept {
        set_state(SessionState::kDisconnected);
        outbound_seq_.store(1, std::memory_order_relaxed);
        expected_inbound_seq_.store(1, std::memory_order_relaxed);
        last_inbound_seq_.store(0, std::memory_order_relaxed);
        test_request_pending_.store(false, std::memory_order_relaxed);
        heartbeat_interval_sec_.store(cfg_.heartbeat_interval_sec, std::memory_order_relaxed);
        SPDLOG_LOGGER_DEBUG(detail::fix_session_logger(), "Session reset");
    }

    // ─────────────────────────────────────────────────────────────────────
    // RX path (called from RX thread via on_message callback)
    // ─────────────────────────────────────────────────────────────────────

    /// Process an incoming FIX message. Called from the RX thread.
    ///
    /// @return true if session-level message (handled internally).
    ///         false if application message (caller should deliver).
    bool on_rx(const uint8_t* data, size_t len) noexcept {
        update_recv_time();

        auto result = parse(data, len);
        if (!result) {
            SPDLOG_LOGGER_WARN(detail::fix_session_logger(),
                "on_rx: parse failed ({} bytes): {} — dropping message",
                len, parse_error_name(result.error()));
            return true;
        }

        auto& msg = *result;
        auto msg_type = msg.msg_type();
        if (!msg_type) {
            SPDLOG_LOGGER_WARN(detail::fix_session_logger(),
                "on_rx: message has no MsgType field");
            return false;
        }

        // ── Sequence number tracking and gap detection ──
        auto received_seq = msg.get_int(tag::MsgSeqNum);
        if (received_seq) {
            if (*received_seq < 1) {
                SPDLOG_LOGGER_WARN(detail::fix_session_logger(),
                    "Invalid MsgSeqNum={} (must be >= 1), ignoring", *received_seq);
                return true;
            }
            uint32_t recv = static_cast<uint32_t>(*received_seq);
            uint32_t expected = expected_inbound_seq_.load(std::memory_order_relaxed);
            last_inbound_seq_.store(recv, std::memory_order_release);

            // Check PossDupFlag
            auto poss_dup = msg.get(tag::PossDupFlag);
            bool is_dup = poss_dup && *poss_dup == "Y";

            if (recv > expected && !is_dup) {
                // Gap detected
                SPDLOG_LOGGER_WARN(detail::fix_session_logger(),
                    "Sequence gap: expected={}, received={} (missing {} msgs)",
                    expected, recv, recv - expected);
                if (cfg_.resend_on_gap) {
                    send_resend_request(expected, recv - 1);
                }
                expected_inbound_seq_.store(recv + 1, std::memory_order_relaxed);
            } else if (recv == expected) {
                expected_inbound_seq_.store(recv + 1, std::memory_order_relaxed);
            } else if (recv < expected && !is_dup) {
                SPDLOG_LOGGER_WARN(detail::fix_session_logger(),
                    "Unexpected low sequence: expected={}, received={}", expected, recv);
            }
            // PossDupFlag=Y: log but don't update expected sequence
            if (is_dup) {
                SPDLOG_LOGGER_DEBUG(detail::fix_session_logger(),
                    "PossDupFlag=Y on MsgSeqNum={}", recv);
            }
        }

        auto type = *msg_type;
        auto cur_state = state_.load(std::memory_order_acquire);

        // ── Logon response ──
        if (type == std::string_view(&tag::msg_type::Logon, 1)) {
            // Override HeartBtInt if server specifies a different value
            constexpr int64_t kMaxHeartbeatSec = 3600;
            if (auto server_hb = msg.get_int(tag::HeartBtInt); server_hb) {
                if (*server_hb > 0 && *server_hb <= kMaxHeartbeatSec) {
                    int prev = heartbeat_interval_sec_.load(std::memory_order_relaxed);
                    if (*server_hb != prev) {
                        SPDLOG_LOGGER_INFO(detail::fix_session_logger(),
                            "Server HeartBtInt={} overrides client value={}",
                            *server_hb, prev);
                        heartbeat_interval_sec_.store(
                            static_cast<int>(*server_hb), std::memory_order_release);
                    }
                } else {
                    SPDLOG_LOGGER_WARN(detail::fix_session_logger(),
                        "Ignoring unreasonable HeartBtInt={}", *server_hb);
                }
            }
            SPDLOG_LOGGER_INFO(detail::fix_session_logger(),
                "Received Logon response (MsgSeqNum={})",
                last_inbound_seq_.load(std::memory_order_relaxed));
            if (cur_state == SessionState::kLogonSent) {
                set_state(SessionState::kActive);
            }
            return true;
        }

        // ── Logout ──
        if (type == std::string_view(&tag::msg_type::Logout, 1)) {
            auto text = msg.get(tag::Text);
            SPDLOG_LOGGER_INFO(detail::fix_session_logger(),
                "Received Logout{}", text ? std::string(": ").append(*text) : "");
            if (cur_state == SessionState::kLogoutSent) {
                set_state(SessionState::kDisconnected);
            } else if (cur_state == SessionState::kActive) {
                send_logout();
                set_state(SessionState::kDisconnected);
            }
            return true;
        }

        // ── Heartbeat ──
        if (type == std::string_view(&tag::msg_type::Heartbeat, 1)) {
            // If we sent a TestRequest and got Heartbeat back, clear the pending flag
            test_request_pending_.store(false, std::memory_order_relaxed);
            SPDLOG_LOGGER_DEBUG(detail::fix_session_logger(),
                "Received Heartbeat (MsgSeqNum={})",
                last_inbound_seq_.load(std::memory_order_relaxed));
            return true;
        }

        // ── TestRequest → respond with Heartbeat echoing TestReqID (tag 112) ──
        if (type == std::string_view(&tag::msg_type::TestRequest, 1)) {
            auto test_req_id = msg.get(tag::TestReqID);
            SPDLOG_LOGGER_DEBUG(detail::fix_session_logger(),
                "Received TestRequest (TestReqID={}), sending Heartbeat",
                test_req_id.value_or("(none)"));
            send_heartbeat(test_req_id.value_or(""));
            return true;
        }

        // ── SequenceReset (MsgType=4) ──
        if (type == "4") {
            auto gap_fill = msg.get(tag::GapFillFlag);
            auto new_seq = msg.get_int(tag::NewSeqNo);
            if (new_seq) {
                if (*new_seq < 1) {
                    SPDLOG_LOGGER_WARN(detail::fix_session_logger(),
                        "Invalid NewSeqNo={} in SequenceReset (must be >= 1), ignoring", *new_seq);
                    return true;
                }
                uint32_t ns = static_cast<uint32_t>(*new_seq);
                if (gap_fill && *gap_fill == "Y") {
                    // GapFill mode: advance expected sequence (must not go backward)
                    uint32_t expected = expected_inbound_seq_.load(std::memory_order_relaxed);
                    if (ns <= expected) {
                        SPDLOG_LOGGER_WARN(detail::fix_session_logger(),
                            "GapFill rejected: NewSeqNo={} <= expected_inbound_seq_={} "
                            "(backward reset not allowed in GapFill mode)", ns, expected);
                        return true;
                    }
                    SPDLOG_LOGGER_INFO(detail::fix_session_logger(),
                        "GapFill: advancing expected sequence to {}", ns);
                } else {
                    // Reset mode: unconditional reset
                    SPDLOG_LOGGER_INFO(detail::fix_session_logger(),
                        "SequenceReset: resetting expected sequence to {}", ns);
                }
                expected_inbound_seq_.store(ns, std::memory_order_relaxed);
            }
            return true;
        }

        // ── ResendRequest (MsgType=2) from server ──
        // We don't maintain an outbound message journal, so respond with
        // SequenceReset-GapFill to skip the requested range.
        if (type == "2") {
            auto begin = msg.get_int(tag::BeginSeqNo);
            auto end = msg.get_int(tag::EndSeqNo);
            SPDLOG_LOGGER_WARN(detail::fix_session_logger(),
                "Received ResendRequest (BeginSeqNo={}, EndSeqNo={}) — "
                "no message journal, sending GapFill",
                begin.value_or(-1), end.value_or(-1));
            // Send SequenceReset-GapFill to skip the gap
            uint32_t next = outbound_seq_.load(std::memory_order_relaxed);
            send_sequence_reset_gap_fill(next);
            return true;
        }

        // ── Application message ──
        return false;
    }

    // ─────────────────────────────────────────────────────────────────────
    // Heartbeat timer (call periodically or rely on on_rx/send_app)
    // ─────────────────────────────────────────────────────────────────────

    /// Check heartbeat timing. Handles three scenarios:
    ///   1. We haven't sent anything for HeartBtInt → send Heartbeat
    ///   2. Server hasn't sent anything for HeartBtInt → send TestRequest
    ///   3. Server hasn't sent anything for HeartBtInt * timeout_factor → server dead
    ///
    /// @return true if session is still healthy, false if server is considered dead
    bool tick() noexcept {
        if (state_.load(std::memory_order_relaxed) != SessionState::kActive) return true;

        auto now = std::chrono::steady_clock::now();
        auto hb_sec = heartbeat_interval_sec_.load(std::memory_order_acquire);

        // Check if we need to send a heartbeat
        maybe_send_heartbeat();

        // Check if server is silent
        auto last_recv = time_point_from_atomic(last_recv_time_);
        auto recv_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_recv).count();

        if (recv_elapsed >= static_cast<int64_t>(hb_sec * cfg_.heartbeat_timeout_factor)) {
            // Server dead — no response even after TestRequest
            if (test_request_pending_.load(std::memory_order_relaxed)) {
                SPDLOG_LOGGER_ERROR(detail::fix_session_logger(),
                    "Server unresponsive: no message for {:.1f}s "
                    "(HeartBtInt={}s, timeout factor={:.1f}), TestRequest unanswered",
                    static_cast<double>(recv_elapsed), hb_sec,
                    cfg_.heartbeat_timeout_factor);
                set_state(SessionState::kDisconnected);
                return false;
            }
        }

        if (recv_elapsed >= hb_sec && !test_request_pending_.load(std::memory_order_relaxed)) {
            // Server idle beyond HeartBtInt — probe with TestRequest
            SPDLOG_LOGGER_DEBUG(detail::fix_session_logger(),
                "Server idle for {}s, sending TestRequest", recv_elapsed);
            send_test_request();
        }

        return true;
    }

    // ─────────────────────────────────────────────────────────────────────
    // TX path (application messages)
    // ─────────────────────────────────────────────────────────────────────

    /// Send an application-level FIX message.
    /// Fills session header: SenderCompID, TargetCompID, MsgSeqNum, SendingTime.
    ///
    /// @param builder  MessageBuilder with MsgType + app fields set. Must NOT be finished.
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
            update_sent_time();
        }
        return ok;
    }

    // ─────────────────────────────────────────────────────────────────────
    // State queries
    // ─────────────────────────────────────────────────────────────────────

    [[nodiscard]] SessionState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint32_t next_outbound_seq() const noexcept {
        return outbound_seq_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint32_t last_inbound_seq() const noexcept {
        return last_inbound_seq_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint32_t expected_inbound_seq() const noexcept {
        return expected_inbound_seq_.load(std::memory_order_relaxed);
    }

private:
    // ─────────────────────────────────────────────────────────────────────
    // State transitions
    // ─────────────────────────────────────────────────────────────────────

    void set_state(SessionState new_state) noexcept {
        auto old = state_.exchange(new_state, std::memory_order_acq_rel);
        if (old != new_state) {
            SPDLOG_LOGGER_DEBUG(detail::fix_session_logger(),
                "State: {} → {}", session_state_name(old), session_state_name(new_state));
            if (cfg_.on_state_change) {
                cfg_.on_state_change(old, new_state);
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    // Thread-safe time tracking
    // ─────────────────────────────────────────────────────────────────────

    void update_sent_time() noexcept {
        last_sent_time_.store(
            std::chrono::steady_clock::now().time_since_epoch().count(),
            std::memory_order_release);
    }

    void update_recv_time() noexcept {
        last_recv_time_.store(
            std::chrono::steady_clock::now().time_since_epoch().count(),
            std::memory_order_release);
    }

    static std::chrono::steady_clock::time_point
    time_point_from_atomic(const std::atomic<int64_t>& a) noexcept {
        return std::chrono::steady_clock::time_point{
            std::chrono::steady_clock::duration{a.load(std::memory_order_acquire)}};
    }

    // ─────────────────────────────────────────────────────────────────────
    // Internal message builders
    // ─────────────────────────────────────────────────────────────────────

    void fill_session_header(MessageBuilder& b) noexcept {
        b.set(tag::SenderCompID, cfg_.sender_comp_id);
        b.set(tag::TargetCompID, cfg_.target_comp_id);
        b.set_int(tag::MsgSeqNum,
                  static_cast<int64_t>(outbound_seq_.fetch_add(1, std::memory_order_relaxed)));

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
        if (ok) update_sent_time();
        return ok;
    }

    bool send_logon() noexcept {
        uint8_t buf[512];
        MessageBuilder b(buf, sizeof(buf));
        b.set(tag::MsgType, "A");
        fill_session_header(b);
        b.set_int(tag::EncryptMethod, 0);
        b.set_int(tag::HeartBtInt, cfg_.heartbeat_interval_sec);
        if (cfg_.reset_seq_on_logon) {
            b.set_bool(tag::ResetSeqNumFlag, true);
        }
        return send_message(b);
    }

    bool send_logout() noexcept {
        uint8_t buf[256];
        MessageBuilder b(buf, sizeof(buf));
        b.set(tag::MsgType, "5");
        fill_session_header(b);
        return send_message(b);
    }

    bool send_heartbeat(std::string_view test_req_id = {}) noexcept {
        uint8_t buf[256];
        MessageBuilder b(buf, sizeof(buf));
        b.set(tag::MsgType, "0");
        fill_session_header(b);
        if (!test_req_id.empty()) {
            b.set(tag::TestReqID, test_req_id);
        }
        SPDLOG_LOGGER_DEBUG(detail::fix_session_logger(),
            "Sending Heartbeat (MsgSeqNum={})",
            outbound_seq_.load(std::memory_order_relaxed) - 1);
        return send_message(b);
    }

    bool send_test_request() noexcept {
        // Generate a unique TestReqID from timestamp
        char id_buf[32];
        auto n = std::snprintf(id_buf, sizeof(id_buf), "TR%llu",
            static_cast<unsigned long long>(
                std::chrono::steady_clock::now().time_since_epoch().count()));

        uint8_t buf[256];
        MessageBuilder b(buf, sizeof(buf));
        b.set(tag::MsgType, "1");
        fill_session_header(b);
        b.set(tag::TestReqID, std::string_view(id_buf, static_cast<size_t>(n)));
        test_request_pending_.store(true, std::memory_order_release);
        SPDLOG_LOGGER_DEBUG(detail::fix_session_logger(),
            "Sending TestRequest (TestReqID={})", std::string_view(id_buf, static_cast<size_t>(n)));
        return send_message(b);
    }

    bool send_resend_request(uint32_t begin_seq, uint32_t end_seq) noexcept {
        uint8_t buf[256];
        MessageBuilder b(buf, sizeof(buf));
        b.set(tag::MsgType, "2");
        fill_session_header(b);
        b.set_int(tag::BeginSeqNo, static_cast<int64_t>(begin_seq));
        b.set_int(tag::EndSeqNo, static_cast<int64_t>(end_seq));
        SPDLOG_LOGGER_INFO(detail::fix_session_logger(),
            "Sending ResendRequest (BeginSeqNo={}, EndSeqNo={})", begin_seq, end_seq);
        return send_message(b);
    }

    bool send_sequence_reset_gap_fill(uint32_t new_seq) noexcept {
        uint8_t buf[256];
        MessageBuilder b(buf, sizeof(buf));
        b.set(tag::MsgType, "4");
        fill_session_header(b);
        b.set_bool(tag::GapFillFlag, true);
        b.set_int(tag::NewSeqNo, static_cast<int64_t>(new_seq));
        SPDLOG_LOGGER_INFO(detail::fix_session_logger(),
            "Sending SequenceReset-GapFill (NewSeqNo={})", new_seq);
        return send_message(b);
    }

    void maybe_send_heartbeat() noexcept {
        auto now = std::chrono::steady_clock::now();
        auto last_sent = time_point_from_atomic(last_sent_time_);
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_sent).count();
        if (elapsed >= heartbeat_interval_sec_.load(std::memory_order_acquire)) {
            send_heartbeat();
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    // State (all cross-thread fields are atomic)
    // ─────────────────────────────────────────────────────────────────────

    SendFn send_;
    FixSessionConfig cfg_;

    std::atomic<SessionState> state_{SessionState::kDisconnected};

    /// Runtime heartbeat interval — initialized from cfg_, updated atomically
    /// by RX thread (Logon response) and read by timer thread (tick/maybe_send_heartbeat).
    std::atomic<int> heartbeat_interval_sec_;

    // Sequence numbers: outbound written by app thread, inbound by RX thread
    std::atomic<uint32_t> outbound_seq_{1};
    std::atomic<uint32_t> last_inbound_seq_{0};
    std::atomic<uint32_t> expected_inbound_seq_{1};

    // Timestamps stored as steady_clock duration counts for atomic access
    std::atomic<int64_t> last_sent_time_{0};
    std::atomic<int64_t> last_recv_time_{0};

    // TestRequest tracking
    std::atomic<bool> test_request_pending_{false};
};

} // namespace eph::fix
