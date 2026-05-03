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
///     stream->on_message = [&](std::span<const uint8_t> app_frame) {
///         captured.assign(app_frame.begin(), app_frame.end());
///     };
///     TestPoller<FakeStream> poller;
///     poller.add(stream.get()).value();
///     stream->inject_rx(wire_bytes);
///     poller.poll();                  // drains rx buffer, fires on_message
///     EXPECT_EQ(stream->collect_tx(), expected_tx);

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "eph/core/error.hpp"
#include "eph/core/packet_view.hpp"
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
    /// @brief Associated PacketView — conforming `eph::core::PacketView`.
    ///
    /// Previously this was a 2-field `{data, length}` stub, which drifted
    /// from the real backends (`SpanView`, `MbufView`) that satisfy the full
    /// `eph::core::PacketView` contract (writable_data / trim_front /
    /// trim_back / arrival_tsc). The drift meant tests that composed a real
    /// `StreamCodec` over `FakeStream` would fail to compile inside the
    /// codec because `decode(view)` invokes `trim_front` / `writable_data`.
    ///
    /// This version carries `base_ + head_/tail_ + tsc_` — the same layout as
    /// the kernel `SpanView`, so the mock exercises the same codec code
    /// paths the real backends do.
    struct PacketView {
        constexpr PacketView(uint8_t* base, std::size_t len,
                             uint64_t tsc = 0) noexcept
            : base_(base), head_(0), tail_(len), tsc_(tsc) {}

        [[nodiscard]] uint8_t* writable_data() noexcept {
            return base_ + head_;
        }
        [[nodiscard]] const uint8_t* data() const noexcept {
            return base_ + head_;
        }
        [[nodiscard]] std::size_t length() const noexcept {
            return tail_ - head_;
        }

        /// @brief Advance head cursor — skb_pull equivalent. Clamps at
        ///        `length()` so an over-trim collapses the view to empty
        ///        rather than underflowing `length()` (matches the
        ///        production `MbufView` / `SpanView` contract; tests that
        ///        feed a real codec must see the same behavior the
        ///        production backends do).
        constexpr void trim_front(std::size_t n) noexcept {
            head_ += (n > tail_ - head_) ? (tail_ - head_) : n;
        }
        /// @brief Pull back tail cursor — skb_trim equivalent. Symmetric
        ///        clamp to `trim_front`.
        constexpr void trim_back(std::size_t n) noexcept {
            tail_ -= (n > tail_ - head_) ? (tail_ - head_) : n;
        }

        [[nodiscard]] uint64_t arrival_tsc() const noexcept { return tsc_; }

    private:
        uint8_t*    base_{nullptr};
        std::size_t head_{0};
        std::size_t tail_{0};
        uint64_t    tsc_{0};
    };

    /// @brief No codec is attached — tests compose FakeStream + a codec
    /// of their choosing at the call site.
    using CodecType = void;

    /// @brief Receive callback: invoked once per injected rx chunk.
    ///
    /// The span argument holds one decoded application frame — the bytes have
    /// already passed through TLS (if enabled) and the codec's framing layer,
    /// so they are the application-layer payload the user cares about.
    /// Signature matches `eph::net::Stream::OnMessage` contract.
    using OnMessage = std::function<void(std::span<const uint8_t>)>;

    // ── Construction ───────────────────────────────────────────────────────

    FakeStream() = default;

    /// @brief Factory for parity with the real `KernelTcpStream::create`
    ///        API. Returns a heap-allocated FakeStream because Poller stores
    ///        raw pointers and ownership tends to live in the test harness.
    [[nodiscard]] static std::unique_ptr<FakeStream> create() {
        return std::make_unique<FakeStream>();
    }

    // ── Test-control API ───────────────────────────────────────────────────

    /// @brief Feed bytes as if they had arrived from the wire. `wire_bytes`
    ///        are raw pre-codec bytes (the same granularity `recv()` would
    ///        return from a real socket); they are copied into an internal
    ///        rx buffer. Call `poll()` on the owning `TestPoller` to drain.
    void inject_rx(std::span<const uint8_t> wire_bytes) {
        rx_buf_.insert(rx_buf_.end(), wire_bytes.begin(), wire_bytes.end());
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

    /// @brief Simulate a peer-initiated disconnect in one call: flips
    ///        `state_` to `Closed` and drops any undelivered rx bytes
    ///        (matching the real-backend sequence where recv() surfaces
    ///        `Disconnected` and `poll_once_` early-returns without
    ///        dispatching buffered ciphertext). Tests that want
    ///        drain-then-close semantics should call `poll_once_()` first
    ///        and then `inject_disconnect()`.
    ///
    /// Added in batch3-round3 MEDIUM-2 — previously tests had to
    /// open-code `set_state(Closed) + rx_buf.clear()`.
    void inject_disconnect() noexcept {
        state_ = TcpState::Closed;
        rx_buf_.clear();
    }

    /// @brief Simulate being attached or detached from a Poller. The
    ///        TestPoller drives this automatically; test code rarely needs
    ///        to call it directly.
    void set_attached(bool a) noexcept { attached_ = a; }

    /// @brief Queue a one-shot `ErrorInfo` to be returned by the next
    ///        `send()` call. Lets tests cover caller behavior under
    ///        every `core::Error` variant (WouldBlock, BufferFull,
    ///        IoError, Timeout, …) — not just the built-in
    ///        `NotAttached` / `Disconnected` paths.
    ///
    /// Multiple calls queue multiple errors (FIFO). Once the queue is
    /// drained, `send()` returns to its normal accumulating behavior.
    /// The state guards (NotAttached / Disconnected) take precedence
    /// over an injected error so that tests cannot accidentally observe
    /// a "send succeeded" path on a closed stream.
    ///
    /// Added in round-46 batch 4 to close the FakeStream error-coverage
    /// gap flagged by the test-blind-spot review: previously there was
    /// no way to verify "caller handles `WouldBlock` from send" or
    /// similar in unit tests.
    void inject_send_error(core::ErrorInfo err) {
        pending_send_errors_.push_back(err);
    }

    /// @brief Convenience wrapper for the common case (queue one error
    ///        with default-constructed detail).
    void inject_send_error(core::Error code,
                            const char* detail = "FakeStream::send: injected") {
        pending_send_errors_.push_back(core::ErrorInfo{code, detail});
    }

    /// @brief Test introspection — how many injected errors remain queued.
    [[nodiscard]] std::size_t pending_send_errors() const noexcept {
        return pending_send_errors_.size();
    }

    // ── Stream concept implementation ──────────────────────────────────────

    /// @brief Public `on_message` sink: assigned by the test before calling
    ///        `poll()`, invoked once per rx buffer drain.
    OnMessage on_message;

    /// @brief Append `app_payload` to the internal TX buffer. Returns the
    ///        number of bytes "sent" (always all of them — no backpressure
    ///        simulation). Fails with `NotAttached` if the fake is not
    ///        currently attached to a poller, mirroring the contract
    ///        documented in `eph::core::Error::NotAttached`. Also fails with
    ///        `Disconnected` when `state_ != Established` so that tests
    ///        driving a closed stream through `send()` see the same error
    ///        the real backends would produce (see `KernelTcpStream::send`).
    [[nodiscard]] std::expected<std::size_t, core::ErrorInfo>
    send(std::span<const uint8_t> app_payload) noexcept {
        if (!attached_) {
            return std::unexpected(core::ErrorInfo{
                core::Error::NotAttached,
                "FakeStream::send called before attach"});
        }
        if (state_ != TcpState::Established) {
            return std::unexpected(core::ErrorInfo{
                core::Error::Disconnected,
                "FakeStream::send: state != Established"});
        }
        // Pop a queued injected error AFTER the state guards so that
        // tests cannot observe an error-injected path on a closed stream
        // (real backends always check session state first).
        if (!pending_send_errors_.empty()) {
            auto err = pending_send_errors_.front();
            pending_send_errors_.pop_front();
            return std::unexpected(err);
        }
        tx_buf_.insert(tx_buf_.end(), app_payload.begin(), app_payload.end());
        return app_payload.size();
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
    /// because the Pollable concept requires it be callable. The real
    /// kernel / dpdk backends declare it `private + friend Poller`.
    std::size_t poll_once_() noexcept {
        // Mirror the real-backend contract: do not dispatch on a closed
        // stream (KernelTcpStream::poll_once_:625, DpdkTcpStream likewise).
        // A test that closes the stream then expects no more on_message
        // fires should see exactly that behavior from the mock.
        if (state_ != TcpState::Established) return 0;
        if (rx_buf_.empty()) return 0;
        if (on_message) {
            // Emit the buffered payload as one application frame. The span
            // carries the full size_t extent — the OnMessage signature no
            // longer clamps to uint16_t.
            on_message(std::span<const uint8_t>(rx_buf_.data(), rx_buf_.size()));
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
    std::vector<uint8_t>          rx_buf_;
    std::vector<uint8_t>          tx_buf_;
    std::deque<core::ErrorInfo>   pending_send_errors_;
    TcpState                      state_{TcpState::Established};
    bool                          attached_{false};
    bool                          closed_gracefully_{false};
};

// Compile-time concept conformance check — if the Stream concept ever evolves
// and FakeStream falls behind, this static_assert fires on inclusion.
static_assert(::eph::core::PacketView<FakeStream::PacketView>,
              "FakeStream::PacketView must satisfy eph::core::PacketView");
static_assert(Pollable<FakeStream>,
              "FakeStream must satisfy eph::net::Pollable");
static_assert(Stream<FakeStream>,
              "FakeStream must satisfy eph::net::Stream");

} // namespace eph::net::test
