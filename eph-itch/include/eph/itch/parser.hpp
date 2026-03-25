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

    /// Multi-line formatted dump for logging/debugging.
    [[nodiscard]] std::string dump() const {
        return std::format(
            "ITCH MessageView:\n"
            "  type: {} ('{:c}')\n"
            "  stock_locate: {}\n"
            "  tracking_number: {}\n"
            "  timestamp_ns: {}\n"
            "  length: {}",
            message_type_name(msg_type), static_cast<char>(msg_type),
            stock_locate(), tracking_number(), timestamp_ns(), length);
    }

    /// JSON-formatted message view for monitoring system integration.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"msg_type\":\"{}\",\"msg_type_char\":\"{:c}\","
            "\"stock_locate\":{},\"tracking_number\":{},"
            "\"timestamp_ns\":{},\"length\":{}}}",
            message_type_name(msg_type), static_cast<char>(msg_type),
            stock_locate(), tracking_number(), timestamp_ns(), length);
    }
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
[[nodiscard]] inline std::expected<MessageView, ParseError>
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
// Parser statistics
// ---------------------------------------------------------------------------

/// Lightweight counters for monitoring ITCH parse throughput and error rates.
///
/// Not thread-safe — use one instance per thread or protect with a mutex.
/// Designed for hot-path accumulation with a periodic snapshot to a monitoring
/// system (Prometheus, StatsD, etc.).
struct ParserStats {
    uint64_t messages_parsed = 0;   ///< Successfully parsed messages
    uint64_t parse_errors    = 0;   ///< Failed parse attempts (unknown type or truncated)
    uint64_t bytes_consumed  = 0;   ///< Total bytes consumed by successful parses
    size_t   first_error_offset = 0;       ///< Byte offset of the first parse error (0 if no errors)
    ParseError first_error_type = {};      ///< Type of the first parse error
    uint8_t    first_error_msg_byte = 0;   ///< Message type byte at the first error location

    /// Record a successful parse.
    void on_message(size_t msg_bytes) noexcept {
        ++messages_parsed;
        bytes_consumed += msg_bytes;
    }

    /// Record a parse error with offset context for debugging.
    void on_error(size_t offset, ParseError err, uint8_t msg_byte) noexcept {
        if (parse_errors == 0) {
            first_error_offset = offset;
            first_error_type = err;
            first_error_msg_byte = msg_byte;
        }
        ++parse_errors;
    }

    /// Reset all counters to zero.
    void reset() noexcept {
        messages_parsed = 0;
        parse_errors    = 0;
        bytes_consumed  = 0;
        first_error_offset = 0;
        first_error_type = {};
        first_error_msg_byte = 0;
    }

    /// Multi-line formatted dump for logging/debugging.
    [[nodiscard]] std::string dump() const {
        std::string s = std::format(
            "ITCH ParserStats:\n"
            "  messages_parsed: {}\n"
            "  parse_errors: {}\n"
            "  bytes_consumed: {}",
            messages_parsed, parse_errors, bytes_consumed);
        if (parse_errors > 0) {
            s += std::format(
                "\n  first_error: {} at offset {} (msg_byte=0x{:02x})",
                parse_error_name(first_error_type),
                first_error_offset, first_error_msg_byte);
        }
        return s;
    }

    /// JSON-formatted stats for monitoring system integration.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"messages_parsed\":{},\"parse_errors\":{},\"bytes_consumed\":{},"
            "\"first_error_offset\":{},\"first_error_type\":\"{}\","
            "\"first_error_msg_byte\":{}}}",
            messages_parsed, parse_errors, bytes_consumed,
            first_error_offset, parse_error_name(first_error_type),
            first_error_msg_byte);
    }

    /// Compute delta between two snapshots for interval-based monitoring.
    /// Counter fields are diffed; first_error_* fields are taken from the
    /// later (lhs) snapshot since they are point-in-time diagnostics.
    [[nodiscard]] friend ParserStats operator-(const ParserStats& lhs,
                                               const ParserStats& rhs) noexcept {
        return ParserStats{
            .messages_parsed     = lhs.messages_parsed - rhs.messages_parsed,
            .parse_errors        = lhs.parse_errors    - rhs.parse_errors,
            .bytes_consumed      = lhs.bytes_consumed  - rhs.bytes_consumed,
            .first_error_offset  = lhs.first_error_offset,
            .first_error_type    = lhs.first_error_type,
            .first_error_msg_byte = lhs.first_error_msg_byte,
        };
    }

    [[nodiscard]] friend bool operator==(const ParserStats&, const ParserStats&) = default;
};

/// Parse consecutive ITCH messages with statistics accumulation.
///
/// Behaves like parse_all() but also populates a ParserStats struct.
///
/// @param data     Pointer to a buffer of concatenated ITCH messages
/// @param len      Number of available bytes
/// @param callback Called with (const MessageView&) for each parsed message.
///                 Return true to continue, false to stop early.
/// @param stats    [out] Statistics accumulator
/// @return Number of bytes successfully consumed
template <typename Fn>
    requires std::invocable<Fn, const MessageView&>
