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

#include <spdlog/spdlog.h>

#include "eph/itch/messages.hpp"  // reuse read_be16/32/64, trim

namespace eph::itch::ouch {

// -------------------------------------------------------------------------
// Wire encoding helpers (big-endian writers)
// -------------------------------------------------------------------------

inline void write_be16(uint8_t* p, uint16_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little)
        v = std::byteswap(v);
    std::memcpy(p, &v, 2);
}

inline void write_be32(uint8_t* p, uint32_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little)
        v = std::byteswap(v);
    std::memcpy(p, &v, 4);
}

inline void write_be64(uint8_t* p, uint64_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little)
        v = std::byteswap(v);
    std::memcpy(p, &v, 8);
}

/// Write a right-padded string field of exactly `width` bytes.
inline void write_padded(uint8_t* p, std::string_view s, size_t width) noexcept {
    const size_t n = std::min(s.size(), width);
    std::memcpy(p, s.data(), n);
    if (n < width) std::memset(p + n, ' ', width - n);
}

// -------------------------------------------------------------------------
// Message type constants
// -------------------------------------------------------------------------

namespace msg_type {
inline constexpr uint8_t kEnterOrder   = 'O';
inline constexpr uint8_t kReplaceOrder = 'U';
inline constexpr uint8_t kCancelOrder  = 'X';
inline constexpr uint8_t kAccepted     = 'A';
inline constexpr uint8_t kExecuted     = 'E';
inline constexpr uint8_t kCanceled     = 'C';
inline constexpr uint8_t kReplaced     = 'U';  // same wire code as ReplaceOrder
}  // namespace msg_type

// =========================================================================
// Inbound message builders (client -> Nasdaq)
// =========================================================================

// -------------------------------------------------------------------------
// EnterOrder ('O')
// -------------------------------------------------------------------------
/// Layout (49 bytes):
///   type(1) token(14) side(1) shares(4) symbol(8) price(4) tif(4)
///   firm(4) display(1) capacity(1) int_mkt_sweep(1) cross_type(1)
///   cl_type(1) = 45... + reserved(4) = 49
///
/// Simplified builder — sets display='Y', capacity='O' (agency),
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
        assert(buf != nullptr && "buffer must not be null");
        // Caller must ensure buf has at least kSize (49) bytes available
        if (!buf) [[unlikely]] {
            SPDLOG_DEBUG("EnterOrder::build: null buffer");
            return 0;
        }
        if (side != 'B' && side != 'S') [[unlikely]] {
            SPDLOG_WARN("EnterOrder::build: invalid side='{}', expected 'B' or 'S'", side);
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

        SPDLOG_DEBUG("EnterOrder::build: token='{}' side={} shares={} symbol='{}' "
                     "price={} tif={}",
                     token, side, shares, symbol, price, time_in_force);
        return kSize;
    }
};

// -------------------------------------------------------------------------
// ReplaceOrder ('U')
// -------------------------------------------------------------------------
/// Layout (47 bytes):
///   type(1) existing_token(14) replacement_token(14) shares(4) price(4)
///   tif(4) display(1) int_mkt_sweep(1) cl_type(1) = 44 + reserved(3) = 47
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
        assert(buf != nullptr && "buffer must not be null");
        // Caller must ensure buf has at least kSize (47) bytes available
        if (!buf) [[unlikely]] {
            SPDLOG_DEBUG("ReplaceOrder::build: null buffer");
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

        SPDLOG_DEBUG("ReplaceOrder::build: existing='{}' replacement='{}' "
                     "shares={} price={} tif={}",
                     existing_token, replacement_token, shares, price,
                     time_in_force);
        return kSize;
    }
};

// -------------------------------------------------------------------------
// CancelOrder ('X')
// -------------------------------------------------------------------------
/// Layout (19 bytes):
///   type(1) token(14) shares(4)
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
        assert(buf != nullptr && "buffer must not be null");
        // Caller must ensure buf has at least kSize (19) bytes available
        if (!buf) [[unlikely]] {
            SPDLOG_DEBUG("CancelOrder::build: null buffer");
            return 0;
        }

        size_t off = 0;
        buf[off] = msg_type::kCancelOrder;       off += 1;   // 0
        write_padded(buf + off, token, 14);      off += 14;  // 1..14
        write_be32(buf + off, shares);           off += 4;   // 15..18

        SPDLOG_DEBUG("CancelOrder::build: token='{}' shares={}", token, shares);
        return kSize;
    }
};

