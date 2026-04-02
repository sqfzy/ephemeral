#pragma once

/// @file messages.hpp
/// ITCH 5.0 (NASDAQ TotalView-ITCH) message type definitions and zero-copy
/// field accessors.
///
/// All accessors operate on raw message bytes — no deserialization, no copies.
/// Multi-byte integers are big-endian on the wire; accessors use std::memcpy +
/// std::byteswap for safe, portable decoding.
///
/// Message layout reference:
///   https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification.pdf

#include <bit>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace eph::itch {

// ---------------------------------------------------------------------------
// Endian helpers
// ---------------------------------------------------------------------------

/// @brief Read a big-endian uint16_t from an arbitrary byte pointer.
/// @param p Pointer to at least 2 bytes of big-endian data.
/// @return Host-endian uint16_t value.
inline uint16_t read_be16(const uint8_t* p) noexcept {
    uint16_t v;
    std::memcpy(&v, p, 2);
    if constexpr (std::endian::native == std::endian::little)
        return std::byteswap(v);
    else
        return v;
}

/// @brief Read a big-endian uint32_t from an arbitrary byte pointer.
/// @param p Pointer to at least 4 bytes of big-endian data.
/// @return Host-endian uint32_t value.
inline uint32_t read_be32(const uint8_t* p) noexcept {
    uint32_t v;
    std::memcpy(&v, p, 4);
    if constexpr (std::endian::native == std::endian::little)
        return std::byteswap(v);
    else
        return v;
}

/// @brief Read a big-endian uint64_t from an arbitrary byte pointer.
/// @param p Pointer to at least 8 bytes of big-endian data.
/// @return Host-endian uint64_t value.
inline uint64_t read_be64(const uint8_t* p) noexcept {
    uint64_t v;
    std::memcpy(&v, p, 8);
    if constexpr (std::endian::native == std::endian::little)
        return std::byteswap(v);
    else
        return v;
}

/// @brief Read a 6-byte big-endian timestamp (nanoseconds since midnight) into a uint64_t.
///
/// The value occupies the low 48 bits.
/// Uses explicit shifts for portability -- avoids assumptions about internal
/// representation of uint64_t when overlapping with byteswap on partial buffers.
///
/// @param p Pointer to at least 6 bytes of big-endian timestamp data.
/// @return Host-endian uint64_t with the 48-bit timestamp in the lower bits.
inline uint64_t read_be48(const uint8_t* p) noexcept {
    return (static_cast<uint64_t>(p[0]) << 40) | (static_cast<uint64_t>(p[1]) << 32) |
           (static_cast<uint64_t>(p[2]) << 24) | (static_cast<uint64_t>(p[3]) << 16) |
           (static_cast<uint64_t>(p[4]) <<  8) |  static_cast<uint64_t>(p[5]);
}

// ---------------------------------------------------------------------------
// String trimming utility
// ---------------------------------------------------------------------------

/// @brief Trim trailing spaces from a right-padded ITCH string field.
///
/// ITCH encodes stock symbols and other text fields as fixed-width,
/// right-padded with ASCII spaces (0x20). This returns a view with
/// trailing spaces removed.
///
/// @param s The string view to trim (typically an 8-byte stock symbol or 4-byte MPID).
/// @return A string_view with trailing spaces removed, or empty if the input is all spaces.
constexpr std::string_view trim(std::string_view s) noexcept {
    auto end = s.find_last_not_of(' ');
    return (end == std::string_view::npos) ? std::string_view{} : s.substr(0, end + 1);
}

// ---------------------------------------------------------------------------
// Price scaling constants
// ---------------------------------------------------------------------------

/// ITCH 5.0 prices are fixed-point with 4 implied decimal places (10^4).
/// Divide raw uint32_t price fields by this to get dollars.
inline constexpr double kItchPriceDivisor = 10'000.0;

/// LULD auction collar messages use 8 implied decimal places (10^8).
inline constexpr double kLuldPriceDivisor = 100'000'000.0;

// ---------------------------------------------------------------------------
// Common header accessors  (all ITCH messages share this layout)
//
//   Offset 0:  message_type  (1 byte)  — stripped before these are called
//   Offset 0:  stock_locate  (2 bytes, big-endian)
//   Offset 2:  tracking_number (2 bytes, big-endian)
//   Offset 4:  timestamp     (6 bytes, big-endian nanoseconds since midnight)
//
// NOTE: the `msg` pointer passed to these functions points to the *body*
//       (i.e. after the 1-byte message-type tag).  This matches the layout
//       inside MessageView, where `data` points to the full message and
//       callers add +1 to skip the type byte.
// ---------------------------------------------------------------------------

/// @brief Stock locate code (2 bytes at offset 0 of body).
/// @param body Pointer to the message body (after the 1-byte type tag).
/// @return The stock locate code identifying which stock this message refers to.
inline uint16_t stock_locate(const uint8_t* body) noexcept {
    return read_be16(body);
}

/// @brief Tracking number (2 bytes at offset 2 of body).
/// @param body Pointer to the message body (after the 1-byte type tag).
/// @return The tracking number assigned by the Nasdaq system.
inline uint16_t tracking_number(const uint8_t* body) noexcept {
    return read_be16(body + 2);
}

/// @brief Nanosecond timestamp since midnight (6 bytes at offset 4 of body).
/// @param body Pointer to the message body (after the 1-byte type tag).
/// @return Nanoseconds elapsed since midnight ET for this trading day.
inline uint64_t timestamp_ns(const uint8_t* body) noexcept {
    return read_be48(body + 4);
}

// ---------------------------------------------------------------------------
// Message type constants and sizes
// ---------------------------------------------------------------------------
// Sizes are the complete on-wire message length INCLUDING the 1-byte type
// field and EXCLUDING any framing layer (e.g. the 2-byte MoldUDP64 length
// prefix).  Verified against NASDAQ TotalView-ITCH 5.0 specification:
//   common header = type(1) + stock_locate(2) + tracking_number(2) + timestamp(6) = 11 bytes
//   each type's size = 11 + message-specific fields.

// System Event ('S')
// Fields: type(1) locate(2) tracking(2) timestamp(6) event_code(1)
inline constexpr uint8_t kSystemEvent     = 'S';
inline constexpr size_t  kSystemEventSize = 12;

