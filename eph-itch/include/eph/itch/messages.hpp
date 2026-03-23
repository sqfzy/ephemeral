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

/// Read a big-endian uint16_t from an arbitrary byte pointer.
inline uint16_t read_be16(const uint8_t* p) noexcept {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return std::byteswap(v);
}

/// Read a big-endian uint32_t from an arbitrary byte pointer.
inline uint32_t read_be32(const uint8_t* p) noexcept {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return std::byteswap(v);
}

/// Read a big-endian uint64_t from an arbitrary byte pointer.
inline uint64_t read_be64(const uint8_t* p) noexcept {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return std::byteswap(v);
}

/// Read a 6-byte big-endian timestamp (nanoseconds since midnight) into a
/// uint64_t.  The value occupies the low 48 bits.
inline uint64_t read_be48(const uint8_t* p) noexcept {
    uint64_t v = 0;
    std::memcpy(reinterpret_cast<uint8_t*>(&v) + 2, p, 6);
    return std::byteswap(v);
}

// ---------------------------------------------------------------------------
// String trimming utility
// ---------------------------------------------------------------------------

/// Trim trailing spaces from a right-padded ITCH string field.
/// ITCH encodes stock symbols and other text fields as fixed-width,
/// right-padded with ASCII spaces (0x20). This returns a view with
/// trailing spaces removed.
constexpr std::string_view trim(std::string_view s) noexcept {
    auto end = s.find_last_not_of(' ');
    return (end == std::string_view::npos) ? std::string_view{} : s.substr(0, end + 1);
}

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

/// Stock locate code (2 bytes at offset 0 of body).
inline uint16_t stock_locate(const uint8_t* body) noexcept {
    return read_be16(body);
}

/// Tracking number (2 bytes at offset 2 of body).
inline uint16_t tracking_number(const uint8_t* body) noexcept {
    return read_be16(body + 2);
}

/// Nanosecond timestamp since midnight (6 bytes at offset 4 of body).
inline uint64_t timestamp_ns(const uint8_t* body) noexcept {
    return read_be48(body + 4);
}

// ---------------------------------------------------------------------------
// Message type constants and sizes
// ---------------------------------------------------------------------------
// Sizes are the complete on-wire message length (including the 1-byte type
// field) but *excluding* any framing layer (e.g. the 2-byte MoldUDP64
// length prefix).

// System Event ('S')
inline constexpr uint8_t kSystemEvent     = 'S';
inline constexpr size_t  kSystemEventSize = 11;

// Stock Directory ('R')
inline constexpr uint8_t kStockDirectory     = 'R';
inline constexpr size_t  kStockDirectorySize = 38;

// Stock Trading Action ('H')
inline constexpr uint8_t kStockTradingAction     = 'H';
inline constexpr size_t  kStockTradingActionSize = 24;

// Reg SHO Restriction ('Y')
inline constexpr uint8_t kRegSHORestriction     = 'Y';
inline constexpr size_t  kRegSHORestrictionSize = 19;

// Market Participant Position ('L')
inline constexpr uint8_t kMarketParticipantPosition     = 'L';
inline constexpr size_t  kMarketParticipantPositionSize = 25;

// MWCB Decline Level ('V')
inline constexpr uint8_t kMWCBDeclineLevel     = 'V';
inline constexpr size_t  kMWCBDeclineLevelSize = 34;

// MWCB Status ('W')
inline constexpr uint8_t kMWCBStatus     = 'W';
inline constexpr size_t  kMWCBStatusSize = 11;

// IPO Quoting Period Update ('K')
inline constexpr uint8_t kIPOQuotingPeriod     = 'K';
inline constexpr size_t  kIPOQuotingPeriodSize = 27;

// LULD Auction Collar ('J')
inline constexpr uint8_t kLULDAuctionCollar     = 'J';
inline constexpr size_t  kLULDAuctionCollarSize = 34;

// Operational Halt ('h')
inline constexpr uint8_t kOperationalHalt     = 'h';
inline constexpr size_t  kOperationalHaltSize = 20;

// Add Order — No MPID ('A')
inline constexpr uint8_t kAddOrder     = 'A';
inline constexpr size_t  kAddOrderSize = 35;

// Add Order with MPID Attribution ('F')
inline constexpr uint8_t kAddOrderMPID     = 'F';
inline constexpr size_t  kAddOrderMPIDSize = 39;

