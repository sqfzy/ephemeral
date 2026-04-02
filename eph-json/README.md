# eph-json

Header-only C++23 library for zero-copy JSON parsing and typed adapter extraction, optimized for flat market data payloads from cryptocurrency exchanges.

## Key Components

All headers are under `include/eph/json/`:

- **parser.hpp** -- Zero-copy, zero-allocation JSON parser for flat key-value objects. Single-pass O(n) parse with O(n) linear-scan field lookup (faster than hash maps for typical 5-15 field exchange messages due to cache locality). Returns a `JsonView` of string_view fields pointing into the original buffer. Nested objects/arrays are captured as opaque string_views for downstream re-parsing.
- **framer.hpp** -- Pass-through `JsonFramer` satisfying the `eph::net::MessageFramer` concept. Since WebSocket already provides message boundaries, this is a semantic identity framer for Transport type aliases.
- **adapters/binance.hpp** -- Typed structs for Binance WebSocket feeds (bookTicker, combined streams) with zero-copy field extraction from `JsonView`. Includes FNV-1a `symbol_hash` for Transport two-phase frame filtering, and helpers for building WebSocket paths and SUBSCRIBE/UNSUBSCRIBE messages.
- **adapters/binance_depth_types.hpp** -- Lightweight data types (`DepthLevel`, `DepthSnapshot`, `ServerTime`) for Binance orderbook depth snapshots. Separated from binance_rest.hpp so consumers that need only the types (e.g., eph-book's BinanceBookAdapter) can avoid pulling in eph-net and HTTP client dependencies.
- **adapters/binance_rest.hpp** -- Typed Binance REST client for orderbook snapshots (`/api/v3/depth`) and clock sync (`/api/v3/time`). Read-only public endpoints for post-reconnect recovery. Also exposes `parse_depth_response()` and `parse_server_time_response()` as free functions for testability.
- **adapters/okx.hpp** -- OKX WebSocket adapters handling the `{"arg":{...},"data":[{...}]}` wrapper format. Typed structs for bbo-tbt and other latency-sensitive feeds. Includes FNV-1a `inst_id_hash` for two-phase frame filtering.
- **adapters/bybit.hpp** -- Bybit WebSocket adapters handling the `{"topic":"...","type":"snapshot|delta","data":{...}}` wrapper format. Typed structs for tickers and order book data. Includes FNV-1a `symbol_hash` for two-phase frame filtering.

## Public API

### Parser (`eph::json`)

| Symbol | Description |
|--------|-------------|
| `parse(const uint8_t* data, size_t len)` | Parse flat JSON object into a `JsonView`. Returns `std::expected<JsonView, ParseError>`. |
| `ParseError` | Enum: `kIncomplete`, `kInvalidFormat`, `kFieldOverflow`. |
| `parse_error_name(ParseError)` | Human-readable name for a `ParseError` value. |
| `Field` | Zero-copy field: `key` (string_view), `value` (string_view), `is_string` (bool). |
| `JsonView` | Zero-copy view into a flat JSON object (max 32 fields). |
| `JsonView::get(key)` | Get raw value as `string_view` (empty if missing). |
| `JsonView::get_string(key)` | Get string value as `optional<string_view>` (nullopt if missing). |
| `JsonView::get_int(key)` | Parse value as `optional<int64_t>`. |
| `JsonView::get_double(key)` | Parse value as `optional<double>`. |
| `JsonView::get_bool(key)` | Parse value as `optional<bool>`. |
| `JsonView::has(key)` | Check if a key exists. |
| `JsonView::field_count()` | Number of parsed fields. |
| `JsonView::field_at(i)` | Access field by index for iteration. |

### Framer (`eph::json`)

| Symbol | Description |
|--------|-------------|
| `JsonFramer` | Pass-through framer for JSON-over-WebSocket. Satisfies `eph::net::MessageFramer`. `encode()` copies bytes, `decode()` wraps entire payload as a `DecodedFrame`. |

### Binance Adapter (`eph::json::binance`)

| Symbol | Description |
|--------|-------------|
| `BookTicker` | Zero-copy view of a Binance bookTicker message. Fields: `symbol`, `bid_price`, `bid_qty`, `ask_price`, `ask_qty`, `update_id`, `event_time`, `txn_time`. Pre-caches parsed bid/ask doubles. |
| `BookTicker::from(JsonView)` | Extract from parsed JSON. Returns `optional<BookTicker>`. |
| `BookTicker::mid_price()` | Compute `(bid + ask) / 2` as `optional<double>`. |
| `BookTicker::spread()` | Compute `ask - bid` as `optional<double>`. |
| `CombinedStream` | Zero-copy view of a Binance combined stream wrapper (`{"stream":"...","data":{...}}`). Fields: `stream`, `symbol`, `data_raw`. |
| `CombinedStream::from(JsonView)` | Extract from parsed JSON. Returns `optional<CombinedStream>`. |
| `extract_symbol(stream)` | Extract symbol from stream suffix: `"btcusdt@bookTicker"` -> `"btcusdt"`. |
| `symbol_hash(data, len)` | FNV-1a hash of the `"s"` field for two-phase frame filtering. Fast pattern scan, avoids full JSON parse. |
| `ws_path(symbol, stream_type)` | Build single-stream WebSocket path: `"/ws/btcusdt@bookTicker"`. |
| `combined_ws_path(symbols, stream_type)` | Build multi-symbol combined stream path. |
| `subscribe_message(symbols, stream_type, id)` | Build SUBSCRIBE JSON message. |
| `unsubscribe_message(symbols, stream_type, id)` | Build UNSUBSCRIBE JSON message. |

### Binance Depth Types (`eph::json::binance`)

| Symbol | Description |
|--------|-------------|
| `DepthLevel` | Price/quantity pair: `double price`, `double qty`. |
| `DepthSnapshot` | Orderbook snapshot: `last_update_id`, `bids` (descending), `asks` (ascending). |
| `ServerTime` | Server time: `server_time_ms` (milliseconds since epoch). |

### Binance REST Client (`eph::json::binance`)

| Symbol | Description |
|--------|-------------|
| `BinanceRestConfig` | Config: `host`, `port`, `timeout`. Defaults to `api.binance.com:443`. |
| `BinanceRestClient` | Typed REST client for public endpoints. No authentication. |
| `BinanceRestClient::get_depth(symbol, limit)` | GET `/api/v3/depth`. Returns `expected<DepthSnapshot, string>`. Valid limits: 5, 10, 20, 50, 100, 500, 1000, 5000. |
| `BinanceRestClient::get_server_time()` | GET `/api/v3/time`. Returns `expected<ServerTime, string>`. |
| `parse_depth_response(body)` | Parse depth JSON body. Exposed for testability. |
| `parse_server_time_response(body)` | Parse server time JSON body. Exposed for testability. |

### OKX Adapter (`eph::json::okx`)

| Symbol | Description |
|--------|-------------|
| `OkxPushMessage` | Zero-copy envelope: `channel`, `inst_id`, `data_raw`. Re-parses the nested `"arg"` object to extract channel metadata. |
| `OkxPushMessage::from(JsonView)` | Extract from parsed JSON. Returns `optional<OkxPushMessage>`. |
| `OkxBookTicker` | Zero-copy view of OKX bbo-tbt data. Fields: `inst_id`, `bid_price`, `bid_qty`, `ask_price`, `ask_qty`, `timestamp_ms`. Navigates `data[0]` automatically. |
| `OkxBookTicker::from(JsonView)` | Extract from outer push message JSON. Returns `optional<OkxBookTicker>`. |
| `OkxBookTicker::mid_price()` | Compute `(bid + ask) / 2` as `optional<double>`. |
| `OkxBookTicker::spread()` | Compute `ask - bid` as `optional<double>`. |
| `inst_id_hash(data, len)` | FNV-1a hash of `"instId"` field for two-phase frame filtering. |
| `subscribe_message(channel, inst_ids, id)` | Build OKX subscribe JSON. |
| `unsubscribe_message(channel, inst_ids, id)` | Build OKX unsubscribe JSON. |

### Bybit Adapter (`eph::json::bybit`)

| Symbol | Description |
|--------|-------------|
| `BybitPushMessage` | Zero-copy envelope: `topic`, `type` ("snapshot"/"delta"), `data_raw`. |
| `BybitPushMessage::from(JsonView)` | Extract from parsed JSON. Returns `optional<BybitPushMessage>`. |
| `BybitBookTicker` | Zero-copy view of Bybit tickers data. Fields: `symbol`, `bid_price`, `bid_qty`, `ask_price`, `ask_qty`, `last_price`, `timestamp_ms`. Navigates nested `data` object automatically. |
| `BybitBookTicker::from(JsonView)` | Extract from outer push message JSON. Returns `optional<BybitBookTicker>`. |
| `BybitBookTicker::mid_price()` | Compute `(bid + ask) / 2` as `optional<double>`. |
| `BybitBookTicker::spread()` | Compute `ask - bid` as `optional<double>`. |
| `symbol_hash(data, len)` | FNV-1a hash of `"symbol"` field for two-phase frame filtering. |
| `subscribe_message(channel, symbols, req_id)` | Build Bybit subscribe JSON. |
| `unsubscribe_message(channel, symbols, req_id)` | Build Bybit unsubscribe JSON. |

## Dependencies

- **eph-core** -- Error traits (`error_traits.hpp`), number parsing (`parse_number.hpp`), framer concept (`framer_concept.hpp`)
- **spdlog** -- Logging (adapters only)
- **eph-net** (optional) -- Required only when using `binance_rest.hpp` (HTTP client)

## Quick Start

```cpp
#include <eph/json/parser.hpp>
#include <eph/json/adapters/binance.hpp>

// Parse raw JSON buffer (zero-copy)
auto result = eph::json::parse(data, len);
if (!result) { /* handle error */ }

// Extract typed Binance bookTicker
if (auto ticker = eph::json::binance::BookTicker::from(result.value())) {
    auto bid = ticker->bid_price;   // string_view into original buffer
    auto ask = ticker->ask_price;
    auto sym = ticker->symbol;      // "BTCUSDT"
    auto mid = ticker->mid_price(); // optional<double>
}
```

```cpp
#include <eph/json/adapters/okx.hpp>

// OKX bbo-tbt feed (wrapped format)
auto result = eph::json::parse(data, len);
if (auto push = eph::json::okx::OkxPushMessage::from(*result)) {
    if (push->channel == "bbo-tbt") {
        if (auto ticker = eph::json::okx::OkxBookTicker::from(*result)) {
            auto bid = ticker->bid_price;  // "87000.0"
            auto inst = ticker->inst_id;   // "BTC-USDT"
        }
    }
}
```

```cpp
#include <eph/json/adapters/bybit.hpp>

// Bybit tickers feed (topic-based wrapper)
auto result = eph::json::parse(data, len);
if (auto push = eph::json::bybit::BybitPushMessage::from(*result)) {
    if (push->topic.starts_with("tickers.")) {
        if (auto ticker = eph::json::bybit::BybitBookTicker::from(*result)) {
            auto bid = ticker->bid_price;  // "87000.0"
        }
    }
}
```

```cpp
#include <eph/json/adapters/binance_rest.hpp>

// Binance REST: orderbook snapshot recovery
eph::json::binance::BinanceRestClient client;
auto depth = client.get_depth("BTCUSDT", 20);
if (depth) {
    for (auto& bid : depth->bids) {
        // bid.price, bid.qty
    }
}
```