// Stock Directory ('R')
// Fields: ...common(11) stock(8) mktcat(1) finstat(1) roundlotsize(4)
//         roundlotonly(1) issuecls(1) issuesubtype(2) auth(1) shortsale(1)
//         ipoflag(1) luld(1) etpflag(1) etplev(4) inverse(1)
inline constexpr uint8_t kStockDirectory     = 'R';
inline constexpr size_t  kStockDirectorySize = 39;

// Stock Trading Action ('H')
// Fields: ...common(11) stock(8) trading_state(1) reserved(1) reason(4)
inline constexpr uint8_t kStockTradingAction     = 'H';
inline constexpr size_t  kStockTradingActionSize = 25;

// Reg SHO Restriction ('Y')
// Fields: ...common(11) stock(8) reg_sho_action(1)
inline constexpr uint8_t kRegSHORestriction     = 'Y';
inline constexpr size_t  kRegSHORestrictionSize = 20;

// Market Participant Position ('L')
// Fields: ...common(11) mpid(4) stock(8) primary_mm(1) mm_mode(1) state(1)
inline constexpr uint8_t kMarketParticipantPosition     = 'L';
inline constexpr size_t  kMarketParticipantPositionSize = 26;

// MWCB Decline Level ('V')
// Fields: ...common(11) level1(8) level2(8) level3(8)
inline constexpr uint8_t kMWCBDeclineLevel     = 'V';
inline constexpr size_t  kMWCBDeclineLevelSize = 35;

// MWCB Status ('W')
// Fields: ...common(11) breached_level(1)
inline constexpr uint8_t kMWCBStatus     = 'W';
inline constexpr size_t  kMWCBStatusSize = 12;

// IPO Quoting Period Update ('K')
// Fields: ...common(11) stock(8) release_time(4) release_qualifier(1) ipo_price(4)
inline constexpr uint8_t kIPOQuotingPeriod     = 'K';
inline constexpr size_t  kIPOQuotingPeriodSize = 28;

// LULD Auction Collar ('J')
// Fields: ...common(11) stock(8) ref_price(4) upper(4) lower(4) extension(4)
inline constexpr uint8_t kLULDAuctionCollar     = 'J';
inline constexpr size_t  kLULDAuctionCollarSize = 35;

// Operational Halt ('h')
// Fields: ...common(11) stock(8) market_code(1) halt_action(1)
inline constexpr uint8_t kOperationalHalt     = 'h';
inline constexpr size_t  kOperationalHaltSize = 21;

// Add Order — No MPID ('A')
// Fields: ...common(11) order_ref(8) side(1) shares(4) stock(8) price(4)
inline constexpr uint8_t kAddOrder     = 'A';
inline constexpr size_t  kAddOrderSize = 36;

// Add Order with MPID Attribution ('F')
// Fields: same as AddOrder + attribution(4)
inline constexpr uint8_t kAddOrderMPID     = 'F';
inline constexpr size_t  kAddOrderMPIDSize = 40;

// Order Executed ('E')
// Fields: ...common(11) order_ref(8) executed_shares(4) match_number(8)
inline constexpr uint8_t kOrderExecuted     = 'E';
inline constexpr size_t  kOrderExecutedSize = 31;

// Order Executed With Price ('C')
// Fields: ...common(11) order_ref(8) exec_shares(4) match_num(8) printable(1) exec_price(4)
inline constexpr uint8_t kOrderExecutedWithPrice     = 'C';
inline constexpr size_t  kOrderExecutedWithPriceSize = 36;

// Order Cancel ('X')
// Fields: ...common(11) order_ref(8) cancelled_shares(4)
inline constexpr uint8_t kOrderCancel     = 'X';
inline constexpr size_t  kOrderCancelSize = 23;

// Order Delete ('D')
// Fields: ...common(11) order_ref(8)
inline constexpr uint8_t kOrderDelete     = 'D';
inline constexpr size_t  kOrderDeleteSize = 19;

// Order Replace ('U')
// Fields: ...common(11) orig_order_ref(8) new_order_ref(8) shares(4) price(4)
inline constexpr uint8_t kOrderReplace     = 'U';
inline constexpr size_t  kOrderReplaceSize = 35;

// Non-Cross Trade ('P')
// Fields: ...common(11) order_ref(8) side(1) shares(4) stock(8) price(4) match_num(8)
inline constexpr uint8_t kNonCrossTrade     = 'P';
inline constexpr size_t  kNonCrossTradeSize = 44;

// Cross Trade ('Q')
// Fields: ...common(11) shares(8) stock(8) cross_price(4) match_num(8) cross_type(1)
inline constexpr uint8_t kCrossTrade     = 'Q';
inline constexpr size_t  kCrossTradeSize = 40;

// Broken Trade ('B')
// Fields: ...common(11) match_number(8)
inline constexpr uint8_t kBrokenTrade     = 'B';
inline constexpr size_t  kBrokenTradeSize = 19;

// Net Order Imbalance Indicator ('I')
// Fields: ...common(11) paired(8) imbalance(8) direction(1) stock(8)
//         far_price(4) near_price(4) current_ref(4) cross_type(1) price_variation(1)
inline constexpr uint8_t kNOII     = 'I';
inline constexpr size_t  kNOIISize = 50;

// Retail Price Improvement Indicator ('N')
// Fields: ...common(11) stock(8) interest_flag(1)
inline constexpr uint8_t kRPII     = 'N';
inline constexpr size_t  kRPIISize = 20;

// ---------------------------------------------------------------------------
// Aggregate message size helpers
// ---------------------------------------------------------------------------

