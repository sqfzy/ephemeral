#pragma once

/// @file codec.hpp
/// StreamCodec / DatagramCodec concepts + OutputBuffer sink.
///
/// This header is part of the Phase 0 slim-down of eph-core (see
/// .artifacts/design-eph-v3.3-architecture-20260410.md). It defines the new
/// stateful codec contract that replaces the old `MessageFramer` concept in
/// `eph-core/framer_concept.hpp`.
///
/// Differences from the old MessageFramer concept:
///   - `decode()` is stateful (takes `T&`, not `const T&`) so that a codec
///     can hold cross-frame state like WebSocket continuation fragments,
///     SoupBinTCP session state, or TLS-into-plaintext scratch.
///   - `decode()` receives an `OutputBuffer&` sink so that a codec can inject
///     an auto-response without involving the application thread. Example:
///     `WsCodec` sees a ping frame, writes the pong reply into `OutputBuffer`,
///     and returns `Ok(None)` — the application never observes the ping.
///   - Two parallel concepts (`StreamCodec` vs `DatagramCodec`) split the
///     TCP-style "partial bytes OK, maintain state" contract from the UDP-style
///     "each datagram is atomic, emit 0..N frames" contract. A `Codec` helper
///     concept matches either.
///
/// Zero-copy is preserved by duck-typing the `PacketView` input: each Stream /
/// Datagram backend provides its own `PacketView` type (e.g. `MbufView` for
/// DPDK, `SpanView` for kernel) and the codec concept only requires that the
/// argument be whatever the codec names via `T::PacketViewRef`. This keeps
/// eph-core free of any dependency on kernel fd's or DPDK mbufs.

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <optional>
#include <utility>

#include "eph/core/error.hpp"

namespace eph::core {

// ---------------------------------------------------------------------------
// OutputBuffer
// ---------------------------------------------------------------------------

/// @brief Write sink passed to `StreamCodec::decode` / `DatagramCodec::decode`.
///
/// Codecs use this to inject auto-responses into the transport's TX path
/// without a round-trip through the application thread. For example,
/// `WsCodec::decode` on seeing a control-frame ping will `append(pong_bytes)`
/// and then return `Ok(None)` so the application never observes the ping.
///
/// Implementation notes:
///   - This is a non-owning view over a caller-provided byte region. The
///     caller (usually the transport's TX staging buffer) is responsible for
///     the lifetime of the underlying storage.
///   - All mutating methods are noexcept and allocation-free; on overflow
///     they return `std::unexpected(Error::BufferFull)`.
///   - Header-only — there is intentionally no OutputBuffer.cpp.
class OutputBuffer {
public:
    /// @brief Construct an OutputBuffer over an existing byte region.
    /// @param base   start of the writable region (non-null if cap > 0)
    /// @param cap    total capacity in bytes
    constexpr OutputBuffer(uint8_t* base, std::size_t cap) noexcept
        : base_(base), cap_(cap), len_(0) {}

    /// @brief Append `len` bytes by copy.
    ///
    /// Prefer `writable_tail()` + `commit()` for zero-copy paths when the
    /// encoder can write directly into the buffer.
    /// @return BufferFull on overflow, Ok otherwise.
    [[nodiscard]] std::expected<void, ErrorInfo>
    append(const uint8_t* data, std::size_t len) noexcept {
        if (len_ + len > cap_) {
            return std::unexpected(ErrorInfo{Error::BufferFull,
                                             "OutputBuffer::append: capacity exceeded"});
        }
        if (len > 0) {
            std::memcpy(base_ + len_, data, len);
            len_ += len;
        }
        return {};
    }

    /// @brief Assert that at least `n` bytes remain available for writing.
    ///
    /// This is a no-op on success (the buffer is statically sized), and is
    /// provided so codecs can fail-fast at the top of an encode path rather
    /// than partway through. It does not grow the buffer.
    /// @return BufferFull when fewer than `n` bytes are available.
    [[nodiscard]] std::expected<void, ErrorInfo>
    reserve(std::size_t n) noexcept {
        if (len_ + n > cap_) {
            return std::unexpected(ErrorInfo{Error::BufferFull,
                                             "OutputBuffer::reserve: insufficient capacity"});
        }
        return {};
    }

    /// @brief Return a raw pointer to `n` bytes at the tail for zero-copy writes.
    ///
    /// After writing, the caller MUST call `commit(actually_written)`. If
    /// fewer than `n` bytes are available, returns nullptr so the caller can
    /// fall back to an error path.
    [[nodiscard]] uint8_t* writable_tail(std::size_t n) noexcept {
        if (len_ + n > cap_) {
            return nullptr;
        }
        return base_ + len_;
    }

