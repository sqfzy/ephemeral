#pragma once

/// @file byte_order.hpp
/// Little-endian byte-reading primitives for Simple Binary Encoding (SBE).
///
/// SBE wire data is little-endian by definition (the Binance and FIX SBE
/// schemas declare `byteOrder="littleEndian"`). All accessors operate directly
/// on raw message bytes — no deserialization, no copies. Multi-byte integers
/// are decoded with std::memcpy + a compile-time-gated std::byteswap so reads
/// are both unaligned-safe (SBE lays fields at unaligned offsets, where a
/// reinterpret_cast would be UB) and portable to big-endian hosts.
///
/// This mirrors eph-itch's `read_be*` helpers (itch/messages.hpp) but for the
/// opposite wire endianness.

#include <bit>
#include <cstdint>
#include <cstring>

namespace eph::sbe {

/// @brief Read a little-endian uint16_t from an arbitrary byte pointer.
/// @param p Pointer to at least 2 readable bytes of little-endian data.
/// @return Host-endian uint16_t value.
inline uint16_t read_le16(const uint8_t* p) noexcept {
    uint16_t v;
    std::memcpy(&v, p, 2);
    if constexpr (std::endian::native == std::endian::big)
        return std::byteswap(v);
    else
        return v;
}

/// @brief Read a little-endian uint32_t from an arbitrary byte pointer.
/// @param p Pointer to at least 4 readable bytes of little-endian data.
/// @return Host-endian uint32_t value.
inline uint32_t read_le32(const uint8_t* p) noexcept {
    uint32_t v;
    std::memcpy(&v, p, 4);
    if constexpr (std::endian::native == std::endian::big)
        return std::byteswap(v);
    else
        return v;
}

/// @brief Read a little-endian uint64_t from an arbitrary byte pointer.
/// @param p Pointer to at least 8 readable bytes of little-endian data.
/// @return Host-endian uint64_t value.
inline uint64_t read_le64(const uint8_t* p) noexcept {
    uint64_t v;
    std::memcpy(&v, p, 8);
    if constexpr (std::endian::native == std::endian::big)
        return std::byteswap(v);
    else
        return v;
}

} // namespace eph::sbe
