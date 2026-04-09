#pragma once

/// @file fake_tcp_transport.hpp
/// Test double for TcpTransport -- programmable mock for integration testing.

#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "eph/core/tcp_concept.hpp"

namespace eph::net::testing {

/// @brief Programmable in-memory TcpTransport test double.
///
/// Satisfies the TcpTransport concept without touching the network, which
/// lets upper layers (framers, session state machines, protocol codecs) be
/// tested deterministically. The class exposes two orthogonal surfaces:
///
///   1. The TcpTransport concept methods (connect/close/reset/send/poll_rx/...)
///      used exactly as a real transport would be.
///   2. A "programmable" surface (inject_rx, set_connect_error, sent_data, ...)
///      that lets tests stage RX data, force failures, and inspect TX bytes.
///
/// The mock begins in @c TcpState::Closed and transitions to
/// @c TcpState::Established on a successful @c connect().
///
/// @note Not thread-safe — intended for single-threaded unit tests.
class FakeTcpTransport {
public:
    // -- TcpTransport concept methods -----------------------------------------

    /// @brief Simulated TCP connect.
    ///
    /// Transitions the mock to @c TcpState::Established unless a failure was
    /// previously staged via @c set_connect_error().
    ///
    /// @param timeout  Unused by the mock (kept for concept compatibility).
    /// @return Success, or the staged connect error string on failure.
    [[nodiscard]] std::expected<void, std::string>
    connect(std::chrono::milliseconds /*timeout*/) {
        if (connect_error_) return std::unexpected(*connect_error_);
        state_ = TcpState::Established;
        return {};
    }

    /// @brief Simulated graceful close.
    ///
    /// Transitions the mock to @c TcpState::Closed. Never fails.
    /// @return Always success.
    [[nodiscard]] std::expected<void, std::string> close() {
        state_ = TcpState::Closed;
        return {};
    }

    /// @brief Simulated forced reset (RST).
    ///
    /// Drops the mock to @c TcpState::Closed without any handshake. Does not
    /// clear queued RX data or previously recorded TX data — tests that need a
    /// clean slate should use @c clear_sent_data() / @c clear_errors() too.
    void reset() noexcept { state_ = TcpState::Closed; }

    /// @brief Simulated send — records the payload into an internal buffer.
    ///
    /// Returns the staged send error if one was set, otherwise the entire
    /// payload is "sent" (copied into @c sent_data()).
    ///
    /// @param data  Payload pointer. Copied into an internal vector.
    /// @param len   Payload length in bytes.
    /// @return Number of bytes "sent" (always @p len on success), or the
    ///         staged error string / "not connected" when not established.
    [[nodiscard]] std::expected<size_t, std::string>
    send(const void* data, size_t len) {
        if (send_error_) return std::unexpected(*send_error_);
        if (state_ != TcpState::Established)
            return std::unexpected(std::string("not connected"));
        sent_data_.emplace_back(
            static_cast<const uint8_t*>(data),
            static_cast<const uint8_t*>(data) + len);
        return len;
    }

    /// @brief Simulated RX poll — delivers one queued packet per call.
    ///
    /// If an RX error was staged, returns it. Otherwise pops the front of the
    /// injected RX queue (if any) and invokes @p callback with its bytes.
    ///
    /// @tparam F         Callable with signature `void(const uint8_t*, uint16_t)`.
    /// @param  callback  Invoked once per delivered packet.
    /// @return 1 if a packet was delivered, 0 if the queue was empty, or the
    ///         staged error string.
    template <typename F>
    [[nodiscard]] std::expected<uint16_t, std::string> poll_rx(F&& callback) {
        if (rx_error_) return std::unexpected(*rx_error_);
        if (rx_queue_.empty()) return 0;
        auto& pkt = rx_queue_.front();
        callback(pkt.data(), static_cast<uint16_t>(pkt.size()));
        rx_queue_.pop_front();
        return 1;
    }

    /// @brief Last RX burst TSC — always 0 in the mock (no hardware timestamp).
    [[nodiscard]] uint64_t last_rx_burst_tsc() const noexcept { return 0; }
    /// @brief Reported MSS — hardcoded to 1460 (standard Ethernet).
    [[nodiscard]] uint16_t mss() const noexcept { return 1460; }
    /// @brief Current simulated TCP state.
    [[nodiscard]] TcpState state() const noexcept { return state_; }
    /// @brief Convenience check — true iff @c state() == @c Established.
    [[nodiscard]] bool is_established() const noexcept {
        return state_ == TcpState::Established;
    }

    // -- Programmable test control --------------------------------------------

    /// @brief Queue a packet to be delivered on the next @c poll_rx() call.
    /// @param data  Bytes to deliver. Moved into the internal queue.
    void inject_rx(std::vector<uint8_t> data) {
        rx_queue_.push_back(std::move(data));
    }

    /// @brief Queue a packet from a raw pointer/length pair.
    /// @param data  Pointer to the payload bytes (copied into the queue).
    /// @param len   Payload length in bytes.
    void inject_rx(const uint8_t* data, size_t len) {
        rx_queue_.emplace_back(data, data + len);
    }

    /// @brief Stage a connect error — the next @c connect() call returns it.
    /// @param err  Error string returned via @c std::unexpected.
    void set_connect_error(std::string err) {
        connect_error_ = std::move(err);
    }

    /// @brief Stage a send error — every subsequent @c send() returns it
    ///        until @c clear_errors() is called.
    /// @param err  Error string returned via @c std::unexpected.
    void set_send_error(std::string err) {
        send_error_ = std::move(err);
    }

    /// @brief Stage an RX error — every subsequent @c poll_rx() returns it
    ///        until @c clear_errors() is called.
    /// @param err  Error string returned via @c std::unexpected.
    void set_rx_error(std::string err) {
        rx_error_ = std::move(err);
    }

    /// @brief Drop all staged connect/send/RX errors.
    void clear_errors() {
        connect_error_.reset();
        send_error_.reset();
        rx_error_.reset();
    }

    /// @brief Simulate an abrupt disconnect (state change, no handshake).
    ///
    /// Useful for exercising reconnect logic in upper layers.
    void inject_disconnect() {
        state_ = TcpState::Closed;
    }

    /// @brief Access the list of payloads recorded by @c send().
    ///
    /// Each vector is one @c send() call; the outer vector preserves order.
    /// @return Reference to the internal TX history buffer.
    [[nodiscard]] const std::vector<std::vector<uint8_t>>& sent_data() const noexcept {
        return sent_data_;
    }

    /// @brief Drop all recorded TX payloads.
    void clear_sent_data() { sent_data_.clear(); }

private:
    TcpState state_ = TcpState::Closed;
    std::deque<std::vector<uint8_t>> rx_queue_;
    std::vector<std::vector<uint8_t>> sent_data_;
    std::optional<std::string> connect_error_;
    std::optional<std::string> send_error_;
    std::optional<std::string> rx_error_;
};

static_assert(TcpTransport<FakeTcpTransport>,
    "FakeTcpTransport must satisfy TcpTransport concept");

} // namespace eph::net::testing
