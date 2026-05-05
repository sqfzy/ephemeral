#pragma once

/// @file session.hpp
/// FIX 4.4 Session layer — complete implementation.
///
/// Provides session management on top of any byte transport. The session
/// is decoupled from networking: it consumes a `send_fn(const uint8_t*,
/// size_t) -> bool` and is fed inbound bytes by the user's RX callback.
/// Compose with `eph::net::kernel::KernelTcpStream<RawStreamCodec>` (or
/// the DPDK equivalent) for production use.
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
/// Usage with a kernel TCP stream:
/// @code
///   namespace en = eph::net::kernel;
///   namespace ec = eph::codec;
///
///   auto poller = en::KernelPoller::create().value();
///   auto stream = en::KernelTcpStream<ec::RawStreamCodec>::create(cfg).value();
///   poller->add(stream.get()).value();
///
///   FixSession session(
///       [&stream](const uint8_t* d, std::size_t l) {
///           auto r = stream->send({d, l});
///           return r.has_value();
///       },
///       {.sender_comp_id = "MY_ALGO", .target_comp_id = "EXCHANGE"});
///
///   stream->on_message = [&](std::span<const uint8_t> bytes) {
///       if (!session.on_rx(bytes.data(), bytes.size())) {
///           // Application message — process it
///       }
///   };
///
///   session.logon();
///   while (running) poller->poll();
///   session.logout();
/// @endcode

#include <algorithm>  // std::min in send_test_request snprintf clamp
#include <atomic>
#include <chrono>
#include <cmath>     // isfinite for FixSessionConfig::validate()
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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

/// @brief FIX session lifecycle state.
enum class SessionState : uint8_t {
    kDisconnected,  ///< Initial or disconnected
    kLogonSent,     ///< Logon sent, waiting for server Logon response
    kActive,        ///< Session active — can send/receive application messages
    kLogoutSent,    ///< Logout sent, waiting for response or timeout
};

/// @brief Get human-readable name for a session state.
/// @param s  The session state to convert.
/// @return A string_view representation (e.g. "ACTIVE", "DISCONNECTED").
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

/// @brief Configuration for a FIX session.
///
/// All fields have sensible defaults. Call validate() to check consistency
/// before passing to FixSession.
struct FixSessionConfig {
    std::string sender_comp_id;                          ///< SenderCompID (tag 49) -- our identity.
    std::string target_comp_id;                          ///< TargetCompID (tag 56) -- counterparty identity.
    int heartbeat_interval_sec = 30;        ///< HeartBtInt (tag 108), may be overridden by server
    bool reset_seq_on_logon = true;         ///< Send ResetSeqNumFlag=Y on Logon.
    std::string begin_string = "FIX.4.4"; ///< FIX protocol version (tag 8).

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

    /// @brief Validate configuration, returning an error description or empty string on success.
    /// @return Empty string_view on success, or a description of the validation failure.
    [[nodiscard]] constexpr std::string_view validate() const noexcept {
        if (sender_comp_id.empty())
            return "sender_comp_id must not be empty";
        if (target_comp_id.empty())
            return "target_comp_id must not be empty";
        if (heartbeat_interval_sec <= 0)
            return "heartbeat_interval_sec must be positive";
        if (begin_string.empty())
            return "begin_string must not be empty";
        // isfinite check MUST run before the bound comparisons. Both
        // `<= 1.0` and `> 10.0` return false for NaN (every NaN
        // comparison is false), so a NaN factor would slip through
        // and later cause UB inside `tick()` at
        //   `static_cast<int64_t>(static_cast<double>(hb_sec) *
        //                         cfg_.heartbeat_timeout_factor)`
        // — converting a non-finite double to int64_t is UB per
        // [conv.fpint]/p1 and the optimizer is allowed to assume it
        // never happens. +inf has the symmetric problem (>10.0 catches
        // it but only by accident; the conversion would still be UB
        // if -ffast-math were ever turned on for this TU).
        if (!std::isfinite(heartbeat_timeout_factor))
            return "heartbeat_timeout_factor must be finite";
        if (heartbeat_timeout_factor <= 1.0)
            return "heartbeat_timeout_factor must be > 1.0";
        if (heartbeat_timeout_factor > 10.0)
            return "heartbeat_timeout_factor must be <= 10.0 (likely misconfiguration)";
        return {};
    }

