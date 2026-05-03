# eph-json

Header-only C++23 library for zero-copy JSON parsing and typed adapter extraction, tuned for flat market-data payloads from cryptocurrency exchanges (Binance, OKX, Bybit). Part of the `ephemeral` HFT networking stack.

## Features

- **Zero-copy, zero-allocation parser** for flat JSON objects. Single-pass O(n) parse; field lookup is O(n) linear scan with a first-char + length pre-filter (faster than hashing for the 5–15 field messages typical in exchange feeds, thanks to cache locality).
- **Compile-time lookup tables** for whitespace skipping and value-terminator scanning — single indexed load per byte instead of chained comparisons.
- **Nested objects/arrays captured as opaque `string_view`s** — the core parser does not descend into them, allowing downstream code to re-parse only when needed.
- **Typed exchange adapters** for Binance (`BookTicker`, `CombinedStream`), OKX (`OkxPushMessage`, `OkxBookTicker`), and Bybit (`BybitPushMessage`, `BybitBookTicker`) that project raw JSON into structs with descriptive field names and cached parsed prices where applicable.
- **Fused single-pass `BookTicker::parse(data, len)` fast path** for Binance — skips the generic `JsonView` intermediate, dispatching each 1-char key directly into the target field. ~2-3× faster than `BookTicker::from(parse(data, len))` on representative payloads; byte-for-byte identical field values (verified by `BinanceBookTickerParse.MatchesFromFlow`). Designed for HFT hot paths with a priori-known stream type.
- **FNV-1a symbol hash extractors** (`binance::symbol_hash`, `okx::inst_id_hash`, `bybit::symbol_hash`) that fast-scan the raw bytes without invoking the full parser — designed for application-layer two-phase frame filters (latest-per-symbol deduplication).
- **Pass-through `JsonFramer`** satisfying `eph::net::MessageFramer`, for JSON-over-WebSocket consumers built on the legacy `MessageFramer` contract (the v3.3 stream stack uses `eph-codec/ws_codec` directly).
- **Binance REST client** (`BinanceRestClient`) for `/api/v3/depth` and `/api/v3/time` — post-reconnect orderbook snapshot recovery and clock drift validation.
- **`std::expected`-based error handling** throughout, with a `ParseError` enum formattable via `std::format`.
- **Observability**: each adapter uses a named `spdlog` logger (`json.binance`, `json.binance_rest`, `json.okx`, `json.bybit`); verbosity controlled at compile time via `SPDLOG_ACTIVE_LEVEL`.

All `string_view` members in parsed results point into the original input buffer. The caller must ensure that buffer outlives the view.

## Key Components

All headers live under `include/eph/json/`:

- **`parser.hpp`** — Zero-copy `JsonView` + `parse()` entry point. Handles strings (with escape awareness, but without escape-sequence normalization), numbers, booleans, null, and captures nested objects/arrays as opaque substrings. `JsonView::kMaxFields = 32`; additional fields trigger `ParseError::kFieldOverflow`. Nested-structure depth is capped at 64 inside the parser.
- **`framer.hpp`** — `JsonFramer` pass-through framer. `encode()` is a memcpy; `decode()` wraps the whole buffer as a single `DecodedFrame`. `max_overhead()` is always `0`. Asserts satisfaction of the `eph::net::MessageFramer` concept at compile time.
- **`adapters/binance.hpp`** — Binance WebSocket adapters: `BookTicker` (pre-caches parsed bid/ask into `cached_bid` / `cached_ask`), `CombinedStream` wrapper, `symbol_hash` FNV-1a pattern scanner, and URL / SUBSCRIBE / UNSUBSCRIBE builders.
- **`adapters/binance_depth_types.hpp`** — Lightweight `DepthLevel`, `DepthSnapshot`, `ServerTime`. Kept separate so consumers needing only the types (e.g., `eph-book`) can avoid transitively pulling in `eph-net` and the HTTP client.
- **`adapters/binance_rest.hpp`** — `BinanceRestClient` built on `eph::net::HttpClient`, plus free-function response parsers (`parse_depth_response`, `parse_server_time_response`) exposed for testability. Validates depth `limit` against Binance's accepted set (5, 10, 20, 50, 100, 500, 1000, 5000) before any network I/O.
- **`adapters/okx.hpp`** — OKX WebSocket adapters for the `{"arg":{...},"data":[{...}]}` envelope: `OkxPushMessage` re-parses `arg` for channel/instId, `OkxBookTicker` extracts the first element of `data[]` and parses it. `inst_id_hash` for two-phase filtering.
- **`adapters/bybit.hpp`** — Bybit WebSocket adapters for the `{"topic":..., "type":"snapshot|delta", "data":{...}}` envelope: `BybitPushMessage`, `BybitBookTicker` (re-parses the nested `data` object), and `symbol_hash` for two-phase filtering.

