# eph-json Usage Guide

How to parse exchange market data with ephemeral's zero-copy JSON parser and typed exchange adapters.

## Parser Overview

`eph::json::parse()` is a single-pass O(n) parser for flat JSON objects (no nested object/array support). It returns a `JsonView` — a zero-copy view with `string_view` fields pointing into the original buffer. No heap allocation.

Designed for the short, flat JSON payloads typical of crypto exchange WebSocket feeds (5-15 fields, mostly string/number values).

## Basic Parsing

```cpp
#include "eph/json/parser.hpp"

void on_message(const uint8_t* data, uint16_t len, uint8_t) {
    auto result = eph::json::parse(data, len);
    if (!result) {
        // ParseError: kIncomplete, kInvalidFormat, or kFieldOverflow
        SPDLOG_WARN("JSON parse failed: {}", result.error());
        return;
    }

    // Zero-copy field access — string_views into the original buffer
    auto price = result->get_string("p");   // std::optional<string_view>
    auto qty   = result->get_string("q");   // std::optional<string_view>
    auto time  = result->get_int("T");      // std::optional<int64_t>
    auto ratio = result->get_double("r");   // std::optional<double>
    auto flag  = result->get_bool("m");     // std::optional<bool>

    // Raw access (returns empty string_view if missing)
    std::string_view raw = result->get("s");

    // Check existence
    if (result->has("e")) { /* ... */ }

    // Iterate all fields
    for (size_t i = 0; i < result->field_count(); ++i) {
        const auto& f = result->field_at(i);
        // f.key, f.value, f.is_string
    }
}
```

**Limits**: Up to 32 fields per object (`JsonView::kMaxFields`). Nested objects/arrays are captured as opaque `string_view` values (not recursively parsed).

**Lifetime**: All `string_view` values point into the original `data` buffer. The buffer must outlive the `JsonView`.

## Exchange Adapters

Typed adapters extract strongly-typed structs from parsed `JsonView` objects. Each adapter handles the exchange-specific field naming and message wrapping.

| Exchange | Namespace | Header | Key Types |
|----------|-----------|--------|-----------|
| Binance  | `eph::json::binance` | `adapters/binance.hpp` | `BookTicker`, `CombinedStream` |
| OKX      | `eph::json::okx`     | `adapters/okx.hpp`     | `OkxBookTicker`, `OkxPushMessage` |
| Bybit    | `eph::json::bybit`   | `adapters/bybit.hpp`   | `BybitBookTicker`, `BybitPushMessage` |

### Binance BookTicker

```cpp
#include "eph/json/adapters/binance.hpp"

void on_message(const uint8_t* data, uint16_t len, uint8_t) {
    auto json = eph::json::parse(data, len);
    if (!json) return;

    auto ticker = eph::json::binance::BookTicker::from(*json);
    if (!ticker) return;

    // Zero-copy string_view fields
    auto symbol = ticker->symbol;       // "BTCUSDT"
    auto bid    = ticker->bid_price;    // "87245.30"
    auto ask    = ticker->ask_price;    // "87255.00"

    // Pre-parsed numeric helpers (cached from from())
    auto mid    = ticker->mid_price();  // std::optional<double>
    auto sprd   = ticker->spread();     // std::optional<double>

    // Integer fields
    int64_t update_id  = ticker->update_id;
    int64_t event_time = ticker->event_time;
}
```

### OKX BookTicker

OKX wraps data in `{"arg":{...},"data":[{...}]}`. Parse the outer wrapper first, then the inner data object.

```cpp
#include "eph/json/adapters/okx.hpp"

auto json = eph::json::parse(data, len);
if (!json) return;

if (auto push = eph::json::okx::OkxPushMessage::from(*json)) {
    if (push->channel == "bbo-tbt") {
        if (auto ticker = eph::json::okx::OkxBookTicker::from(*json)) {
            auto bid = ticker->bid_price;  // "87245.3"
            auto inst = ticker->inst_id;   // "BTC-USDT"
        }
    }
}
```

### Bybit BookTicker