namespace detail {
/// @brief Compile-time array of all ITCH 5.0 message sizes.
///
/// Adding a new message type here automatically updates kMaxMessageSize/kMinMessageSize.
inline constexpr size_t kAllMessageSizes[] = {
    kSystemEventSize, kStockDirectorySize, kStockTradingActionSize,
    kRegSHORestrictionSize, kMarketParticipantPositionSize,
    kMWCBDeclineLevelSize, kMWCBStatusSize, kIPOQuotingPeriodSize,
    kLULDAuctionCollarSize, kOperationalHaltSize,
    kAddOrderSize, kAddOrderMPIDSize, kOrderExecutedSize,
    kOrderExecutedWithPriceSize, kOrderCancelSize, kOrderDeleteSize,
    kOrderReplaceSize, kNonCrossTradeSize, kCrossTradeSize,
    kBrokenTradeSize, kNOIISize, kRPIISize,
};

consteval size_t max_of(const size_t* arr, size_t n) {
    size_t m = arr[0];
    for (size_t i = 1; i < n; ++i) if (arr[i] > m) m = arr[i];
    return m;
}
consteval size_t min_of(const size_t* arr, size_t n) {
    size_t m = arr[0];
    for (size_t i = 1; i < n; ++i) if (arr[i] < m) m = arr[i];
    return m;
}
} // namespace detail

/// Maximum on-wire message size across all ITCH 5.0 message types.
/// Useful for buffer pre-allocation: any single ITCH message fits in this many bytes.
/// Automatically derived from the per-type size constants — no manual update needed.
inline constexpr size_t kMaxMessageSize = detail::max_of(
    detail::kAllMessageSizes,
    sizeof(detail::kAllMessageSizes) / sizeof(detail::kAllMessageSizes[0]));

/// Minimum on-wire message size across all ITCH 5.0 message types.
inline constexpr size_t kMinMessageSize = detail::min_of(
    detail::kAllMessageSizes,
    sizeof(detail::kAllMessageSizes) / sizeof(detail::kAllMessageSizes[0]));

// ---------------------------------------------------------------------------
// Message classification helpers
// ---------------------------------------------------------------------------
// Classify ITCH message types into logical categories for feed handler routing.
// All functions are constexpr — usable in compile-time dispatch or switch guards.

/// @brief Check if a message type is a system or reference-data message.
///
/// System messages carry market-structure metadata rather than order/trade
/// activity: events, directories, halts, collars, etc.
///
/// @param type The 1-byte ITCH message type character.
/// @return True if the type is a system/reference-data message.
inline constexpr bool is_system_message(uint8_t type) noexcept {
    switch (type) {
    case kSystemEvent:
    case kStockDirectory:
    case kStockTradingAction:
    case kRegSHORestriction:
    case kMarketParticipantPosition:
    case kMWCBDeclineLevel:
    case kMWCBStatus:
    case kIPOQuotingPeriod:
    case kLULDAuctionCollar:
    case kOperationalHalt:
        return true;
    default:
        return false;
    }
}

/// @brief Check if a message type is an order-lifecycle message.
///
/// Order messages represent changes to the order book: add, execute,
/// cancel, delete, replace.
///
/// @param type The 1-byte ITCH message type character.
/// @return True if the type is an order-lifecycle message.
inline constexpr bool is_order_message(uint8_t type) noexcept {
    switch (type) {
    case kAddOrder:
    case kAddOrderMPID:
    case kOrderExecuted:
    case kOrderExecutedWithPrice:
    case kOrderCancel:
    case kOrderDelete:
    case kOrderReplace:
        return true;
    default:
        return false;
    }
}

/// @brief Check if a message type is a trade or trade-status message.
///
/// Trade messages include non-cross trades, cross trades, and broken trades.
///
/// @param type The 1-byte ITCH message type character.
/// @return True if the type is a trade message.
inline constexpr bool is_trade_message(uint8_t type) noexcept {
    switch (type) {
    case kNonCrossTrade:
    case kCrossTrade:
    case kBrokenTrade:
        return true;
    default:
        return false;
    }
}

/// @brief Check if a message type is an imbalance indicator message.
///
/// Imbalance messages carry auction imbalance (NOII) and retail interest
/// (RPII) data used for pre-open/pre-close strategies.
///
/// @param type The 1-byte ITCH message type character.
/// @return True if the type is an imbalance indicator message.
inline constexpr bool is_imbalance_message(uint8_t type) noexcept {
    return type == kNOII || type == kRPII;
}

/// @brief Check if a message type byte is a known ITCH 5.0 type.
/// @param type The 1-byte ITCH message type character.
/// @return True if the type matches any of the 22 defined ITCH 5.0 message types.
inline constexpr bool is_known_type(uint8_t type) noexcept {
    return is_system_message(type) || is_order_message(type) ||
           is_trade_message(type) || is_imbalance_message(type);
}

// ---------------------------------------------------------------------------
// Per-message field accessors
// ---------------------------------------------------------------------------
// Each namespace operates on the *full* message pointer (byte 0 = type tag).
// Field offsets are absolute within the message.

// ---- SystemEvent ('S') ---------------------------------------------------
/// @brief Accessors for SystemEvent ('S') messages.
///
/// Layout: type(1) locate(2) tracking(2) timestamp(6) event_code(1)
/// Total: 12 bytes. Common header = 11 bytes; event_code at absolute offset 11.
namespace system_event {

/// @brief Extract the system event code.
///
/// Event code values: 'O'=start-of-messages, 'S'=start-of-system-hours,
/// 'Q'=start-of-market-hours, 'M'=end-of-market-hours,
/// 'E'=end-of-system-hours, 'C'=end-of-messages.
///
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Single character event code.
inline char event_code(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[11]);
}

} // namespace system_event

// ---- StockDirectory ('R') ------------------------------------------------
/// @brief Accessors for StockDirectory ('R') messages.
///
/// Layout: type(1) locate(2) tracking(2) timestamp(6) stock(8)
/// market_category(1) financial_status(1) round_lot_size(4)
/// round_lots_only(1) issue_classification(1) issue_subtype(2)
/// authenticity(1) short_sale_threshold(1) ipo_flag(1)
/// luld_ref_price_tier(1) etp_flag(1) etp_leverage_factor(4)
/// inverse_indicator(1).
/// Total: 39 bytes.
namespace stock_directory {

/// @brief Stock symbol, right-padded with spaces (8 bytes at offset 11).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 8-character string_view into the message buffer (includes padding).
inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 11), 8};
}

/// @brief Stock symbol with trailing spaces removed.
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Trimmed string_view of the stock symbol.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

