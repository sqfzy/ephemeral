#pragma once

/// @file parser.hpp
/// Zero-copy ITCH 5.0 message parser.
///
/// Parses raw ITCH message bytes (after framing-layer extraction) into a
/// lightweight MessageView that points back into the original receive buffer.
/// No allocations, no copies.

#include <cstdint>
#include <expected>
#include <string_view>

#include "eph/itch/messages.hpp"

namespace eph::itch {

// ---------------------------------------------------------------------------
// Parse errors
// ---------------------------------------------------------------------------

enum class ParseError : uint8_t {
    kIncomplete,   ///< Not enough data to determine message type
    kUnknownType,  ///< Unrecognised message type byte
    kTruncated,    ///< Message shorter than the fixed size for its type
};

/// Human-readable name for a ParseError value.
constexpr std::string_view parse_error_name(ParseError e) noexcept {
    switch (e) {
    case ParseError::kIncomplete:  return "incomplete";
    case ParseError::kUnknownType: return "unknown message type";
    case ParseError::kTruncated:   return "truncated message";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Lookup helpers (defined before parse() so it can call them)
// ---------------------------------------------------------------------------

/// Get the fixed size for a known ITCH 5.0 message type, or 0 for unknown.
inline constexpr size_t message_size(uint8_t type) noexcept {
    switch (type) {
    case kSystemEvent:              return kSystemEventSize;
    case kStockDirectory:           return kStockDirectorySize;
    case kStockTradingAction:       return kStockTradingActionSize;
    case kRegSHORestriction:        return kRegSHORestrictionSize;
    case kMarketParticipantPosition: return kMarketParticipantPositionSize;
    case kMWCBDeclineLevel:         return kMWCBDeclineLevelSize;
    case kMWCBStatus:               return kMWCBStatusSize;
    case kIPOQuotingPeriod:         return kIPOQuotingPeriodSize;
    case kLULDAuctionCollar:        return kLULDAuctionCollarSize;
    case kOperationalHalt:          return kOperationalHaltSize;
    case kAddOrder:                 return kAddOrderSize;
    case kAddOrderMPID:             return kAddOrderMPIDSize;
    case kOrderExecuted:            return kOrderExecutedSize;
    case kOrderExecutedWithPrice:   return kOrderExecutedWithPriceSize;
    case kOrderCancel:              return kOrderCancelSize;
    case kOrderDelete:              return kOrderDeleteSize;
    case kOrderReplace:             return kOrderReplaceSize;
    case kNonCrossTrade:            return kNonCrossTradeSize;
    case kCrossTrade:               return kCrossTradeSize;
    case kBrokenTrade:              return kBrokenTradeSize;
    case kNOII:                     return kNOIISize;
    case kRPII:                     return kRPIISize;
    default:                        return 0;
    }
}

/// Get a human-readable name for an ITCH message type byte.
inline constexpr std::string_view message_type_name(uint8_t type) noexcept {
    switch (type) {
    case kSystemEvent:              return "SystemEvent";
    case kStockDirectory:           return "StockDirectory";
    case kStockTradingAction:       return "StockTradingAction";
    case kRegSHORestriction:        return "RegSHORestriction";
    case kMarketParticipantPosition: return "MarketParticipantPosition";
    case kMWCBDeclineLevel:         return "MWCBDeclineLevel";
    case kMWCBStatus:               return "MWCBStatus";
    case kIPOQuotingPeriod:         return "IPOQuotingPeriod";
    case kLULDAuctionCollar:        return "LULDAuctionCollar";
    case kOperationalHalt:          return "OperationalHalt";
    case kAddOrder:                 return "AddOrder";
    case kAddOrderMPID:             return "AddOrderMPID";
    case kOrderExecuted:            return "OrderExecuted";
    case kOrderExecutedWithPrice:   return "OrderExecutedWithPrice";
    case kOrderCancel:              return "OrderCancel";
    case kOrderDelete:              return "OrderDelete";
    case kOrderReplace:             return "OrderReplace";
    case kNonCrossTrade:            return "NonCrossTrade";
    case kCrossTrade:               return "CrossTrade";
    case kBrokenTrade:              return "BrokenTrade";
    case kNOII:                     return "NOII";
    case kRPII:                     return "RPII";
    default:                        return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// MessageView
// ---------------------------------------------------------------------------

/// Zero-copy message view — points into the original receive buffer.
struct MessageView {
    uint8_t        msg_type;  ///< Message type character (e.g. 'A' for AddOrder)
    const uint8_t* data;      ///< Pointer to message start (byte 0 = type tag)
    uint16_t       length;    ///< Total message length in bytes

    // Convenience accessors for the common header fields.
    // Body starts at data+1 (skip the type byte).
    uint16_t stock_locate()    const noexcept { return eph::itch::stock_locate(data + 1); }
    uint16_t tracking_number() const noexcept { return eph::itch::tracking_number(data + 1); }
    uint64_t timestamp_ns()    const noexcept { return eph::itch::timestamp_ns(data + 1); }
};

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

/// Parse a single ITCH message from a raw byte buffer.
///
/// The buffer must contain the message body starting with the 1-byte type tag.
/// Any framing (e.g. MoldUDP64 2-byte length prefix) must already be stripped.
///
/// @param data  Pointer to message data (first byte is the message type)
/// @param len   Number of available bytes starting at @p data
/// @return MessageView on success, ParseError on failure
inline std::expected<MessageView, ParseError>
parse(const uint8_t* data, size_t len) noexcept {
    if (len == 0) return std::unexpected(ParseError::kIncomplete);

    const uint8_t type = data[0];
    const size_t  expected = message_size(type);

    if (expected == 0) return std::unexpected(ParseError::kUnknownType);
    if (len < expected) return std::unexpected(ParseError::kTruncated);

    return MessageView{
        .msg_type = type,
        .data     = data,
        .length   = static_cast<uint16_t>(expected),
    };
}

/// Parse consecutive ITCH messages from a buffer, invoking a callback for each.
///
/// Processes messages sequentially from the start of the buffer.
/// Stops on the first parse error or when the buffer is exhausted.
/// The callback receives a MessageView for each successfully parsed message.
/// Return false from the callback to stop early.
///
/// @param data     Pointer to a buffer of concatenated ITCH messages
/// @param len      Number of available bytes
/// @param callback Called with (MessageView) for each parsed message.
///                 Return true to continue, false to stop early.
/// @return Number of bytes successfully consumed (sum of parsed message lengths)
template <typename Fn>
    requires std::invocable<Fn, const MessageView&>
size_t parse_all(const uint8_t* data, size_t len, Fn&& callback) noexcept(
    noexcept(callback(std::declval<const MessageView&>()))) {
    size_t offset = 0;
    while (offset < len) {
        auto result = parse(data + offset, len - offset);
        if (!result) break;

        if constexpr (std::is_same_v<std::invoke_result_t<Fn, const MessageView&>, bool>) {
            if (!callback(*result)) {
                offset += result->length;
                break;
            }
        } else {
            callback(*result);
        }
        offset += result->length;
    }
    return offset;
}

} // namespace eph::itch