// =========================================================================
// Outbound message views (Nasdaq -> client), zero-copy
// =========================================================================

// -------------------------------------------------------------------------
// AcceptedView ('A', 66 bytes)
// -------------------------------------------------------------------------
/// Layout:
///   type(1) timestamp(8) token(14) side(1) shares(4) symbol(8) price(4)
///   tif(4) firm(4) display(1) order_ref(8) capacity(1) int_mkt_sweep(1)
///   cross_type(1) order_state(1) bbo_weight(1) = 62 + reserved(4) = 66
/// @warning All accessors require valid() == true. Calling on an invalid
///          view is undefined behavior. Always check valid() first.
class AcceptedView {
public:
    static constexpr size_t kSize = 66;

    explicit AcceptedView(const uint8_t* data, size_t len) noexcept
        : data_(len >= kSize ? data : nullptr) {
        if (!data_ && data) [[unlikely]] {
            SPDLOG_WARN("AcceptedView: buffer too small, need {} but got {}", kSize, len);
        }
    }

    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }

    // All accessors assert valid() in debug builds to catch misuse early.
    [[nodiscard]] uint8_t  msg_type()  const noexcept { assert(valid() && "must check valid() before accessing fields"); return data_[0]; }
    [[nodiscard]] uint64_t timestamp() const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be64(data_ + 1); }

    [[nodiscard]] std::string_view token() const noexcept {
        assert(valid() && "must check valid() before accessing fields");
        return {reinterpret_cast<const char*>(data_ + 9), 14};
    }

    [[nodiscard]] char     side()   const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[23]); }
    [[nodiscard]] uint32_t shares() const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be32(data_ + 24); }

    [[nodiscard]] std::string_view symbol() const noexcept {
        assert(valid() && "must check valid() before accessing fields");
        return {reinterpret_cast<const char*>(data_ + 28), 8};
    }

    [[nodiscard]] uint32_t price()         const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be32(data_ + 36); }
    [[nodiscard]] uint32_t time_in_force() const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be32(data_ + 40); }

    [[nodiscard]] std::string_view firm() const noexcept {
        assert(valid() && "must check valid() before accessing fields");
        return {reinterpret_cast<const char*>(data_ + 44), 4};
    }

    [[nodiscard]] char     display()        const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[48]); }
    [[nodiscard]] uint64_t order_reference() const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be64(data_ + 49); }
    [[nodiscard]] char     capacity()       const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[57]); }
    [[nodiscard]] char     order_state()    const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[60]); }

private:
    const uint8_t* data_ = nullptr;
};

// -------------------------------------------------------------------------
// ExecutedView ('E', 40 bytes)
// -------------------------------------------------------------------------
/// Layout:
///   type(1) timestamp(8) token(14) executed_shares(4) execution_price(4)
///   liquidity_flag(1) match_number(8) = 40
/// @warning All accessors require valid() == true. Calling on an invalid
///          view is undefined behavior. Always check valid() first.
class ExecutedView {
public:
    static constexpr size_t kSize = 40;

    explicit ExecutedView(const uint8_t* data, size_t len) noexcept
        : data_(len >= kSize ? data : nullptr) {
        if (!data_ && data) [[unlikely]] {
            SPDLOG_WARN("ExecutedView: buffer too small, need {} but got {}", kSize, len);
        }
    }

    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }

    // All accessors assert valid() in debug builds to catch misuse early.
    [[nodiscard]] uint8_t  msg_type()  const noexcept { assert(valid() && "must check valid() before accessing fields"); return data_[0]; }
    [[nodiscard]] uint64_t timestamp() const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be64(data_ + 1); }

    [[nodiscard]] std::string_view token() const noexcept {
        assert(valid() && "must check valid() before accessing fields");
        return {reinterpret_cast<const char*>(data_ + 9), 14};
    }

    [[nodiscard]] uint32_t executed_shares()  const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be32(data_ + 23); }
    [[nodiscard]] uint32_t execution_price()  const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be32(data_ + 27); }
    [[nodiscard]] char     liquidity_flag()   const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[31]); }
    [[nodiscard]] uint64_t match_number()     const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be64(data_ + 32); }

private:
    const uint8_t* data_ = nullptr;
};