/// @brief Market category code (1 byte at offset 19).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Market category: 'Q'=NASDAQ Global Select, 'G'=NASDAQ Global, 'S'=NASDAQ Capital, etc.
inline char market_category(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[19]);
}

/// @brief Financial status indicator (1 byte at offset 20).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Financial status: 'N'=normal, 'D'=deficient, 'E'=delinquent, 'Q'=bankrupt, etc.
inline char financial_status(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[20]);
}

/// @brief Round lot size for the issue (4 bytes BE at offset 21).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Number of shares in a round lot (typically 100).
inline uint32_t round_lot_size(const uint8_t* msg) noexcept {
    return read_be32(msg + 21);
}

/// @brief Round lots only flag (1 byte at offset 25).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 'Y' if only round lots are accepted, 'N' otherwise.
inline char round_lots_only(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[25]);
}

/// @brief Issue classification (1 byte at offset 26).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Classification code (e.g. 'A'=American Depository Share, 'C'=common stock).
inline char issue_classification(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[26]);
}

/// @brief Issue sub-type (2 bytes at offset 27).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 2-character issue sub-type code.
inline std::string_view issue_subtype(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 27), 2};
}

/// @brief Authenticity flag (1 byte at offset 29).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 'P'=live/production, 'T'=test.
inline char authenticity(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[29]);
}

/// @brief Short sale threshold indicator (1 byte at offset 30).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 'Y'=restricted, 'N'=not restricted, ' '=not available.
inline char short_sale_threshold(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[30]);
}

/// @brief IPO flag (1 byte at offset 31).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 'Y'=NASDAQ listed IPO scheduled today, 'N'=not, ' '=not available.
inline char ipo_flag(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[31]);
}

/// @brief LULD reference price tier (1 byte at offset 32).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return '1'=Tier 1 NMS, '2'=Tier 2 NMS, ' '=not available.
inline char luld_ref_price_tier(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[32]);
}

/// @brief ETP flag (1 byte at offset 33).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 'Y'=ETP/ETF, 'N'=not, ' '=not available.
inline char etp_flag(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[33]);
}

/// @brief ETP leverage factor (4 bytes BE at offset 34).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Leverage factor for leveraged/inverse ETPs (e.g. 2, 3).
inline uint32_t etp_leverage_factor(const uint8_t* msg) noexcept {
    return read_be32(msg + 34);
}

/// @brief Inverse indicator (1 byte at offset 38).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 'Y'=inverse ETP, 'N'=not inverse.
inline char inverse_indicator(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[38]);
}

} // namespace stock_directory

// ---- AddOrder ('A') -- No MPID --------------------------------------------
/// @brief Accessors for AddOrder ('A') messages (no MPID attribution).
///
/// Layout: type(1) locate(2) tracking(2) timestamp(6)
/// order_ref(8) side(1) shares(4) stock(8) price(4).
/// Total: 36 bytes.
namespace add_order {

/// @brief Order reference number (8 bytes BE at offset 11).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Unique order reference number assigned by the matching engine.
inline uint64_t order_ref(const uint8_t* msg) noexcept {
    return read_be64(msg + 11);
}

/// @brief Buy/sell indicator (1 byte at offset 19).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 'B'=buy, 'S'=sell.
inline char side(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[19]);
}

/// @brief Number of shares in the order (4 bytes BE at offset 20).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Share quantity.
inline uint32_t shares(const uint8_t* msg) noexcept {
    return read_be32(msg + 20);
}

/// @brief Stock symbol, right-padded with spaces (8 bytes at offset 24).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 8-character string_view into the message buffer (includes padding).
inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 24), 8};
}

/// @brief Stock symbol with trailing spaces removed.
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Trimmed string_view of the stock symbol.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

/// @brief Raw price field (4 bytes BE at offset 32).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Price in fixed-point format (divide by 10000 for dollars).
inline uint32_t price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 32);
}

/// @brief Price in dollars (ITCH prices have 4 implied decimal places).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Price as a floating-point dollar value.
inline double price(const uint8_t* msg) noexcept {
    return price_raw(msg) / kItchPriceDivisor;
}

} // namespace add_order

// ---- AddOrderMPID ('F') --------------------------------------------------
/// @brief Accessors for AddOrder with MPID Attribution ('F') messages.
///
/// Same field layout as AddOrder ('A') with a 4-byte MPID attribution appended.
/// Layout: type(1) locate(2) tracking(2) timestamp(6)
/// order_ref(8) side(1) shares(4) stock(8) price(4) attribution(4).
/// Total: 40 bytes.
namespace add_order_mpid {

/// @brief Order reference number (8 bytes BE at offset 11).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Unique order reference number assigned by the matching engine.
inline uint64_t order_ref(const uint8_t* msg) noexcept {
    return read_be64(msg + 11);
}

/// @brief Buy/sell indicator (1 byte at offset 19).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 'B'=buy, 'S'=sell.
inline char side(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[19]);
}

/// @brief Number of shares in the order (4 bytes BE at offset 20).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Share quantity.
inline uint32_t shares(const uint8_t* msg) noexcept {
    return read_be32(msg + 20);
}

/// @brief Stock symbol, right-padded with spaces (8 bytes at offset 24).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 8-character string_view into the message buffer (includes padding).
inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 24), 8};
}

/// @brief Stock symbol with trailing spaces removed.
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Trimmed string_view of the stock symbol.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

/// @brief Raw price field (4 bytes BE at offset 32).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Price in fixed-point format (divide by 10000 for dollars).
inline uint32_t price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 32);
}

/// @brief Price in dollars (4 implied decimal places).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Price as a floating-point dollar value.
inline double price(const uint8_t* msg) noexcept {
    return price_raw(msg) / kItchPriceDivisor;
}

/// @brief 4-character MPID attribution, right-padded with spaces.
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 4-character string_view of the market participant identifier.
inline std::string_view attribution(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 36), 4};
}

/// @brief MPID attribution with trailing spaces removed.
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Trimmed string_view of the MPID.
inline std::string_view attribution_trimmed(const uint8_t* msg) noexcept {
    return trim(attribution(msg));
}

} // namespace add_order_mpid