    /// Multi-line formatted dump for logging/debugging.
    /// Callbacks are shown as set/unset (closures cannot be serialized).
    [[nodiscard]] std::string dump() const {
        return std::format(
            "FixSessionConfig:\n"
            "  sender_comp_id: {}\n"
            "  target_comp_id: {}\n"
            "  begin_string: {}\n"
            "  heartbeat: {}s (timeout factor: {:.1f}x)\n"
            "  reset_seq_on_logon: {}\n"
            "  resend_on_gap: {}\n"
            "  on_state_change: {}",
            sender_comp_id, target_comp_id, begin_string,
            heartbeat_interval_sec, heartbeat_timeout_factor,
            reset_seq_on_logon, resend_on_gap,
            static_cast<bool>(on_state_change));
    }

    /// JSON-formatted config for monitoring system integration.
    [[nodiscard]] std::string to_json() const {
        // Minimal JSON escape for string fields — replace \ and " only.
        // eph-fix does not depend on eph-core, so we inline a simple escape.
        auto esc = [](std::string_view s) -> std::string {
            std::string out;
            out.reserve(s.size());
            for (char c : s) {
                if (c == '"')       out += "\\\"";
                else if (c == '\\') out += "\\\\";
                else                out += c;
            }
            return out;
        };
        return std::format(
            "{{"
            "\"sender_comp_id\":\"{}\",\"target_comp_id\":\"{}\","
            "\"begin_string\":\"{}\","
            "\"heartbeat_interval_sec\":{},\"heartbeat_timeout_factor\":{:.2f},"
            "\"reset_seq_on_logon\":{},\"resend_on_gap\":{}}}",
            esc(sender_comp_id), esc(target_comp_id),
            esc(begin_string),
            heartbeat_interval_sec, heartbeat_timeout_factor,
            reset_seq_on_logon ? "true" : "false",
            resend_on_gap ? "true" : "false");
    }

    /// Check for non-fatal contradictions or likely misconfigurations.
    /// Returns a list of warning messages (empty if no issues).
    /// Unlike validate() which blocks construction, these are advisory.
    [[nodiscard]] std::vector<std::string> warnings() const {
        std::vector<std::string> w;
        if (heartbeat_interval_sec > 120)
            w.emplace_back(std::format(
                "heartbeat_interval_sec={} is unusually large (>120s) -- "
                "may cause slow disconnect detection", heartbeat_interval_sec));
        if (heartbeat_interval_sec < 5)
            w.emplace_back(std::format(
                "heartbeat_interval_sec={} is very short (<5s) -- "
                "may generate excessive Heartbeat traffic", heartbeat_interval_sec));
        if (!reset_seq_on_logon)
            w.emplace_back("reset_seq_on_logon=false -- sequence numbers must "
                           "be persisted across sessions to avoid gaps");
        if (begin_string != "FIX.4.4" && begin_string != "FIX.4.2" &&
            begin_string != "FIXT.1.1")
            w.emplace_back(std::format(
                "begin_string=\"{}\" is not a commonly used FIX version "
                "(expected FIX.4.2, FIX.4.4, or FIXT.1.1)", begin_string));
        return w;
    }

