#pragma once

/// @file ouch.hpp
/// Nasdaq OUCH 5.0 order entry protocol — zero-copy views and builders.
///
/// OUCH is a binary protocol used over SoupBinTCP for Nasdaq order entry.
/// This header implements the 4 most critical message types:
///
/// Inbound (client -> Nasdaq):
///   EnterOrder  ('O', 49 bytes)  — enter a new order
///   ReplaceOrder('U', 47 bytes)  — replace an existing order
///   CancelOrder ('X', 19 bytes)  — cancel an existing order
///
/// Outbound (Nasdaq -> client):
///   OrderAccepted ('A', 66 bytes) — confirmation of new order
///   OrderExecuted ('E', 40 bytes) — execution report
///   OrderCanceled ('C', 28 bytes) — cancel confirmation
///   OrderReplaced ('U', 80 bytes) — replace confirmation
///
/// Wire format: all multi-byte integers are big-endian, tokens are 14-char
/// alphanumeric (right-padded with spaces), symbols are 8-char (right-padded
/// with spaces), prices are uint32_t (price x 10000).
///
/// Reference:
///   https://www.nasdaqtrader.com/content/technicalsupport/specifications/TradingProducts/OUCH5.0.pdf

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "eph/core/log.hpp"

#include "eph/itch/messages.hpp"  // reuse read_be16/32/64, trim

namespace eph::itch::ouch {

/// @brief Internal detail namespace for OUCH module logger.
namespace detail {
/// @brief Get or create the OUCH module logger.
/// @return Raw pointer to the spdlog logger instance (never null after first call).
inline spdlog::logger* ouch_logger() {
    static spdlog::logger* l = ::eph::log::get("itch.ouch");
    return l;
}
} // namespace detail

// -------------------------------------------------------------------------
// Wire encoding helpers (big-endian writers)
// -------------------------------------------------------------------------

/// @brief Write a uint16_t in big-endian format to an arbitrary byte pointer.
/// @param p Pointer to at least 2 bytes of output buffer.
/// @param v Host-endian value to write.
inline void write_be16(uint8_t* p, uint16_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little)
        v = std::byteswap(v);
    std::memcpy(p, &v, 2);
}

/// @brief Write a uint32_t in big-endian format to an arbitrary byte pointer.
/// @param p Pointer to at least 4 bytes of output buffer.
/// @param v Host-endian value to write.
inline void write_be32(uint8_t* p, uint32_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little)
        v = std::byteswap(v);
    std::memcpy(p, &v, 4);
}

/// @brief Write a uint64_t in big-endian format to an arbitrary byte pointer.
/// @param p Pointer to at least 8 bytes of output buffer.
/// @param v Host-endian value to write.
inline void write_be64(uint8_t* p, uint64_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little)
        v = std::byteswap(v);
    std::memcpy(p, &v, 8);
}

/// @brief Write a right-padded string field of exactly @p width bytes.
///
/// Copies up to @p width bytes from @p s, then pads the remainder with spaces.
///
/// @param p     Pointer to output buffer with at least @p width bytes available.
/// @param s     Source string (truncated if longer than @p width).
/// @param width Exact number of bytes to write.
inline void write_padded(uint8_t* p, std::string_view s, size_t width) noexcept {
    const size_t n = std::min(s.size(), width);
    // Guard against `std::memcpy(p, nullptr, 0)`: ISO C requires both
    // src and dst to be valid pointers even when count==0 (UBSan
    // -fsanitize=undefined catches this). std::string_view::data() may
    // legitimately return nullptr for an empty view, so a default-
    // constructed std::string_view{} feeding into write_padded would
    // hit the UB. Mirrors the same defensive pattern in
    // eph-utils/cpu.hpp set_thread_name and eph-net/hmac.hpp.
    if (n > 0) std::memcpy(p, s.data(), n);
    if (n < width) std::memset(p + n, ' ', width - n);
}

// -------------------------------------------------------------------------
// Message type constants
// -------------------------------------------------------------------------