// ---- OrderExecuted ('E') -------------------------------------------------
/// @brief Accessors for OrderExecuted ('E') messages.
///
/// Layout: type(1) locate(2) tracking(2) timestamp(6)
/// order_ref(8) executed_shares(4) match_number(8).
/// Total: 31 bytes.
///
/// @note The execution price is the price of the original order. For executions
///       at a different price, see OrderExecutedWithPrice ('C').
namespace order_executed {

/// @brief Order reference number (8 bytes BE at offset 11).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Order reference number that was executed against.
inline uint64_t order_ref(const uint8_t* msg) noexcept {
    return read_be64(msg + 11);
}

/// @brief Number of shares executed (4 bytes BE at offset 19).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Number of shares in this execution.
inline uint32_t executed_shares(const uint8_t* msg) noexcept {
    return read_be32(msg + 19);
}

/// @brief Match number for this execution (8 bytes BE at offset 23).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Unique day-level match number for trade reconciliation.
inline uint64_t match_number(const uint8_t* msg) noexcept {
    return read_be64(msg + 23);
}

} // namespace order_executed

// ---- OrderExecutedWithPrice ('C') ----------------------------------------
/// @brief Accessors for OrderExecutedWithPrice ('C') messages.
///
/// Layout: type(1) locate(2) tracking(2) timestamp(6)
/// order_ref(8) executed_shares(4) match_number(8) printable(1) execution_price(4).
/// Total: 36 bytes.
///
/// @note Unlike OrderExecuted ('E'), this message includes an explicit
///       execution price that may differ from the original order price.
namespace order_executed_price {

/// @brief Order reference number (8 bytes BE at offset 11).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Order reference number that was executed against.
inline uint64_t order_ref(const uint8_t* msg) noexcept {
    return read_be64(msg + 11);
}

/// @brief Number of shares executed (4 bytes BE at offset 19).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Number of shares in this execution.
inline uint32_t executed_shares(const uint8_t* msg) noexcept {
    return read_be32(msg + 19);
}

/// @brief Match number for this execution (8 bytes BE at offset 23).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Unique day-level match number for trade reconciliation.
inline uint64_t match_number(const uint8_t* msg) noexcept {
    return read_be64(msg + 23);
}

/// @brief Printable flag (1 byte at offset 31).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 'Y'=printable (reported to tape), 'N'=non-printable (not reported).
inline char printable(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[31]);
}

/// @brief Raw execution price (4 bytes BE at offset 32).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Price in fixed-point format (divide by 10000 for dollars).
inline uint32_t execution_price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 32);
}

/// @brief Execution price in dollars (4 implied decimal places).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Price as a floating-point dollar value.
inline double execution_price(const uint8_t* msg) noexcept {
    return execution_price_raw(msg) / kItchPriceDivisor;
}

} // namespace order_executed_price

// ---- OrderCancel ('X') ---------------------------------------------------
/// @brief Accessors for OrderCancel ('X') messages.
///
/// Layout: type(1) locate(2) tracking(2) timestamp(6)
/// order_ref(8) cancelled_shares(4).
/// Total: 23 bytes.
namespace order_cancel {

/// @brief Order reference number (8 bytes BE at offset 11).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Order reference number being partially cancelled.
inline uint64_t order_ref(const uint8_t* msg) noexcept {
    return read_be64(msg + 11);
}

/// @brief Number of shares cancelled (4 bytes BE at offset 19).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Number of shares removed from the order.
inline uint32_t cancelled_shares(const uint8_t* msg) noexcept {
    return read_be32(msg + 19);
}

} // namespace order_cancel

// ---- OrderDelete ('D') ---------------------------------------------------
/// @brief Accessors for OrderDelete ('D') messages.
///
/// Layout: type(1) locate(2) tracking(2) timestamp(6) order_ref(8).
/// Total: 19 bytes. The entire remaining quantity is removed from the book.
namespace order_delete {

/// @brief Order reference number (8 bytes BE at offset 11).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Order reference number being fully removed from the book.
inline uint64_t order_ref(const uint8_t* msg) noexcept {
    return read_be64(msg + 11);
}

} // namespace order_delete

// ---- OrderReplace ('U') --------------------------------------------------
/// @brief Accessors for OrderReplace ('U') messages.
///
/// Layout: type(1) locate(2) tracking(2) timestamp(6)
/// original_order_ref(8) new_order_ref(8) shares(4) price(4).
/// Total: 35 bytes.
///
/// @note A replace atomically removes the original order and inserts a new one.
///       The new order loses time priority at the replaced price level.
namespace order_replace {

/// @brief Original order reference number (8 bytes BE at offset 11).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Order reference number of the order being replaced.
inline uint64_t original_order_ref(const uint8_t* msg) noexcept {
    return read_be64(msg + 11);
}

/// @brief New order reference number (8 bytes BE at offset 19).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Order reference number assigned to the replacement order.
inline uint64_t new_order_ref(const uint8_t* msg) noexcept {
    return read_be64(msg + 19);
}

/// @brief New share quantity (4 bytes BE at offset 27).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Number of shares in the replacement order.
inline uint32_t shares(const uint8_t* msg) noexcept {
    return read_be32(msg + 27);
}

/// @brief Raw price field of the replacement order (4 bytes BE at offset 31).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Price in fixed-point format (divide by 10000 for dollars).
inline uint32_t price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 31);
}

/// @brief Replacement price in dollars (4 implied decimal places).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Price as a floating-point dollar value.
inline double price(const uint8_t* msg) noexcept {
    return price_raw(msg) / kItchPriceDivisor;
}

} // namespace order_replace

// ---- NonCrossTrade ('P') -------------------------------------------------
/// @brief Accessors for NonCrossTrade ('P') messages.
///
/// Layout: type(1) locate(2) tracking(2) timestamp(6)
/// order_ref(8) side(1) shares(4) stock(8) price(4) match_number(8).
/// Total: 44 bytes. Represents a normal (non-cross) trade execution.
namespace non_cross_trade {

/// @brief Order reference number of the non-displayable order (8 bytes BE at offset 11).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Order reference number.
inline uint64_t order_ref(const uint8_t* msg) noexcept {
    return read_be64(msg + 11);
}

/// @brief Buy/sell indicator (1 byte at offset 19).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 'B'=buy, 'S'=sell.
inline char side(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[19]);
}

