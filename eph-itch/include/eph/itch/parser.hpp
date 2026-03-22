#pragma once

/// @file parser.hpp
/// Zero-copy ITCH 5.0 message parser.
///
/// Parses raw ITCH message bytes (after framing-layer extraction) into a
/// lightweight MessageView that points back into the original receive buffer.
/// No allocations, no copies.

#include <cstdint>
#include <expected>
#include <format>
#include <string_view>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "eph/itch/messages.hpp"

namespace eph::itch {

namespace detail {
inline std::shared_ptr<spdlog::logger> itch_parser_logger() {
    static auto l = [] {
        auto lg = spdlog::get("itch.parser");
        if (!lg) lg = spdlog::stdout_color_mt("itch.parser");
        return lg;
    }();
    return l;
}
} // namespace detail

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

    if (expected == 0) {
        SPDLOG_LOGGER_WARN(detail::itch_parser_logger(),
            "ITCH parse: unknown message type=0x{:02x} ('{:c}'), len={}",
            type, static_cast<char>(type), len);
        return std::unexpected(ParseError::kUnknownType);
    }
    if (len < expected) {
        SPDLOG_LOGGER_WARN(detail::itch_parser_logger(),
            "ITCH parse: truncated {} message: have {} bytes, need {}",
            message_type_name(type), len, expected);
        return std::unexpected(ParseError::kTruncated);
    }

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

// ---------------------------------------------------------------------------
// Tag types for type-safe dispatch
// ---------------------------------------------------------------------------

/// Tag types — empty structs used for compile-time message type discrimination.
/// Pass these to handler overloads or use with `if constexpr` for zero-overhead
/// dispatch on ITCH message types.
namespace msg {
struct SystemEvent {};
struct StockDirectory {};
struct StockTradingAction {};
struct RegSHORestriction {};
struct MarketParticipantPosition {};
struct MWCBDeclineLevel {};
struct MWCBStatus {};
struct IPOQuotingPeriod {};
struct LULDAuctionCollar {};
struct OperationalHalt {};
struct AddOrder {};
struct AddOrderMPID {};
struct OrderExecuted {};
struct OrderExecutedWithPrice {};
struct OrderCancel {};
struct OrderDelete {};
struct OrderReplace {};
struct NonCrossTrade {};
struct CrossTrade {};
struct BrokenTrade {};
struct NOII {};
struct RPII {};
struct Unknown {};
} // namespace msg

// ---------------------------------------------------------------------------
// dispatch() — type-safe visitor for ITCH messages
// ---------------------------------------------------------------------------

/// Dispatch a parsed ITCH message to a handler using tag-type overload resolution.
///
/// The handler is invoked as `handler(Tag{}, body)` where:
///   - Tag is one of the msg:: structs above (compile-time message type)
///   - body points to the message body (past the 1-byte type field)
///
/// Usage with overload set:
///   struct MyHandler {
///       void operator()(itch::msg::AddOrder, const uint8_t* body) { ... }
///       void operator()(itch::msg::OrderDelete, const uint8_t* body) { ... }
///       template <typename T>
///       void operator()(T, const uint8_t*) { /* default: ignore */ }
///   };
///   itch::dispatch(msg_view, MyHandler{});
///
/// Usage with if-constexpr lambda:
///   itch::dispatch(msg_view, [](auto tag, const uint8_t* body) {
///       if constexpr (std::is_same_v<decltype(tag), itch::msg::AddOrder>) {
///           // handle AddOrder
///       }
///   });
///
/// The handler return value (if any) is forwarded back to the caller.
/// If the message type is unknown, msg::Unknown is dispatched.
template <typename Handler>
decltype(auto) dispatch(const MessageView& view, Handler&& handler) {
    const uint8_t* body = view.data + 1; // skip type byte

    switch (view.msg_type) {
    case kSystemEvent:              return handler(msg::SystemEvent{}, body);
    case kStockDirectory:           return handler(msg::StockDirectory{}, body);
    case kStockTradingAction:       return handler(msg::StockTradingAction{}, body);
    case kRegSHORestriction:        return handler(msg::RegSHORestriction{}, body);
    case kMarketParticipantPosition: return handler(msg::MarketParticipantPosition{}, body);
    case kMWCBDeclineLevel:         return handler(msg::MWCBDeclineLevel{}, body);
    case kMWCBStatus:               return handler(msg::MWCBStatus{}, body);
    case kIPOQuotingPeriod:         return handler(msg::IPOQuotingPeriod{}, body);
    case kLULDAuctionCollar:        return handler(msg::LULDAuctionCollar{}, body);
    case kOperationalHalt:          return handler(msg::OperationalHalt{}, body);
    case kAddOrder:                 return handler(msg::AddOrder{}, body);
    case kAddOrderMPID:             return handler(msg::AddOrderMPID{}, body);
    case kOrderExecuted:            return handler(msg::OrderExecuted{}, body);
    case kOrderExecutedWithPrice:   return handler(msg::OrderExecutedWithPrice{}, body);
    case kOrderCancel:              return handler(msg::OrderCancel{}, body);
    case kOrderDelete:              return handler(msg::OrderDelete{}, body);
    case kOrderReplace:             return handler(msg::OrderReplace{}, body);
    case kNonCrossTrade:            return handler(msg::NonCrossTrade{}, body);
    case kCrossTrade:               return handler(msg::CrossTrade{}, body);
    case kBrokenTrade:              return handler(msg::BrokenTrade{}, body);
    case kNOII:                     return handler(msg::NOII{}, body);
    case kRPII:                     return handler(msg::RPII{}, body);
    default:                        return handler(msg::Unknown{}, body);
    }
}

} // namespace eph::itch

/// std::formatter specialization for itch::ParseError.
template <>
struct std::formatter<eph::itch::ParseError> : std::formatter<std::string_view> {
    auto format(eph::itch::ParseError e, auto& ctx) const {
        return std::formatter<std::string_view>::format(
            eph::itch::parse_error_name(e), ctx);
    }
};

/// std::formatter specialization for itch::MessageView.
///
/// Formats as "ITCH[TypeName locate=N ts=Nns len=N]".
/// Example: "ITCH[AddOrder locate=42 ts=123456789ns len=35]"
template <>
struct std::formatter<eph::itch::MessageView> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    auto format(const eph::itch::MessageView& msg, auto& ctx) const {
        return std::format_to(ctx.out(), "ITCH[{} locate={} ts={}ns len={}]",
                              eph::itch::message_type_name(msg.msg_type),
                              msg.stock_locate(),
                              msg.timestamp_ns(),
                              msg.length);
    }
};