/// @brief OUCH 5.0 message type byte constants.
namespace msg_type {
inline constexpr uint8_t kEnterOrder   = 'O';  ///< Client -> Nasdaq: enter a new order
inline constexpr uint8_t kReplaceOrder = 'U';  ///< Client -> Nasdaq: replace an existing order
inline constexpr uint8_t kCancelOrder  = 'X';  ///< Client -> Nasdaq: cancel an existing order
inline constexpr uint8_t kAccepted     = 'A';  ///< Nasdaq -> Client: order accepted
inline constexpr uint8_t kExecuted     = 'E';  ///< Nasdaq -> Client: execution report
inline constexpr uint8_t kCanceled     = 'C';  ///< Nasdaq -> Client: cancel confirmation
inline constexpr uint8_t kReplaced     = 'U';  ///< Nasdaq -> Client: replace confirmation (same wire code as ReplaceOrder)
}  // namespace msg_type

// =========================================================================
// Inbound message builders (client -> Nasdaq)
// =========================================================================

// -------------------------------------------------------------------------
// EnterOrder ('O')
// -------------------------------------------------------------------------
/// @brief Builder for OUCH EnterOrder ('O') inbound messages.
///
/// Layout (49 bytes):
///   type(1) token(14) side(1) shares(4) symbol(8) price(4) tif(4)
///   firm(4) display(1) capacity(1) int_mkt_sweep(1) cross_type(1)
///   cl_type(1) = 45... + reserved(4) = 49.
///
/// Simplified builder -- sets display='Y', capacity='O' (agency),
/// int_mkt_sweep='N', cross_type='N', cl_type=' ', reserved to spaces.
struct EnterOrder {
    static constexpr size_t kSize = 49;

    /// Build an EnterOrder message into `buf`.
    /// @param buf    Output buffer, must have at least kSize bytes
    /// @param token  14-char order token (right-padded with spaces)
    /// @param side   'B' (buy) or 'S' (sell)
    /// @param shares Number of shares
    /// @param symbol 8-char stock symbol (right-padded with spaces)
    /// @param price  Price x 10000 (e.g. $100.00 = 1000000)
    /// @param time_in_force  Seconds until expiry, 0 = Day order
    /// @param firm   4-char MPID
    /// @return kSize on success, 0 on failure
    static size_t build(uint8_t* buf,
                        std::string_view token,
                        char side,
                        uint32_t shares,
                        std::string_view symbol,
                        uint32_t price,
                        uint32_t time_in_force,
                        std::string_view firm) noexcept {
        // Caller must ensure buf has at least kSize (49) bytes available
        if (!buf) [[unlikely]] {
            EPH_LOG_DEBUG(detail::ouch_logger(),"EnterOrder::build: null buffer");
            return 0;
        }
        if (side != 'B' && side != 'S') [[unlikely]] {
            EPH_LOG_WARN(detail::ouch_logger(),"EnterOrder::build: invalid side='{}', expected 'B' or 'S'", side);
            return 0;
        }
        // Reject oversize text fields up-front. write_padded would silently
        // truncate, which is catastrophic in HFT order entry: two distinct
        // 15-char tokens that share a 14-char prefix would collide on the
        // wire, mis-routing later cancel/replace messages. Same applies to
        // symbol (8) and firm (4) — wrong symbol = wrong instrument routed.
        if (token.size() > 14) [[unlikely]] {
            EPH_LOG_WARN(detail::ouch_logger(),"EnterOrder::build: token too long ({} > 14)", token.size());
            return 0;
        }
        if (symbol.size() > 8) [[unlikely]] {
            EPH_LOG_WARN(detail::ouch_logger(),"EnterOrder::build: symbol too long ({} > 8)", symbol.size());
            return 0;
        }
        if (firm.size() > 4) [[unlikely]] {
            EPH_LOG_WARN(detail::ouch_logger(),"EnterOrder::build: firm too long ({} > 4)", firm.size());
            return 0;
        }

        std::memset(buf, ' ', kSize);

        size_t off = 0;
        buf[off] = msg_type::kEnterOrder;                 off += 1;   // 0
        write_padded(buf + off, token, 14);               off += 14;  // 1..14
        buf[off] = static_cast<uint8_t>(side);            off += 1;   // 15
        write_be32(buf + off, shares);                    off += 4;   // 16..19
        write_padded(buf + off, symbol, 8);               off += 8;   // 20..27
        write_be32(buf + off, price);                     off += 4;   // 28..31
        write_be32(buf + off, time_in_force);             off += 4;   // 32..35
        write_padded(buf + off, firm, 4);                 off += 4;   // 36..39
        buf[off] = 'Y';                                   off += 1;   // 40  display
        buf[off] = 'O';                                   off += 1;   // 41  capacity (agency)
        buf[off] = 'N';                                   off += 1;   // 42  intermarket sweep
        buf[off] = 'N';                                   off += 1;   // 43  cross type
        buf[off] = ' ';                                   off += 1;   // 44  customer type
        // 45..48: reserved (4 bytes, already set to spaces)

        EPH_LOG_DEBUG(detail::ouch_logger(),"EnterOrder::build: token='{}' side={} shares={} symbol='{}' "
                     "price={} tif={}",
                     token, side, shares, symbol, price, time_in_force);
        return kSize;
    }
};