/// @brief Number of shares traded (4 bytes BE at offset 20).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Share quantity.
inline uint32_t shares(const uint8_t* msg) noexcept {
    return read_be32(msg + 20);
}

/// @brief Stock symbol, right-padded with spaces (8 bytes at offset 24).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 8-character string_view into the message buffer (includes padding).
inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 24), 8};
}

/// @brief Stock symbol with trailing spaces removed.
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Trimmed string_view of the stock symbol.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

/// @brief Raw trade price (4 bytes BE at offset 32).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Price in fixed-point format (divide by 10000 for dollars).
inline uint32_t price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 32);
}

/// @brief Trade price in dollars (4 implied decimal places).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Price as a floating-point dollar value.
inline double price(const uint8_t* msg) noexcept {
    return price_raw(msg) / kItchPriceDivisor;
}

/// @brief Match number for this trade (8 bytes BE at offset 36).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Unique day-level match number for trade reconciliation.
inline uint64_t match_number(const uint8_t* msg) noexcept {
    return read_be64(msg + 36);
}

} // namespace non_cross_trade

// ---- CrossTrade ('Q') ----------------------------------------------------
/// @brief Accessors for CrossTrade ('Q') messages.
///
/// Layout: type(1) locate(2) tracking(2) timestamp(6)
/// shares(8) stock(8) cross_price(4) match_number(8) cross_type(1).
/// Total: 40 bytes. Represents a cross (auction) trade execution.
namespace cross_trade {

/// @brief Number of shares matched in the cross (8 bytes BE at offset 11).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Total share quantity in this cross trade.
inline uint64_t shares(const uint8_t* msg) noexcept {
    return read_be64(msg + 11);
}

/// @brief Stock symbol, right-padded with spaces (8 bytes at offset 19).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 8-character string_view into the message buffer (includes padding).
inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 19), 8};
}

/// @brief Stock symbol with trailing spaces removed.
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Trimmed string_view of the stock symbol.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

/// @brief Raw cross price (4 bytes BE at offset 27).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Price in fixed-point format (divide by 10000 for dollars).
inline uint32_t cross_price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 27);
}

/// @brief Cross price in dollars (4 implied decimal places).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Price as a floating-point dollar value.
inline double cross_price(const uint8_t* msg) noexcept {
    return cross_price_raw(msg) / kItchPriceDivisor;
}

/// @brief Match number for this cross trade (8 bytes BE at offset 31).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Unique day-level match number for trade reconciliation.
inline uint64_t match_number(const uint8_t* msg) noexcept {
    return read_be64(msg + 31);
}

/// @brief Cross type indicator (1 byte at offset 39).
///
/// Values: 'O'=opening, 'C'=closing, 'H'=halted/IPO,
///         'I'=intraday/post-close.
///
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Single character cross type code.
inline char cross_type(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[39]);
}

} // namespace cross_trade

// ---- StockTradingAction ('H') --------------------------------------------
/// @brief Accessors for StockTradingAction ('H') messages.
///
/// Layout: type(1) locate(2) tracking(2) timestamp(6)
/// stock(8) trading_state(1) reserved(1) reason(4).
/// Total: 25 bytes. Indicates changes to a stock's trading status.
namespace stock_trading_action {

/// @brief Stock symbol, right-padded with spaces (8 bytes at offset 11).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 8-character string_view into the message buffer (includes padding).
inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 11), 8};
}

/// @brief Stock symbol with trailing spaces removed.
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Trimmed string_view of the stock symbol.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

/// @brief Trading state (1 byte at offset 19).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 'H'=halted, 'P'=paused, 'Q'=quotation-only, 'T'=trading.
inline char trading_state(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[19]);
}

/// @brief Reserved byte (1 byte at offset 20).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Reserved byte value.
inline char reserved(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[20]);
}

/// @brief Reason for the trading action (4 bytes at offset 21).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 4-character reason code string_view.
inline std::string_view reason(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 21), 4};
}

} // namespace stock_trading_action

// ---- RegSHORestriction ('Y') ---------------------------------------------
/// @brief Accessors for RegSHORestriction ('Y') messages.
///
/// Layout: type(1) locate(2) tracking(2) timestamp(6) stock(8) reg_sho_action(1).
/// Total: 20 bytes. Indicates Regulation SHO short sale price test restrictions.
namespace reg_sho_restriction {

/// @brief Stock symbol, right-padded with spaces (8 bytes at offset 11).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 8-character string_view into the message buffer (includes padding).
inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 11), 8};
}

/// @brief Stock symbol with trailing spaces removed.
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Trimmed string_view of the stock symbol.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

/// @brief Reg SHO short sale restriction action (1 byte at offset 19).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return '0'=no restriction, '1'=restriction activated, '2'=restriction continued.
inline char reg_sho_action(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[19]);
}

} // namespace reg_sho_restriction

// ---- MarketParticipantPosition ('L') -------------------------------------
/// @brief Accessors for MarketParticipantPosition ('L') messages.
///
/// Layout: type(1) locate(2) tracking(2) timestamp(6)
/// mpid(4) stock(8) primary_market_maker(1) market_maker_mode(1)
/// market_participant_state(1).
/// Total: 26 bytes. Provides market maker registration information.
namespace market_participant_position {

/// @brief Market participant identifier (4 bytes at offset 11).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 4-character MPID string_view.
inline std::string_view mpid(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 11), 4};
}

/// @brief Stock symbol, right-padded with spaces (8 bytes at offset 15).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 8-character string_view into the message buffer (includes padding).
inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 15), 8};
}

/// @brief Stock symbol with trailing spaces removed.
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Trimmed string_view of the stock symbol.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

/// @brief Primary market maker flag (1 byte at offset 23).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 'Y'=primary market maker, 'N'=not primary.
inline char primary_market_maker(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[23]);
}

/// @brief Market maker mode (1 byte at offset 24).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 'N'=normal, 'P'=passive, 'S'=syndicate, 'R'=pre-syndicate, 'L'=penalty.
inline char market_maker_mode(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[24]);
}