size_t parse_all(const uint8_t* data, size_t len, Fn&& callback, ParserStats& stats) noexcept(
    noexcept(callback(std::declval<const MessageView&>()))) {
    size_t offset = 0;
    while (offset < len) {
        auto result = parse(data + offset, len - offset);
        if (!result) {
            // kIncomplete is normal (partial trailing data), not an error
            if (result.error() != ParseError::kIncomplete) {
                uint8_t err_byte = (offset < len) ? data[offset] : 0;
                stats.on_error(offset, result.error(), err_byte);
                // parse() already logs at WARN level; stats capture the details
            }
            break;
        }

        stats.on_message(result->length);

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
/// The handler is invoked as `handler(Tag{}, msg)` where:
///   - Tag is one of the msg:: structs above (compile-time message type)
///   - msg points to the full message (byte 0 = type tag), matching the pointer
///     convention used by all per-message accessor namespaces (e.g.
///     add_order::price(msg), cross_trade::shares(msg))
///
/// Usage with overload set:
///   struct MyHandler {
///       void operator()(itch::msg::AddOrder, const uint8_t* msg) {
///           auto ref = itch::add_order::order_ref(msg);  // works directly
///       }
///       void operator()(itch::msg::OrderDelete, const uint8_t* msg) { ... }
///       template <typename T>
///       void operator()(T, const uint8_t*) { /* default: ignore */ }
///   };
///   itch::dispatch(msg_view, MyHandler{});
///
/// Usage with if-constexpr lambda:
///   itch::dispatch(msg_view, [](auto tag, const uint8_t* msg) {
///       if constexpr (std::is_same_v<decltype(tag), itch::msg::AddOrder>) {
///           auto price = itch::add_order::price(msg);  // zero-overhead
///       }
///   });
///
/// For common header fields (stock_locate, tracking_number, timestamp_ns),
/// use the MessageView convenience methods or pass msg+1 to the free functions.
///
/// The handler return value (if any) is forwarded back to the caller.
/// If the message type is unknown, msg::Unknown is dispatched.
template <typename Handler>
decltype(auto) dispatch(const MessageView& view, Handler&& handler) {
    const uint8_t* msg = view.data; // full message pointer (byte 0 = type)

    switch (view.msg_type) {
    case kSystemEvent:              return handler(msg::SystemEvent{}, msg);
    case kStockDirectory:           return handler(msg::StockDirectory{}, msg);
    case kStockTradingAction:       return handler(msg::StockTradingAction{}, msg);
    case kRegSHORestriction:        return handler(msg::RegSHORestriction{}, msg);
    case kMarketParticipantPosition: return handler(msg::MarketParticipantPosition{}, msg);
    case kMWCBDeclineLevel:         return handler(msg::MWCBDeclineLevel{}, msg);
    case kMWCBStatus:               return handler(msg::MWCBStatus{}, msg);
    case kIPOQuotingPeriod:         return handler(msg::IPOQuotingPeriod{}, msg);
    case kLULDAuctionCollar:        return handler(msg::LULDAuctionCollar{}, msg);
    case kOperationalHalt:          return handler(msg::OperationalHalt{}, msg);
    case kAddOrder:                 return handler(msg::AddOrder{}, msg);
    case kAddOrderMPID:             return handler(msg::AddOrderMPID{}, msg);
    case kOrderExecuted:            return handler(msg::OrderExecuted{}, msg);
    case kOrderExecutedWithPrice:   return handler(msg::OrderExecutedWithPrice{}, msg);
    case kOrderCancel:              return handler(msg::OrderCancel{}, msg);
    case kOrderDelete:              return handler(msg::OrderDelete{}, msg);
    case kOrderReplace:             return handler(msg::OrderReplace{}, msg);
    case kNonCrossTrade:            return handler(msg::NonCrossTrade{}, msg);
    case kCrossTrade:               return handler(msg::CrossTrade{}, msg);
    case kBrokenTrade:              return handler(msg::BrokenTrade{}, msg);
    case kNOII:                     return handler(msg::NOII{}, msg);
    case kRPII:                     return handler(msg::RPII{}, msg);
    default:                        return handler(msg::Unknown{}, msg);
    }
}

// ---------------------------------------------------------------------------
// dispatch_all() — parse + dispatch in one pass
// ---------------------------------------------------------------------------

/// Parse consecutive ITCH messages and dispatch each to a handler via tag-type
/// overload resolution. Combines parse_all() + dispatch() into a single call.
///
/// The handler is invoked as `handler(Tag{}, body)` for each successfully parsed
/// message (same signature as dispatch()). If the handler returns bool, returning
/// false stops early.
///
/// @param data     Pointer to a buffer of concatenated ITCH messages
/// @param len      Number of available bytes
/// @param handler  Handler object/lambda with overloads for msg:: tag types
/// @return Number of bytes successfully consumed
template <typename Handler>
size_t dispatch_all(const uint8_t* data, size_t len, Handler&& handler) {
    return parse_all(data, len, [&](const MessageView& view) {
        return dispatch(view, handler);
    });
}

/// Parse consecutive ITCH messages with dispatch and statistics accumulation.
///
/// Combines parse_all()+dispatch() with ParserStats tracking.
///
/// @param data     Pointer to a buffer of concatenated ITCH messages
/// @param len      Number of available bytes
/// @param handler  Handler object/lambda with overloads for msg:: tag types
/// @param stats    [out] Statistics accumulator
/// @return Number of bytes successfully consumed
template <typename Handler>
size_t dispatch_all(const uint8_t* data, size_t len, Handler&& handler,
                    ParserStats& stats) {
    return parse_all(data, len, [&](const MessageView& view) {
        return dispatch(view, handler);
    }, stats);
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

/// std::formatter specialization for itch::ParserStats.
template <>
struct std::formatter<eph::itch::ParserStats> : std::formatter<std::string> {
    auto format(const eph::itch::ParserStats& s, auto& ctx) const {
        return std::formatter<std::string>::format(
            std::format("ITCH(parsed={} errors={} bytes={})",
                s.messages_parsed, s.parse_errors, s.bytes_consumed),
            ctx);
    }
};