// -------------------------------------------------------------------------
// ReplaceOrder ('U')
// -------------------------------------------------------------------------
/// @brief Builder for OUCH ReplaceOrder ('U') inbound messages.
///
/// Layout (47 bytes):
///   type(1) existing_token(14) replacement_token(14) shares(4) price(4)
///   tif(4) display(1) int_mkt_sweep(1) cl_type(1) = 44 + reserved(3) = 47.
struct ReplaceOrder {
    static constexpr size_t kSize = 47;

    /// Build a ReplaceOrder message.
    /// @param buf               Output buffer, must have at least kSize bytes
    /// @param existing_token    Token of the order to replace
    /// @param replacement_token Token for the replacement order
    /// @param shares            New share quantity
    /// @param price             New price x 10000
    /// @param time_in_force     Seconds until expiry, 0 = Day
    /// @return kSize on success, 0 on failure
    static size_t build(uint8_t* buf,
                        std::string_view existing_token,
                        std::string_view replacement_token,
                        uint32_t shares,
                        uint32_t price,
                        uint32_t time_in_force) noexcept {
        // Caller must ensure buf has at least kSize (47) bytes available
        if (!buf) [[unlikely]] {
            EPH_LOG_DEBUG(detail::ouch_logger(),"ReplaceOrder::build: null buffer");
            return 0;
        }
        // Reject oversize tokens — silent truncation would route the replace
        // to the wrong existing order or stamp the wrong replacement_token.
        if (existing_token.size() > 14) [[unlikely]] {
            EPH_LOG_WARN(detail::ouch_logger(),"ReplaceOrder::build: existing_token too long ({} > 14)", existing_token.size());
            return 0;
        }
        if (replacement_token.size() > 14) [[unlikely]] {
            EPH_LOG_WARN(detail::ouch_logger(),"ReplaceOrder::build: replacement_token too long ({} > 14)", replacement_token.size());
            return 0;
        }

        std::memset(buf, ' ', kSize);

        size_t off = 0;
        buf[off] = msg_type::kReplaceOrder;                   off += 1;   // 0
        write_padded(buf + off, existing_token, 14);           off += 14;  // 1..14
        write_padded(buf + off, replacement_token, 14);        off += 14;  // 15..28
        write_be32(buf + off, shares);                         off += 4;   // 29..32
        write_be32(buf + off, price);                          off += 4;   // 33..36
        write_be32(buf + off, time_in_force);                  off += 4;   // 37..40
        buf[off] = 'Y';                                        off += 1;   // 41  display
        buf[off] = 'N';                                        off += 1;   // 42  intermarket sweep
        buf[off] = ' ';                                        off += 1;   // 43  customer type
        // 44..46: reserved (3 bytes, already set to spaces)

        EPH_LOG_DEBUG(detail::ouch_logger(),"ReplaceOrder::build: existing='{}' replacement='{}' "
                     "shares={} price={} tif={}",
                     existing_token, replacement_token, shares, price,
                     time_in_force);
        return kSize;
    }
};