// Order Executed ('E')
inline constexpr uint8_t kOrderExecuted     = 'E';
inline constexpr size_t  kOrderExecutedSize = 30;

// Order Executed With Price ('C')
inline constexpr uint8_t kOrderExecutedWithPrice     = 'C';
inline constexpr size_t  kOrderExecutedWithPriceSize = 35;

// Order Cancel ('X')
inline constexpr uint8_t kOrderCancel     = 'X';
inline constexpr size_t  kOrderCancelSize = 22;

// Order Delete ('D')
inline constexpr uint8_t kOrderDelete     = 'D';
inline constexpr size_t  kOrderDeleteSize = 18;

// Order Replace ('U')
inline constexpr uint8_t kOrderReplace     = 'U';
inline constexpr size_t  kOrderReplaceSize = 34;

// Non-Cross Trade ('P')
inline constexpr uint8_t kNonCrossTrade     = 'P';
inline constexpr size_t  kNonCrossTradeSize = 43;

// Cross Trade ('Q')
inline constexpr uint8_t kCrossTrade     = 'Q';
inline constexpr size_t  kCrossTradeSize = 39;

// Broken Trade ('B')
inline constexpr uint8_t kBrokenTrade     = 'B';
inline constexpr size_t  kBrokenTradeSize = 18;

// Net Order Imbalance Indicator ('I')
inline constexpr uint8_t kNOII     = 'I';
inline constexpr size_t  kNOIISize = 50;

// Retail Price Improvement Indicator ('N')
inline constexpr uint8_t kRPII     = 'N';
inline constexpr size_t  kRPIISize = 20;

// ---------------------------------------------------------------------------
// Per-message field accessors
// ---------------------------------------------------------------------------
// Each namespace operates on the *full* message pointer (byte 0 = type tag).
// Field offsets are absolute within the message.

// ---- SystemEvent ('S') ---------------------------------------------------
// Layout: type(1) locate(2) tracking(2) timestamp(6) event_code(1)
// Total: 11 + 1 = 12... spec says 11 including type byte, so event_code @10
namespace system_event {

/// Event code: 'O'=start-of-messages, 'S'=start-of-system-hours,
///             'Q'=start-of-market-hours, 'M'=end-of-market-hours,
///             'E'=end-of-system-hours, 'C'=end-of-messages
inline char event_code(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[10]);
}

} // namespace system_event

// ---- StockDirectory ('R') ------------------------------------------------
// Layout: type(1) locate(2) tracking(2) timestamp(6) stock(8)
//         market_category(1) financial_status(1) round_lot_size(4)
//         round_lots_only(1) issue_classification(1) issue_subtype(2)
//         authenticity(1) short_sale_threshold(1) ipo_flag(1)
//         luld_ref_price_tier(1) etp_flag(1) etp_leverage_factor(4)
//         inverse_indicator(1)
// Total: 38
namespace stock_directory {

inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 11), 8};
}

/// Stock symbol with trailing spaces removed.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

inline char market_category(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[19]);
}

inline char financial_status(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[20]);
}

inline uint32_t round_lot_size(const uint8_t* msg) noexcept {
    return read_be32(msg + 21);
}

inline char round_lots_only(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[25]);
}

inline char issue_classification(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[26]);
}

inline std::string_view issue_subtype(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 27), 2};
}

inline char authenticity(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[29]);
}

inline char short_sale_threshold(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[30]);
}

inline char ipo_flag(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[31]);
}

inline char luld_ref_price_tier(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[32]);
}

inline char etp_flag(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[33]);
}

inline uint32_t etp_leverage_factor(const uint8_t* msg) noexcept {
    return read_be32(msg + 34);
}

inline char inverse_indicator(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[37]);
}

} // namespace stock_directory

// ---- AddOrder ('A') — No MPID --------------------------------------------
// Layout: type(1) locate(2) tracking(2) timestamp(6)
//         order_ref(8) side(1) shares(4) stock(8) price(4)
// Total: 35
namespace add_order {

inline uint64_t order_ref(const uint8_t* msg) noexcept {
    return read_be64(msg + 11);
}

inline char side(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[19]);
}

inline uint32_t shares(const uint8_t* msg) noexcept {
    return read_be32(msg + 20);
}

inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 24), 8};
}

/// Stock symbol with trailing spaces removed.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

inline uint32_t price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 32);
}

