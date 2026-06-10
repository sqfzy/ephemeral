#pragma once

/// @file book_ticker.hpp
/// Zero-copy accessors for the Binance spot SBE BookTickerResponse message
/// (template id 212, schema 3:2).
///
/// Authoritative layout (eph-sbe/schemas/spot_3_2.xml, `<sbe:message id="212">`):
///   message root block: empty (blockLength = 0)
///   repeating group "tickers" (groupSize16Encoding header, 4 bytes):
///     per-entry fixed block (blockLength = 34 bytes):
///       field id=1 priceExponent  exponent8 (int8)   @ +0
///       field id=2 qtyExponent    exponent8 (int8)   @ +1
///       field id=3 bidPrice       mantissa64 (int64) @ +2   optional (null=INT64_MIN)
///       field id=4 bidQty         mantissa64 (int64) @ +10
///       field id=5 askPrice       mantissa64 (int64) @ +18  optional (null=INT64_MIN)
///       field id=6 askQty         mantissa64 (int64) @ +26
///     data id=200 symbol          varString8         @ +34  (uint8 len + UTF-8)
///
/// Prices/quantities are decimal: value = mantissa × 10^exponent. The two
/// optional price fields return std::nullopt when the wire carries the SBE null
/// sentinel (INT64_MIN).
///
/// Accessors take a pointer to the start of a per-ticker fixed block and assume
/// the entry was bounds-validated by for_each_ticker(); they perform no bounds
/// checks themselves (same contract as eph-itch's per-message accessors).

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

#include "eph/sbe/binance/schema.hpp"
#include "eph/sbe/byte_order.hpp"
#include "eph/sbe/errors.hpp"
#include "eph/sbe/parser.hpp"

