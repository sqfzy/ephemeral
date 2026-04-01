#pragma once

/// @file framer_concept.hpp
/// MessageFramer concept — pluggable message framing for Transport.
///
/// Framers handle the wire format layer: encoding application payloads
/// into framed bytes and decoding framed bytes back into payloads.
/// Built-in implementations: WsFramer (WebSocket), RawFramer (pass-through),
/// LengthPrefixFramer (2-byte big-endian length prefix).

#include <cstdint>
#include <cstring>
#include <expected>
#include <string_view>

#include "eph/core/error_traits.hpp"

namespace eph::net {

/// Error codes returned by MessageFramer::decode().
enum class FrameError : uint8_t {
    kIncomplete,       ///< Need more data (partial frame in buffer)
    kInvalidFormat,    ///< Malformed frame (corrupt header, bad length)
    kPayloadTooLarge,  ///< Payload exceeds maximum allowed size
};

/// Human-readable name for FrameError.
constexpr std::string_view frame_error_name(FrameError e) noexcept {
    switch (e) {
    case FrameError::kIncomplete:      return "incomplete";
    case FrameError::kInvalidFormat:   return "invalid format";
    case FrameError::kPayloadTooLarge: return "payload too large";
    }
    return "unknown";
}

// ADL alias for ErrorEnum concept satisfaction
constexpr std::string_view error_name(FrameError e) noexcept {
    return frame_error_name(e);
}

/// Decoded frame — zero-copy view into the receive buffer.
///
/// @note Lifetime: `payload` points into the original receive buffer (zero-copy view).
///       The pointer is valid only until the next call to `decode()` or until the
///       underlying buffer is reused/freed. Callers must copy the data if they need
///       it to outlive the current decode cycle.
struct DecodedFrame {
    const uint8_t* payload;      ///< Pointer to payload data (within input buffer)
    size_t         payload_len;  ///< Payload length in bytes
    /// Message type identifier — semantics vary by protocol:
    ///   - WebSocket: opcode (1=text, 2=binary, 8=close, 9=ping, 10=pong)
    ///   - ITCH: message type character (e.g., 'A'=AddOrder, 'S'=SystemEvent)
    ///   - FIX: MsgType tag 35 value mapped to uint16_t
    ///   - JSON/Raw: 0 (unused)
    uint8_t        msg_type;
    bool           is_control;   ///< Control frame (WS ping/pong/close). Data framers always false.
    size_t         total_len;    ///< Total consumed bytes including frame header
};

/// Concept for pluggable message framers.
///
/// A MessageFramer translates between application payloads and wire-format
/// framed bytes. It must provide:
///   - encode(): payload → framed bytes (for TX)
///   - decode(): framed bytes → DecodedFrame (for RX, instance method)
///   - max_overhead(): maximum bytes added by framing (for buffer pre-allocation)
///
/// decode() is an instance method (not static) to support stateful framers
/// that maintain per-connection decoding context (e.g., SBE schema cache,
/// protocol version negotiation state). Stateless framers (WsFramer, RawFramer,
/// LengthPrefixFramer) incur zero overhead — the compiler eliminates the
/// unused `this` pointer for types with no member state.
template <typename F>
concept MessageFramer = requires(F f, uint8_t* out, const uint8_t* in,
                                  size_t len, uint8_t msg_type) {
    /// Encode a payload into framed wire format.
    /// @pre `buf` must have sufficient capacity for the framed message.
    ///      Required size depends on the framer: length-prefix adds 2 bytes,
    ///      FIX adds checksum/body-length tags, WebSocket adds 2-14 byte header.
    ///      Callers should use `max_frame_overhead()` if available, or allocate
    ///      conservatively (payload_size + 64 bytes covers all built-in framers).
    /// @param out       Output buffer (must have at least len + max_overhead() bytes)
    /// @param in        Input payload data
    /// @param len       Input payload length
    /// @param msg_type  Protocol-specific message type hint
    /// @return Total bytes written to out (header + payload)
    { f.encode(out, in, len, msg_type) } noexcept -> std::same_as<size_t>;

    /// Decode a framed message from the receive buffer.
    /// @param in   Input buffer (may contain partial or multiple frames)
    /// @param len  Available bytes in input buffer
    /// @return Decoded frame on success, FrameError on failure
    { f.decode(in, len) } noexcept -> std::same_as<std::expected<DecodedFrame, FrameError>>;

    /// Maximum framing overhead in bytes (used for buffer pre-allocation).
    { F::max_overhead() } noexcept -> std::convertible_to<size_t>;
};

} // namespace eph::net

/// std::formatter specialization for FrameError (via ErrorEnumFormatter).
template <>
struct std::formatter<eph::net::FrameError>
    : eph::net::ErrorEnumFormatter<eph::net::FrameError> {};
