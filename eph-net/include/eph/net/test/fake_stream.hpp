#pragma once

/// @file test/fake_stream.hpp
/// In-memory `Stream` concept mock for unit tests.
///
/// `FakeStream` satisfies `eph::net::Stream` without any syscalls — tests
/// can drive a Poller against it, inject bytes via `inject_rx()`, and
/// inspect the resulting TX via `collect_tx()`.
///
/// Intended usage pattern:
///
///     auto stream = FakeStream::create();
///     stream->on_message = [&](const uint8_t* p, uint16_t n) {
///         captured.assign(p, p + n);
///     };
///     TestPoller<FakeStream> poller;
///     poller.add(stream.get()).value();
///     stream->inject_rx(raw_bytes);
///     poller.poll();                  // drains rx buffer, fires on_message
///     EXPECT_EQ(stream->collect_tx(), expected_tx);

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "eph/core/error.hpp"
#include "eph/net/concepts.hpp"
#include "eph/net/tcp_state.hpp"

namespace eph::net::test {

// ---------------------------------------------------------------------------
// FakeStream
// ---------------------------------------------------------------------------

/// @brief In-memory mock satisfying `eph::net::Stream`.
///
/// The implementation is deliberately dumb: bytes go in via `inject_rx`, come
/// out via `on_message` when the Poller calls `poll_once_()`, and
/// `send`-written bytes accumulate in an internal TX buffer observable via
/// `collect_tx`. There is no codec — `CodecType` is the empty `void` tag
/// expected by the Stream concept. Tests that want codec behavior should
/// wire a real codec around this fake at the application layer.
class FakeStream {
public:
    /// @brief Associated PacketView type — contiguous read-only span.
    ///
    /// For testing purposes we only need the data pointer + length;
    /// backends that require mbuf-style mutation belong in the real kernel
    /// or DPDK streams.
    struct PacketView {
        const uint8_t* data_;
        std::size_t    length_;

        [[nodiscard]] const uint8_t* data() const noexcept { return data_; }
        [[nodiscard]] std::size_t length() const noexcept { return length_; }
    };

    /// @brief No codec is attached — tests compose FakeStream + a codec
    /// of their choosing at the call site.
    using CodecType = void;

    /// @brief Receive callback: invoked once per injected rx chunk.
    /// Signature matches `eph::net::Stream::OnMessage` contract.
    using OnMessage = std::function<void(const uint8_t*, uint16_t)>;

    // ── Construction ───────────────────────────────────────────────────────

    FakeStream() = default;

    /// @brief Factory for parity with the real `KernelTcpStream::create`
    ///        API. Returns a heap-allocated FakeStream because Poller stores
    ///        raw pointers and ownership tends to live in the test harness.
    [[nodiscard]] static std::unique_ptr<FakeStream> create() {
        return std::make_unique<FakeStream>();
    }

    // ── Test-control API ───────────────────────────────────────────────────

    /// @brief Feed bytes as if they had arrived from the wire. The data is
    ///        copied into an internal rx buffer; call `poll()` on the owning
    ///        `TestPoller` to drain.
    void inject_rx(std::span<const uint8_t> data) {
        rx_buf_.insert(rx_buf_.end(), data.begin(), data.end());
    }

    /// @brief Return a view into the accumulated TX buffer — bytes the code
    ///        under test wrote via `send()`. The span is valid until the
    ///        next `send()` or `clear_tx()` call.
    [[nodiscard]] std::span<const uint8_t> collect_tx() const noexcept {
        return {tx_buf_.data(), tx_buf_.size()};
    }

    /// @brief Drop all accumulated TX bytes without affecting state.
    void clear_tx() noexcept { tx_buf_.clear(); }

    /// @brief Force the reported `TcpState` — useful for tests that need
    ///        to simulate half-open or closing sessions.
    void set_state(TcpState s) noexcept { state_ = s; }

    /// @brief Simulate being attached or detached from a Poller. The
    ///        TestPoller drives this automatically; test code rarely needs
    ///        to call it directly.
    void set_attached(bool a) noexcept { attached_ = a; }

    // ── Stream concept implementation ──────────────────────────────────────

    /// @brief Public `on_message` sink: assigned by the test before calling
    ///        `poll()`, invoked once per rx buffer drain.
    OnMessage on_message;

    /// @brief Append `data` to the internal TX buffer. Returns the number of
    ///        bytes "sent" (always all of them — no backpressure simulation).
    ///        Fails with `NotAttached` if the fake is not currently attached
    ///        to a poller, mirroring the contract documented in
    ///        `eph::core::Error::NotAttached`.
    [[nodiscard]] std::expected<std::size_t, core::ErrorInfo>
    send(std::span<const uint8_t> data) {
        if (!attached_) {
            return std::unexpected(core::ErrorInfo{
                core::Error::NotAttached,
                "FakeStream::send called before attach"});
        }
        tx_buf_.insert(tx_buf_.end(), data.begin(), data.end());
        return data.size();
    }

    /// @brief Flip the state to `Closed` and record a graceful close.
    [[nodiscard]] std::expected<void, core::ErrorInfo> close_gracefully() noexcept {
        state_ = TcpState::Closed;
        closed_gracefully_ = true;
        return {};
    }

    [[nodiscard]] bool is_attached() const noexcept { return attached_; }
    [[nodiscard]] TcpState state() const noexcept { return state_; }

    /// @brief Whether the graceful-close path has been exercised. Test-only.
    [[nodiscard]] bool closed_gracefully() const noexcept {
        return closed_gracefully_;
    }

    // ── Pollable concept implementation ───────────────────────────────────

    /// @brief Drain the rx buffer by handing it to `on_message`. Called by
    ///        `TestPoller::poll`. Returns the number of frames delivered
    ///        (0 if the rx buffer is empty, 1 otherwise — the fake emits
    ///        the entire buffered payload as one frame).
    ///
    /// `poll_once_` is logically private (Poller-internal) but kept public
    /// because the v3.3 Pollable concept requires it be callable. The real
    /// kernel / dpdk backends declare it `private + friend Poller`.
    std::size_t poll_once_() noexcept {
        if (rx_buf_.empty()) return 0;
        if (on_message) {
            // Cap at uint16_t as the Stream concept's OnMessage signature
            // uses uint16_t — this matches the design doc's wire-format
            // assumption that a single frame fits in 65535 bytes.
            const auto len = static_cast<uint16_t>(
                rx_buf_.size() > 0xFFFF ? 0xFFFF : rx_buf_.size());
            on_message(rx_buf_.data(), len);
        }
        rx_buf_.clear();
        return 1;
    }

    [[nodiscard]] bool is_attached_() const noexcept { return attached_; }

    /// @brief Pollable concept contract — backend handle. For the fake this
    ///        is a stable pointer to the FakeStream instance itself; tests
    ///        can use it as a unique id.
    [[nodiscard]] void* native_handle() const noexcept {
        return const_cast<void*>(static_cast<const void*>(this));
    }

private:
    std::vector<uint8_t> rx_buf_;
    std::vector<uint8_t> tx_buf_;
    TcpState             state_{TcpState::Established};
    bool                 attached_{false};
    bool                 closed_gracefully_{false};
};

// Compile-time concept conformance check — if the Stream concept ever evolves
// and FakeStream falls behind, this static_assert fires on inclusion.
static_assert(Pollable<FakeStream>,
              "FakeStream must satisfy eph::net::Pollable");
static_assert(Stream<FakeStream>,
              "FakeStream must satisfy eph::net::Stream");

} // namespace eph::net::test