/// @brief Market participant state (1 byte at offset 25).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 'A'=active, 'E'=excused/withdrawn, 'W'=withdrawn, 'S'=suspended, 'D'=deleted.
inline char market_participant_state(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[25]);
}

} // namespace market_participant_position

// ---- MWCBDeclineLevel ('V') ----------------------------------------------
/// @brief Accessors for MWCBDeclineLevel ('V') messages.
///
/// Layout: type(1) locate(2) tracking(2) timestamp(6) level1(8) level2(8) level3(8).
/// Total: 35 bytes. Prices use price8 format (8 implied decimal places).
/// Market-Wide Circuit Breaker decline levels trigger trading halts when
/// the S&P 500 drops by a specified percentage.
namespace mwcb_decline_level {

/// @brief Level 1 MWCB value (raw, 8 bytes big-endian at offset 11).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Raw 64-bit fixed-point value (divide by 10^8 for dollars).
inline uint64_t level1_raw(const uint8_t* msg) noexcept {
    return read_be64(msg + 11);
}

/// @brief Level 1 MWCB value in dollars (price8: 8 implied decimal places).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Level 1 decline threshold as a floating-point dollar value.
inline double level1(const uint8_t* msg) noexcept {
    return level1_raw(msg) / kLuldPriceDivisor;
}

/// @brief Level 2 MWCB value (raw, 8 bytes big-endian at offset 19).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Raw 64-bit fixed-point value (divide by 10^8 for dollars).
inline uint64_t level2_raw(const uint8_t* msg) noexcept {
    return read_be64(msg + 19);
}

/// @brief Level 2 MWCB value in dollars (price8: 8 implied decimal places).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Level 2 decline threshold as a floating-point dollar value.
inline double level2(const uint8_t* msg) noexcept {
    return level2_raw(msg) / kLuldPriceDivisor;
}

/// @brief Level 3 MWCB value (raw, 8 bytes big-endian at offset 27).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Raw 64-bit fixed-point value (divide by 10^8 for dollars).
inline uint64_t level3_raw(const uint8_t* msg) noexcept {
    return read_be64(msg + 27);
}

/// @brief Level 3 MWCB value in dollars (price8: 8 implied decimal places).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Level 3 decline threshold as a floating-point dollar value.
inline double level3(const uint8_t* msg) noexcept {
    return level3_raw(msg) / kLuldPriceDivisor;
}

} // namespace mwcb_decline_level

// ---- MWCBStatus ('W') ----------------------------------------------------
/// @brief Accessors for MWCBStatus ('W') messages.
///
/// Layout: type(1) locate(2) tracking(2) timestamp(6) breached_level(1).
/// Total: 12 bytes. Indicates which MWCB level has been breached.
namespace mwcb_status {

/// @brief Which MWCB decline level has been breached (1 byte at offset 11).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return '1'=Level 1, '2'=Level 2, '3'=Level 3.
inline char breached_level(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[11]);
}

} // namespace mwcb_status

// ---- IPOQuotingPeriod ('K') ----------------------------------------------
/// @brief Accessors for IPOQuotingPeriod ('K') messages.
///
/// Layout: type(1) locate(2) tracking(2) timestamp(6)
/// stock(8) ipo_quotation_release_time(4) ipo_quotation_release_qualifier(1) ipo_price(4).
/// Total: 28 bytes. Provides IPO quotation period update information.
namespace ipo_quoting_period {

/// @brief Stock symbol, right-padded with spaces (8 bytes at offset 11).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 8-character string_view into the message buffer (includes padding).
inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 11), 8};
}

/// @brief Stock symbol with trailing spaces removed.
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Trimmed string_view of the stock symbol.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

/// @brief IPO quotation release time as seconds after midnight (4 bytes BE at offset 19).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Seconds after midnight when the IPO quotation period is expected to begin.
inline uint32_t ipo_quotation_release_time(const uint8_t* msg) noexcept {
    return read_be32(msg + 19);
}

/// @brief IPO quotation release qualifier (1 byte at offset 23).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 'A'=anticipated, 'C'=cancelled.
inline char ipo_quotation_release_qualifier(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[23]);
}

/// @brief IPO price, raw fixed-point (4 bytes BE at offset 24, price4 format).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Raw price value (divide by 10000 for dollars).
inline uint32_t ipo_price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 24);
}

/// @brief IPO price in dollars (4 implied decimal places).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return IPO price as a floating-point dollar value.
inline double ipo_price(const uint8_t* msg) noexcept {
    return ipo_price_raw(msg) / kItchPriceDivisor;
}

} // namespace ipo_quoting_period

// ---- LULDAuctionCollar ('J') ---------------------------------------------
/// @brief Accessors for LULDAuctionCollar ('J') messages.
///
/// Layout: type(1) locate(2) tracking(2) timestamp(6) stock(8)
/// auction_collar_reference_price(4) upper_auction_collar_price(4)
/// lower_auction_collar_price(4) auction_collar_extension(4).
/// Total: 35 bytes. Defines Limit Up-Limit Down auction collar prices.
namespace luld_auction_collar {

/// @brief Stock symbol, right-padded with spaces (8 bytes at offset 11).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 8-character string_view into the message buffer (includes padding).
inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 11), 8};
}

/// @brief Stock symbol with trailing spaces removed.
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Trimmed string_view of the stock symbol.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

/// @brief Auction collar reference price, raw (4 bytes BE at offset 19).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Raw price value (divide by 10000 for dollars).
inline uint32_t auction_collar_reference_price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 19);
}

/// @brief Auction collar reference price in dollars (4 implied decimal places).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Reference price as a floating-point dollar value.
inline double auction_collar_reference_price(const uint8_t* msg) noexcept {
    return auction_collar_reference_price_raw(msg) / kItchPriceDivisor;
}

/// @brief Upper auction collar price, raw (4 bytes BE at offset 23).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Raw price value (divide by 10000 for dollars).
inline uint32_t upper_auction_collar_price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 23);
}

/// @brief Upper auction collar price in dollars (4 implied decimal places).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Upper collar price as a floating-point dollar value.
inline double upper_auction_collar_price(const uint8_t* msg) noexcept {
    return upper_auction_collar_price_raw(msg) / kItchPriceDivisor;
}