// -------------------------------------------------------------------------
// CanceledView ('C', 28 bytes)
// -------------------------------------------------------------------------
/// Layout:
///   type(1) timestamp(8) token(14) decrement_shares(4) reason(1) = 28
/// @warning All accessors require valid() == true. Calling on an invalid
///          view is undefined behavior. Always check valid() first.
class CanceledView {
public:
    static constexpr size_t kSize = 28;

    explicit CanceledView(const uint8_t* data, size_t len) noexcept
        : data_(len >= kSize ? data : nullptr) {
        if (!data_ && data) [[unlikely]] {
            SPDLOG_WARN("CanceledView: buffer too small, need {} but got {}", kSize, len);
        }
    }

    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }

    // All accessors assert valid() in debug builds to catch misuse early.
    [[nodiscard]] uint8_t  msg_type()  const noexcept { assert(valid() && "must check valid() before accessing fields"); return data_[0]; }
    [[nodiscard]] uint64_t timestamp() const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be64(data_ + 1); }

    [[nodiscard]] std::string_view token() const noexcept {
        assert(valid() && "must check valid() before accessing fields");
        return {reinterpret_cast<const char*>(data_ + 9), 14};
    }

    [[nodiscard]] uint32_t decrement_shares() const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be32(data_ + 23); }
    [[nodiscard]] char     reason()           const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[27]); }

private:
    const uint8_t* data_ = nullptr;
};

// -------------------------------------------------------------------------
// ReplacedView ('U', 80 bytes)
// -------------------------------------------------------------------------
/// Layout:
///   type(1) timestamp(8) replacement_token(14) side(1) shares(4) symbol(8)
///   price(4) tif(4) firm(4) display(1) order_ref(8) capacity(1)
///   int_mkt_sweep(1) cross_type(1) order_state(1) previous_token(14)
///   = 75 + reserved(5) = 80
/// @warning All accessors require valid() == true. Calling on an invalid
///          view is undefined behavior. Always check valid() first.
class ReplacedView {
public:
    static constexpr size_t kSize = 80;

    explicit ReplacedView(const uint8_t* data, size_t len) noexcept
        : data_(len >= kSize ? data : nullptr) {
        if (!data_ && data) [[unlikely]] {
            SPDLOG_WARN("ReplacedView: buffer too small, need {} but got {}", kSize, len);
        }
    }

    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }

    // All accessors assert valid() in debug builds to catch misuse early.
    [[nodiscard]] uint8_t  msg_type()  const noexcept { assert(valid() && "must check valid() before accessing fields"); return data_[0]; }
    [[nodiscard]] uint64_t timestamp() const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be64(data_ + 1); }

    [[nodiscard]] std::string_view replacement_token() const noexcept {
        assert(valid() && "must check valid() before accessing fields");
        return {reinterpret_cast<const char*>(data_ + 9), 14};
    }

    [[nodiscard]] char     side()   const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[23]); }
    [[nodiscard]] uint32_t shares() const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be32(data_ + 24); }

    [[nodiscard]] std::string_view symbol() const noexcept {
        assert(valid() && "must check valid() before accessing fields");
        return {reinterpret_cast<const char*>(data_ + 28), 8};
    }

    [[nodiscard]] uint32_t price()          const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be32(data_ + 36); }
    [[nodiscard]] uint32_t time_in_force()  const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be32(data_ + 40); }

    [[nodiscard]] std::string_view firm() const noexcept {
        assert(valid() && "must check valid() before accessing fields");
        return {reinterpret_cast<const char*>(data_ + 44), 4};
    }

    [[nodiscard]] char     display()        const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[48]); }
    [[nodiscard]] uint64_t order_reference() const noexcept { assert(valid() && "must check valid() before accessing fields"); return read_be64(data_ + 49); }
    [[nodiscard]] char     capacity()       const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[57]); }
    [[nodiscard]] char     int_mkt_sweep()  const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[58]); }
    [[nodiscard]] char     cross_type()     const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[59]); }
    [[nodiscard]] char     order_state()    const noexcept { assert(valid() && "must check valid() before accessing fields"); return static_cast<char>(data_[60]); }

    [[nodiscard]] std::string_view previous_token() const noexcept {
        assert(valid() && "must check valid() before accessing fields");
        return {reinterpret_cast<const char*>(data_ + 61), 14};
    }

private:
    const uint8_t* data_ = nullptr;
};

}  // namespace eph::itch::ouch