The aggregate header `json.hpp` includes the parser, framer, and Binance WebSocket adapter. Consumers wanting only the parser should include `eph/json/parser.hpp` directly to minimize compile-time dependencies.

## Requirements

- **Compiler**: C++23 (GCC 14+ or Clang with `-std=c++23`); uses `std::expected`, `std::format`, designated initializers, and `[[nodiscard]]` extensively.
- **Build system**: [xmake](https://xmake.io/).
- **Dependencies**:
  - `eph-core` — error traits (`error_traits.hpp`), number parsing (`parse_number.hpp`), framer concept (`framer_concept.hpp`).
  - `spdlog` — structured logging (adapters only; parser itself has no logger).
  - `eph-net` — only required when using `binance_rest.hpp` (provides `HttpClient`).
  - `gtest` — unit test framework (tests only).
  - `google/benchmark` — microbenchmarks only.

## Build

This subproject is built from the parent repo root:

```bash
# Build the header-only library target
xmake build eph-json

# Build every target in the repo (includes eph-json tests and benchmarks)
xmake build
```

## Test

```bash
# Run all eph-json tests
xmake run test_json       # core parser tests
xmake run test_binance    # Binance adapter tests
xmake run test_okx        # OKX adapter tests
xmake run test_bybit      # Bybit adapter tests
```

## Benchmark

```bash
xmake run bench_json_parse
```

Microbenchmarks cover four scenarios over a representative Binance bookTicker payload: (1) `parse()` alone, (2) `parse()` + `BookTicker::from()` extraction, (3) `BookTicker::parse()` fused single-pass extraction (direct competitor to scenario 2), (4) `symbol_hash()` on the raw bytes.

## Project Structure

```
eph-json/
├── include/eph/
│   ├── json.hpp                     # Aggregate header (parser + framer + binance)
│   └── json/
│       ├── parser.hpp               # Core zero-copy parser + JsonView + ParseError
│       ├── framer.hpp               # JsonFramer pass-through MessageFramer
│       └── adapters/
│           ├── binance.hpp          # Binance WS: BookTicker, CombinedStream, symbol_hash, ws_path, subscribe
│           ├── binance_depth_types.hpp  # DepthLevel, DepthSnapshot, ServerTime
│           ├── binance_rest.hpp     # BinanceRestClient (depth + server time)
│           ├── okx.hpp              # OKX WS: OkxPushMessage, OkxBookTicker, inst_id_hash
│           └── bybit.hpp            # Bybit WS: BybitPushMessage, BybitBookTicker, symbol_hash
├── tests/
│   ├── test_json.cpp                # Parser + JsonView tests
│   ├── test_binance.cpp             # Binance adapter tests
│   ├── test_okx.cpp                 # OKX adapter tests
│   └── test_bybit.cpp               # Bybit adapter tests
├── benchmarks/
│   └── bench_json_parse.cpp         # google/benchmark microbenchmarks
└── xmake.lua                        # Header-only target + test/bench rules
```

## Public API Reference

### Parser (`eph::json`)

| Symbol | Description |
|--------|-------------|
| `parse(const uint8_t* data, size_t len)` | Parse a flat JSON object into a `JsonView`. Returns `std::expected<JsonView, ParseError>`. |
| `ParseError` | `kIncomplete` (no closing brace / truncated), `kInvalidFormat` (missing quotes/colons, etc.), `kFieldOverflow` (> `kMaxFields`). |
| `parse_error_name(ParseError)` / `error_name(ParseError)` | Human-readable `string_view` name. |
| `Field` | `{ string_view key; string_view value; bool is_string; }`. |
| `JsonView` | Zero-copy view. `kMaxFields = 32`. |
| `JsonView::get(key)` | Raw value as `string_view` (empty if missing; empty also possible for legitimately empty values). |
| `JsonView::get_string(key)` | `optional<string_view>` (nullopt if the key is absent). |
| `JsonView::get_int(key)` | `optional<int64_t>` via `eph::core::parse_int`. |
| `JsonView::get_double(key)` | `optional<double>` via `eph::core::parse_number`. |
| `JsonView::get_bool(key)` | `optional<bool>` — accepts literal `true`/`false` only. |
| `JsonView::has(key)` | Key presence check. |
| `JsonView::field_count()` / `field_at(i)` | Positional iteration helpers. `field_at(i)` returns a static empty `Field` when `i` is out of bounds. |

### Framer (`eph::json`)

| Symbol | Description |
|--------|-------------|
| `JsonFramer` | Pass-through framer satisfying `eph::net::MessageFramer`. |
| `JsonFramer::max_overhead()` | Always `0`. |
| `JsonFramer::encode(out, data, len, msg_type)` | Identity memcpy. Returns `len`, or `0` if any pointer is null or `len == 0`. |
| `JsonFramer::decode(data, len)` | Wraps the buffer as a single `DecodedFrame{ msg_type = 0, is_control = false }`. Returns `FrameError::kIncomplete` if `len == 0`. |

### Binance WebSocket Adapter (`eph::json::binance`)

| Symbol | Description |
|--------|-------------|
| `BookTicker` | Typed view of a bookTicker push. String views for `symbol`/`bid_price`/`bid_qty`/`ask_price`/`ask_qty`; integer `update_id`/`event_time`/`txn_time`; `cached_bid`/`cached_ask` populated by `from()` or `parse()`. |
| `BookTicker::from(JsonView)` | Factory. Requires `s`, `b`, `B`, `a`, `A`; optional `u`, `E`, `T`. |
| `BookTicker::parse(const uint8_t* data, size_t len)` | Fused single-pass factory — scans payload once and dispatches each 1-char key directly into the target field without the `JsonView` intermediate. Same required/optional set as `from()`. ~2-3× faster on bookTicker payloads (see `BM_BinanceBookTickerParse`). Unknown fields (multi-char keys, or future additions) are silently skipped. Returns `nullopt` on malformed JSON or any missing required field. |
| `BookTicker::mid_price()` / `spread()` | `optional<double>`; uses cached values when available. |
| `CombinedStream` | Typed view of `{"stream":..., "data":{...}}`. `data_raw` is the opaque inner object (caller re-parses it). |
| `CombinedStream::from(JsonView)` | Factory; requires `stream` and non-empty `data`. |
| `extract_symbol(stream)` | Splits on first `'@'`: `"btcusdt@bookTicker"` → `"btcusdt"`. Returns the full input when no `'@'` is present. |
| `symbol_hash(data, len)` | Fast-scans for `"s":"` and FNV-1a hashes the value. Returns `0` if not found. |
| `ws_path(symbol, stream_type)` | Builds `"/ws/<sym>@<stream>"`. |
| `combined_ws_path(symbols, stream_type)` | Builds `"/stream?streams=<sym1>@<s>/<sym2>@<s>/..."`. |
| `subscribe_message(symbols, stream_type, id)` | `{"method":"SUBSCRIBE","params":[...],"id":N}`. |
| `unsubscribe_message(symbols, stream_type, id)` | `{"method":"UNSUBSCRIBE","params":[...],"id":N}`. |

### Binance Depth Types (`eph::json::binance`)

| Symbol | Description |
|--------|-------------|
| `DepthLevel` | `{ double price; double qty; }`. |
| `DepthSnapshot` | `{ int64_t last_update_id; vector<DepthLevel> bids, asks; }`. |
| `ServerTime` | `{ int64_t server_time_ms; }`. |

### Binance REST response parsers (`eph::json::binance`)

The legacy `BinanceRestClient` HTTPS wrapper was removed along with
`eph-transport`. Callers perform the GET via their own
`eph-net-kernel` + TLS stack and feed the raw response body into the
pure-function parsers below — same single-pass `parser.hpp` engine
the WebSocket adapters use.

| Symbol | Description |
|--------|-------------|
| `parse_depth_response(body)` | Parse the response body of `GET /api/v3/depth`. Returns `expected<DepthSnapshot, string>` with `last_update_id` + parsed bid/ask `vector<DepthLevel>`. |
| `parse_server_time_response(body)` | Parse the response body of `GET /api/v3/time`. Returns `expected<ServerTime, string>` with `server_time_ms`. |
| `parse_depth_levels(...)` | Helper used by `parse_depth_response`; exported for tests. |

### OKX Adapter (`eph::json::okx`)

| Symbol | Description |
|--------|-------------|
| `OkxPushMessage` | `{ channel, inst_id, data_raw }`. `from()` re-parses the nested `arg` to extract `channel` and `instId`. |
| `OkxBookTicker` | `{ inst_id, bid_price, bid_qty, ask_price, ask_qty, timestamp_ms }`. `from()` pulls `data[0]` and re-parses it. |
| `OkxBookTicker::mid_price()` / `spread()` | On-demand string→double parse each call (no cache). |
| `inst_id_hash(data, len)` | Fast-scans for `"instId":"` and FNV-1a hashes the value. |
| `subscribe_message(channel, inst_ids, id)` | `{"op":"subscribe","args":[{"channel":...,"instId":...},...],"id":"N"}`. |
| `unsubscribe_message(channel, inst_ids, id)` | `{"op":"unsubscribe","args":[...],"id":"N"}`. |

### Bybit Adapter (`eph::json::bybit`)

| Symbol | Description |
|--------|-------------|
| `BybitPushMessage` | `{ topic, type, data_raw }` (`type` is `"snapshot"` or `"delta"`). |
| `BybitBookTicker` | `{ symbol, bid_price, bid_qty, ask_price, ask_qty, last_price, timestamp_ms }`. `from()` re-parses the nested `data` object. `last_price` and `ts` are optional (may be absent in deltas). |
| `BybitBookTicker::mid_price()` / `spread()` | On-demand string→double parse each call (no cache). |
| `symbol_hash(data, len)` | Fast-scans for `"symbol":"` and FNV-1a hashes the value. |
| `subscribe_message(channel, symbols, req_id)` | `{"op":"subscribe","args":["<channel>.<SYMBOL>",...],"req_id":"N"}`. |
| `unsubscribe_message(channel, symbols, req_id)` | `{"op":"unsubscribe","args":[...],"req_id":"N"}`. |

## Usage Examples

### Parse raw JSON and extract fields

```cpp
#include <cstring>
#include <print>
#include <eph/json/parser.hpp>

const char* raw = R"({"price":"87245.30","qty":"1.5","side":"buy"})";
auto result = eph::json::parse(
    reinterpret_cast<const uint8_t*>(raw), std::strlen(raw));

if (!result) {
    // ParseError is std::format-able via the provided formatter specialization.
    std::println("parse failed: {}", result.error());
    return;
}

auto price = result->get_string("price");  // optional<string_view>("87245.30")
auto qty   = result->get_double("qty");    // optional<double>(1.5)
auto side  = result->get_string("side");   // optional<string_view>("buy")
auto miss  = result->get_string("foo");    // nullopt

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

if (auto ticker = eph::json::binance::BookTicker::from(*result)) {
    auto sym = ticker->symbol;       // "BTCUSDT" (string_view into buffer)
    auto bid = ticker->bid_price;    // "87245.30"
    auto mid = ticker->mid_price();  // optional<double> — uses cached parse
    auto spr = ticker->spread();     // optional<double>
}

// Two-phase filter hash — no full JSON parse needed
uint32_t h = eph::json::binance::symbol_hash(data, len);
```

### Binance bookTicker — fused single-pass fast path

When the stream is known a priori to be bookTicker (e.g. a direct
`/ws/<sym>@bookTicker` subscription), skip the generic `JsonView` intermediate:

```cpp
#include <eph/json/adapters/binance.hpp>

// Same required/optional fields as BookTicker::from(). Returns nullopt
// on any missing required field (s/b/B/a/A) or malformed JSON.
if (auto ticker = eph::json::binance::BookTicker::parse(data, len)) {
    auto bid = ticker->bid_price;    // string_view into the input buffer
    auto mid = ticker->mid_price();  // cached_bid/cached_ask pre-populated
}
```

Field values are byte-for-byte identical to
`BookTicker::from(parse(data, len))` (contract verified by
`BinanceBookTickerParse.MatchesFromFlow`). Unknown fields (multi-char keys,
or future Binance additions) are silently skipped — existing callers keep
working across additive schema changes.

### Binance combined stream

```cpp
#include <array>
#include <eph/json/adapters/binance.hpp>

std::array<std::string_view, 2> syms = {"btcusdt", "ethusdt"};
auto path = eph::json::binance::combined_ws_path(syms, "bookTicker");
// -> "/stream?streams=btcusdt@bookTicker/ethusdt@bookTicker"

auto sub_msg = eph::json::binance::subscribe_message(syms, "bookTicker", 1);

auto result = eph::json::parse(data, len);
if (auto cs = eph::json::binance::CombinedStream::from(*result)) {
    auto sym = cs->symbol;  // "btcusdt" extracted from stream name
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
        if (auto ticker = eph::json::okx::OkxBookTicker::from(*result)) {
            auto bid  = ticker->bid_price;     // "87000.0"
            auto inst = ticker->inst_id;       // "BTC-USDT"
            auto ts   = ticker->timestamp_ms;  // int64_t ms since epoch
        }
    }
}
```

### Bybit tickers feed

```cpp
#include <eph/json/adapters/bybit.hpp>

auto result = eph::json::parse(data, len);
if (auto push = eph::json::bybit::BybitPushMessage::from(*result)) {
    if (push->topic.starts_with("tickers.") && push->type == "snapshot") {
        if (auto ticker = eph::json::bybit::BybitBookTicker::from(*result)) {
            auto bid  = ticker->bid_price;   // "87000.0"
            auto last = ticker->last_price;  // "87000.5"
            auto mid  = ticker->mid_price();
        }
    }
}
```

### Binance REST: orderbook snapshot recovery

```cpp
#include <eph/json/adapters/binance_rest.hpp>

eph::json::binance::BinanceRestClient client;  // defaults to api.binance.com:443

auto depth = client.get_depth("BTCUSDT", 20);
if (depth) {
    // Use depth->last_update_id to align with WebSocket depth updates
    for (auto& bid : depth->bids) { /* bid.price, bid.qty */ }
    for (auto& ask : depth->asks) { /* ask.price, ask.qty */ }
}

auto now = client.get_server_time();
if (now) {
    auto drift = local_ms - now->server_time_ms;
}
```

## Design Notes

- **Why linear field lookup?** For 5–15 fields, a small contiguous array with a first-char + length pre-filter beats hash-map lookup (which would incur pointer chasing and a hash computation). Confirmed by benchmarks on representative Binance / OKX payloads.
- **Why byte-at-a-time string scanning?** Typical key/value strings are 1–10 bytes. SIMD (`memchr`) call overhead dominates the body for strings this short.
- **Escape sequence handling.** The parser recognizes `\` so the string scanner doesn't terminate prematurely on `\"`, but it does NOT normalize `\n`, `\uXXXX`, etc. — the field value is a raw slice of the input. Callers needing strict RFC 8259 normalization must re-process the slice.
- **Nested objects/arrays** are captured as opaque `string_view`s spanning `{...}` or `[...]`. Parsing depth is bounded at 64 to prevent stack/iteration runaway on pathological inputs.
- **`get()` vs `get_string()`.** `get()` returns an empty `string_view` for BOTH missing keys and present-but-empty values; `get_string()` distinguishes them with `optional`.
- **`CombinedStream::from()` requires a non-empty `data_raw`.** An empty nested object (`"data":{}`) would be reported as missing; this is an intentional simplification for HFT payloads, which never have empty `data`.
- **Price caching in `BookTicker`.** Prices are pre-parsed once inside `BookTicker::from()` / `BookTicker::parse()` into `cached_bid`/`cached_ask`, so `mid_price()` / `spread()` are branch-light even when called repeatedly per message. `OkxBookTicker` and `BybitBookTicker` do NOT cache — their prices are re-parsed on every `mid_price()`/`spread()` call.
- **`BookTicker::from()` vs `BookTicker::parse()`.** The `from(JsonView)` flow is generic — it works against any `JsonView` regardless of how it was produced (including a combined-stream inner `data`). `parse(data, len)` is specialised: a single pass over the raw bytes dispatching on 1-char keys, ~2-3× faster for dedicated `@bookTicker` subscriptions where the payload shape is known. Use `parse()` on the dedicated hot path; use `from()` when navigating a combined-stream envelope (where the outer JSON has already been parsed into a `JsonView`).

## License

See repository root.