/// @brief Lower auction collar price, raw (4 bytes BE at offset 27).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Raw price value (divide by 10000 for dollars).
inline uint32_t lower_auction_collar_price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 27);
}

/// @brief Lower auction collar price in dollars (4 implied decimal places).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Lower collar price as a floating-point dollar value.
inline double lower_auction_collar_price(const uint8_t* msg) noexcept {
    return lower_auction_collar_price_raw(msg) / kItchPriceDivisor;
}

/// @brief Auction collar extension (4 bytes BE at offset 31).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Number of seconds the auction collar extension is in effect.
inline uint32_t auction_collar_extension(const uint8_t* msg) noexcept {
    return read_be32(msg + 31);
}

} // namespace luld_auction_collar

// ---- OperationalHalt ('h') -----------------------------------------------
/// @brief Accessors for OperationalHalt ('h') messages.
///
/// Layout: type(1) locate(2) tracking(2) timestamp(6)
/// stock(8) market_code(1) operational_halt_action(1).
/// Total: 21 bytes. Indicates operational halt or resumption for a stock.
namespace operational_halt {

/// @brief Stock symbol, right-padded with spaces (8 bytes at offset 11).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 8-character string_view into the message buffer (includes padding).
inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 11), 8};
}

/// @brief Stock symbol with trailing spaces removed.
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Trimmed string_view of the stock symbol.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

/// @brief Market code (1 byte at offset 19).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 'Q'=NASDAQ, 'B'=BX, 'X'=PSX.
inline char market_code(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[19]);
}

/// @brief Operational halt action (1 byte at offset 20).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 'H'=halted, 'T'=resumed.
inline char operational_halt_action(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[20]);
}

} // namespace operational_halt

// ---- BrokenTrade ('B') ---------------------------------------------------
/// @brief Accessors for BrokenTrade ('B') messages.
///
/// Layout: type(1) locate(2) tracking(2) timestamp(6) match_number(8).
/// Total: 19 bytes. Indicates a previously reported trade has been broken/cancelled.
namespace broken_trade {

/// @brief Match number of the broken trade (8 bytes BE at offset 11).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Match number of the trade that has been broken.
inline uint64_t match_number(const uint8_t* msg) noexcept {
    return read_be64(msg + 11);
}

} // namespace broken_trade

// ---- NOII ('I') -- Net Order Imbalance Indicator --------------------------
/// @brief Accessors for NOII ('I') -- Net Order Imbalance Indicator messages.
///
/// Layout: type(1) locate(2) tracking(2) timestamp(6)
/// paired_shares(8) imbalance_shares(8) imbalance_direction(1)
/// stock(8) far_price(4) near_price(4) current_reference_price(4)
/// cross_type(1) price_variation_indicator(1).
/// Total: 50 bytes. Published during pre-open and pre-close periods to
/// indicate auction imbalance state.
namespace noii {

/// @brief Paired shares (8 bytes BE at offset 11).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Number of shares that are paired at the current reference price.
inline uint64_t paired_shares(const uint8_t* msg) noexcept {
    return read_be64(msg + 11);
}

/// @brief Imbalance shares (8 bytes BE at offset 19).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Number of shares on the imbalance side (buy or sell excess).
inline uint64_t imbalance_shares(const uint8_t* msg) noexcept {
    return read_be64(msg + 19);
}

/// @brief Imbalance direction (1 byte at offset 27).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 'B'=buy, 'S'=sell, 'N'=no imbalance, 'O'=insufficient orders.
inline char imbalance_direction(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[27]);
}

/// @brief Stock symbol, right-padded with spaces (8 bytes at offset 28).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 8-character string_view into the message buffer (includes padding).
inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 28), 8};
}

/// @brief Stock symbol with trailing spaces removed.
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Trimmed string_view of the stock symbol.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

/// @brief Far price, raw (4 bytes BE at offset 36).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Raw price value (divide by 10000 for dollars).
inline uint32_t far_price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 36);
}

/// @brief Far price in dollars (4 implied decimal places).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Far clearing price as a floating-point dollar value.
inline double far_price(const uint8_t* msg) noexcept {
    return far_price_raw(msg) / kItchPriceDivisor;
}

/// @brief Near price, raw (4 bytes BE at offset 40).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Raw price value (divide by 10000 for dollars).
inline uint32_t near_price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 40);
}

/// @brief Near price in dollars (4 implied decimal places).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Near clearing price as a floating-point dollar value.
inline double near_price(const uint8_t* msg) noexcept {
    return near_price_raw(msg) / kItchPriceDivisor;
}

/// @brief Current reference price, raw (4 bytes BE at offset 44).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Raw price value (divide by 10000 for dollars).
inline uint32_t current_reference_price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 44);
}

/// @brief Current reference price in dollars (4 implied decimal places).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Current reference price as a floating-point dollar value.
inline double current_reference_price(const uint8_t* msg) noexcept {
    return current_reference_price_raw(msg) / kItchPriceDivisor;
}

/// @brief Cross type for the auction (1 byte at offset 48).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 'O'=opening, 'C'=closing, 'H'=halted/IPO.
inline char cross_type(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[48]);
}

/// @brief Price variation indicator (1 byte at offset 49).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Indicates the price variation from the reference price.
inline char price_variation_indicator(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[49]);
}

} // namespace noii

// ---- RPII ('N') -- Retail Price Improvement Indicator ---------------------
/// @brief Accessors for RPII ('N') -- Retail Price Improvement Indicator messages.
///
/// Layout: type(1) locate(2) tracking(2) timestamp(6) stock(8) interest_flag(1).
/// Total: 20 bytes. Indicates retail interest availability at improved prices.
namespace rpii {

/// @brief Stock symbol, right-padded with spaces (8 bytes at offset 11).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 8-character string_view into the message buffer (includes padding).
inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 11), 8};
}

/// @brief Stock symbol with trailing spaces removed.
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return Trimmed string_view of the stock symbol.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

/// @brief Retail interest flag (1 byte at offset 19).
/// @param msg Pointer to the full message (byte 0 = type tag).
/// @return 'B'=buy-side, 'S'=sell-side, 'A'=both, 'N'=none.
inline char interest_flag(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[19]);
}

} // namespace rpii

} // namespace eph::itch