// -------------------------------------------------------------------------
// CancelOrder ('X')
// -------------------------------------------------------------------------
/// @brief Builder for OUCH CancelOrder ('X') inbound messages.
///
/// Layout (19 bytes):
///   type(1) token(14) shares(4).
struct CancelOrder {
    static constexpr size_t kSize = 19;

    /// Build a CancelOrder message.
    /// @param buf    Output buffer, must have at least kSize bytes
    /// @param token  14-char order token
    /// @param shares Shares to cancel (0 = cancel entire remaining quantity)
    /// @return kSize on success, 0 on failure
    static size_t build(uint8_t* buf,
                        std::string_view token,
                        uint32_t shares) noexcept {
        // Caller must ensure buf has at least kSize (19) bytes available
        if (!buf) [[unlikely]] {
            EPH_LOG_DEBUG(detail::ouch_logger(),"CancelOrder::build: null buffer");
            return 0;
        }
        // Reject oversize token — silent truncation would target the wrong
        // existing order. CancelOrder doesn't memset so we still need to
        // pad explicitly via write_padded after the bound check.
        if (token.size() > 14) [[unlikely]] {
            EPH_LOG_WARN(detail::ouch_logger(),"CancelOrder::build: token too long ({} > 14)", token.size());
            return 0;
        }

        size_t off = 0;
        buf[off] = msg_type::kCancelOrder;       off += 1;   // 0
        write_padded(buf + off, token, 14);      off += 14;  // 1..14
        write_be32(buf + off, shares);           off += 4;   // 15..18

        EPH_LOG_DEBUG(detail::ouch_logger(),"CancelOrder::build: token='{}' shares={}", token, shares);
        return kSize;
    }
};

// =========================================================================
// Outbound message views (Nasdaq -> client), zero-copy
// =========================================================================

// -------------------------------------------------------------------------
// AcceptedView ('A', 66 bytes)
// -------------------------------------------------------------------------
/// @brief Zero-copy view over an OUCH OrderAccepted ('A') outbound message (66 bytes).
///
/// Layout:
///   type(1) timestamp(8) token(14) side(1) shares(4) symbol(8) price(4)
///   tif(4) firm(4) display(1) order_ref(8) capacity(1) int_mkt_sweep(1)
///   cross_type(1) order_state(1) bbo_weight(1) = 62 + reserved(4) = 66.
///
/// @warning All accessors require valid() == true. Calling on an invalid
///          view is undefined behavior. Always check valid() first.
class AcceptedView {
public:
    /// @brief Minimum wire size for this message.
    static constexpr size_t kSize = 66;

    /// @brief Construct a view over raw message bytes.
    /// @param data Pointer to the start of the OUCH message.
    /// @param len  Number of available bytes starting at @p data.
    /// @note If @p len < kSize, the view is invalid (valid() returns false).
    explicit AcceptedView(const uint8_t* data, size_t len) noexcept
        : data_(len >= kSize ? data : nullptr) {
        if (!data_ && data) [[unlikely]] {
            EPH_LOG_WARN(detail::ouch_logger(),"AcceptedView: buffer too small, need {} but got {}", kSize, len);
        }
    }