    /// Equality comparison. Callbacks are excluded (closures are not comparable).
    [[nodiscard]] friend bool operator==(const FixSessionConfig& a,
                                         const FixSessionConfig& b) noexcept {
        return a.sender_comp_id          == b.sender_comp_id
            && a.target_comp_id          == b.target_comp_id
            && a.heartbeat_interval_sec  == b.heartbeat_interval_sec
            && a.reset_seq_on_logon      == b.reset_seq_on_logon
            && a.begin_string            == b.begin_string
            && a.heartbeat_timeout_factor == b.heartbeat_timeout_factor
            && a.resend_on_gap           == b.resend_on_gap;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Logger
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Internal implementation details for the session module.
namespace detail {
/// @brief Get or create the spdlog logger for the session module.
/// @return Shared pointer to the "fix.session" logger.
inline const std::shared_ptr<spdlog::logger>& fix_session_logger() {
    static auto l = [] {
        auto lg = spdlog::get("fix.session");
        if (!lg) {
            // try/catch around the create: a concurrent first-call from
            // another TU can win the registry slot and make this throw
            // "logger already exists". Recover via spdlog::get.
            try { lg = spdlog::stdout_color_mt("fix.session"); }
            catch (const spdlog::spdlog_ex&) { lg = spdlog::get("fix.session"); }
        }
        if (!lg) lg = spdlog::default_logger();
        return lg;
    }();
    return l;
}
} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// FixSession
// ─────────────────────────────────────────────────────────────────────────────

/// @brief FIX 4.4 session manager -- handles Logon/Logout, Heartbeat, sequence tracking.
///
/// Thread model:
///   - `state_`, `expected_inbound_seq_`, `last_inbound_seq_`, `outbound_seq_`:
///     accessed from both TX (caller) and RX (poll) threads via atomics.
///   - `state_` uses acquire/release ordering as the primary synchronization point.
///   - Sequence counters use relaxed ordering where sequenced after a state_ check,
///     or release/acquire at boundaries visible to the other thread.
///   - Configuration (`cfg_`) is immutable after construction — no synchronization needed.
class FixSession {
public:
    /// @brief Send callback type: returns true if the message was sent successfully.
    using SendFn = std::function<bool(const uint8_t* data, size_t len)>;

    /// @brief Construct a FIX session.
    /// @param send_fn  Callback invoked to send raw bytes over the transport.
    /// @param cfg      Session configuration (sender/target IDs, heartbeat, etc.).
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

    /// @brief Send Logon and wait for server Logon response.
    ///
    /// Blocks the calling thread (spin-wait on atomic state).
    /// @param timeout  Maximum time to wait for server Logon response.
    /// @return void on success, or error string on timeout/failure.
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

    /// @brief Send Logout and wait for response.
    /// @param timeout  Maximum time to wait for server Logout response.
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

    /// @brief Reset session state (call before re-logon after disconnect).
    /// @note Resets all sequence numbers to 1 and clears pending TestRequest state.
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

    /// @brief Process an incoming FIX message. Called from the RX thread.
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
            // FIX 4.4 Vol 2 §4 caps MsgSeqNum at uint32. A peer
            // sending MsgSeqNum > UINT32_MAX is sending a protocol-
            // illegal value; the unchecked uint32_t cast below would
            // silently truncate to the low 32 bits and the gap-
            // detection comparison would treat the truncated value as
            // a fresh sequence, advancing `expected_inbound_seq_` to
            // an arbitrary number. Ignore the message so local state
            // stays consistent with the peer's actual wire counter.
            // Same protocol cap is enforced on NewSeqNo in the
            // SequenceReset handler below.
            if (*received_seq >
                static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
                SPDLOG_LOGGER_ERROR(detail::fix_session_logger(),
                    "MsgSeqNum={} exceeds UINT32_MAX — FIX 4.4 caps "
                    "MsgSeqNum at uint32; ignoring message to avoid "
                    "local sequence corruption", *received_seq);
                return true;
            }
            uint32_t recv = static_cast<uint32_t>(*received_seq);
            uint32_t expected = expected_inbound_seq_.load(std::memory_order_relaxed);
            last_inbound_seq_.store(recv, std::memory_order_release);

            // PossDupFlag handling: per FIX 4.4 spec (Vol 2, §4), messages with
            // PossDupFlag=Y are possible retransmissions. This session layer logs them
            // and skips sequence number advancement, but does NOT filter them from the
            // application callback. Callers must implement their own idempotency logic
            // (e.g., dedup by ClOrdID/ExecID) to handle duplicate execution reports.
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
                // Boundary guard: `recv + 1` in uint32_t wraps to 0 when
                // recv == UINT32_MAX. Silently storing 0 corrupts every
                // subsequent gap-detect comparison (anything looks like a
                // fresh gap relative to expected=0), and a peer that
                // controls a single MsgSeqNum field can trigger it. FIX
                // 4.4 §4 caps MsgSeqNum at 4-byte unsigned; recovery
                // requires Logoff + Logon with ResetSeqNumFlag=Y. Until
                // then, freeze the watermark at UINT32_MAX and ERROR-log
                // for the operator.
                if (recv == UINT32_MAX) [[unlikely]] {
                    SPDLOG_LOGGER_ERROR(detail::fix_session_logger(),
                        "Inbound MsgSeqNum at UINT32_MAX boundary "
                        "(recv={}, expected={}): cannot advance, session "
                        "must be reset (Logoff + Logon with "
                        "ResetSeqNumFlag=Y)", recv, expected);
                    expected_inbound_seq_.store(UINT32_MAX,
                        std::memory_order_relaxed);
                } else {
                    expected_inbound_seq_.store(recv + 1, std::memory_order_relaxed);
                }
            } else if (recv == expected) {
                if (recv == UINT32_MAX) [[unlikely]] {
                    // Same boundary as the gap branch — see above.
                    SPDLOG_LOGGER_ERROR(detail::fix_session_logger(),
                        "Inbound MsgSeqNum at UINT32_MAX boundary "
                        "(recv={}): cannot advance, session must be reset",
                        recv);
                    expected_inbound_seq_.store(UINT32_MAX,
                        std::memory_order_relaxed);
                } else {
                    expected_inbound_seq_.store(recv + 1, std::memory_order_relaxed);
                }
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
                    int prev = heartbeat_interval_sec_.load(std::memory_order_acquire);
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
                if (!send_logout()) {
                    SPDLOG_LOGGER_ERROR(detail::fix_session_logger(),
                        "Failed to send Logout response to server-initiated Logout");
                }
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
                // FIX 4.4 Vol 2 §4 caps MsgSeqNum at uint32. A NewSeqNo
                // beyond UINT32_MAX is illegal on the wire and the
                // unchecked uint32_t cast below would silently truncate
                // the high bits, corrupting `expected_inbound_seq_`
                // (e.g. NewSeqNo=UINT32_MAX+5 stores as 4 — every
                // subsequent legitimate message looks like a fresh gap
                // and the session enters a permanent ResendRequest /
                // GapFill ping-pong). Ignore the message instead so
                // local state stays consistent; an operator is expected
                // to follow up with a Logoff + Logon ResetSeqNumFlag=Y.
                if (*new_seq >
                    static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
                    SPDLOG_LOGGER_ERROR(detail::fix_session_logger(),
                        "SequenceReset NewSeqNo={} exceeds UINT32_MAX — "
                        "FIX 4.4 caps MsgSeqNum at uint32; ignoring "
                        "to avoid local sequence corruption (operator "
                        "should drive a Logoff + Logon with "
                        "ResetSeqNumFlag=Y)", *new_seq);
                    return true;
                }
                uint32_t ns = static_cast<uint32_t>(*new_seq);
                if (gap_fill && *gap_fill == "Y") {
                    // GapFill mode: advance expected sequence (must not go backward)
                    uint32_t expected = expected_inbound_seq_.load(std::memory_order_relaxed);
                    if (ns < expected) {
                        SPDLOG_LOGGER_WARN(detail::fix_session_logger(),
                            "GapFill rejected: NewSeqNo={} < expected_inbound_seq_={} "
                            "(backward reset not allowed in GapFill mode)", ns, expected);
                        return true;
                    }
                    if (ns == expected) {
                        SPDLOG_LOGGER_DEBUG(detail::fix_session_logger(),
                            "GapFill no-op: NewSeqNo={} == expected_inbound_seq_={}", ns, expected);
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

    /// @brief Check heartbeat timing. Handles three scenarios:
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

        // Compute the dead-server timeout budget = hb_sec * factor, then
        // float→int64 cast for comparison against the seconds counter.
        // Defense-in-depth NaN/Inf clamp: FixSessionConfig::validate()
        // catches non-finite `heartbeat_timeout_factor` at config-time,
        // BUT the FixSession ctor does not enforce validate() — a caller
        // that constructs a session without checking validate()'s
        // returned diagnostic would feed NaN/inf into this multiply.
        // `static_cast<int64_t>(non-finite double)` is UB per
        // [conv.fpint]/p1 and the optimiser is allowed to assume it
        // never happens, which would silently corrupt the
        // server-dead-detection branch (and on x86 GCC actually returns
        // INT64_MIN, making `recv_elapsed >= INT64_MIN` always true →
        // every tick disconnects). Fall back to disabling the dead-
        // server check (treat as effectively-infinite budget) when the
        // product is non-finite — same fail-soft policy as the
        // ReconnectPolicy NaN clamp.
        const double timeout_d =
            static_cast<double>(hb_sec) * cfg_.heartbeat_timeout_factor;
        const int64_t timeout_secs =
            std::isfinite(timeout_d)
                ? static_cast<int64_t>(timeout_d)
                : std::numeric_limits<int64_t>::max();

        if (recv_elapsed >= timeout_secs) {
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

    /// @brief Send an application-level FIX message.
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

        if (!fill_session_header(builder)) return false;
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

    /// @brief Get current session state.
    [[nodiscard]] SessionState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    /// @brief Get next outbound sequence number to be assigned.
    [[nodiscard]] uint32_t next_outbound_seq() const noexcept {
        return outbound_seq_.load(std::memory_order_relaxed);
    }

    /// @brief Get last received inbound sequence number.
    [[nodiscard]] uint32_t last_inbound_seq() const noexcept {
        return last_inbound_seq_.load(std::memory_order_relaxed);
    }

    /// @brief Get next expected inbound sequence number.
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

    /// @return false if sequence number is exhausted (session must be reset).
    ///
    /// Design limitation — sequence number space:
    ///   FIX MsgSeqNum is a uint32, giving ~4.29 billion messages per session.
    ///   At sustained 1M msg/s this is ~71 minutes; at 10K msg/s it is ~5 days.
    ///   There is no automatic wrap-around because the FIX protocol requires
    ///   monotonically increasing sequence numbers within a session.
    ///
    ///   Operators MUST monitor sequence numbers and proactively initiate a
    ///   Logoff followed by a Logon with ResetSeqNumFlag=Y (tag 141) before
    ///   exhaustion.  A WARN is emitted at 90% capacity (~3.86B) to give
    ///   advance notice.  Once UINT32_MAX is reached, the session refuses
    ///   to send and must be torn down and re-established.
    [[nodiscard]] bool fill_session_header(MessageBuilder& b) noexcept {
        b.set(tag::SenderCompID, cfg_.sender_comp_id);
        b.set(tag::TargetCompID, cfg_.target_comp_id);

        // Atomic claim of the next MsgSeqNum.  The TX (app) thread and the RX
        // poll thread both call this (RX → on_rx → send_heartbeat /
        // send_test_request / send_sequence_reset_gap_fill) — see the
        // class-level "Thread model" comment.  A simple load+store would race
        // and produce duplicate MsgSeqNum on the wire (a fatal session-level
        // FIX violation that the counter-party drops the session for); a
        // single fetch_add could not cleanly enforce the UINT32_MAX boundary.
        // We therefore CAS-claim seq in a loop, refusing the claim once the
        // counter is exhausted.
        uint32_t seq = outbound_seq_.load(std::memory_order_relaxed);
        for (;;) {
            if (seq >= UINT32_MAX) [[unlikely]] {
                SPDLOG_LOGGER_ERROR(detail::fix_session_logger(),
                    "Outbound sequence number exhausted (seq={}): cannot send, "
                    "must reset session", seq);
                return false;
            }
            if (outbound_seq_.compare_exchange_weak(
                    seq, seq + 1,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                break;  // We exclusively own `seq` for this message.
            }
            // Another thread won this slot — `seq` was reloaded, retry.
        }

        // Warn at 90% capacity so operators can schedule a session reset.
        constexpr uint32_t warn_threshold =
            static_cast<uint32_t>(static_cast<uint64_t>(UINT32_MAX) * 9 / 10);
        if (seq >= warn_threshold) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::fix_session_logger(),
                "Outbound sequence number at {}% capacity (seq={}): "
                "schedule session reset (Logoff + Logon with ResetSeqNumFlag=Y) "
                "before exhaustion",
                static_cast<unsigned>(
                    static_cast<uint64_t>(seq) * 100 / UINT32_MAX),
                seq);
        }

        b.set_int(tag::MsgSeqNum, static_cast<int64_t>(seq));

        uint64_t now_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        b.set_timestamp(tag::SendingTime, now_ns);
        return true;
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
        if (!fill_session_header(b)) return false;
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
        if (!fill_session_header(b)) return false;
        return send_message(b);
    }

    bool send_heartbeat(std::string_view test_req_id = {}) noexcept {
        uint8_t buf[256];
        MessageBuilder b(buf, sizeof(buf));
        b.set(tag::MsgType, "0");
        if (!fill_session_header(b)) return false;
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

        // snprintf returns negative on encoding error (vanishingly rare for
        // a simple format) or the would-be-written length on truncation.
        // Both edges hit the same string_view-builder downstream: a naked
        // `static_cast<size_t>(n)` wraps a -1 to ~SIZE_MAX, and a >= sizeof
        // result over-reads past `id_buf`. Clamp to [0, sizeof - 1] so the
        // resulting view is always inside the buffer.
        const size_t id_len = (n < 0)
            ? 0u
            : std::min(static_cast<size_t>(n), sizeof(id_buf) - 1);

        uint8_t buf[256];
        MessageBuilder b(buf, sizeof(buf));
        b.set(tag::MsgType, "1");
        if (!fill_session_header(b)) return false;
        b.set(tag::TestReqID, std::string_view(id_buf, id_len));
        SPDLOG_LOGGER_DEBUG(detail::fix_session_logger(),
            "Sending TestRequest (TestReqID={})", std::string_view(id_buf, id_len));
        // Mark pending ONLY if the wire write succeeded. Previous order
        // set pending=true before send_message returned — on TX failure
        // (TLS desync, socket disconnect, builder overflow) the bucket
        // would be left "pending probe never answered", and the next
        // tick() would observe `test_request_pending_=true` past the
        // timeout window and declare the server dead even though we
        // never actually probed it. Move the store after send so a
        // failed send leaves the pending flag false and the next
        // tick() can re-issue the probe legitimately.
        const bool ok = send_message(b);
        if (ok) {
            test_request_pending_.store(true, std::memory_order_release);
        } else {
            // Use the same clamped id_len from above. Reusing the raw `n`
            // here would re-introduce the wrap/over-read bug: a negative
            // n (snprintf encoding error) would `static_cast<size_t>` to
            // ~SIZE_MAX and the log formatter would walk off the buffer;
            // n >= sizeof(id_buf) (truncation) would over-read past the
            // null terminator. The success branch builds the wire field
            // from `id_len`; the diagnostic must use the same length.
            SPDLOG_LOGGER_WARN(detail::fix_session_logger(),
                "send_test_request: TX failed (TestReqID={}) — pending flag "
                "NOT set; next tick() will re-probe instead of declaring "
                "the server dead",
                std::string_view(id_buf, id_len));
        }
        return ok;
    }

    bool send_resend_request(uint32_t begin_seq, uint32_t end_seq) noexcept {
        uint8_t buf[256];
        MessageBuilder b(buf, sizeof(buf));
        b.set(tag::MsgType, "2");
        if (!fill_session_header(b)) return false;
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
        if (!fill_session_header(b)) return false;
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

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specializations
// ─────────────────────────────────────────────────────────────────────────────

template <>
struct std::formatter<eph::fix::FixSessionConfig> : std::formatter<std::string> {
    auto format(const eph::fix::FixSessionConfig& c, auto& ctx) const {
        return std::formatter<std::string>::format(c.dump(), ctx);
    }
};

template <>
struct std::formatter<eph::fix::SessionState> : std::formatter<std::string_view> {
    auto format(eph::fix::SessionState s, auto& ctx) const {
        return std::formatter<std::string_view>::format(
            eph::fix::session_state_name(s), ctx);
    }
};