/// Price in dollars (ITCH prices have 4 implied decimal places).
inline double price(const uint8_t* msg) noexcept {
    return price_raw(msg) / 10000.0;
}

} // namespace add_order

// ---- AddOrderMPID ('F') --------------------------------------------------
// Same as AddOrder but with 4-byte MPID attribution appended.
// Layout: type(1) locate(2) tracking(2) timestamp(6)
//         order_ref(8) side(1) shares(4) stock(8) price(4) attribution(4)
// Total: 39
namespace add_order_mpid {

inline uint64_t order_ref(const uint8_t* msg) noexcept {
    return read_be64(msg + 11);
}

inline char side(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[19]);
}

inline uint32_t shares(const uint8_t* msg) noexcept {
    return read_be32(msg + 20);
}

inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 24), 8};
}

/// Stock symbol with trailing spaces removed.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

inline uint32_t price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 32);
}

inline double price(const uint8_t* msg) noexcept {
    return price_raw(msg) / 10000.0;
}

/// 4-character MPID attribution (right-padded with spaces).
inline std::string_view attribution(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 36), 4};
}

/// MPID attribution with trailing spaces removed.
inline std::string_view attribution_trimmed(const uint8_t* msg) noexcept {
    return trim(attribution(msg));
}

} // namespace add_order_mpid

// ---- OrderExecuted ('E') -------------------------------------------------
// Layout: type(1) locate(2) tracking(2) timestamp(6)
//         order_ref(8) executed_shares(4) match_number(8)
// Total: 30
namespace order_executed {

inline uint64_t order_ref(const uint8_t* msg) noexcept {
    return read_be64(msg + 11);
}

inline uint32_t executed_shares(const uint8_t* msg) noexcept {
    return read_be32(msg + 19);
}

inline uint64_t match_number(const uint8_t* msg) noexcept {
    return read_be64(msg + 23);
}

} // namespace order_executed

// ---- OrderExecutedWithPrice ('C') ----------------------------------------
// Layout: type(1) locate(2) tracking(2) timestamp(6)
//         order_ref(8) executed_shares(4) match_number(8)
//         printable(1) execution_price(4)
// Total: 35
namespace order_executed_price {

inline uint64_t order_ref(const uint8_t* msg) noexcept {
    return read_be64(msg + 11);
}

inline uint32_t executed_shares(const uint8_t* msg) noexcept {
    return read_be32(msg + 19);
}

inline uint64_t match_number(const uint8_t* msg) noexcept {
    return read_be64(msg + 23);
}

/// 'Y' = printable, 'N' = non-printable
inline char printable(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[31]);
}

inline uint32_t execution_price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 32);
}

inline double execution_price(const uint8_t* msg) noexcept {
    return execution_price_raw(msg) / 10000.0;
}

} // namespace order_executed_price

// ---- OrderCancel ('X') ---------------------------------------------------
// Layout: type(1) locate(2) tracking(2) timestamp(6)
//         order_ref(8) cancelled_shares(4)
// Total: 22
namespace order_cancel {

inline uint64_t order_ref(const uint8_t* msg) noexcept {
    return read_be64(msg + 11);
}

inline uint32_t cancelled_shares(const uint8_t* msg) noexcept {
    return read_be32(msg + 19);
}

} // namespace order_cancel

// ---- OrderDelete ('D') ---------------------------------------------------
// Layout: type(1) locate(2) tracking(2) timestamp(6) order_ref(8)
// Total: 18
namespace order_delete {

inline uint64_t order_ref(const uint8_t* msg) noexcept {
    return read_be64(msg + 11);
}

} // namespace order_delete

// ---- OrderReplace ('U') --------------------------------------------------
// Layout: type(1) locate(2) tracking(2) timestamp(6)
//         original_order_ref(8) new_order_ref(8) shares(4) price(4)
// Total: 34
namespace order_replace {

inline uint64_t original_order_ref(const uint8_t* msg) noexcept {
    return read_be64(msg + 11);
}

inline uint64_t new_order_ref(const uint8_t* msg) noexcept {
    return read_be64(msg + 19);
}

inline uint32_t shares(const uint8_t* msg) noexcept {
    return read_be32(msg + 27);
}

inline uint32_t price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 31);
}

inline double price(const uint8_t* msg) noexcept {
    return price_raw(msg) / 10000.0;
}

} // namespace order_replace

