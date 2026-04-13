#pragma once

/// @file span_packet_view.hpp
/// Minimal span-backed PacketView used as the canonical `PacketViewRef` type
/// for the codec implementations in `eph-codec`.
///
/// Background: the `StreamCodec` / `DatagramCodec` concepts in `eph-core`
/// require each codec to name a `PacketViewRef` associated type. Each backend
/// provides its own PacketView implementation (kernel:
/// `detail::SpanView`, DPDK: `detail::MbufView`). `SpanPacketView` provides
/// a single concrete type so the codec headers can satisfy the concept and
/// be unit-testable independently of any backend.
///
/// The codec `decode` methods are still templated over `class PacketView`, so
/// later backends can pass their own view types without modification; the
/// associated `PacketViewRef` type alias exists purely to satisfy the
/// compile-time `requires` clause.
///
/// Requirements satisfied (from codec.hpp file-level comment):
///   - uint8_t*       writable_data() noexcept;
///   - const uint8_t* data() const noexcept;
///   - size_t         length() const noexcept;
///   - void           trim_front(size_t n) noexcept;   // skb_pull equivalent
///   - void           trim_back(size_t n)  noexcept;   // skb_trim equivalent
///   - uint64_t       arrival_tsc() const noexcept;

#include <cstddef>
#include <cstdint>

#include "eph/core/packet_view.hpp"

namespace eph::codec {

/// @brief Contiguous byte window into an externally-owned buffer.
///
/// Holds a moving `[head, tail)` slice over a caller-provided base pointer.
/// `trim_front` advances `head` (skb_pull), `trim_back` pulls `tail` in
/// (skb_trim). The `SpanPacketView` does NOT own its storage.
///
/// This is header-only, trivially copyable, and all operations are noexcept.
struct SpanPacketView {
    /// @brief Construct a view over `[base, base + len)` with an optional TSC tag.
    /// @param base         pointer to the first byte (may be nullptr iff len == 0)
    /// @param len          initial length of the window in bytes
    /// @param arrival_tsc  optional TSC timestamp for latency tracking (default 0)
    constexpr SpanPacketView(uint8_t* base, std::size_t len, uint64_t arrival_tsc = 0) noexcept
        : base_(base), head_(0), tail_(len), tsc_(arrival_tsc) {}

    /// @brief Writable pointer to the current head of the window.
    ///
    /// Used by in-place mutating codecs (e.g. a TLS decryptor that decrypts
    /// in place). Stream codecs that only need read access should prefer
    /// `data()`.
    [[nodiscard]] uint8_t* writable_data() noexcept { return base_ + head_; }

    /// @brief Read-only pointer to the current head of the window.
    [[nodiscard]] const uint8_t* data() const noexcept { return base_ + head_; }

    /// @brief Number of bytes in the current window.
    [[nodiscard]] std::size_t length() const noexcept { return tail_ - head_; }

    /// @brief Advance the head by `n` bytes (skb_pull equivalent).
    ///
    /// Callers must ensure `n <= length()`. Out-of-range calls would corrupt
    /// the window; we deliberately do not bounds-check on the hot path.
    void trim_front(std::size_t n) noexcept { head_ += n; }

    /// @brief Pull the tail back by `n` bytes (skb_trim equivalent).
    void trim_back(std::size_t n) noexcept { tail_ -= n; }

    /// @brief TSC timestamp captured at NIC arrival (0 if untracked).
    [[nodiscard]] uint64_t arrival_tsc() const noexcept { return tsc_; }

private:
    uint8_t*    base_;  ///< caller-owned backing storage
    std::size_t head_;  ///< current read head (offset from base_)
    std::size_t tail_;  ///< one past the last valid byte (offset from base_)
    uint64_t    tsc_;   ///< NIC arrival timestamp for latency tracking
};

// Formal concept verification.
static_assert(::eph::core::PacketView<SpanPacketView>,
              "SpanPacketView must satisfy the eph::core::PacketView concept");

} // namespace eph::codec