namespace eph::sbe::binance {

/// @brief Internal detail namespace for the Binance SBE accessor logger.
namespace detail {
/// @brief Get or create the Binance SBE module logger ("sbe.binance").
inline const std::shared_ptr<spdlog::logger>& sbe_binance_logger() {
    static auto l = [] {
        auto lg = spdlog::get("sbe.binance");
        if (!lg) {
            try { lg = spdlog::stdout_color_mt("sbe.binance"); }
            catch (const spdlog::spdlog_ex&) { lg = spdlog::get("sbe.binance"); }
        }
        if (!lg) lg = spdlog::default_logger();
        return lg;
    }();
    return l;
}
} // namespace detail

namespace book_ticker {

/// @brief Size in bytes of a ticker entry's fixed block (schema 3:2).
inline constexpr std::size_t kBlockLength = 34;

// Field offsets within the per-ticker fixed block — cite spot_3_2.xml field ids.
inline constexpr std::size_t kOffPriceExponent = 0;  ///< field id=1
inline constexpr std::size_t kOffQtyExponent   = 1;  ///< field id=2
inline constexpr std::size_t kOffBidPrice      = 2;  ///< field id=3 (optional)
inline constexpr std::size_t kOffBidQty        = 10; ///< field id=4
inline constexpr std::size_t kOffAskPrice      = 18; ///< field id=5 (optional)
inline constexpr std::size_t kOffAskQty        = 26; ///< field id=6
inline constexpr std::size_t kOffSymbol        = kBlockLength; ///< data id=200 (trails block)

/// @brief Price decimal exponent for this ticker (field id=1).
[[nodiscard]] inline int8_t price_exponent(const uint8_t* block) noexcept {
    return read_le_i8(block + kOffPriceExponent);
}

/// @brief Quantity decimal exponent for this ticker (field id=2).
[[nodiscard]] inline int8_t qty_exponent(const uint8_t* block) noexcept {
    return read_le_i8(block + kOffQtyExponent);
}

/// @brief Raw best-bid price mantissa (field id=3); INT64_MIN denotes null.
[[nodiscard]] inline int64_t bid_price_mantissa(const uint8_t* block) noexcept {
    return read_le_i64(block + kOffBidPrice);
}

/// @brief Best-bid price = bidPrice × 10^priceExponent, or nullopt if null.
[[nodiscard]] inline std::optional<double> bid_price(const uint8_t* block) noexcept {
    const int64_t m = bid_price_mantissa(block);
    if (m == kNullMantissa) return std::nullopt;
    return decode_decimal(m, price_exponent(block));
}

/// @brief Best-bid quantity = bidQty × 10^qtyExponent (field id=4).
[[nodiscard]] inline double bid_qty(const uint8_t* block) noexcept {
    return decode_decimal(read_le_i64(block + kOffBidQty), qty_exponent(block));
}

/// @brief Raw best-ask price mantissa (field id=5); INT64_MIN denotes null.
[[nodiscard]] inline int64_t ask_price_mantissa(const uint8_t* block) noexcept {
    return read_le_i64(block + kOffAskPrice);
}

/// @brief Best-ask price = askPrice × 10^priceExponent, or nullopt if null.
[[nodiscard]] inline std::optional<double> ask_price(const uint8_t* block) noexcept {
    const int64_t m = ask_price_mantissa(block);
    if (m == kNullMantissa) return std::nullopt;
    return decode_decimal(m, price_exponent(block));
}

/// @brief Best-ask quantity = askQty × 10^qtyExponent (field id=6).
[[nodiscard]] inline double ask_qty(const uint8_t* block) noexcept {
    return decode_decimal(read_le_i64(block + kOffAskQty), qty_exponent(block));
}

/// @brief Symbol (field data id=200), zero-copy view of the UTF-8 bytes.
///
/// Valid for schema 3:2, where the fixed block is exactly kBlockLength bytes so
/// the varString8 length prefix sits at block + kBlockLength.
[[nodiscard]] inline std::string_view symbol(const uint8_t* block) noexcept {
    const uint8_t len = block[kOffSymbol];
    return std::string_view{reinterpret_cast<const char*>(block + kOffSymbol + 1), len};
}

} // namespace book_ticker

/// @brief Iterate every ticker entry in a BookTickerResponse message.
///
/// Decodes the `tickers` repeating-group header, then walks each entry with full
/// bounds checking (untrusted-input safe: a forged numInGroup or symbol length
/// cannot read past the buffer). The fixed block is located using the on-wire
/// group blockLength so trailing schema additions do not break iteration; the
/// scalar accessors read fixed offsets ≤ 26, valid as long as Binance only
/// appends fields (guarded by binance::is_supported()).
///
/// @tparam Fn  Callable invoked as `fn(const uint8_t* ticker_block)` per entry.
/// @param view Parsed message view (template id should be tid::kBookTicker).
/// @param fn   Per-ticker callback receiving the fixed-block pointer.
/// @return Number of tickers delivered, or ParseError on truncation / bad group.
template <typename Fn>
[[nodiscard]] std::expected<std::size_t, ParseError>
for_each_ticker(const MessageView& view, Fn&& fn) noexcept {
    if (!is_supported(view)) [[unlikely]] {
        SPDLOG_LOGGER_WARN(detail::sbe_binance_logger(),
            "for_each_ticker: unexpected schema {}:{} (accessors target {}:{})",
            view.schema_id, view.version, kSchemaId, kSchemaVersion);
        // Offsets may not match; refuse rather than decode silently-wrong data.
        return std::unexpected(ParseError::kMalformedGroup);
    }

    const uint8_t* const body = view.body();
    const std::size_t avail = view.body_len();
    if (avail < kGroupHeaderSize) [[unlikely]] {
        SPDLOG_LOGGER_WARN(detail::sbe_binance_logger(),
            "for_each_ticker: body {} < group header {}", avail, kGroupHeaderSize);
        return std::unexpected(ParseError::kTruncated);
    }

    const GroupHeader gh = read_group_header(body);
    // A fixed block smaller than our known layout means the scalar accessors
    // would read past the entry — reject as malformed.
    if (gh.block_length < book_ticker::kBlockLength) [[unlikely]] {
        SPDLOG_LOGGER_WARN(detail::sbe_binance_logger(),
            "for_each_ticker: group blockLength {} < expected {}",
            gh.block_length, book_ticker::kBlockLength);
        return std::unexpected(ParseError::kMalformedGroup);
    }

    std::size_t off = kGroupHeaderSize;
    for (uint16_t i = 0; i < gh.num_in_group; ++i) {
        // Need the fixed block + the 1-byte symbol length prefix.
        if (off + gh.block_length + 1 > avail) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::sbe_binance_logger(),
                "for_each_ticker: truncated entry {} at off {} (avail {})",
                i, off, avail);
            return std::unexpected(ParseError::kTruncated);
        }
        const uint8_t* const block = body + off;
        const std::size_t sym_len = block[gh.block_length];
        const std::size_t entry = gh.block_length + 1 + sym_len;
        if (off + entry > avail) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::sbe_binance_logger(),
                "for_each_ticker: symbol of entry {} overruns buffer "
                "(off {}, entry {}, avail {})", i, off, entry, avail);
            return std::unexpected(ParseError::kTruncated);
        }
        fn(block);
        off += entry;
    }
    return static_cast<std::size_t>(gh.num_in_group);
}

} // namespace eph::sbe::binance