Bybit uses topic-based wrapping: `{"topic":"tickers.BTCUSDT","type":"snapshot","data":{...}}`.

```cpp
#include "eph/json/adapters/bybit.hpp"

auto json = eph::json::parse(data, len);
if (!json) return;

if (auto push = eph::json::bybit::BybitPushMessage::from(*json)) {
    if (push->topic.starts_with("tickers.")) {
        if (auto ticker = eph::json::bybit::BybitBookTicker::from(*json)) {
            auto bid = ticker->bid_price;
        }
    }
}
```

## Subscription Helpers (Binance)

The Binance adapter provides helpers for building WebSocket subscription paths and messages.

```cpp
using namespace eph::json::binance;

// Single-stream path (subscription via URL)
auto path = ws_path("btcusdt", "bookTicker");
// -> "/ws/btcusdt@bookTicker"

// Combined-stream path (multiple symbols, one connection)
std::array syms = {
    std::string_view{"btcusdt"},
    std::string_view{"ethusdt"},
    std::string_view{"solusdt"},
};
auto combined = combined_ws_path(syms, "bookTicker");
// -> "/stream?streams=btcusdt@bookTicker/ethusdt@bookTicker/solusdt@bookTicker"

// Dynamic subscription after connection (JSON message)
auto sub_msg = subscribe_message(syms, "bookTicker", /*id=*/1);
// -> {"method":"SUBSCRIBE","params":["btcusdt@bookTicker",...],"id":1}

auto unsub_msg = unsubscribe_message(syms, "bookTicker", /*id=*/2);
```

## Combined Stream Parsing

When using combined streams, the outer wrapper contains `{"stream":"...","data":{...}}`. Use `CombinedStream` to unwrap, then parse the inner data.

```cpp
auto json = eph::json::parse(data, len);
if (!json) return;

auto cs = eph::json::binance::CombinedStream::from(*json);
if (!cs) return;

auto symbol = cs->symbol;     // "btcusdt" (extracted from stream name)
auto stream = cs->stream;     // "btcusdt@bookTicker"

// Re-parse the inner data object
auto inner = eph::json::parse(
    reinterpret_cast<const uint8_t*>(cs->data_raw.data()),
    cs->data_raw.size());
if (!inner) return;

auto ticker = eph::json::binance::BookTicker::from(*inner);
```

## Symbol Hash for Application-Layer Bucketing

For latest-per-symbol deduplication, use `symbol_hash()` in your message
handler (the previous `cfg.on_frame_filter` / `make_twophase_filter`
transport hook was retired in v3.3 / T3.19):

```cpp
#include "eph/json/adapters/binance.hpp"

stream->on_message = [&](std::span<const std::uint8_t> frame) {
    auto h = eph::json::binance::symbol_hash(frame.data(), frame.size());
    auto& slot = latest_per_bucket[h % N];
    slot.assign(frame.begin(), frame.end());  // keep only the latest
    // strategy reads `slot` on its own cadence
};
```

`symbol_hash` performs a fast pattern scan for the `"s":"..."` field
without a full JSON parse, suitable for the hot path.

## REST API (Binance)

For orderbook snapshot recovery (post-reconnect), use `BinanceRestClient`:

```cpp
#include "eph/json/adapters/binance_rest.hpp"

auto client = eph::json::binance::BinanceRestClient({});
auto depth = client.get_depth("BTCUSDT", 20);
if (depth) {
    for (auto& bid : depth->bids) {
        book.update_bid(bid.price, bid.qty);
    }
}
```

## Performance Notes

- **Zero allocation**: `parse()` uses a stack-allocated `std::array<Field, 32>`. No heap.
- **Linear scan lookup**: `get(key)` is O(n) with first-char + length pre-filter. Faster than hash maps for 5-15 fields due to cache locality.
- **Escape handling**: Backslash sequences are preserved as-is in `string_view` values (no unescaping). Sufficient for exchange data which does not use escapes in price/quantity fields.
- **LUT-based scanning**: Whitespace and value-terminator checks use 256-byte lookup tables instead of branch chains.
