#pragma once

/// @file adapters/binance_depth_types.hpp
/// Lightweight data types for Binance orderbook depth snapshots.
///
/// Separated from binance_rest.hpp so consumers that need only the types
/// (e.g., eph-book's BinanceBookAdapter) can avoid pulling in eph-net
/// and HTTP client dependencies.

#include <cstdint>
#include <vector>

namespace eph::json::binance {

/// @brief A single price/quantity level in the orderbook.
///
/// Represents one rung of the Binance order book with pre-parsed
/// double values. Produced by parse_depth_levels() in binance_rest.hpp
/// from the quoted-string arrays Binance returns.
struct DepthLevel {
    double price;  ///< Level price (e.g., 87245.30)
    double qty;    ///< Aggregate quantity available at this price level
};

/// @brief Orderbook depth snapshot from GET /api/v3/depth.
///
/// Contains a sequenced snapshot of the top-of-book bids and asks.
/// Used for post-reconnect recovery to seed a local order book
/// before applying incremental WebSocket depth updates.
///
/// @note The @c last_update_id is required for depth update sequencing:
///       only apply WebSocket depth events whose @c U <= last_update_id+1
///       and @c u >= last_update_id+1 (Binance docs: "How to manage a
///       local order book correctly").
struct DepthSnapshot {
    int64_t last_update_id = 0;          ///< Sequence ID for depth update alignment
    std::vector<DepthLevel> bids;        ///< Bid levels, sorted descending by price
    std::vector<DepthLevel> asks;        ///< Ask levels, sorted ascending by price
};

/// @brief Server time from GET /api/v3/time.
///
/// Used for clock drift validation -- comparing local time against
/// Binance server time to detect NTP or system clock issues that
/// would affect order timing and latency measurement.
struct ServerTime {
    int64_t server_time_ms = 0;  ///< Binance server time in milliseconds since Unix epoch
};

} // namespace eph::json::binance
