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

class FakeTcpTransport {
public:
    // -- TcpTransport concept methods -----------------------------------------

    [[nodiscard]] std::expected<void, std::string>
    connect(std::chrono::milliseconds /*timeout*/) {
        if (connect_error_) return std::unexpected(*connect_error_);
        state_ = TcpState::Established;
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> close() {
        state_ = TcpState::Closed;
        return {};
    }

    void reset() noexcept { state_ = TcpState::Closed; }

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

    template <typename F>
    [[nodiscard]] std::expected<uint16_t, std::string> poll_rx(F&& callback) {
        if (rx_error_) return std::unexpected(*rx_error_);
        if (rx_queue_.empty()) return 0;
        auto& pkt = rx_queue_.front();
        callback(pkt.data(), static_cast<uint16_t>(pkt.size()));
        rx_queue_.pop_front();
        return 1;
    }

    [[nodiscard]] uint64_t last_rx_burst_tsc() const noexcept { return 0; }
    [[nodiscard]] uint16_t mss() const noexcept { return 1460; }
    [[nodiscard]] TcpState state() const noexcept { return state_; }
    [[nodiscard]] bool is_established() const noexcept {
        return state_ == TcpState::Established;
    }

    // -- Programmable test control --------------------------------------------

    /// Inject data that will be returned by next poll_rx() call.
    void inject_rx(std::vector<uint8_t> data) {
        rx_queue_.push_back(std::move(data));
    }

    /// Inject data from raw pointer + length.
    void inject_rx(const uint8_t* data, size_t len) {
        rx_queue_.emplace_back(data, data + len);
    }

    /// Make next connect() fail with this error.
    void set_connect_error(std::string err) {
        connect_error_ = std::move(err);
    }

    /// Make next send() fail with this error.
    void set_send_error(std::string err) {
        send_error_ = std::move(err);
    }

    /// Make next poll_rx() fail with this error.
    void set_rx_error(std::string err) {
        rx_error_ = std::move(err);
    }

    /// Clear all injected errors.
    void clear_errors() {
        connect_error_.reset();
        send_error_.reset();
        rx_error_.reset();
    }

    /// Simulate disconnect (state change without close handshake).
    void inject_disconnect() {
        state_ = TcpState::Closed;
    }

    /// Access sent data for verification.
    [[nodiscard]] const std::vector<std::vector<uint8_t>>& sent_data() const noexcept {
        return sent_data_;
    }

    /// Clear sent data buffer.
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
