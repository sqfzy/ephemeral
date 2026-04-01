# eph-json

Header-only C++23 library for zero-copy JSON parsing and typed adapter extraction, optimized for flat market data payloads from cryptocurrency exchanges.

## Key Components

All headers are under `include/eph/json/`:

- **parser.hpp** -- Zero-copy, zero-allocation JSON parser for flat key-value objects. Single-pass O(n) parse with O(n) linear-scan field lookup (faster than hash maps for typical 5-15 field exchange messages due to cache locality). Returns a `JsonView` of string_view fields pointing into the original buffer.
- **framer.hpp** -- Pass-through `JsonFramer` satisfying the `eph::net::MessageFramer` concept. Since WebSocket already provides message boundaries, this is a semantic identity framer for Transport type aliases.
- **adapters/binance.hpp** -- Typed structs for Binance WebSocket feeds (bookTicker, trade, depth) with zero-copy field extraction from `JsonView`. Includes FNV-1a symbol hash for Transport two-phase frame filtering.
- **adapters/binance_rest.hpp** -- Typed Binance REST client for orderbook snapshots (`/api/v3/depth`) and clock sync (`/api/v3/time`). Read-only public endpoints for post-reconnect recovery.
- **adapters/okx.hpp** -- OKX WebSocket adapters handling the `{"arg":{...},"data":[{...}]}` wrapper format. Typed structs for bbo-tbt and other latency-sensitive feeds.
- **adapters/bybit.hpp** -- Bybit WebSocket adapters handling the `{"topic":"...","type":"snapshot|delta","data":{...}}` wrapper format. Typed structs for tickers and order book data.

## Dependencies

- **eph-core** -- Error traits, number parsing (`parse_number.hpp`), framer concept
- **spdlog** -- Logging (adapters only)

## Quick Start

```cpp
#include <eph/json/parser.hpp>
#include <eph/json/adapters/binance.hpp>

// Parse raw JSON buffer (zero-copy)
auto result = eph::json::parse(data, len);
if (!result) { /* handle error */ }

// Extract typed Binance bookTicker
if (auto ticker = eph::json::binance::BinanceBookTicker::from(result.value())) {
    auto bid = ticker->bid_price;   // string_view into original buffer
    auto ask = ticker->ask_price;
    auto sym = ticker->symbol;      // "btcusdt"
}
```