    /// @brief Check whether this view points to a valid (sufficiently large) buffer.
    /// @return True if the underlying buffer has at least kSize bytes.
    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }

    /// @brief Message type byte ('A' for OrderAccepted).
    [[nodiscard]] uint8_t  msg_type()  const noexcept { assert(valid() && "must check valid() before accessing fields"); return data_[0]; }
    /// @brief Nanosecond timestamp from the matching engine (8 bytes BE).
    [[nodiscard]] uint64_t timestamp() const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be64(data_ + 1); }

    /// @brief 14-character order token (right-padded with spaces).
    [[nodiscard]] std::string_view token() const noexcept {
        assert(valid() && "must check valid() before accessing fields");
        return {reinterpret_cast<const char*>(data_ + 9), 14};
    }

    /// @brief Buy/sell indicator: 'B'=buy, 'S'=sell.
    [[nodiscard]] char     side()   const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[23]); }
    /// @brief Number of shares in the accepted order.
    [[nodiscard]] uint32_t shares() const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be32(data_ + 24); }

    /// @brief 8-character stock symbol (right-padded with spaces).
    [[nodiscard]] std::string_view symbol() const noexcept {
        assert(valid() && "must check valid() before accessing fields");
        return {reinterpret_cast<const char*>(data_ + 28), 8};
    }

    /// @brief Price x 10000 (divide by 10000 for dollars).
    [[nodiscard]] uint32_t price()         const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be32(data_ + 36); }
    /// @brief Time-in-force in seconds (0 = Day order).
    [[nodiscard]] uint32_t time_in_force() const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be32(data_ + 40); }

    /// @brief 4-character MPID of the entering firm.
    [[nodiscard]] std::string_view firm() const noexcept {
        assert(valid() && "must check valid() before accessing fields");
        return {reinterpret_cast<const char*>(data_ + 44), 4};
    }

    /// @brief Display attribute: 'Y'=displayed, 'N'=non-displayed, etc.
    [[nodiscard]] char     display()        const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[48]); }
    /// @brief Nasdaq-assigned order reference number (8 bytes BE).
    [[nodiscard]] uint64_t order_reference() const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be64(data_ + 49); }
    /// @brief Capacity: 'O'=agency, 'P'=principal, 'R'=riskless principal.
    [[nodiscard]] char     capacity()       const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[57]); }
    /// @brief Intermarket sweep eligibility: 'Y'=eligible, 'N'=not eligible.
    ///
    /// Operationally consequential — risk / surveillance teams need to know
    /// whether the accepted order was tagged as an ISO so reg-NMS protections
    /// can be reconciled later. ReplacedView already exposed this getter; the
    /// gap on AcceptedView was a copy-paste omission.
    [[nodiscard]] char     int_mkt_sweep()  const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[58]); }
    /// @brief Cross type: 'N'=continuous, 'O'=opening, 'C'=closing, 'H'=halted.
    [[nodiscard]] char     cross_type()     const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[59]); }
    /// @brief Order state: 'L'=live, 'D'=dead.
    [[nodiscard]] char     order_state()    const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[60]); }
    /// @brief BBO weight indicator (1 byte at offset 61).
    ///
    /// Nasdaq sets this to communicate how the order's price relates to the
    /// NBBO at acceptance time — useful for execution-quality analytics.
    [[nodiscard]] char     bbo_weight()     const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[61]); }

private:
    const uint8_t* data_ = nullptr;
};

// -------------------------------------------------------------------------
// ExecutedView ('E', 40 bytes)
// -------------------------------------------------------------------------
/// @brief Zero-copy view over an OUCH OrderExecuted ('E') outbound message (40 bytes).
///
/// Layout:
///   type(1) timestamp(8) token(14) executed_shares(4) execution_price(4)
///   liquidity_flag(1) match_number(8) = 40.
///
/// @warning All accessors require valid() == true. Calling on an invalid
///          view is undefined behavior. Always check valid() first.
class ExecutedView {
public:
    /// @brief Minimum wire size for this message.
    static constexpr size_t kSize = 40;

    /// @brief Construct a view over raw message bytes.
    /// @param data Pointer to the start of the OUCH message.
    /// @param len  Number of available bytes starting at @p data.
    /// @note If @p len < kSize, the view is invalid (valid() returns false).
    explicit ExecutedView(const uint8_t* data, size_t len) noexcept
        : data_(len >= kSize ? data : nullptr) {
        if (!data_ && data) [[unlikely]] {
            EPH_LOG_WARN(detail::ouch_logger(),"ExecutedView: buffer too small, need {} but got {}", kSize, len);
        }
    }

