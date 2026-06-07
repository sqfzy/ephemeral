#pragma once

/// @file mbuf_view.hpp
/// `PacketView` implementation backed by the payload slice of a DPDK
/// `rte_mbuf`.
///
/// The view is a thin cursor over a contiguous byte range already extracted
/// by `packet_parse::parse_packet()` / `parse_udp_packet()`. It satisfies the
/// minimum PacketView surface the Codec concept relies on:
///
///     const uint8_t* data() const noexcept;
///     uint8_t*       writable_data() noexcept;
///     size_t         length() const noexcept;
///     void           trim_front(size_t n) noexcept;
///     void           trim_back(size_t n) noexcept;

#include <cstddef>
#include <cstdint>

#include "eph/core/packet_view.hpp"

namespace eph::net::dpdk::detail {

/// @brief Mutable span cursor over a DPDK payload region.
///
/// Movement of `data_` / `length_` is done via `trim_front` / `trim_back`;
/// the underlying memory is not copied or freed by this type. Ownership of
/// the backing mbuf lives at the enclosing `DpdkTcpStream::process_burst_`
/// scope, which frees the mbuf after the codec drain loop completes.
class MbufView {
public:
    constexpr MbufView() noexcept = default;

    /// @brief Construct from a payload pointer + length.
    /// @param payload     Pointer to the first payload byte (writable; TLS
    ///                    decrypt mutates in place).
    /// @param len         Payload length in bytes.
    constexpr MbufView(uint8_t* payload, std::size_t len) noexcept
        : data_(payload), length_(len) {}

    /// @name PacketView concept API
    /// @{

    [[nodiscard]] const uint8_t* data() const noexcept { return data_; }

    [[nodiscard]] uint8_t* writable_data() noexcept { return data_; }

    [[nodiscard]] std::size_t length() const noexcept { return length_; }

    /// @brief Advance the head cursor by `n` bytes (skb_pull equivalent).
    ///        If `n` exceeds `length_` the view becomes empty. Safe on a
    ///        default-constructed (null) view — avoids nullptr arithmetic UB.
    void trim_front(std::size_t n) noexcept {
        if (n >= length_) {
            if (data_) data_ += length_;
            length_ = 0;
        } else {
            if (data_) data_ += n;
            length_ -= n;
        }
    }

    /// @brief Shrink the tail by `n` bytes (skb_trim equivalent).
    void trim_back(std::size_t n) noexcept {
        if (n >= length_) {
            length_ = 0;
        } else {
            length_ -= n;
        }
    }

    /// @}

private:
    uint8_t*    data_   = nullptr;
    std::size_t length_ = 0;
};

// Formal concept verification.
static_assert(::eph::core::PacketView<MbufView>,
              "MbufView must satisfy the eph::core::PacketView concept");

} // namespace eph::net::dpdk::detail
