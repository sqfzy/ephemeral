#pragma once

/// @file span_view.hpp
/// Kernel-backend PacketView: contiguous span over a caller-owned byte buffer.
///
/// Part of Phase 3 of the v3.3 refactor (see
/// .artifacts/design-eph-v3.3-architecture-20260410.md). This is the kernel
/// counterpart to the DPDK `MbufView`: both satisfy the PacketView duck-typing
/// required by `eph::core::StreamCodec` / `eph::core::DatagramCodec`.
///
/// Methods (all noexcept, header-only):
///   - `uint8_t*       writable_data() noexcept;`
///   - `const uint8_t* data() const noexcept;`
///   - `size_t         length() const noexcept;`
///   - `void           trim_front(size_t n) noexcept;`  // skb_pull equivalent
///   - `void           trim_back(size_t n) noexcept;`   // skb_trim equivalent
///   - `uint64_t       arrival_tsc() const noexcept;`
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
struct SpanView {
    /// @brief Construct a view over `[base, base + len)`.
    /// @param base         pointer to the first byte (may be nullptr iff len == 0)
    /// @param len          length of the window
    /// @param arrival_tsc  optional arrival timestamp (TSC ticks); 0 if unknown
    constexpr SpanView(uint8_t* base, std::size_t len,
                       uint64_t arrival_tsc = 0) noexcept
        : base_(base), head_(0), tail_(len), tsc_(arrival_tsc) {}

    [[nodiscard]] uint8_t*       writable_data() noexcept { return base_ + head_; }
    [[nodiscard]] const uint8_t* data() const noexcept    { return base_ + head_; }
    [[nodiscard]] std::size_t    length() const noexcept  { return tail_ - head_; }

    /// @brief Advance head (consume bytes). Caller guarantees n <= length().
    constexpr void trim_front(std::size_t n) noexcept { head_ += n; }
    /// @brief Pull tail back (truncate payload). Caller guarantees n <= length().
    constexpr void trim_back(std::size_t n)  noexcept { tail_ -= n; }

    [[nodiscard]] uint64_t arrival_tsc() const noexcept { return tsc_; }

private:
    uint8_t*    base_;
    std::size_t head_;
    std::size_t tail_;
    uint64_t    tsc_;
};

// Phase 5: formal concept verification.
static_assert(::eph::core::PacketView<SpanView>,
              "SpanView must satisfy the eph::core::PacketView concept");

} // namespace eph::net::kernel::detail