    /// @brief Check whether this view points to a valid (sufficiently large) buffer.
    /// @return True if the underlying buffer has at least kSize bytes.
    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }

    /// @brief Message type byte ('E' for OrderExecuted).
    [[nodiscard]] uint8_t  msg_type()  const noexcept { assert(valid() && "must check valid() before accessing fields"); return data_[0]; }
    /// @brief Nanosecond timestamp from the matching engine (8 bytes BE).
    [[nodiscard]] uint64_t timestamp() const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be64(data_ + 1); }

    /// @brief 14-character order token (right-padded with spaces).
    [[nodiscard]] std::string_view token() const noexcept {
        assert(valid() && "must check valid() before accessing fields");
        return {reinterpret_cast<const char*>(data_ + 9), 14};
    }

    /// @brief Number of shares executed in this fill.
    [[nodiscard]] uint32_t executed_shares()  const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be32(data_ + 23); }
    /// @brief Execution price x 10000 (divide by 10000 for dollars).
    [[nodiscard]] uint32_t execution_price()  const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be32(data_ + 27); }
    /// @brief Liquidity flag indicating how the execution was matched.
    [[nodiscard]] char     liquidity_flag()   const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[31]); }
    /// @brief Unique day-level match number for trade reconciliation.
    [[nodiscard]] uint64_t match_number()     const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be64(data_ + 32); }

private:
    const uint8_t* data_ = nullptr;
};

// -------------------------------------------------------------------------
// CanceledView ('C', 28 bytes)
// -------------------------------------------------------------------------
/// @brief Zero-copy view over an OUCH OrderCanceled ('C') outbound message (28 bytes).
///
/// Layout:
///   type(1) timestamp(8) token(14) decrement_shares(4) reason(1) = 28.
///
/// @warning All accessors require valid() == true. Calling on an invalid
///          view is undefined behavior. Always check valid() first.
class CanceledView {
public:
    /// @brief Minimum wire size for this message.
    static constexpr size_t kSize = 28;

    /// @brief Construct a view over raw message bytes.
    /// @param data Pointer to the start of the OUCH message.
    /// @param len  Number of available bytes starting at @p data.
    /// @note If @p len < kSize, the view is invalid (valid() returns false).
    explicit CanceledView(const uint8_t* data, size_t len) noexcept
        : data_(len >= kSize ? data : nullptr) {
        if (!data_ && data) [[unlikely]] {
            EPH_LOG_WARN(detail::ouch_logger(),"CanceledView: buffer too small, need {} but got {}", kSize, len);
        }
    }

    /// @brief Check whether this view points to a valid (sufficiently large) buffer.
    /// @return True if the underlying buffer has at least kSize bytes.
    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }

    /// @brief Message type byte ('C' for OrderCanceled).
    [[nodiscard]] uint8_t  msg_type()  const noexcept { assert(valid() && "must check valid() before accessing fields"); return data_[0]; }
    /// @brief Nanosecond timestamp from the matching engine (8 bytes BE).
    [[nodiscard]] uint64_t timestamp() const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be64(data_ + 1); }

    /// @brief 14-character order token (right-padded with spaces).
    [[nodiscard]] std::string_view token() const noexcept {
        assert(valid() && "must check valid() before accessing fields");
        return {reinterpret_cast<const char*>(data_ + 9), 14};
    }

    /// @brief Number of shares decremented (cancelled) from the order.
    [[nodiscard]] uint32_t decrement_shares() const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be32(data_ + 23); }
    /// @brief Cancel reason code: 'U'=user requested, 'S'=system, 'T'=timeout, etc.
    [[nodiscard]] char     reason()           const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[27]); }

private:
    const uint8_t* data_ = nullptr;
};

// -------------------------------------------------------------------------
// ReplacedView ('U', 80 bytes)
// -------------------------------------------------------------------------
/// @brief Zero-copy view over an OUCH OrderReplaced ('U') outbound message (80 bytes).
///
/// Layout:
///   type(1) timestamp(8) replacement_token(14) side(1) shares(4) symbol(8)
///   price(4) tif(4) firm(4) display(1) order_ref(8) capacity(1)
///   int_mkt_sweep(1) cross_type(1) order_state(1) previous_token(14)
///   = 75 + reserved(5) = 80.
///
/// @warning All accessors require valid() == true. Calling on an invalid
///          view is undefined behavior. Always check valid() first.
class ReplacedView {
public:
    /// @brief Minimum wire size for this message.
    static constexpr size_t kSize = 80;