// ---- NonCrossTrade ('P') -------------------------------------------------
// Layout: type(1) locate(2) tracking(2) timestamp(6)
//         order_ref(8) side(1) shares(4) stock(8) price(4) match_number(8)
// Total: 43
namespace non_cross_trade {

inline uint64_t order_ref(const uint8_t* msg) noexcept {
    return read_be64(msg + 11);
}

inline char side(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[19]);
}

inline uint32_t shares(const uint8_t* msg) noexcept {
    return read_be32(msg + 20);
}

inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 24), 8};
}

/// Stock symbol with trailing spaces removed.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

inline uint32_t price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 32);
}

inline double price(const uint8_t* msg) noexcept {
    return price_raw(msg) / 10000.0;
}

inline uint64_t match_number(const uint8_t* msg) noexcept {
    return read_be64(msg + 36);
}

} // namespace non_cross_trade

// ---- CrossTrade ('Q') ----------------------------------------------------
// Layout: type(1) locate(2) tracking(2) timestamp(6)
//         shares(8) stock(8) cross_price(4) match_number(8) cross_type(1)
// Total: 39
namespace cross_trade {

inline uint64_t shares(const uint8_t* msg) noexcept {
    return read_be64(msg + 11);
}

inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 19), 8};
}

/// Stock symbol with trailing spaces removed.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

inline uint32_t cross_price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 27);
}

inline double cross_price(const uint8_t* msg) noexcept {
    return cross_price_raw(msg) / 10000.0;
}

inline uint64_t match_number(const uint8_t* msg) noexcept {
    return read_be64(msg + 31);
}

/// Cross type: 'O'=opening, 'C'=closing, 'H'=halted/IPO,
///             'I'=intraday/post-close
inline char cross_type(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[38]);
}

} // namespace cross_trade

// ---- StockTradingAction ('H') --------------------------------------------
// Layout: type(1) locate(2) tracking(2) timestamp(6)
//         stock(8) trading_state(1) reserved(1) reason(4)
// Body size: 24
namespace stock_trading_action {

/// Stock symbol, right-padded with spaces (8 bytes at offset 11).
inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 11), 8};
}

/// Stock symbol with trailing spaces removed.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

/// Trading state: 'H'=halted, 'P'=paused, 'Q'=quotation-only, 'T'=trading.
inline char trading_state(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[19]);
}

/// Reserved (1 byte at offset 20).
inline char reserved(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[20]);
}

/// Reason for the trading action (4 bytes at offset 21).
inline std::string_view reason(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 21), 4};
}

} // namespace stock_trading_action

// ---- RegSHORestriction ('Y') ---------------------------------------------
// Layout: type(1) locate(2) tracking(2) timestamp(6)
//         stock(8) reg_sho_action(1)
// Body size: 19
namespace reg_sho_restriction {

/// Stock symbol, right-padded with spaces (8 bytes at offset 11).
inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 11), 8};
}

/// Stock symbol with trailing spaces removed.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

/// Reg SHO action: '0'=no restriction, '1'=short sale restriction activated,
///                 '2'=short sale restriction continued.
inline char reg_sho_action(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[19]);
}

} // namespace reg_sho_restriction

// ---- MarketParticipantPosition ('L') -------------------------------------
// Layout: type(1) locate(2) tracking(2) timestamp(6)
//         mpid(4) stock(8) primary_market_maker(1) market_maker_mode(1)
//         market_participant_state(1)
// Body size: 25
namespace market_participant_position {

/// Market participant identifier (4 bytes at offset 11).
inline std::string_view mpid(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 11), 4};
}

/// Stock symbol, right-padded with spaces (8 bytes at offset 15).
inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 15), 8};
}

/// Stock symbol with trailing spaces removed.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

/// Primary market maker: 'Y' or 'N'.
inline char primary_market_maker(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[23]);
}

/// Market maker mode: 'N'=normal, 'P'=passive, 'S'=syndicate,
///                    'R'=pre-syndicate, 'L'=penalty.
inline char market_maker_mode(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[24]);
}

/// Market participant state: 'A'=active, 'E'=excused/withdrawn,
///                           'W'=withdrawn, 'S'=suspended, 'D'=deleted.
inline char market_participant_state(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[25]);
}

} // namespace market_participant_position

