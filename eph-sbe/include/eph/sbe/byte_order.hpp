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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string_view>

#include "eph/sbe/errors.hpp"

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

// ---------------------------------------------------------------------------
// Signed reads — SBE encodes signed integers as two's-complement little-endian.
// Decode the unsigned bit pattern then bit_cast to the signed type (no UB).
// ---------------------------------------------------------------------------

/// @brief Read a little-endian int8_t (single byte, no endianness).
inline int8_t read_le_i8(const uint8_t* p) noexcept {
    return static_cast<int8_t>(*p);
}

/// @brief Read a little-endian int16_t (two's-complement).
inline int16_t read_le_i16(const uint8_t* p) noexcept {
    return std::bit_cast<int16_t>(read_le16(p));
}

/// @brief Read a little-endian int32_t (two's-complement).
inline int32_t read_le_i32(const uint8_t* p) noexcept {
    return std::bit_cast<int32_t>(read_le32(p));
}

/// @brief Read a little-endian int64_t (two's-complement).
inline int64_t read_le_i64(const uint8_t* p) noexcept {
    return std::bit_cast<int64_t>(read_le64(p));
}

// ---------------------------------------------------------------------------
// SBE composite readers
// ---------------------------------------------------------------------------

/// @brief Result of reading a `varString8` variable-length string.
struct VarString {
    std::string_view value;   ///< Zero-copy view of the UTF-8 bytes.
    std::size_t      advance; ///< Total bytes consumed (1-byte length + data).
};

/// @brief Read an SBE `varString8` (uint8 length prefix + UTF-8 data).
///
/// Bounds-checked against the caller's remaining buffer before any deref of the
/// data bytes — essential when parsing untrusted wire input (a forged length
/// prefix must not read past the buffer).
///
/// @param p         Pointer to the length prefix.
/// @param remaining Readable bytes available at @p p.
/// @return {string_view into the buffer, bytes consumed}, or
///         ParseError::kTruncated if the prefix or the declared data overruns.
[[nodiscard]] inline std::expected<VarString, ParseError>
read_var_string8(const uint8_t* p, std::size_t remaining) noexcept {
    if (remaining < 1) [[unlikely]]
        return std::unexpected(ParseError::kTruncated);
    const std::size_t len = p[0];
    if (remaining < 1 + len) [[unlikely]]
        return std::unexpected(ParseError::kTruncated);
    return VarString{
        .value   = std::string_view{reinterpret_cast<const char*>(p + 1), len},
        .advance = 1 + len,
    };
}

/// @brief Reconstitute an SBE decimal: value = mantissa × 10^exponent.
///
/// Binance encodes prices/quantities as a (mantissa64, exponent8) pair so the
/// scale travels with each field. Exponents are small (typically -8..0), so
/// std::pow on doubles is exact enough for display/strategy use; callers needing
/// integer-exact arithmetic should consume the raw mantissa/exponent directly.
///
/// @param mantissa The signed mantissa (mantissa64).
/// @param exponent The base-10 exponent (exponent8), usually ≤ 0.
/// @return The decoded floating-point value.
[[nodiscard]] inline double decode_decimal(int64_t mantissa, int8_t exponent) noexcept {
    return static_cast<double>(mantissa) * std::pow(10.0, static_cast<double>(exponent));
}

} // namespace eph::sbe