    /// @brief Construct a view over raw message bytes.
    /// @param data Pointer to the start of the OUCH message.
    /// @param len  Number of available bytes starting at @p data.
    /// @note If @p len < kSize, the view is invalid (valid() returns false).
    explicit ReplacedView(const uint8_t* data, size_t len) noexcept
        : data_(len >= kSize ? data : nullptr) {
        if (!data_ && data) [[unlikely]] {
            EPH_LOG_WARN(detail::ouch_logger(),"ReplacedView: buffer too small, need {} but got {}", kSize, len);
        }
    }

    /// @brief Check whether this view points to a valid (sufficiently large) buffer.
    /// @return True if the underlying buffer has at least kSize bytes.
    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }

    /// @brief Message type byte ('U' for OrderReplaced).
    [[nodiscard]] uint8_t  msg_type()  const noexcept { assert(valid() && "must check valid() before accessing fields"); return data_[0]; }
    /// @brief Nanosecond timestamp from the matching engine (8 bytes BE).
    [[nodiscard]] uint64_t timestamp() const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be64(data_ + 1); }

    /// @brief 14-character replacement order token (right-padded with spaces).
    [[nodiscard]] std::string_view replacement_token() const noexcept {
        assert(valid() && "must check valid() before accessing fields");
        return {reinterpret_cast<const char*>(data_ + 9), 14};
    }

    /// @brief Buy/sell indicator: 'B'=buy, 'S'=sell.
    [[nodiscard]] char     side()   const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[23]); }
    /// @brief Number of shares in the replacement order.
    [[nodiscard]] uint32_t shares() const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be32(data_ + 24); }

    /// @brief 8-character stock symbol (right-padded with spaces).
    [[nodiscard]] std::string_view symbol() const noexcept {
        assert(valid() && "must check valid() before accessing fields");
        return {reinterpret_cast<const char*>(data_ + 28), 8};
    }

    /// @brief Price x 10000 (divide by 10000 for dollars).
    [[nodiscard]] uint32_t price()          const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be32(data_ + 36); }
    /// @brief Time-in-force in seconds (0 = Day order).
    [[nodiscard]] uint32_t time_in_force()  const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be32(data_ + 40); }

    /// @brief 4-character MPID of the entering firm.
    [[nodiscard]] std::string_view firm() const noexcept {
        assert(valid() && "must check valid() before accessing fields");
        return {reinterpret_cast<const char*>(data_ + 44), 4};
    }

    /// @brief Display attribute: 'Y'=displayed, 'N'=non-displayed, etc.
    [[nodiscard]] char     display()        const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[48]); }
    /// @brief Nasdaq-assigned order reference number (8 bytes BE).
    [[nodiscard]] uint64_t order_reference() const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be64(data_ + 49); }
    /// @brief Capacity: 'O'=agency, 'P'=principal, 'R'=riskless principal.
    [[nodiscard]] char     capacity()       const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[57]); }
    /// @brief Intermarket sweep eligibility: 'Y'=eligible, 'N'=not eligible.
    [[nodiscard]] char     int_mkt_sweep()  const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[58]); }
    /// @brief Cross type: 'N'=continuous, 'O'=opening, 'C'=closing, 'H'=halted.
    [[nodiscard]] char     cross_type()     const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[59]); }
    /// @brief Order state: 'L'=live, 'D'=dead.
    [[nodiscard]] char     order_state()    const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[60]); }

    /// @brief 14-character token of the order that was replaced (right-padded with spaces).
    [[nodiscard]] std::string_view previous_token() const noexcept {
        assert(valid() && "must check valid() before accessing fields");
        return {reinterpret_cast<const char*>(data_ + 61), 14};
    }

private:
    const uint8_t* data_ = nullptr;
};

}  // namespace eph::itch::ouch