// ---- MWCBDeclineLevel ('V') ----------------------------------------------
// Layout: type(1) locate(2) tracking(2) timestamp(6)
//         level1(8) level2(8) level3(8)
// Body size: 34
// Prices are in price8 format (8 implied decimal places).
namespace mwcb_decline_level {

/// Level 1 MWCB value (raw, 8 bytes big-endian at offset 11).
inline uint64_t level1_raw(const uint8_t* msg) noexcept {
    return read_be64(msg + 11);
}

/// Level 1 MWCB value in dollars (price8: 8 implied decimal places).
inline double level1(const uint8_t* msg) noexcept {
    return level1_raw(msg) / 100000000.0;
}

/// Level 2 MWCB value (raw, 8 bytes big-endian at offset 19).
inline uint64_t level2_raw(const uint8_t* msg) noexcept {
    return read_be64(msg + 19);
}

/// Level 2 MWCB value in dollars (price8: 8 implied decimal places).
inline double level2(const uint8_t* msg) noexcept {
    return level2_raw(msg) / 100000000.0;
}

/// Level 3 MWCB value (raw, 8 bytes big-endian at offset 27).
inline uint64_t level3_raw(const uint8_t* msg) noexcept {
    return read_be64(msg + 27);
}

/// Level 3 MWCB value in dollars (price8: 8 implied decimal places).
inline double level3(const uint8_t* msg) noexcept {
    return level3_raw(msg) / 100000000.0;
}

} // namespace mwcb_decline_level

// ---- MWCBStatus ('W') ----------------------------------------------------
// Layout: type(1) locate(2) tracking(2) timestamp(6)
//         breached_level(1)
// Body size: 11
namespace mwcb_status {

/// Breached level: '1', '2', or '3'.
inline char breached_level(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[11]);
}

} // namespace mwcb_status

// ---- IPOQuotingPeriod ('K') ----------------------------------------------
// Layout: type(1) locate(2) tracking(2) timestamp(6)
//         stock(8) ipo_quotation_release_time(4)
//         ipo_quotation_release_qualifier(1) ipo_price(4)
// Body size: 27
namespace ipo_quoting_period {

/// Stock symbol, right-padded with spaces (8 bytes at offset 11).
inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 11), 8};
}

/// Stock symbol with trailing spaces removed.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

/// IPO quotation release time as seconds after midnight (4 bytes BE at offset 19).
inline uint32_t ipo_quotation_release_time(const uint8_t* msg) noexcept {
    return read_be32(msg + 19);
}

/// IPO quotation release qualifier: 'A'=anticipated, 'C'=cancelled.
inline char ipo_quotation_release_qualifier(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[23]);
}

/// IPO price (raw, 4 bytes BE at offset 24, price4 format).
inline uint32_t ipo_price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 24);
}

/// IPO price in dollars (4 implied decimal places).
inline double ipo_price(const uint8_t* msg) noexcept {
    return ipo_price_raw(msg) / 10000.0;
}

} // namespace ipo_quoting_period

// ---- LULDAuctionCollar ('J') ---------------------------------------------
// Layout: type(1) locate(2) tracking(2) timestamp(6)
//         stock(8) auction_collar_reference_price(4)
//         upper_auction_collar_price(4) lower_auction_collar_price(4)
//         auction_collar_extension(4)
// Body size: 34
namespace luld_auction_collar {

/// Stock symbol, right-padded with spaces (8 bytes at offset 11).
inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 11), 8};
}

/// Stock symbol with trailing spaces removed.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

/// Auction collar reference price (raw, 4 bytes BE at offset 19).
inline uint32_t auction_collar_reference_price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 19);
}

/// Auction collar reference price in dollars (4 implied decimal places).
inline double auction_collar_reference_price(const uint8_t* msg) noexcept {
    return auction_collar_reference_price_raw(msg) / 10000.0;
}

/// Upper auction collar price (raw, 4 bytes BE at offset 23).
inline uint32_t upper_auction_collar_price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 23);
}

/// Upper auction collar price in dollars (4 implied decimal places).
inline double upper_auction_collar_price(const uint8_t* msg) noexcept {
    return upper_auction_collar_price_raw(msg) / 10000.0;
}

/// Lower auction collar price (raw, 4 bytes BE at offset 27).
inline uint32_t lower_auction_collar_price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 27);
}

/// Lower auction collar price in dollars (4 implied decimal places).
inline double lower_auction_collar_price(const uint8_t* msg) noexcept {
    return lower_auction_collar_price_raw(msg) / 10000.0;
}

