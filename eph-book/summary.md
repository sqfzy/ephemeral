# Project: eph-book

> Header-only C++23 order-book maintenance and market-microstructure signal
> library. Provides a fixed-capacity L2 `ArrayBook`, a dynamic-depth L3 `MapBook`,
> a family of generic signal calculators, and feed-specific adapters for
> Binance `bookTicker` / REST depth and NASDAQ ITCH 5.0 order events.

**Language**: C++23 | **Build**: xmake | **Kind**: header-only | **Namespace**: `eph::book`

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
9. [Benchmarks](#benchmarks)
10. [Observability](#observability)

---

## Overview

`eph-book` is the **order-book layer** of the `eph` HFT / market-data stack. It
sits between the feed parsers (`eph-json` for Binance JSON, `eph-itch` for
NASDAQ ITCH) and downstream signal / strategy code. The module is strictly
header-only: there is no `.cpp` in `include/`, and every function is either
`inline` or a template.

It provides four kinds of artefact, each a drop-in header:

1. **Two book implementations, one query interface.**
   - `ArrayBook<MaxLevels>` — fixed-capacity (5-20 levels typical), zero-allocation,
     sorted `std::array` storage. Best-price at index 0 on both sides. Hot-path
     optimal for crypto L2 and equity top-of-book feeds.
   - `MapBook` — dynamic depth (1000+ levels), `std::map`-backed, `O(log n)`
     insert/delete, `O(1)` BBO via `begin()` / `rbegin()`. Prices quantized to
     `1e-9` on insert to defuse floating-point rounding drift.
   Both expose the same BBO and depth-query methods, so signal functions and
   downstream code can be generic over either.
2. **Signal calculators** (`signals.hpp`) — pure function templates for order
   imbalance, weighted mid / microprice, spread in bps, VWAP, and depth ratio.
   `noexcept`, allocation-free, generic over any book exposing `best_bid()` /
   `best_ask()` / `total_bid_qty()` / `total_ask_qty()`.
3. **Feed adapters** — `BinanceBookAdapter<MaxLevels>` translates Binance
   WebSocket `bookTicker` BBO updates and REST depth snapshots into
   `ArrayBook`, tracking `last_update_id` for gap detection during reconnect.
   `ItchBookBuilder<MaxLevels>` aggregates order-level ITCH 5.0 events into an
   L2 `ArrayBook` with an internal order map and per-price accumulators for
   `O(1)` incremental updates.
4. **Canonical `PriceLevel`** — the shared `(price, qty)` pair used by every
   component and accepted by signals via `std::span<const PriceLevel>`.

The module does **not** own transport, parsing, or strategy logic. It ingests
parsed messages (from `eph-json` or `eph-itch`) and exposes a clean queryable
book plus derived signals.

---

## Architecture

Strictly layered, header-only. Each layer depends only on layers below it.

### Component Diagram

```
  +--------------+  +--------------+
  |  eph-json    |  |  eph-itch    |   (feed parsers — not linked by eph-book)
  |  (Binance)   |  |  (NASDAQ)    |
  +------+-------+  +-------+------+
         | parsed msg       | parsed msg
         v                  v
  +-----------------+ +------------------+
  | BinanceBook     | | ItchBookBuilder  |   (adapters — include/eph/book/)
  | Adapter<N>      | | <N>              |   feed-specific translation
  +--------+--------+ +--------+---------+
           |                   | aggregates into
           v                   v
  +--------------------------------------+
  |     ArrayBook<N>  |   MapBook        |   (core books — shared query surface)
  |  fixed-capacity   |  dynamic-depth   |
  |  std::array       |  std::map        |
  +------------------+-------------------+
                     | best_bid() / best_ask()
                     | total_bid_qty() / total_ask_qty()
                     | bids() / asks()
                     v
  +--------------------------------------+
  |            signals.hpp               |   (pure-function signal calculators)
  | order_imbalance, weighted_mid,       |
  | microprice, spread_bps, vwap,        |
  | depth_ratio                          |
  +--------------------------------------+
```

### Design Decisions

- **Two books, one query interface.** Generic signal code over any book type
  with the expected accessors. Pick `ArrayBook` for shallow + cache-hot; pick
  `MapBook` for deep + mutable.
- **Two update styles, one update API.** `update_bid(price, qty)` /
  `update_ask(price, qty)` accept both crypto-style explicit L2 snapshots *and*
  (after external order-to-level aggregation) ITCH-style order-level feeds. The
  adapters own the feed-specific translation.
- **Header-only, zero dispatch.** No virtuals, no abstract bases. Books are
  plain value types; signal functions are templates.
- **Allocation-free on the hot path** for `ArrayBook` and all signal
  functions. `MapBook::update_*` allocates per insert (map nodes), which is
  why it coexists with `ArrayBook` rather than replacing it.
- **Named loggers, compile-time filtered.** Each component owns a lazily
  constructed `spdlog` logger (`book.array_book`, `book.map_book`,
  `book.binance_adapter`, `book.itch_adapter`). Levels compile out via
  `SPDLOG_ACTIVE_LEVEL`.

---

## Module Map

```
eph-book/
├── include/eph/
│   ├── book.hpp                    # Convenience header (pulls ArrayBook + MapBook)
│   └── book/
│       ├── array_book.hpp          # PriceLevel, ArrayBook<MaxLevels>
│       ├── map_book.hpp            # MapBook (std::map-backed)
│       ├── signals.hpp             # order_imbalance, weighted_mid, microprice,
│       │                           # spread_bps, vwap, depth_ratio
│       ├── binance_adapter.hpp     # BinanceBookAdapter<MaxLevels>  (needs eph-json)
│       └── itch_adapter.hpp        # Order, ItchBookBuilder<MaxLevels>  (needs eph-itch)
├── tests/
│   ├── test_array_book.cpp         # 504 lines — sort invariants, eviction, NaN, edges
│   ├── test_map_book.cpp           # 537 lines — deep-book + ArrayBook cross-validation
│   └── test_signals.cpp            # 300 lines — every signal across both book types
├── benchmarks/
│   ├── bench_array_book.cpp        # bookTicker-driven micro-benchmarks
│   └── bench_map_book.cpp          # deep-book inserts + BBO queries
├── docs/
│   └── ONBOARDING.md               # Developer onboarding guide
├── README.md
├── CHANGELOG.md
├── summary.md                      # (this file)
└── xmake.lua                       # header-only target + test/bench glob loops
```

### File Responsibilities

| File | Symbols | Role |
|---|---|---|
| `array_book.hpp` | `PriceLevel`, `ArrayBook<N>` | L2 fixed-capacity sorted-array book; the canonical `PriceLevel` shape lives here. |
| `map_book.hpp` | `MapBook` | L3 dynamic-depth book; re-uses `PriceLevel`. Prices quantized to `1e-9`. |
| `signals.hpp` | `order_imbalance`, `weighted_mid`, `microprice`, `spread_bps`, `vwap`, `depth_ratio` | Pure signal calculators templated on the book type. |
| `binance_adapter.hpp` | `BinanceBookAdapter<N>` | Binance `bookTicker` + REST depth → `ArrayBook`. |
| `itch_adapter.hpp` | `Order`, `ItchBookBuilder<N>` | ITCH 5.0 order events → `ArrayBook` with internal order map and per-price accumulators. |
| `book.hpp` | (includes) | Convenience aggregate pulling `ArrayBook` + `MapBook`. |

---

## Data Flow

### Crypto L2 flow (Binance)

```
  WebSocket frame (WsCodec from eph-net-*)
        │
        ▼  decoded JSON bytes
  eph::json::binance::BookTicker::from(JsonView)
        │
        ▼  typed BookTicker (string_view fields)
  BinanceBookAdapter<N>::update_from_ticker(BookTicker)
        │  parses the 4 string fields via eph::core::parse_number
        ▼
  ArrayBook<N>::update_bid / update_ask
        │
        ▼  signal queries (generic over book type)
  signals::order_imbalance / weighted_mid / spread_bps / ...
```

Reconnect / gap-recovery:

```
  REST GET /api/v3/depth  (eph::json::binance::parse_depth_response)
        │
        ▼  DepthSnapshot { last_update_id, bids[], asks[] }
  BinanceBookAdapter::load_snapshot(snapshot)
        │  clears the book, inserts every level, remembers last_update_id
        ▼
  ArrayBook populated; caller filters subsequent bookTicker updates
  using U <= last_update_id().
```

### Equity L3 flow (ITCH 5.0)

```
  Raw ITCH message stream (eph::itch::Framer)
        │
        ▼  MessageView { msg_type, data }
  ItchBookBuilder<N>::process(MessageView)
        │  dispatch on msg_type → handle_* helper
        │
        ├── 'A'/'F' AddOrder      → orders_[ref] = {price, qty, side}; add_qty()
        ├── 'E'/'C' OrderExecuted → clamp vs remaining; sub_qty()
        ├── 'X' OrderCancel       → clamp vs remaining; sub_qty()
        ├── 'D' OrderDelete       → sub_qty(full remaining); erase(ref)
        └── 'U' OrderReplace      → sub_qty(old); erase(old); add_qty(new)
        │
        ▼  per-price accumulators (std::map<price, total_qty>)
  ArrayBook<N>::update_bid / update_ask with aggregated level qty
        │
        ▼
  Queries / signals (same interface as Binance path)
```

---

## Key Components

### `PriceLevel` (defined in `array_book.hpp`)

```cpp
struct PriceLevel {
    double price = 0.0;  // price in native exchange units
    double qty   = 0.0;  // aggregate quantity at this price
};
```

Canonical level representation shared by every book and accepted by signals
via `std::span<const PriceLevel>`.

### `ArrayBook<MaxLevels = 20>`

Fixed-capacity sorted-array book. Invariants maintained on every mutation:

1. `bids_[0..bid_count_)` sorted **descending** by price (best bid at index 0).
2. `asks_[0..ask_count_)` sorted **ascending** by price (best ask at index 0).
3. `bid_count_`, `ask_count_` ∈ `[0, MaxLevels]`.

Mutation semantics in `update_side()` (private core):

- NaN price → WARN log + no-op.
- Negative qty → WARN log + clamped to 0 (treated as removal).
- Existing level + `qty > 0` → in-place update.
- Existing level + `qty <= 0` → shift-remove, decrement count.
- New price + `qty > 0` + book has room → shift-insert at sorted position.
- New price + `qty > 0` + book full + price worse than worst → silently drop.
- New price + `qty > 0` + book full + price better → insert at sorted
  position; the old worst level is evicted.

Prices are compared with a fixed epsilon of `1e-12`. Use `MapBook` if your
feed has sub-1e-12 tick resolution (`MapBook` quantizes to `1e-9`).

All mutators are `noexcept`. All accessors are `[[nodiscard]]`. The class is
trivially relocatable and safe in pre-allocated arenas.

### `MapBook`

Dynamic-depth book backed by:
- `std::map<double, double, std::greater<>> bids_` — sorted descending.
- `std::map<double, double> asks_` — sorted ascending.

BBO is `O(1)` via `begin()` on each map. Depth queries are `O(n)`.

Prices are quantized via `quantize(p) = round(p / 1e-9) * 1e-9` before use as
a map key. This defuses the common failure mode of two feeds reporting the
same price via different floating-point rounding paths and producing distinct
map keys.

`bids()` / `asks()` return owning `std::vector<PriceLevel>` copies (not
spans) because `std::map` is node-based. For the hot path, prefer
`top_bids(n)` / `top_asks(n)` which stop at N.

### `ItchBookBuilder<MaxLevels>`

Owns:

- `ArrayBook<MaxLevels> book_` — the public L2 book.
- `std::unordered_map<uint64_t, Order> orders_` — live order state keyed by
  ITCH `order_ref` (64-bit), each entry holding `{price, remaining_qty, side}`.
- `std::map<double, double> bid_qty_`, `ask_qty_` — per-price aggregated
  quantity so add/execute/cancel on a single order is `O(1)` against the L2
  book (no need to recompute the whole level).

`process()` is a single `switch` on `msg.msg_type`. Unrecognized types are
logged at TRACE and ignored. Over-execution and over-cancel races (`shares >
remaining_qty`) are clamped to the remaining quantity with a WARN log
carrying the offending values.

`Order` struct (side is a single char for feed-byte parity):

```cpp
struct Order {
    double price;
    double remaining_qty;
    char   side;   // 'B' = buy, 'S' = sell
};
```

### `BinanceBookAdapter<MaxLevels>`

Thin wrapper over `ArrayBook<MaxLevels>`. Two entry points:

- `update_from_ticker(BookTicker)` — parses four `string_view` price/qty
  fields via `eph::core::parse_number` and writes the BBO to the book. Returns
  `false` on any parse failure (full message is logged at WARN).
- `load_snapshot(DepthSnapshot)` — clears the book, walks every snapshot
  level, and records `snapshot.last_update_id` so callers can filter stale
  incremental updates.

`last_update_id()` returns `int64_t` and is `0` until the first
`load_snapshot` call.

### Signal calculators (`signals.hpp`)

All templates are `noexcept` and allocation-free. They accept any book
exposing the required accessors.

| Function | Requires | Returns |
|---|---|---|
| `order_imbalance(book)` | `total_bid_qty() + total_ask_qty()` | `double` in `[-1, 1]`, 0.0 if both sides empty. |
| `weighted_mid(book)` | `best_bid() + best_ask()` | `optional<double>` — `(bid*ask_qty + ask*bid_qty) / (bid_qty + ask_qty)`. |
| `microprice(book)` | `best_bid() + best_ask()` | Currently delegates to `weighted_mid`; semantically reserved for future multi-level extension. |
| `spread_bps(book)` | `best_bid() + best_ask()` | `optional<double>` — `(ask - bid) / mid * 10000`, `nullopt` if `mid <= 0`. |
| `vwap(span<PriceLevel>)` | any contiguous `PriceLevel` range | `optional<double>` — sum(p*q)/sum(q). |
| `depth_ratio(book)` | `total_bid_qty() + total_ask_qty()` | `double` — `bid_qty / ask_qty`, returns 0.0 if ask side empty (no divide-by-zero). |

---

## Entry Points & APIs

### Minimum working L2 example

```cpp
#include <eph/book/array_book.hpp>
#include <eph/book/signals.hpp>

eph::book::ArrayBook<20> book;
book.update_bid(50'000.0, 1.5);
book.update_ask(50'001.0, 0.8);
book.update_bid(49'999.0, 2.0);

auto best = book.best_bid();           // PriceLevel{50000.0, 1.5}
auto mid  = book.mid_price();          // 50000.5
auto imb  = eph::book::order_imbalance(book);
auto wmid = eph::book::weighted_mid(book);
auto bvw  = eph::book::vwap(book.bids());
```

### Deep book example

```cpp
#include <eph/book/map_book.hpp>
#include <eph/book/signals.hpp>

eph::book::MapBook book;
for (const auto& [px, q] : bids_feed) book.update_bid(px, q);
for (const auto& [px, q] : asks_feed) book.update_ask(px, q);

auto top5 = book.top_bids(5);                       // std::vector<PriceLevel>
double imb = eph::book::order_imbalance(book);      // same signal, different book type
```

### Binance reconnect flow

```cpp
#include <eph/book/binance_adapter.hpp>

eph::book::BinanceBookAdapter<20> adapter;

// 1. REST snapshot for initial state.
if (auto snap = eph::json::binance::parse_depth_response(buf, len)) {
    adapter.load_snapshot(*snap);
    auto base = adapter.last_update_id();
    // drop incoming deltas with U <= base
}

// 2. Incremental BBO updates.
if (auto tk = eph::json::binance::BookTicker::from(json_view)) {
    adapter.update_from_ticker(*tk);
}
```

### ITCH 5.0 flow

```cpp
#include <eph/book/itch_adapter.hpp>

eph::book::ItchBookBuilder<20> builder;
for (const auto& msg : itch_messages) {
    if (builder.process(msg)) {
        // book mutated — recompute signals if needed
    }
}
std::size_t live = builder.order_count();
builder.clear();   // for a new trading session
```

See the **Key Components** section above for per-method behavior, or the
Doxygen-style comments in `include/eph/book/*.hpp` for authoritative details.

---

## Dependencies

| Dependency | Required by | Notes |
|---|---|---|
| `eph-core` | all components | `parse_number.hpp` used by `BinanceBookAdapter`. Public dep in `xmake.lua`. |
| `spdlog` | all components | Public package dep; `SPDLOG_ACTIVE_LEVEL` defined from the parent project. |
| `eph-json` | `binance_adapter.hpp` only | **Not** linked by `eph-book`; callers must include it themselves. |
| `eph-itch` | `itch_adapter.hpp` only | **Not** linked by `eph-book`; callers must include it themselves. |
| `gtest` | tests only | Fetched by root xmake. |
| `benchmark` | benchmarks only | Fetched by root xmake. |

This split is deliberate: core `eph-book` consumers (raw `ArrayBook` / `MapBook`
+ signals) avoid pulling `eph-json` or `eph-itch`. Only users that include
an adapter header need those transitively.

---

## Testing

GoogleTest, compiled in isolation per file via the `eph-test` rule.

| Target | What it covers |
|---|---|
| `test_array_book` | Sort-order invariants (descending bids / ascending asks), BBO extraction, mid/spread math, in-place updates, `qty == 0` removal, full-book eviction, NaN rejection, negative-qty clamp, empty-side edge cases, `is_crossed` / `is_locked` boundaries, `total_bid_qty` / `total_ask_qty`, `max_levels` constant, `clear()`. |
| `test_map_book` | Same semantics as ArrayBook plus: deep-book paths (100+ levels), `top_bids(n)` / `top_asks(n)`, price quantization (near-duplicate prices collapse to one key), `is_crossed` / `is_locked` parity with ArrayBook, allocation behavior vs ArrayBook on identical input sequences. |
| `test_signals` | Each signal (`order_imbalance`, `weighted_mid`, `microprice`, `spread_bps`, `vwap`, `depth_ratio`) exercised across both book types, symmetric + asymmetric inputs, empty-side / empty-book cases, `vwap` over subspans, `microprice` == `weighted_mid` parity. |

Run via:

```bash
xmake run test_array_book
xmake run test_map_book
xmake run test_signals
```

All three finish in well under a second.

---

## Benchmarks

Google Benchmark, compiled via the `eph-bench` rule. These benchmarks link
`eph-json` for realistic Binance `bookTicker` input.

| Target | Scenarios |
|---|---|
| `bench_array_book` | Single BBO update, full parse-to-book-update cycle, BBO queries, deep-insert eviction path. |
| `bench_map_book` | Deep-level insert/delete, BBO queries, `top_bids(n)` extraction. |

Run via:

```bash
xmake run bench_array_book
xmake run bench_map_book
```

Per the project CLAUDE.md rule: **always capture a baseline before changing
hot-path code, and verify no regression after.** Micro-benchmark variance is
a few percent run-to-run on a warm machine; investigate regressions exceeding
that noise floor.

---

## Observability

Every component uses a named `spdlog` logger under the `book.*` namespace:

| Component | Logger name |
|---|---|
| `ArrayBook` | `book.array_book` |
| `MapBook` | `book.map_book` |
| `BinanceBookAdapter` | `book.binance_adapter` |
| `ItchBookBuilder` | `book.itch_adapter` |

Each logger is lazily constructed via `spdlog::stdout_color_mt` with a
`spdlog::get` fallback — safe against duplicate-registration on TU
re-initialization.

Log level is compile-time filtered via `SPDLOG_ACTIVE_LEVEL`, set by the
parent project:

- Debug builds (`xmake f -m debug`): `SPDLOG_LEVEL_TRACE`.
- Release builds (`xmake f -m release`): `SPDLOG_LEVEL_INFO`.

Actionable-logging convention: every WARN log carries the offending input
values (NaN price, clamped quantity, missing order-ref, unknown ITCH
msg_type, …). Use `SPDLOG_LEVEL_TRACE` builds to get an entry per insert /
update / remove, which is the canonical way to diagnose a book divergence.

Per-function severity mapping in `include/eph/book/*.hpp`:

- `TRACE` — every level mutation (`update_bid`, `update_ask`, `add_qty`,
  `sub_qty`, executed / cancelled shares).
- `DEBUG` — book-level mutations and batch events (snapshot load complete,
  `clear()`, order-ref inserted / erased).
- `WARN` — invariant fallbacks (NaN price, negative qty clamped, missing
  order-ref, exchange over-execution clamped).
- `ERROR` — currently unused; all fallbacks are `WARN` + keep-going.

---

## See Also

- `README.md` — user-facing overview with copy-pasteable snippets.
- `CHANGELOG.md` — version history.
- `docs/ONBOARDING.md` — developer onboarding (clone / build / test / common
  tasks / troubleshooting).
- Root `CLAUDE.md` — project-level build, test, and convention guide.
- Sibling modules: `eph-json` (Binance JSON parser), `eph-itch` (ITCH 5.0
  parser), `eph-core` (`parse_number`, `PacketView`, shared error types).
