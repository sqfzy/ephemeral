# Project: eph-json

> Header-only C++23 zero-copy JSON parser and typed exchange-adapter layer for Binance, OKX, and Bybit market-data feeds, plus a minimal Binance REST client for orderbook recovery and clock sync.

**Language**: C++23 | **Build**: xmake | **Kind**: header-only | **Namespace**: `eph::json`

---

## Table of Contents
1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Module Map](#module-map)
4. [Data Flow](#data-flow)
5. [Key Components](#key-components)
6. [Entry Points & APIs](#entry-points--apis)
7. [Dependencies](#dependencies)
8. [Testing](#testing)

---

## Overview

`eph-json` is a focused JSON library for one job: turning cryptocurrency-exchange WebSocket and REST messages into zero-copy typed views with as little CPU work as possible. It is NOT a general-purpose JSON library — it handles only the flat key-value shape used by the Binance / OKX / Bybit market-data feeds: a single outer object with string, number, boolean, and null scalars, plus nested objects or arrays captured as opaque substrings to be re-parsed on demand.

Inside the `ephemeral` HFT stack, `eph-json` sits between the transport layer (`eph-net` / `eph-transport` / `eph-dpdk`, which deliver framed WebSocket messages) and application logic (`eph-book`, trading strategies). The module provides four things:

1. A **core parser** (`parse()` → `JsonView`) that walks the bytes once, populating a fixed-size `std::array<Field, 32>` of `string_view` pairs. No heap allocation. Field lookup is O(n) linear scan with a first-char + length pre-filter, which outperforms hash maps for the 5–15 field messages typical in exchange payloads thanks to cache locality.
2. **Typed exchange adapters** — `binance::BookTicker` / `CombinedStream`, `okx::OkxPushMessage` / `OkxBookTicker`, `bybit::BybitPushMessage` / `BybitBookTicker` — each with a static `from(JsonView)` factory returning `optional<Struct>` when all required fields are present. Binance's `BookTicker` additionally exposes a fused single-pass `parse(data, len)` fast path that skips the generic `JsonView` intermediate (~2-3× faster on `@bookTicker` hot paths).
3. **Fast-path hash extractors** (`binance::symbol_hash`, `okx::inst_id_hash`, `bybit::symbol_hash`) that scan the raw bytes for a specific `"key":"value"` pattern and FNV-1a-hash the value — without invoking the full parser. These are designed to drive Transport's two-phase frame filter (latest-per-symbol deduplication).
4. A **Binance REST client** (`BinanceRestClient`) for two public endpoints: `GET /api/v3/depth` (orderbook snapshot, for post-reconnect recovery) and `GET /api/v3/time` (server clock, for drift validation). No authentication; order placement uses the FIX API (`eph-fix`) instead.

The parser itself has no logger and no observability — it is on the hot path. Adapters each log at DEBUG level (through a named `spdlog` logger) when a required field is missing. Verbosity is controlled at compile time via `SPDLOG_ACTIVE_LEVEL`, set to `SPDLOG_LEVEL_TRACE` in debug builds and `SPDLOG_LEVEL_INFO` otherwise.

---

## Architecture

Layered, header-only. Each layer depends only on layers below it. The core parser is hot-path code with hand-tuned LUTs and branch hints; the adapter layer is thin and composed from `JsonView` accessors.

### Component Diagram

```
        +----------------------------------------------+
        |          Application / Strategy              |
        |     (eph-book, trading logic, ws clients)    |
        +---------------------+------------------------+
                              | consumes typed structs
                              v
 +-------------------------------------------------------------+
 |                  Exchange Adapters                          |
 |  +--------------+  +-------------+  +--------------------+  |
 |  | binance.hpp  |  |  okx.hpp    |  |    bybit.hpp       |  |
 |  | BookTicker   |  | OkxPushMsg  |  | BybitPushMsg       |  |
 |  | CombinedStrm |  | OkxBookTckr |  | BybitBookTicker    |  |
 |  | symbol_hash  |  | inst_id_hash|  | symbol_hash        |  |
 |  | subscribe()  |  | subscribe() |  | subscribe()        |  |
 |  +--------------+  +-------------+  +--------------------+  |
 |                                                             |
 |  +---------------------+ +--------------------------+       |
 |  | binance_depth_types | |     binance_rest.hpp     |       |
 |  | DepthLevel / Snap   | | BinanceRestClient        |       |
 |  | ServerTime          | | parse_depth_response     |       |
 |  |                     | | parse_server_time_resp.  |       |
 |  +---------------------+ +------------+-------------+       |
 +--------------------------+-------------+--------------------+
                            | builds on   | builds on
                            v             v
       +-------------------------+   +----------------+
       |   Core parser +         |   |    eph-net     |
       |   framer                |   |  HttpClient    |
       |  +-------------------+  |   |  (HTTPS)       |
       |  |  parser.hpp       |  |   +----------------+
       |  |   parse() ->      |  |
       |  |   JsonView        |  |
       |  |   Field[kMaxField]|  |
       |  |   ParseError      |  |
       |  +---------+---------+  |
       |  +---------+---------+  |
       |  |  framer.hpp       |  |
       |  |  JsonFramer       |  |
       |  |  (pass-through)   |  |
       |  +-------------------+  |
       +------------+------------+
                    | uses
                    v
       +-------------------------+
       |         eph-core        |
       |  parse_int / parse_     |
       |  number / error_traits  |
       |  framer_concept         |
       +-------------------------+
```

---

## Module Map

| File | Responsibility | Key Types / Functions | Depends On |
|---|---|---|---|
| `include/eph/json.hpp` | Aggregate convenience header. | — | `parser.hpp`, `framer.hpp`, `adapters/binance.hpp` |
| `include/eph/json/parser.hpp` | Zero-copy flat-object parser, compile-time LUTs, field lookup. | `parse`, `JsonView`, `Field`, `ParseError`, `parse_error_name`, `kWsLut`, `kValTermLut`, `skip_ws`, `scan_string` | `eph::core::parse_int`, `eph::core::parse_number`, `eph::core::ErrorEnumFormatter` |
| `include/eph/json/framer.hpp` | Pass-through `MessageFramer` for JSON-over-WebSocket. | `JsonFramer` | `eph::net::MessageFramer` concept, `DecodedFrame`, `FrameError` |
| `include/eph/json/adapters/binance.hpp` | Binance WS typed adapters + URL / subscribe builders + `symbol_hash` + fused `BookTicker::parse`. | `BookTicker` (with `from(JsonView)` and fused `parse(data,len)`), `CombinedStream`, `extract_symbol`, `symbol_hash`, `ws_path`, `combined_ws_path`, `subscribe_message`, `unsubscribe_message`, `detail::binance_logger` | `parser.hpp`, `eph::core::parse_number`, `eph::core::parse_int`, `spdlog` |
| `include/eph/json/adapters/binance_depth_types.hpp` | Lightweight types shared by REST and `eph-book`, deliberately free of HTTP deps. | `DepthLevel`, `DepthSnapshot`, `ServerTime` | `<cstdint>`, `<vector>` |
| `include/eph/json/adapters/binance_rest.hpp` | Typed Binance REST client + free-function response parsers. | `BinanceRestConfig`, `BinanceRestClient`, `parse_depth_response`, `parse_server_time_response`, `detail::parse_depth_levels`, `detail::binance_rest_logger` | `parser.hpp`, `binance_depth_types.hpp`, `eph::net::HttpClient`, `eph::core::parse_number`, `spdlog` |
| `include/eph/json/adapters/okx.hpp` | OKX WS typed adapters for the `{"arg":..., "data":[...]}` envelope + `inst_id_hash` + subscribe builders. | `OkxPushMessage`, `OkxBookTicker`, `inst_id_hash`, `subscribe_message`, `unsubscribe_message`, `detail::first_array_element`, `detail::okx_logger` | `parser.hpp`, `eph::core::parse_number`, `eph::core::parse_int`, `spdlog` |
| `include/eph/json/adapters/bybit.hpp` | Bybit WS typed adapters for the `{"topic":..., "type":..., "data":{...}}` envelope + `symbol_hash` + subscribe builders. | `BybitPushMessage`, `BybitBookTicker`, `symbol_hash`, `subscribe_message`, `unsubscribe_message`, `detail::bybit_logger` | `parser.hpp`, `eph::core::parse_number`, `eph::core::parse_int`, `spdlog` |
| `tests/test_json.cpp` | Parser + `JsonView` unit tests. | gtest `TEST(...)` cases | `parser.hpp`, `gtest` |
| `tests/test_binance.cpp` | Binance adapter tests. | gtest | `adapters/binance.hpp` |
| `tests/test_okx.cpp` | OKX adapter tests. | gtest | `adapters/okx.hpp` |
| `tests/test_bybit.cpp` | Bybit adapter tests. | gtest | `adapters/bybit.hpp` |
| `benchmarks/bench_json_parse.cpp` | `parse`, `parse + from`, `symbol_hash` microbenchmarks on a representative Binance bookTicker payload. | `BM_JsonParse`, `BM_JsonParseAndExtract`, `BM_SymbolHash` | `google/benchmark`, `parser.hpp`, `adapters/binance.hpp` |
| `xmake.lua` | Header-only target + auto-registered tests/benchmarks under `eph-test` / `eph-bench` rules. | target `eph-json` | `eph-core`, `spdlog` |

---

## Data Flow

Two typical flows: a **WebSocket hot path** and a **REST recovery path**.

### WebSocket hot path

Raw bytes arrive from `eph-transport` (via either POSIX sockets in `eph-net` or DPDK user-space TCP in `eph-dpdk`). WebSocket framing has already delineated message boundaries, so each callback invocation hands `eph-json` exactly one complete JSON payload.

```
  [NIC / kernel socket]
          |
          v
   eph-transport (WS frame reassembly, control handling)
          |   raw JSON bytes (const uint8_t*, size_t)
          |
          |-- fast-path: symbol_hash(data, len) --> Transport two-phase
          |                                         frame filter (latest-
          |                                         per-symbol dedup)
          |
          |-- dedicated @bookTicker stream only:
          |   binance::BookTicker::parse(data, len) --> optional<BookTicker>
          |   (fused single-pass, skips JsonView intermediate, ~2-3× faster)
          |
          v
   eph::json::parse(data, len) ----> std::expected<JsonView, ParseError>
          |
          v
   JsonView (array of 32 Fields; O(n) linear lookup with first-char
             + length pre-filter)
          |
          v
   binance::BookTicker::from(json)  / okx::OkxBookTicker::from(json)
          |                          / bybit::BybitBookTicker::from(json)
          v
   Typed struct with string_views pointing BACK INTO the original buffer
          |
          v
   Application consumes mid_price() / spread() / raw fields
```

Lifetime: the original transport buffer must outlive the `JsonView` and every typed struct derived from it, because every `string_view` is a slice of that buffer.

### REST recovery path

After a WebSocket reconnect, `eph-book` (or similar) needs a fresh orderbook snapshot to seed the local book before applying incremental depth updates.

```
  BinanceRestClient::get_depth("BTCUSDT", 20)
          |
          | validates limit against kValidDepthLimits ({5,10,20,50,100,500,1000,5000})
          |
          v
  HttpClient::get("/api/v3/depth?symbol=...&limit=...")   [eph-net, HTTPS]
          |
          v
  HttpResponse { status_code, body }
          |
          v
  parse_depth_response(body)
          |   +-- eph::json::parse(body)
          |   +-- json.get_int("lastUpdateId")
          |   +-- detail::parse_depth_levels(json.get("bids"))   <- opaque array substring
          |   +-- detail::parse_depth_levels(json.get("asks"))
          |
          v
  DepthSnapshot { last_update_id, vector<DepthLevel> bids, vector<DepthLevel> asks }
```

`parse_depth_levels` does a tiny hand-rolled walk over `[["price","qty"], ...]` because the core parser deliberately does not descend into nested arrays — depth levels are the one well-defined exception that needs structured extraction.

---

## Key Components

### `eph::json::JsonView`

**File**: `include/eph/json/parser.hpp`
**Purpose**: Fixed-size zero-copy view over a flat JSON object.
**Interface**:
```
class JsonView {
    static constexpr size_t kMaxFields = 32;
    std::string_view                  get(std::string_view key) const noexcept;
    std::optional<std::string_view>   get_string(std::string_view key) const noexcept;
    std::optional<int64_t>            get_int(std::string_view key) const noexcept;
    std::optional<double>             get_double(std::string_view key) const noexcept;
    std::optional<bool>               get_bool(std::string_view key) const noexcept;
    bool                              has(std::string_view key) const noexcept;
    size_t                            field_count() const noexcept;
    const Field&                      field_at(size_t i) const noexcept;  // bounds-checked
};
```
**Notes**:
- `get()` returns empty `string_view` for BOTH missing and present-but-empty values; `get_string()` distinguishes them.
- `find_field()` uses a first-char + length pre-filter before full comparison — most mismatches short-circuit on a single branch.
- `field_at(i)` is bounds-checked: out-of-range returns a static empty `Field` rather than UB, hardening against off-by-one bugs.
- `parse()` is a `friend` so it can write directly into the internal array without exposing a public `push_back`.

### `eph::json::parse`

**File**: `include/eph/json/parser.hpp`
**Purpose**: Single-pass parser producing a `JsonView`.
**Interface**:
```
[[nodiscard]] std::expected<JsonView, ParseError>
parse(const uint8_t* data, size_t len) noexcept;
```
**Notes**:
- Walks the buffer once, skipping whitespace via `detail::kWsLut` (a 256-byte compile-time LUT) and scanning string contents via `detail::scan_string` (which handles `\` to avoid terminating prematurely on `\"`, but does NOT decode escape sequences — values are raw slices of the input).
- For numbers, booleans, and null, it scans until a terminator byte from `detail::kValTermLut` (`,`, `}`, or whitespace) — one LUT check per byte instead of four `||`-chained comparisons.
- Nested objects `{...}` and arrays `[...]` are captured as opaque substrings by counting brace/bracket depth while respecting quoted strings. Depth is capped at 64.
- Field overflow (more than `kMaxFields = 32` fields) returns `ParseError::kFieldOverflow`.
- `[[likely]]` / `[[unlikely]]` annotations bias the compiler's basic-block layout toward the common path: inside the field loop, most bytes are not `}`, most fields are not the first, and most values are strings.

### `eph::json::JsonFramer`

**File**: `include/eph/json/framer.hpp`
**Purpose**: Semantic identity framer so JSON-over-WebSocket Transport type aliases have a distinct framer type (rather than reusing `RawFramer`), while contributing zero overhead.
**Interface**:
```
class JsonFramer {
    static constexpr size_t max_overhead() noexcept;                                        // 0
    [[nodiscard]] size_t    encode(uint8_t* out, const uint8_t* data, size_t len, uint8_t) noexcept;
    [[nodiscard]] std::expected<eph::net::DecodedFrame, eph::net::FrameError>
                            decode(const uint8_t* data, size_t len) noexcept;
};
static_assert(eph::net::MessageFramer<JsonFramer>);
```
**Notes**:
- `encode` is an identity `memcpy`; it returns 0 (not an error) on null pointers or zero length.
- `decode` wraps the buffer as a single `DecodedFrame { msg_type = 0, is_control = false }` without touching the bytes.
- Designed to be used as the `Framer` template argument in Transport type aliases. Using it over raw TCP (without WebSocket above it) will treat each recv buffer as one message, which is almost certainly wrong.

### `eph::json::binance::BookTicker`

**File**: `include/eph/json/adapters/binance.hpp`
**Purpose**: Typed view of a Binance bookTicker push.
**Interface**:
```
struct BookTicker {
    std::string_view symbol, bid_price, bid_qty, ask_price, ask_qty;
    int64_t update_id, event_time, txn_time;
    std::optional<double> cached_bid, cached_ask;
    // Generic factory: navigate any JsonView (works for combined-stream inner data).
    static std::optional<BookTicker> from(const JsonView&) noexcept;
    // Fused single-pass factory: scan raw bytes once, dispatch on 1-char keys
    // directly into the target field. ~2-3× faster than from(parse(...)).
    static std::optional<BookTicker> parse(const uint8_t* data, size_t len) noexcept;
    std::optional<double> mid_price() const noexcept;
    std::optional<double> spread()    const noexcept;
};
```
**Notes**:
- `from()` reads `s`, `b`, `B`, `a`, `A` (required) and `u`, `E`, `T` (optional). Missing required fields are logged at DEBUG to `json.binance`.
- Bid/ask prices are pre-parsed once inside `from()` / `parse()` into `cached_bid` / `cached_ask`, so subsequent `mid_price()` / `spread()` calls are branch-light. This matters because hot loops compute mid/spread on every message.
- OKX / Bybit tickers do NOT cache prices — they re-parse on every call, as their hot path is less price-math-intensive.
- **`parse(data, len)` fused fast path**: specialised alternative for dedicated `@bookTicker` subscriptions where the payload shape is known a priori. Walks the buffer once with a 5-bit `required` mask over `s/b/B/a/A`; dispatches each 1-char key via a small switch straight into the target `BookTicker` field; multi-char keys (including future Binance additions like `e`) are silently skipped via `skip_value`. Returns `nullopt` on malformed JSON or any missing required bit. Field values are byte-for-byte identical to `from(parse(data,len))` — contract enforced by `BinanceBookTickerParse.MatchesFromFlow`. Prefer `parse()` on the dedicated hot path; use `from()` when navigating a combined-stream envelope whose outer JSON is already a `JsonView`.

### `eph::json::binance::symbol_hash` (and OKX / Bybit equivalents)

**File**: `include/eph/json/adapters/binance.hpp`, `.../okx.hpp`, `.../bybit.hpp`
**Purpose**: FNV-1a hash of the symbol/instId field, computed directly from raw bytes without invoking the core parser.
**Interface**:
```
// Binance
[[nodiscard]] uint32_t binance::symbol_hash(const uint8_t* data, size_t len) noexcept;
// OKX
[[nodiscard]] uint32_t okx::inst_id_hash(const uint8_t* data, size_t len) noexcept;
// Bybit
[[nodiscard]] uint32_t bybit::symbol_hash(const uint8_t* data, size_t len) noexcept;
```
**Notes**:
- All three fast-scan for a specific pattern (`"s":"`, `"instId":"`, `"symbol":"`), copy the following quoted value, and FNV-1a it.
- Return `0` if the pattern is not found — the Transport two-phase frame filter treats `0` as "don't dedup, deliver unconditionally".
- Not a general JSON lookup — it will match the first occurrence of the pattern, which is fine for crypto feeds where these fields are at known positions near the start of the message.

### `eph::json::binance::BinanceRestClient`

**File**: `include/eph/json/adapters/binance_rest.hpp`
**Purpose**: Typed REST client for Binance public read-only endpoints.
**Interface**:
```
struct BinanceRestConfig {
    std::string host = "api.binance.com";
    uint16_t port = 443;
    std::chrono::milliseconds timeout{5000};
    constexpr std::string_view validate() const noexcept;
    std::string dump() const;
    std::string to_json() const;
    std::vector<std::string> warnings() const;
    friend bool operator==(const BinanceRestConfig&, const BinanceRestConfig&) = default;
};

class BinanceRestClient {
    static constexpr std::array<int, 8> kValidDepthLimits = {5,10,20,50,100,500,1000,5000};
    explicit BinanceRestClient(Config = {});
    std::expected<DepthSnapshot, std::string> get_depth(std::string_view symbol, int limit = 20) noexcept;
    std::expected<ServerTime,    std::string> get_server_time() noexcept;
};
```
**Notes**:
- `get_depth` validates `limit` BEFORE making a network request and returns an error if it is not in `kValidDepthLimits` — Binance rejects other values, and failing early avoids a wasted round trip.
- `parse_depth_response` and `parse_server_time_response` are exposed as free functions so they can be unit-tested without standing up an HTTP mock.
- No authentication — only public endpoints. Order placement uses the FIX API (`eph-fix`).
- `BinanceRestConfig::warnings()` flags non-fatal misconfigurations (non-443 port, very short / very long timeout, unrecognized host) for operator inspection.

### `eph::json::okx::OkxPushMessage` / `OkxBookTicker`

**File**: `include/eph/json/adapters/okx.hpp`
**Purpose**: Navigate OKX's `{"arg":{...},"data":[{...}]}` envelope.
**Notes**:
- `OkxPushMessage::from()` re-parses the nested `arg` object to extract `channel` and `instId` for dispatch-by-channel.
- `OkxBookTicker::from()` calls `detail::first_array_element()` on `data`, which manually walks brace depth (respecting quoted strings and escapes) to find the first object in the `data` array, then re-parses it. This is a deliberate workaround — the core parser does not descend into arrays.
- Unlike Binance, OKX timestamps arrive as strings (`"ts":"1711612345678"`), so `timestamp_ms` is parsed via `detail::parse_string_int`.

### `eph::json::bybit::BybitPushMessage` / `BybitBookTicker`

**File**: `include/eph/json/adapters/bybit.hpp`
**Purpose**: Navigate Bybit's `{"topic":..., "type":..., "data":{...}}` envelope.
**Notes**:
- Bybit's `data` is a single object (not an array), so `BybitBookTicker::from()` just re-parses `data_raw` directly — no `first_array_element` helper needed.
- `type` is `"snapshot"` or `"delta"`. `lastPrice` and `ts` fields inside `data` are optional because they may be absent on delta updates.

---

## Entry Points & APIs

| Entrypoint | Type | Description |
|---|---|---|
| `eph::json::parse(data, len)` | Free function | Top-level parser, returns `expected<JsonView, ParseError>`. |
| `JsonView::get*` | Member accessors | Keyed field lookup returning `string_view` or typed `optional<T>`. |
| `eph::json::JsonFramer` | Class | Pass-through `MessageFramer` for JSON-over-WebSocket Transport type aliases. |
| `binance::BookTicker::from` / `CombinedStream::from` | Static factories | Typed-struct extraction from a `JsonView`. |
| `binance::BookTicker::parse(data, len)` | Static factory | Fused single-pass extraction from raw bookTicker bytes (no `JsonView` intermediate). ~2-3× faster than `from(parse(...))`. Same required/optional field set. |
| `okx::OkxPushMessage::from` / `OkxBookTicker::from` | Static factories | OKX envelope + bbo-tbt extraction. |
| `bybit::BybitPushMessage::from` / `BybitBookTicker::from` | Static factories | Bybit envelope + tickers extraction. |
| `binance::symbol_hash` / `okx::inst_id_hash` / `bybit::symbol_hash` | Free functions | Raw-byte FNV-1a extractors for two-phase frame filtering. |
| `binance::{ws_path, combined_ws_path, subscribe_message, unsubscribe_message}` | Free functions | Binance WebSocket URL and control-message builders. |
| `okx::{subscribe_message, unsubscribe_message}` | Free functions | OKX subscribe/unsubscribe JSON builders. |
| `bybit::{subscribe_message, unsubscribe_message}` | Free functions | Bybit subscribe/unsubscribe JSON builders. |
| `binance::BinanceRestClient::{get_depth, get_server_time}` | Member functions | REST endpoints. |
| `binance::{parse_depth_response, parse_server_time_response}` | Free functions | Response body parsers, public for testability. |

---

## Dependencies

### Internal (module graph)

```
     eph-json --> eph-core        (parse_int, parse_number,
         |                         framer_concept, error_traits,
         |                         ErrorEnumFormatter)
         |
         +--> eph-net             (HttpClient, MessageFramer concept,
         |                         DecodedFrame, FrameError)
         |    [only when including adapters/binance_rest.hpp
         |     or framer.hpp]
         |
         +--> spdlog              (adapters only; parser has no logger)
```

### External

| Package | Purpose | Scope |
|---|---|---|
| `spdlog` | Structured logging inside adapters. Verbosity compile-filtered via `SPDLOG_ACTIVE_LEVEL`. | adapters only |
| `gtest` | Unit test framework. | `tests/` |
| `google/benchmark` | Microbenchmark harness. | `benchmarks/` |

The `eph-json` xmake target itself declares `headeronly` kind, public deps on `eph-core` and public package `spdlog`, and adds `SPDLOG_ACTIVE_LEVEL` as a public define. The REST client transitively requires `eph-net` via the `HttpClient` include.

---

## Testing

| Test Suite | Location | Coverage Focus |
|---|---|---|
| `test_json` | `tests/test_json.cpp` | Core parser: happy path, missing braces / quotes / colons, field overflow, nested-object capture, escape handling, `get` vs `get_string`, `field_at` bounds checking, `parse_int` / `parse_double` / `parse_bool` semantics. |
| `test_binance` | `tests/test_binance.cpp` | `BookTicker::from` happy path + missing-required-field branches, `mid_price` / `spread`, **`BookTicker::parse` fused fast path** (`MatchesFromFlow` byte-equivalence contract, `OptionalFieldsMissing`, `MissingSymbolReturnsNullopt`, `MissingBidReturnsNullopt`, `UnknownFutureFieldIsSkipped`, `MalformedJsonReturnsNullopt`, `FieldReorderingPreservesValues`), `symbol_hash` stability / different-symbol distinctness / not-found zero, `extract_symbol`, `ws_path`, `combined_ws_path`, `subscribe_message` / `unsubscribe_message`, `CombinedStream::from`. |
| `test_okx` | `tests/test_okx.cpp` | `OkxBookTicker::from` happy / missing data / empty array / missing bidPx / missing timestamp, `mid_price`, `spread`, `OkxPushMessage::from` happy / trades channel / missing arg / missing data / missing channel / missing instId, `inst_id_hash` stability and distinctness, subscribe round-trip. |
| `test_bybit` | `tests/test_bybit.cpp` | `BybitBookTicker::from` happy / missing data / missing bid1Price / missing symbol / missing timestamp / missing last_price, `mid_price`, `spread`, `BybitPushMessage::from` happy / delta / orderbook topic / missing fields, `symbol_hash` stability and distinctness, subscribe helpers. |

Key test scenarios:
- **Boundary conditions** — zero-length buffer, buffer ending mid-key / mid-value / mid-string, nested structure at max depth, exactly `kMaxFields` fields vs `kMaxFields + 1`.
- **Escape handling** — strings containing `\"` should not terminate the scan.
- **Opaque nested values** — nested objects and arrays captured as raw slices, distinguishable by `Field::is_string == false`.
- **Hash stability** — same payload same hash; different symbol different hash; payload without the target pattern returns `0`.
- **Subscribe round-trip** — `subscribe_message` output can be parsed back and inspected.

Benchmark coverage:

| Benchmark | File | Measures |
|---|---|---|
| `BM_JsonParse` | `benchmarks/bench_json_parse.cpp` | `parse()` alone, reporting items/s and bytes/s. |
| `BM_JsonParseAndExtract` | `benchmarks/bench_json_parse.cpp` | `parse()` + `BookTicker::from()`, items/s. |
| `BM_BinanceBookTickerParse` | `benchmarks/bench_json_parse.cpp` | `BookTicker::parse(data,len)` fused single-pass fast path, items/s and bytes/s — direct competitor to `BM_JsonParseAndExtract`. |
| `BM_SymbolHash` | `benchmarks/bench_json_parse.cpp` | `symbol_hash()` on raw bytes, items/s — no parser in the loop. |

Baseline inputs are representative Binance `bookTicker` payloads. Per repo conventions (see `CLAUDE.md`), any parser or adapter change must re-run these benchmarks and verify no regression before being considered complete.