/// Auction collar extension (4 bytes BE at offset 31).
inline uint32_t auction_collar_extension(const uint8_t* msg) noexcept {
    return read_be32(msg + 31);
}

} // namespace luld_auction_collar

// ---- OperationalHalt ('h') -----------------------------------------------
// Layout: type(1) locate(2) tracking(2) timestamp(6)
//         stock(8) market_code(1) operational_halt_action(1)
// Body size: 20
namespace operational_halt {

/// Stock symbol, right-padded with spaces (8 bytes at offset 11).
inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 11), 8};
}

/// Stock symbol with trailing spaces removed.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

/// Market code: 'Q'=NASDAQ, 'B'=BX, 'X'=PSX.
inline char market_code(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[19]);
}

/// Operational halt action: 'H'=halted, 'T'=resumed.
inline char operational_halt_action(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[20]);
}

} // namespace operational_halt

// ---- BrokenTrade ('B') ---------------------------------------------------
// Layout: type(1) locate(2) tracking(2) timestamp(6) match_number(8)
// Body size: 18
namespace broken_trade {

/// Match number of the broken trade (8 bytes BE at offset 11).
inline uint64_t match_number(const uint8_t* msg) noexcept {
    return read_be64(msg + 11);
}

} // namespace broken_trade

// ---- NOII ('I') — Net Order Imbalance Indicator --------------------------
// Layout: type(1) locate(2) tracking(2) timestamp(6)
//         paired_shares(8) imbalance_shares(8) imbalance_direction(1)
//         stock(8) far_price(4) near_price(4) current_reference_price(4)
//         cross_type(1) price_variation_indicator(1)
// Body size: 49
namespace noii {

/// Paired shares (8 bytes BE at offset 11).
inline uint64_t paired_shares(const uint8_t* msg) noexcept {
    return read_be64(msg + 11);
}

/// Imbalance shares (8 bytes BE at offset 19).
inline uint64_t imbalance_shares(const uint8_t* msg) noexcept {
    return read_be64(msg + 19);
}

/// Imbalance direction: 'B'=buy, 'S'=sell, 'N'=no imbalance, 'O'=insufficient orders.
inline char imbalance_direction(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[27]);
}

/// Stock symbol, right-padded with spaces (8 bytes at offset 28).
inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 28), 8};
}

/// Stock symbol with trailing spaces removed.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

/// Far price (raw, 4 bytes BE at offset 36).
inline uint32_t far_price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 36);
}

/// Far price in dollars (4 implied decimal places).
inline double far_price(const uint8_t* msg) noexcept {
    return far_price_raw(msg) / 10000.0;
}

/// Near price (raw, 4 bytes BE at offset 40).
inline uint32_t near_price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 40);
}

/// Near price in dollars (4 implied decimal places).
inline double near_price(const uint8_t* msg) noexcept {
    return near_price_raw(msg) / 10000.0;
}

/// Current reference price (raw, 4 bytes BE at offset 44).
inline uint32_t current_reference_price_raw(const uint8_t* msg) noexcept {
    return read_be32(msg + 44);
}

/// Current reference price in dollars (4 implied decimal places).
inline double current_reference_price(const uint8_t* msg) noexcept {
    return current_reference_price_raw(msg) / 10000.0;
}

/// Cross type: 'O'=opening, 'C'=closing, 'H'=halted/IPO.
inline char cross_type(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[48]);
}

/// Price variation indicator (1 byte at offset 49).
inline char price_variation_indicator(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[49]);
}

} // namespace noii

// ---- RPII ('N') — Retail Price Improvement Indicator ---------------------
// Layout: type(1) locate(2) tracking(2) timestamp(6)
//         stock(8) interest_flag(1)
// Body size: 19
namespace rpii {

/// Stock symbol, right-padded with spaces (8 bytes at offset 11).
inline std::string_view stock(const uint8_t* msg) noexcept {
    return {reinterpret_cast<const char*>(msg + 11), 8};
}

/// Stock symbol with trailing spaces removed.
inline std::string_view stock_trimmed(const uint8_t* msg) noexcept {
    return trim(stock(msg));
}

/// Interest flag: 'B'=buy-side, 'S'=sell-side, 'A'=both, 'N'=none.
inline char interest_flag(const uint8_t* msg) noexcept {
    return static_cast<char>(msg[19]);
}

} // namespace rpii

} // namespace eph::itch
