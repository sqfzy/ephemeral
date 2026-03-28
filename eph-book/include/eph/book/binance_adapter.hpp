#pragma once

/// @file binance_adapter.hpp
/// Binance bookTicker to ArrayBook adapter — bridges Binance BBO updates
/// into the L2 price-level book.
///
/// Unlike the ITCH adapter (order-level events requiring aggregation),
/// Binance bookTicker provides direct BBO snapshots. The adapter simply
/// parses the BookTicker fields and updates the book's best bid and ask.
///
/// Usage requires both eph-book and eph-json headers to be available on
/// the include path. eph-book does NOT depend on eph-json; the user must
/// ensure both are linked when including this header.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <spdlog/spdlog.h>

#include "eph/book/array_book.hpp"
#include "eph/json/adapters/binance.hpp"
#include "eph/json/adapters/binance_depth_types.hpp"
#include "eph/json/parser.hpp"

namespace eph::book {

// ---------------------------------------------------------------------------
// BinanceBookAdapter — bookTicker to ArrayBook bridge
// ---------------------------------------------------------------------------

/// Adapts Binance WebSocket bookTicker messages into ArrayBook updates.
///
/// bookTicker provides only BBO (best bid/ask), so the adapter updates
/// level 0 on each side. For deeper books, the ArrayBook can also be
/// populated by other feeds.
///
/// @tparam MaxLevels  Maximum price levels per side in the underlying ArrayBook.
template <std::size_t MaxLevels = 20>
class BinanceBookAdapter {
public:
    /// Update book from a Binance bookTicker message.
    /// Only updates the BBO (best bid/ask) since bookTicker contains BBO only.
    ///
    /// @param ticker  Parsed BookTicker from eph::json::binance::BookTicker::from()
    /// @return true if the book was modified, false on parse failure
    bool update_from_ticker(const eph::json::binance::BookTicker& ticker) noexcept {
        SPDLOG_DEBUG("BinanceBookAdapter::update_from_ticker symbol={}", ticker.symbol);

        // Parse string price/qty fields into doubles.
        auto bid_price = parse_number(ticker.bid_price);
        auto bid_qty   = parse_number(ticker.bid_qty);
        auto ask_price = parse_number(ticker.ask_price);
        auto ask_qty   = parse_number(ticker.ask_qty);

        if (!bid_price || !bid_qty || !ask_price || !ask_qty) {
            SPDLOG_WARN("BinanceBookAdapter: failed to parse ticker fields "
                        "(bid_price={} bid_qty={} ask_price={} ask_qty={})",
                        ticker.bid_price, ticker.bid_qty,
                        ticker.ask_price, ticker.ask_qty);
            return false;
        }

        SPDLOG_TRACE("BinanceBookAdapter: bid={}@{} ask={}@{}",
                     *bid_price, *bid_qty, *ask_price, *ask_qty);

        book_.update_bid(*bid_price, *bid_qty);
        book_.update_ask(*ask_price, *ask_qty);
        return true;
    }

    /// Load a full depth snapshot from Binance REST API response.
    /// Clears existing book and replaces with snapshot data.
    /// This is the reconnection recovery path: REST snapshot -> order book.
    ///
    /// @param snapshot  Parsed DepthSnapshot from parse_depth_response()
    /// @return The number of levels loaded (bids + asks)
    std::size_t load_snapshot(const eph::json::binance::DepthSnapshot& snapshot) noexcept {
        SPDLOG_DEBUG("BinanceBookAdapter::load_snapshot last_update_id={} "
                     "bids={} asks={}",
                     snapshot.last_update_id,
                     snapshot.bids.size(),
                     snapshot.asks.size());

        book_.clear();
        last_update_id_ = snapshot.last_update_id;

        for (const auto& level : snapshot.bids) {
            SPDLOG_TRACE("load_snapshot bid price={} qty={}", level.price, level.qty);
            book_.update_bid(level.price, level.qty);
        }

        for (const auto& level : snapshot.asks) {
            SPDLOG_TRACE("load_snapshot ask price={} qty={}", level.price, level.qty);
            book_.update_ask(level.price, level.qty);
        }

        auto total = book_.bid_depth() + book_.ask_depth();
        SPDLOG_DEBUG("BinanceBookAdapter::load_snapshot complete: {} levels loaded", total);
        return total;
    }

    /// Last update ID from the most recently loaded snapshot.
    /// Used to validate sequence continuity when applying incremental updates:
    /// incremental updates with U <= last_update_id should be dropped.
    [[nodiscard]] int64_t last_update_id() const noexcept { return last_update_id_; }

    /// Get the current book state (const).
    [[nodiscard]] const ArrayBook<MaxLevels>& book() const noexcept { return book_; }

    /// Get the current book state (mutable).
    [[nodiscard]] ArrayBook<MaxLevels>& book() noexcept { return book_; }

private:
    ArrayBook<MaxLevels> book_;
    int64_t last_update_id_ = 0;

    /// Parse a string_view as double (price/quantity fields).
    /// Handles the decimal format used by Binance: "87245.30000000".
    static std::optional<double> parse_number(std::string_view sv) noexcept {
        if (sv.empty()) return std::nullopt;
        bool negative = false;
        std::size_t pos = 0;
        if (sv[0] == '-') { negative = true; pos = 1; }
        if (pos >= sv.size()) return std::nullopt;

        double result = 0.0;
        // Integer part
        for (; pos < sv.size() && sv[pos] != '.'; ++pos) {
            char c = sv[pos];
            if (c < '0' || c > '9') return std::nullopt;
            result = result * 10.0 + (c - '0');
        }
        // Fractional part
        if (pos < sv.size() && sv[pos] == '.') {
            ++pos;
            double divisor = 10.0;
            for (; pos < sv.size(); ++pos) {
                char c = sv[pos];
                if (c < '0' || c > '9') return std::nullopt;
                result += (c - '0') / divisor;
                divisor *= 10.0;
            }
        }
        return negative ? -result : result;
    }
};

} // namespace eph::book
