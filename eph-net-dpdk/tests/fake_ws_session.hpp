/// @file fake_ws_session.hpp
/// Test-only fake that substitutes `::eph::dpdk::TcpSession<>` in the
/// `PlainDpdkWsSink` / `TlsDpdkWsSink` templates for unit testing the
/// sinks' `ByteSink` contract without a live NIC / DPDK EAL / TLS stack.
///
/// Duck-types the subset of TcpSession API the sinks actually use:
///
///     size_t                        mss() const noexcept;
///     expected<size_t, ErrorInfo>   send(const uint8_t*, size_t) noexcept;
///     template <class Cb>
///     expected<void, ErrorInfo>     poll_rx(Cb&&) noexcept;
///
/// Behavior is entirely driven by public script fields — each test sets
/// them, drives the sink, and inspects tx_captured / the sink's return.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <vector>

#include "eph/core/error.hpp"

namespace eph::net::dpdk::testing {

struct FakeDpdkSessionForWs {
    // ── Script fields (set by test before driving the sink) ──────────────

    /// Value returned by `mss()`. Default matches production for realism;
    /// tests can shrink it to force the sink's internal chunk loop.
    std::size_t              mss_val{1460};

    /// Every byte the sink passed to `send()` accumulated here, in call
    /// order. Tests inspect this to confirm the sink forwarded payload
    /// correctly.
    std::vector<uint8_t>     tx_captured{};

    /// Bytes the sink's `poll_rx()` callback will be invoked with, in 64-B
    /// batches. Tests preload ciphertext / plaintext here; the fake emits
    /// until exhausted then reports empty bursts.
    std::vector<uint8_t>     rx_script{};
    std::size_t              rx_off{0};

    /// If true, `poll_rx` completes immediately without invoking the
    /// callback — models a NIC with no packets inbound. Used to force the
    /// sink's bounded-retry loop to exhaust and return `WouldBlock`.
    bool                     block_forever{false};

    /// If true, `send` returns `Error::Disconnected` without buffering.
    bool                     fail_send{false};

    /// If true, `poll_rx` returns `Error::Disconnected` without invoking
    /// the callback.
    bool                     fail_poll_rx{false};

    /// Index of the send call (0-based) at which to return 0 instead of
    /// the requested length. Used to exercise the sink's `BufferFull`
    /// translation path. SIZE_MAX disables.
    std::size_t              send_returns_zero_at{
        std::numeric_limits<std::size_t>::max()};

    /// Count of send() invocations so far (for send_returns_zero_at).
    std::size_t              send_calls{0};

    // ── Mimicked TcpSession<> surface ─────────────────────────────────────

    std::size_t mss() const noexcept { return mss_val; }

    std::expected<std::size_t, ::eph::core::ErrorInfo>
    send(const uint8_t* data, std::size_t len) noexcept {
        const auto call_idx = send_calls++;
        if (fail_send) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::Disconnected,
                "FakeDpdkSessionForWs::send: scripted failure"});
        }
        if (call_idx == send_returns_zero_at) {
            return std::size_t{0};
        }
        tx_captured.insert(tx_captured.end(), data, data + len);
        return len;
    }

    template <class Cb>
    std::expected<void, ::eph::core::ErrorInfo>
    poll_rx(Cb&& cb) noexcept {
        if (fail_poll_rx) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::Disconnected,
                "FakeDpdkSessionForWs::poll_rx: scripted failure"});
        }
        if (block_forever) {
            // Empty burst: no callback invocation, void success.
            return {};
        }
        if (rx_off >= rx_script.size()) {
            // Script exhausted → empty burst (same as block_forever once
            // all scripted bytes are delivered).
            return {};
        }
        constexpr std::size_t kBurstChunk = 64;
        const std::size_t avail = rx_script.size() - rx_off;
        const std::size_t n     = std::min(avail, kBurstChunk);
        cb(rx_script.data() + rx_off, static_cast<uint16_t>(n));
        rx_off += n;
        return {};
    }
};

} // namespace eph::net::dpdk::testing
