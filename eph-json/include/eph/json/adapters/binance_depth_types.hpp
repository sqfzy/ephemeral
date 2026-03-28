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

/// A single price/quantity level in the orderbook.
struct DepthLevel {
    double price;
    double qty;
};

/// Orderbook depth snapshot from GET /api/v3/depth.
struct DepthSnapshot {
    int64_t last_update_id = 0;
    std::vector<DepthLevel> bids;  ///< Sorted descending by price
    std::vector<DepthLevel> asks;  ///< Sorted ascending by price
};

/// Server time from GET /api/v3/time.
struct ServerTime {
    int64_t server_time_ms = 0;  ///< Milliseconds since epoch
};

} // namespace eph::json::binance
