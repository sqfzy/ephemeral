# eph-json

Header-only C++23 library for zero-copy JSON parsing and typed adapter extraction, optimized for flat market data payloads from cryptocurrency exchanges. Part of the eph HFT ecosystem.

## Overview

eph-json sits between the network transport layer (eph-net/eph-transport) and the application logic (eph-book, trading strategies). It provides:

- A **zero-copy, zero-allocation JSON parser** tuned for the 5-15 field flat objects typical of exchange WebSocket feeds. Single-pass O(n) parse with O(n) linear-scan field lookup that outperforms hash maps due to cache locality.
- **Typed exchange adapters** for Binance, OKX, and Bybit that map raw JSON into structs with descriptive field names, pre-parsed prices, and computed mid/spread.
- **FNV-1a symbol hash extractors** for each exchange, designed for Transport's two-phase frame filter (latest-per-symbol deduplication without full JSON parsing).
- A **pass-through JSON framer** satisfying the `eph::net::MessageFramer` concept for WebSocket transport type aliases.

All string_view members in parsed results point into the original buffer -- the caller must ensure the buffer outlives the view.

## Key Components

All headers are under `include/eph/json/`:

- **parser.hpp** -- Zero-copy, zero-allocation JSON parser for flat key-value objects. Single-pass O(n) parse with O(n) linear-scan field lookup (faster than hash maps for typical 5-15 field exchange messages due to cache locality). Returns a `JsonView` of string_view fields pointing into the original buffer. Nested objects/arrays are captured as opaque string_views for downstream re-parsing. Uses compile-time LUTs for whitespace skipping and value termination.
- **framer.hpp** -- Pass-through `JsonFramer` satisfying the `eph::net::MessageFramer` concept. Since WebSocket already provides message boundaries, this is a semantic identity framer for Transport type aliases. `encode()` is a plain memcpy, `decode()` wraps the buffer as a `DecodedFrame` with zero overhead.
- **adapters/binance.hpp** -- Typed structs for Binance WebSocket feeds (`BookTicker`, `CombinedStream`) with zero-copy field extraction from `JsonView`. Includes FNV-1a `symbol_hash` for Transport two-phase frame filtering (fast pattern scan of `"s":"` without full JSON parse), and helpers for building WebSocket paths (`ws_path`, `combined_ws_path`) and SUBSCRIBE/UNSUBSCRIBE messages.
- **adapters/binance_depth_types.hpp** -- Lightweight data types (`DepthLevel`, `DepthSnapshot`, `ServerTime`) for Binance orderbook depth snapshots. Separated from binance_rest.hpp so consumers that need only the types (e.g., eph-book's BinanceBookAdapter) can avoid pulling in eph-net and HTTP client dependencies.
- **adapters/binance_rest.hpp** -- Typed Binance REST client (`BinanceRestClient`) for read-only public endpoints: orderbook snapshots (`GET /api/v3/depth`) and clock sync (`GET /api/v3/time`). Used for post-reconnect recovery. Exposes `parse_depth_response()` and `parse_server_time_response()` as free functions for testability. No authentication required.
- **adapters/okx.hpp** -- OKX WebSocket adapters handling the `{"arg":{...},"data":[{...}]}` wrapper format. `OkxPushMessage` extracts channel routing info by re-parsing the nested "arg" object. `OkxBookTicker` navigates into `data[0]` to extract bbo-tbt fields. Includes FNV-1a `inst_id_hash` for two-phase frame filtering.
- **adapters/bybit.hpp** -- Bybit WebSocket adapters handling the `{"topic":"...","type":"snapshot|delta","data":{...}}` wrapper format. `BybitPushMessage` extracts topic/type routing. `BybitBookTicker` re-parses the nested data object for tickers fields (`bid1Price`/`bid1Size`/`ask1Price`/`ask1Size`). Includes FNV-1a `symbol_hash` for two-phase frame filtering.

The aggregate header `json.hpp` includes parser, framer, and the Binance WebSocket adapter.

## Public API Reference

### Parser (`eph::json`)

| Symbol | Description |
|--------|-------------|
| `parse(const uint8_t* data, size_t len)` | Parse flat JSON object into a `JsonView`. Returns `std::expected<JsonView, ParseError>`. |
| `ParseError` | Enum: `kIncomplete` (no closing brace), `kInvalidFormat` (malformed JSON), `kFieldOverflow` (more than 32 fields). |
| `parse_error_name(ParseError)` | Human-readable name for a `ParseError` value. |
| `Field` | Zero-copy field: `key` (`string_view`), `value` (`string_view`), `is_string` (`bool`). |
| `JsonView` | Zero-copy view into a flat JSON object (max 32 fields via `kMaxFields`). |
| `JsonView::get(key)` | Get raw value as `string_view` (empty if missing). |
| `JsonView::get_string(key)` | Get string value as `optional<string_view>` (nullopt if missing). |
| `JsonView::get_int(key)` | Parse value as `optional<int64_t>` via `eph::core::parse_int`. |
| `JsonView::get_double(key)` | Parse value as `optional<double>` via `eph::core::parse_number`. |
| `JsonView::get_bool(key)` | Parse value as `optional<bool>` ("true"/"false" literals only). |
| `JsonView::has(key)` | Check if a key exists. |
| `JsonView::field_count()` | Number of parsed fields (0 to `kMaxFields`). |
| `JsonView::field_at(i)` | Access field by index for iteration. Returns static empty Field if out of bounds. |

### Framer (`eph::json`)

| Symbol | Description |
|--------|-------------|
| `JsonFramer` | Pass-through framer for JSON-over-WebSocket. Satisfies `eph::net::MessageFramer`. |
| `JsonFramer::max_overhead()` | Always returns 0 (no framing overhead). |
| `JsonFramer::encode(out, data, len, msg_type)` | Identity copy. Returns bytes written, or 0 on null/empty input. |
| `JsonFramer::decode(data, len)` | Wraps entire buffer as `DecodedFrame`. Returns `FrameError::kIncomplete` if len is 0. |

### Binance WebSocket Adapter (`eph::json::binance`)

| Symbol | Description |
|--------|-------------|
| `BookTicker` | Zero-copy view of a bookTicker message. Fields: `symbol`, `bid_price`, `bid_qty`, `ask_price`, `ask_qty`, `update_id`, `event_time`, `txn_time`. Pre-caches parsed bid/ask doubles. |
| `BookTicker::from(JsonView)` | Extract from parsed JSON. Returns `optional<BookTicker>`. Requires fields: s, b, B, a, A. |
| `BookTicker::mid_price()` | `(bid + ask) / 2` as `optional<double>`. Uses cached parsed values. |
| `BookTicker::spread()` | `ask - bid` as `optional<double>`. Uses cached parsed values. |
| `CombinedStream` | Zero-copy view of a combined stream wrapper (`{"stream":"...","data":{...}}`). Fields: `stream`, `symbol`, `data_raw`. |
| `CombinedStream::from(JsonView)` | Extract from parsed JSON. Returns `optional<CombinedStream>`. |
| `extract_symbol(stream)` | Extract symbol from stream suffix: `"btcusdt@bookTicker"` -> `"btcusdt"`. Splits on first `@`. |
| `symbol_hash(data, len)` | FNV-1a hash of the `"s"` field value. Fast pattern scan without full JSON parse. Returns 0 if not found. |
| `ws_path(symbol, stream_type)` | Build single-stream WebSocket path: `"/ws/btcusdt@bookTicker"`. |
| `combined_ws_path(symbols, stream_type)` | Build multi-symbol combined stream path: `"/stream?streams=btcusdt@bookTicker/ethusdt@bookTicker"`. |
| `subscribe_message(symbols, stream_type, id)` | Build `{"method":"SUBSCRIBE","params":[...],"id":N}` JSON message. |
| `unsubscribe_message(symbols, stream_type, id)` | Build `{"method":"UNSUBSCRIBE","params":[...],"id":N}` JSON message. |

### Binance Depth Types (`eph::json::binance`)

| Symbol | Description |
|--------|-------------|
| `DepthLevel` | Price/quantity pair: `double price`, `double qty`. |
| `DepthSnapshot` | Orderbook snapshot: `int64_t last_update_id`, `vector<DepthLevel> bids` (descending), `vector<DepthLevel> asks` (ascending). |
| `ServerTime` | Server time: `int64_t server_time_ms` (milliseconds since epoch). |

### Binance REST Client (`eph::json::binance`)

| Symbol | Description |
|--------|-------------|
| `BinanceRestConfig` | Config struct: `string host` (default `"api.binance.com"`), `uint16_t port` (default 443), `chrono::milliseconds timeout` (default 5000ms). |
| `BinanceRestClient` | Typed REST client for public read-only endpoints. No authentication. Constructed with `BinanceRestConfig`. |
| `BinanceRestClient::kValidDepthLimits` | `constexpr array<int, 8>`: valid depth limit values (5, 10, 20, 50, 100, 500, 1000, 5000). |
| `BinanceRestClient::get_depth(symbol, limit)` | GET `/api/v3/depth`. Returns `expected<DepthSnapshot, string>`. Validates limit before request. |
| `BinanceRestClient::get_server_time()` | GET `/api/v3/time`. Returns `expected<ServerTime, string>`. |
| `parse_depth_response(body)` | Parse depth JSON body into `DepthSnapshot`. Public for testability. |
| `parse_server_time_response(body)` | Parse server time JSON body into `ServerTime`. Public for testability. |

### OKX Adapter (`eph::json::okx`)

| Symbol | Description |
|--------|-------------|
| `OkxPushMessage` | Zero-copy envelope: `channel`, `inst_id`, `data_raw`. Re-parses the nested `"arg"` object to extract channel and instId. |
| `OkxPushMessage::from(JsonView)` | Extract from parsed JSON. Returns `optional<OkxPushMessage>`. Requires arg and data fields. |
| `OkxBookTicker` | Zero-copy view of bbo-tbt data. Fields: `inst_id`, `bid_price`, `bid_qty`, `ask_price`, `ask_qty`, `timestamp_ms`. Navigates `data[0]` automatically. |
| `OkxBookTicker::from(JsonView)` | Extract from outer push message JSON. Returns `optional<OkxBookTicker>`. Requires instId, bidPx, bidSz, askPx, askSz. |
| `OkxBookTicker::mid_price()` | `(bid + ask) / 2` as `optional<double>`. |
| `OkxBookTicker::spread()` | `ask - bid` as `optional<double>`. |
| `inst_id_hash(data, len)` | FNV-1a hash of `"instId"` field value. Fast pattern scan without full JSON parse. Returns 0 if not found. |
| `subscribe_message(channel, inst_ids, id)` | Build `{"op":"subscribe","args":[{"channel":"...","instId":"..."},...],"id":"N"}` JSON. |
| `unsubscribe_message(channel, inst_ids, id)` | Build `{"op":"unsubscribe","args":[...],"id":"N"}` JSON. |

### Bybit Adapter (`eph::json::bybit`)

| Symbol | Description |
|--------|-------------|
| `BybitPushMessage` | Zero-copy envelope: `topic`, `type` ("snapshot"/"delta"), `data_raw`. |
| `BybitPushMessage::from(JsonView)` | Extract from parsed JSON. Returns `optional<BybitPushMessage>`. Requires topic, type, and data fields. |
| `BybitBookTicker` | Zero-copy view of tickers data. Fields: `symbol`, `bid_price`, `bid_qty`, `ask_price`, `ask_qty`, `last_price`, `timestamp_ms`. Re-parses nested data object. |
| `BybitBookTicker::from(JsonView)` | Extract from outer push message JSON. Returns `optional<BybitBookTicker>`. Requires symbol, bid1Price, bid1Size, ask1Price, ask1Size. |
| `BybitBookTicker::mid_price()` | `(bid + ask) / 2` as `optional<double>`. |
| `BybitBookTicker::spread()` | `ask - bid` as `optional<double>`. |
| `symbol_hash(data, len)` | FNV-1a hash of `"symbol"` field value. Fast pattern scan without full JSON parse. Returns 0 if not found. |
| `subscribe_message(channel, symbols, req_id)` | Build `{"op":"subscribe","args":["channel.SYMBOL",...],"req_id":"N"}` JSON. |
| `unsubscribe_message(channel, symbols, req_id)` | Build `{"op":"unsubscribe","args":["channel.SYMBOL",...],"req_id":"N"}` JSON. |

## Dependencies

- **eph-core** -- Error traits (`error_traits.hpp`), number parsing (`parse_number.hpp`), framer concept (`framer_concept.hpp`)
- **spdlog** -- Logging (adapters only, compile-time filtered via `SPDLOG_ACTIVE_LEVEL`)
- **eph-net** (optional) -- Required only when using `binance_rest.hpp` (HTTP client via `eph::net::HttpClient`)

## Usage Examples

### Parse raw JSON and extract fields

```cpp
#include <eph/json/parser.hpp>

// Zero-copy parse of a flat JSON object
const char* raw = R"({"price":"87245.30","qty":"1.5","side":"buy"})";
auto result = eph::json::parse(
    reinterpret_cast<const uint8_t*>(raw), std::strlen(raw));

if (!result) {
    // result.error() is a ParseError -- formattable via std::format
    std::println("parse failed: {}", result.error());
    return;
}

auto price = result->get_string("price");  // optional<string_view>("87245.30")
auto qty   = result->get_double("qty");    // optional<double>(1.5)
auto side  = result->get_string("side");   // optional<string_view>("buy")
auto miss  = result->get_string("foo");    // nullopt (key absent)

// Iterate all fields
for (size_t i = 0; i < result->field_count(); ++i) {
    auto& f = result->field_at(i);
    // f.key, f.value, f.is_string
}
```

### Binance bookTicker (WebSocket)

```cpp
#include <eph/json/parser.hpp>
#include <eph/json/adapters/binance.hpp>

auto result = eph::json::parse(data, len);
if (!result) { /* handle error */ }

// Extract typed BookTicker from a Binance bookTicker message
if (auto ticker = eph::json::binance::BookTicker::from(*result)) {
    auto sym = ticker->symbol;       // "BTCUSDT" (string_view into buffer)
    auto bid = ticker->bid_price;    // "87245.30" (string_view)
    auto mid = ticker->mid_price();  // optional<double> -- uses cached parse
    auto spr = ticker->spread();     // optional<double>
}

// Two-phase frame filter hash (no full JSON parse needed)
uint32_t hash = eph::json::binance::symbol_hash(data, len);
```

### Binance combined stream

```cpp
#include <eph/json/adapters/binance.hpp>

// Build subscription path for multiple symbols
std::array<std::string_view, 2> syms = {"btcusdt", "ethusdt"};
auto path = eph::json::binance::combined_ws_path(syms, "bookTicker");
// -> "/stream?streams=btcusdt@bookTicker/ethusdt@bookTicker"

auto sub_msg = eph::json::binance::subscribe_message(syms, "bookTicker", 1);
// -> {"method":"SUBSCRIBE","params":["btcusdt@bookTicker","ethusdt@bookTicker"],"id":1}

// Parse a combined stream wrapper
auto result = eph::json::parse(data, len);
if (auto cs = eph::json::binance::CombinedStream::from(*result)) {
    auto sym = cs->symbol;  // "btcusdt" (extracted from stream name)
    // Re-parse cs->data_raw for the inner message
    auto inner = eph::json::parse(
        reinterpret_cast<const uint8_t*>(cs->data_raw.data()),
        cs->data_raw.size());
    if (auto ticker = eph::json::binance::BookTicker::from(*inner)) {
        // ...
    }
}
```

### OKX bbo-tbt feed

```cpp
#include <eph/json/adapters/okx.hpp>

auto result = eph::json::parse(data, len);
if (auto push = eph::json::okx::OkxPushMessage::from(*result)) {
    if (push->channel == "bbo-tbt") {
        // OkxBookTicker::from navigates data[0] and re-parses automatically
        if (auto ticker = eph::json::okx::OkxBookTicker::from(*result)) {
            auto bid  = ticker->bid_price;    // "87000.0"
            auto inst = ticker->inst_id;      // "BTC-USDT"
            auto ts   = ticker->timestamp_ms; // int64_t ms since epoch
        }
    }
}

// Build subscription messages
std::array<std::string_view, 2> ids = {"BTC-USDT", "ETH-USDT"};
auto sub = eph::json::okx::subscribe_message("bbo-tbt", ids, 1);
```

### Bybit tickers feed

```cpp
#include <eph/json/adapters/bybit.hpp>

auto result = eph::json::parse(data, len);
if (auto push = eph::json::bybit::BybitPushMessage::from(*result)) {
    if (push->topic.starts_with("tickers.") && push->type == "snapshot") {
        // BybitBookTicker::from re-parses the nested data object
        if (auto ticker = eph::json::bybit::BybitBookTicker::from(*result)) {
            auto bid  = ticker->bid_price;    // "87000.0"
            auto last = ticker->last_price;   // "87000.5"
            auto mid  = ticker->mid_price();  // optional<double>
        }
    }
}

// Build subscription messages
std::array<std::string_view, 2> syms = {"BTCUSDT", "ETHUSDT"};
auto sub = eph::json::bybit::subscribe_message("tickers", syms, 1);
```

### Binance REST: orderbook snapshot recovery

```cpp
#include <eph/json/adapters/binance_rest.hpp>

// Default config: api.binance.com:443, 5s timeout
eph::json::binance::BinanceRestClient client;

auto depth = client.get_depth("BTCUSDT", 20);
if (depth) {
    // depth->last_update_id for sequencing with WebSocket depth updates
    for (auto& bid : depth->bids) {
        // bid.price (double), bid.qty (double)
    }
    for (auto& ask : depth->asks) {
        // ask.price (double), ask.qty (double)
    }
}

// Clock drift validation
auto time = client.get_server_time();
if (time) {
    auto drift_ms = local_time_ms - time->server_time_ms;
}
```
