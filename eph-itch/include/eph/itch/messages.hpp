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
inline constexpr size_t  kNOIISize = 49;

// Retail Price Improvement Indicator ('N')
inline constexpr uint8_t kRPII     = 'N';
inline constexpr size_t  kRPIISize = 19;

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

} // namespace eph::itch
