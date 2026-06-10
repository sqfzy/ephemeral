#pragma once

/// @file message_header.hpp
/// SBE message header — the 8-byte, schema-independent framing that prefixes
/// every Simple Binary Encoding message.
///
/// Wire layout (all little-endian, per the SBE standard):
///   off 0  blockLength uint16  — size of the message root block (fixed fields)
///   off 2  templateId  uint16  — message type id within the schema
///   off 4  schemaId     uint16  — schema id (e.g. Binance spot = 3)
///   off 6  version      uint16  — schema version (e.g. 2)
///
/// This composite is `messageHeader` in every SBE XML schema and never varies
/// by message type, so parsing it requires no schema knowledge.

#include <cstddef>
#include <cstdint>
#include <expected>

#include "eph/sbe/byte_order.hpp"
#include "eph/sbe/errors.hpp"

namespace eph::sbe {

/// @brief Decoded SBE message header (the 8-byte messageHeader composite).
struct SbeHeader {
    uint16_t block_length; ///< Size of the message root block in bytes.
    uint16_t template_id;  ///< Message type id within the schema.
    uint16_t schema_id;    ///< Schema id the message belongs to.
    uint16_t version;      ///< Schema version.
};

/// @brief Number of bytes in the SBE messageHeader composite.
inline constexpr std::size_t kHeaderSize = 8;

/// @brief Decode the 8-byte SBE message header from raw bytes.
///
/// Schema-independent: only validates that at least `kHeaderSize` bytes are
/// available and decodes the four little-endian uint16 fields.
///
/// @param data Pointer to the start of an SBE message (the messageHeader).
/// @param len  Number of readable bytes at @p data.
/// @return The decoded SbeHeader, or ParseError::kIncomplete if `len < 8`.
[[nodiscard]] inline std::expected<SbeHeader, ParseError>
parse_header(const uint8_t* data, std::size_t len) noexcept {
    if (len < kHeaderSize) [[unlikely]]
        return std::unexpected(ParseError::kIncomplete);
    return SbeHeader{
        .block_length = read_le16(data),
        .template_id  = read_le16(data + 2),
        .schema_id    = read_le16(data + 4),
        .version      = read_le16(data + 6),
    };
}

} // namespace eph::sbe
