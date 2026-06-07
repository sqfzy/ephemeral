#pragma once

/// @file span_view.hpp
/// Kernel-backend PacketView: contiguous span over a caller-owned byte buffer.
///
/// This is the kernel
/// counterpart to the DPDK `MbufView`: both satisfy the PacketView duck-typing
/// required by `eph::core::StreamCodec` / `eph::core::DatagramCodec`.
///
/// Methods (all noexcept, header-only):
///   - `uint8_t*       writable_data() noexcept;`
///   - `const uint8_t* data() const noexcept;`
///   - `size_t         length() const noexcept;`
///   - `void           trim_front(size_t n) noexcept;`  // skb_pull equivalent
///   - `void           trim_back(size_t n) noexcept;`   // skb_trim equivalent
///
/// The view does NOT own its storage. Lifetime is managed by the Stream /
/// Datagram object that constructed it — typically the reassembly buffer of
/// the enclosing KernelTcpStream or the receive staging buffer of a
/// KernelUdpSocket.

#include <cstddef>
#include <cstdint>

#include "eph/core/packet_view.hpp"

namespace eph::net::kernel::detail {

/// @brief Contiguous mutable byte window; matches the PacketView contract.
///
/// By construction callers hand a `payload` pointer that refers to the
/// application-layer plaintext the codec should decode — the SpanView does
/// not distinguish ciphertext from plaintext because TLS decrypt has
/// already happened one layer up (see KernelTcpStream::drain_codec_ which
/// always wraps plaintext before passing it in).
struct SpanView {
    /// @brief Construct a view over `[payload, payload + len)`.
    /// @param payload      pointer to the first application-layer byte
    ///                     (may be nullptr iff len == 0)
    /// @param len          length of the window
    constexpr SpanView(uint8_t* payload, std::size_t len) noexcept
        : base_(payload), head_(0), tail_(len) {}

    [[nodiscard]] uint8_t*       writable_data() noexcept { return base_ + head_; }
    [[nodiscard]] const uint8_t* data() const noexcept    { return base_ + head_; }
    [[nodiscard]] std::size_t    length() const noexcept  { return tail_ - head_; }

    /// @brief Advance head (consume bytes). Clamps at `length()` so an
    ///        over-trim collapses the view to empty rather than driving
    ///        `head_` past `tail_` (which would underflow `length()` to a
    ///        wrap-around huge value and yield UB on the next access).
    ///
    /// Aligns with the DPDK sibling `MbufView::trim_front` so codec authors
    /// can rely on identical PacketView semantics across both backends —
    /// the previous "caller guarantees n <= length" footgun let a single
    /// codec slip the kernel path while staying correct on DPDK.
    constexpr void trim_front(std::size_t n) noexcept {
        head_ += (n > tail_ - head_) ? (tail_ - head_) : n;
    }
    /// @brief Pull tail back (truncate payload). Clamps at `length()` —
    ///        symmetric to `trim_front`. An over-trim collapses the view
    ///        to empty rather than driving `tail_` below `head_`.
    constexpr void trim_back(std::size_t n)  noexcept {
        tail_ -= (n > tail_ - head_) ? (tail_ - head_) : n;
    }

private:
    uint8_t*    base_;
    std::size_t head_;
    std::size_t tail_;
};

// Formal concept verification.
static_assert(::eph::core::PacketView<SpanView>,
              "SpanView must satisfy the eph::core::PacketView concept");

} // namespace eph::net::kernel::detail
