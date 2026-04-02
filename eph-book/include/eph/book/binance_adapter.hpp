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
#include <spdlog/sinks/stdout_color_sinks.h>

#include "eph/book/array_book.hpp"
#include "eph/core/parse_number.hpp"
#include "eph/json/adapters/binance.hpp"
#include "eph/json/adapters/binance_depth_types.hpp"
#include "eph/json/parser.hpp"

namespace eph::book {

/// @cond INTERNAL
namespace detail {
/// @brief Lazily-constructed spdlog logger for BinanceBookAdapter diagnostics.
/// @return Pointer to the "book.binance_adapter" logger instance.
inline spdlog::logger* binance_adapter_logger() {
    static auto l = [] {
        try {
            return spdlog::stdout_color_mt("book.binance_adapter");
        } catch (const spdlog::spdlog_ex&) {
            return spdlog::get("book.binance_adapter");
        }
    }();
    return l.get();
}
} // namespace detail
/// @endcond

// ---------------------------------------------------------------------------
// BinanceBookAdapter — bookTicker to ArrayBook bridge
// ---------------------------------------------------------------------------

/// @brief Adapts Binance WebSocket bookTicker messages into ArrayBook updates.
///
/// Binance `bookTicker` provides only BBO (best bid/ask), so the adapter
/// updates level 0 on each side.  For deeper books, the underlying ArrayBook
/// can also be populated by other feeds or via load_snapshot().
///
/// Typical lifecycle:
///   1. Construct a `BinanceBookAdapter`.
///   2. Call load_snapshot() with a REST depth response for initial state.
///   3. Call update_from_ticker() for each incoming WebSocket bookTicker message.
///   4. Query the book via `book()`.
///
/// @tparam MaxLevels  Maximum price levels per side in the underlying ArrayBook.
///                    Defaults to 20.
template <std::size_t MaxLevels = 20>
class BinanceBookAdapter {
public:
    /// @brief Update book from a Binance bookTicker message.
    ///
    /// Only updates the BBO (best bid/ask) since bookTicker contains BBO only.
    /// String price/quantity fields are parsed to doubles internally.
    ///
    /// @param ticker  Parsed BookTicker obtained from `eph::json::binance::BookTicker::from()`.
    /// @return `true` if the book was modified, `false` on parse failure
    ///         (e.g., non-numeric price/qty strings).
    bool update_from_ticker(const eph::json::binance::BookTicker& ticker) noexcept {
        SPDLOG_LOGGER_DEBUG(detail::binance_adapter_logger(), "BinanceBookAdapter::update_from_ticker symbol={}", ticker.symbol);

        // Parse string price/qty fields into doubles.
        auto bid_price = parse_number(ticker.bid_price);
        auto bid_qty   = parse_number(ticker.bid_qty);
        auto ask_price = parse_number(ticker.ask_price);
        auto ask_qty   = parse_number(ticker.ask_qty);

        if (!bid_price || !bid_qty || !ask_price || !ask_qty) {
            SPDLOG_LOGGER_WARN(detail::binance_adapter_logger(), "BinanceBookAdapter: failed to parse ticker fields "
                        "(bid_price={} bid_qty={} ask_price={} ask_qty={})",
                        ticker.bid_price, ticker.bid_qty,
                        ticker.ask_price, ticker.ask_qty);
            return false;
        }

        SPDLOG_LOGGER_TRACE(detail::binance_adapter_logger(), "BinanceBookAdapter: bid={}@{} ask={}@{}",
                     *bid_price, *bid_qty, *ask_price, *ask_qty);

        book_.update_bid(*bid_price, *bid_qty);
        book_.update_ask(*ask_price, *ask_qty);
        return true;
    }

    /// @brief Load a full depth snapshot from a Binance REST API response.
    ///
    /// Clears the existing book and replaces it with the snapshot data.
    /// This is the reconnection recovery path: REST snapshot -> order book.
    /// After loading, use last_update_id() to validate sequence continuity
    /// of subsequent incremental updates.
    ///
    /// @param snapshot  Parsed DepthSnapshot obtained from `parse_depth_response()`.
    /// @return The number of levels loaded (bids + asks), capped by MaxLevels per side.
    std::size_t load_snapshot(const eph::json::binance::DepthSnapshot& snapshot) noexcept {
        SPDLOG_LOGGER_DEBUG(detail::binance_adapter_logger(), "BinanceBookAdapter::load_snapshot last_update_id={} "
                     "bids={} asks={}",
                     snapshot.last_update_id,
                     snapshot.bids.size(),
                     snapshot.asks.size());

        book_.clear();
        last_update_id_ = snapshot.last_update_id;

        for (const auto& level : snapshot.bids) {
            SPDLOG_LOGGER_TRACE(detail::binance_adapter_logger(), "load_snapshot bid price={} qty={}", level.price, level.qty);
            book_.update_bid(level.price, level.qty);
        }

        for (const auto& level : snapshot.asks) {
            SPDLOG_LOGGER_TRACE(detail::binance_adapter_logger(), "load_snapshot ask price={} qty={}", level.price, level.qty);
            book_.update_ask(level.price, level.qty);
        }

        auto total = book_.bid_depth() + book_.ask_depth();
        SPDLOG_LOGGER_DEBUG(detail::binance_adapter_logger(), "BinanceBookAdapter::load_snapshot complete: {} levels loaded", total);
        return total;
    }

    /// @brief Last update ID from the most recently loaded snapshot.
    ///
    /// Used to validate sequence continuity when applying incremental updates:
    /// incremental updates with `U <= last_update_id()` should be dropped.
    ///
    /// @return The `lastUpdateId` field from the last call to load_snapshot(),
    ///         or 0 if no snapshot has been loaded.
    [[nodiscard]] int64_t last_update_id() const noexcept { return last_update_id_; }

    /// @brief Get the current book state (const).
    /// @return Const reference to the underlying ArrayBook.
    [[nodiscard]] const ArrayBook<MaxLevels>& book() const noexcept { return book_; }

    /// @brief Get the current book state (mutable).
    /// @return Mutable reference to the underlying ArrayBook.
    [[nodiscard]] ArrayBook<MaxLevels>& book() noexcept { return book_; }

private:
    ArrayBook<MaxLevels> book_;
    int64_t last_update_id_ = 0;

    /// Parse a string_view as double (price/quantity fields).
    static std::optional<double> parse_number(std::string_view sv) noexcept {
        return eph::core::parse_number(sv);
    }
};

} // namespace eph::book