    /// @brief Commit `n` bytes previously written via `writable_tail`.
    ///
    /// The caller is trusted to pass a value ≤ what `writable_tail` granted.
    /// Debug builds could assert, but we keep it branchless here to not
    /// penalise the hot path.
    constexpr void commit(std::size_t n) noexcept { len_ += n; }

    /// @brief Number of bytes still available for writing.
    [[nodiscard]] constexpr std::size_t available() const noexcept {
        return cap_ - len_;
    }

    /// @brief Number of bytes committed so far.
    [[nodiscard]] constexpr std::size_t size() const noexcept { return len_; }

    /// @brief Pointer to the start of the committed region.
    [[nodiscard]] constexpr const uint8_t* data() const noexcept { return base_; }

private:
    uint8_t*    base_;
    std::size_t cap_;
    std::size_t len_;
};

// ---------------------------------------------------------------------------
// StreamCodec concept (TCP-style, incremental decode)
// ---------------------------------------------------------------------------

/// @brief Stateful, TCP-style message codec.
///
/// A StreamCodec owns cross-call state (e.g. WebSocket continuation fragments,
/// partial lengths from a length-prefix framer). Each call to `decode()`:
///
///   - consumes as many bytes as possible from `view`,
///   - optionally writes an auto-response into `out` (see `OutputBuffer`),
///   - returns either:
///       * `Ok(Some(frame))` — one complete application frame was decoded;
///         remaining bytes (if any) stay in `view` for the next call,
///       * `Ok(None)`        — not enough data yet, call again after more RX
///         (also returned after a control-frame-only path like WS ping/pong),
///       * `Err(ErrorInfo)`  — protocol violation; caller should drop the
///         connection.
///
/// The `view` argument is typed as `T::PacketViewRef`, duck-typed to whatever
/// PacketView type the surrounding Stream backend provides. See file-level
/// comment for the full PacketView requirements.
template <class T>
concept StreamCodec = requires(T& t,
                               typename T::PacketViewRef view,
                               OutputBuffer& out,
                               uint8_t* enc_buf,
                               std::size_t enc_cap) {
    // Associated types
    typename T::Frame;
    typename T::PacketViewRef;

    // Stateful decode — T is taken by non-const reference on purpose.
    { t.decode(view, out) } -> std::same_as<
        std::expected<std::optional<typename T::Frame>, ErrorInfo>>;

    // Encode a single frame into a caller-provided byte region. Returns the
    // number of bytes written, or BufferFull / CodecBad on error.
    { t.encode(enc_buf, enc_cap, std::declval<typename T::Frame>()) }
        -> std::same_as<std::expected<std::size_t, ErrorInfo>>;

    // Static traits — max bytes encode() might prepend/append per frame, and
    // a constant discriminator so generic code can branch at compile time.
    { T::max_overhead } -> std::convertible_to<std::size_t>;
    { T::is_streaming } -> std::convertible_to<bool>;
};

// ---------------------------------------------------------------------------
// DatagramCodec concept (UDP-style, one packet = 0..N frames)
// ---------------------------------------------------------------------------

/// @brief Stateful, UDP-style datagram codec.
///
/// Unlike `StreamCodec`, a DatagramCodec must consume an entire datagram in
/// one call (there is no "partial" in UDP). It may emit zero, one, or many
/// application frames from a single datagram — MoldUDP64 is the canonical
/// many-frame case.
///
/// Frames are reported via a sink callback (`std::function<void(Frame)>`)
/// rather than a return value, because the fan-out is not known to the
/// caller in advance. The return value is the number of frames emitted
/// (useful for metrics), or an ErrorInfo on protocol error.
template <class T>
concept DatagramCodec = requires(T& t,
                                 typename T::PacketViewRef dgram,
                                 OutputBuffer& out,
                                 std::function<void(typename T::Frame)> sink,
                                 uint8_t* enc_buf,
                                 std::size_t enc_cap) {
    typename T::Frame;
    typename T::PacketViewRef;

    // Must consume the entire datagram. Reports emitted frames via `sink`.
    { t.decode(dgram, out, sink) } -> std::same_as<
        std::expected<std::size_t, ErrorInfo>>;

    { t.encode(enc_buf, enc_cap, std::declval<typename T::Frame>()) }
        -> std::same_as<std::expected<std::size_t, ErrorInfo>>;

    { T::max_overhead } -> std::convertible_to<std::size_t>;
    { T::is_streaming } -> std::convertible_to<bool>;  // expected to be false
};

// ---------------------------------------------------------------------------
// Codec — union of the two
// ---------------------------------------------------------------------------

/// @brief Matches any StreamCodec or DatagramCodec.
///
/// Use this when generic code doesn't care about the distinction (e.g. for
/// metrics sinks or for a function that only needs `T::Frame` / `encode()`).
template <class T>
concept Codec = StreamCodec<T> || DatagramCodec<T>;

} // namespace eph::core
