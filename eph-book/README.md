# eph-book

Header-only C++23 library for L2/L3 order book maintenance and market microstructure signal computation, designed for HFT market data processing.

## Overview

eph-book is the order book layer of the eph ecosystem. It provides two book implementations -- a fixed-capacity `ArrayBook` optimized for shallow, latency-critical L2 feeds and a dynamic-depth `MapBook` for deep L3/full-depth books -- together with pure-function trading signal calculators and feed-specific adapters for Binance and ITCH 5.0 protocols.

All components live in namespace `eph::book`. The library is entirely header-only, allocation-free on the hot path (ArrayBook + signals), and uses `noexcept` throughout for deterministic performance.

## Key Components

All headers are under `include/eph/book/`:

| Header | Description |
|--------|-------------|
| `array_book.hpp` | Fixed-size, zero-allocation L2 order book backed by sorted `std::array`. Cache-friendly for shallow books (5-20 levels) typical of crypto L2 feeds and equity top-of-book. Template parameter `MaxLevels` caps each side independently (default 20). Defines the shared `PriceLevel` struct used throughout the library. |
| `map_book.hpp` | Deep L3 order book backed by `std::map` with O(log n) insert/delete/lookup for 1000+ price levels. Bids sorted descending (`std::greater`), asks ascending, so `begin()` always yields BBO. Prices quantized to 1e-9 multiples to prevent near-duplicate keys. Includes `top_bids(n)` / `top_asks(n)` for display/logging. |
| `signals.hpp` | Pure function templates computing trading signals from any book type: order imbalance, weighted mid / microprice, spread (bps), VWAP, and depth ratio. All noexcept, allocation-free on the hot path. |
| `binance_adapter.hpp` | Bridges Binance `bookTicker` BBO snapshots and REST depth snapshots into ArrayBook. Handles string-to-double parsing internally and tracks `last_update_id` for sequence validation. Requires eph-json on the include path. |
| `itch_adapter.hpp` | Bridges ITCH 5.0 order-level events into aggregated L2 price levels in ArrayBook. Maintains an internal order map (`order_ref -> {price, qty, side}`) with incremental per-price-level quantity tracking for O(1) book updates. Handles AddOrder, AddOrderMPID, OrderExecuted, OrderExecutedWithPrice, OrderCancel, OrderDelete, and OrderReplace. Requires eph-itch on the include path. |

The convenience header `include/eph/book.hpp` includes both `array_book.hpp` and `map_book.hpp`.

## Public API Reference

### Types

| Type | Header | Description |
|------|--------|-------------|
| `PriceLevel` | `array_book.hpp` | `{double price, double qty}` -- a single price level on one side of the book. Canonical level representation used by all book types and signal functions. |
| `Order` | `itch_adapter.hpp` | `{double price, double remaining_qty, char side}` -- per-order state for ITCH aggregation. Side is `'B'` (buy) or `'S'` (sell). |

### ArrayBook\<MaxLevels\>

Fixed-capacity sorted L2 book. Namespace: `eph::book`. Default `MaxLevels = 20`.

Compile-time constant: `ArrayBook::max_levels` exposes the template parameter.

| Method | Returns | Description |
|--------|---------|-------------|
| `update_bid(price, qty)` | `void` | Insert/update bid level; qty=0 removes. NaN prices rejected, negative qty clamped to 0. |
| `update_ask(price, qty)` | `void` | Insert/update ask level; same semantics as `update_bid`. |
| `best_bid()` | `optional<PriceLevel>` | Highest bid, or `nullopt` if empty. |
| `best_ask()` | `optional<PriceLevel>` | Lowest ask, or `nullopt` if empty. |
| `mid_price()` | `optional<double>` | `(best_bid + best_ask) / 2`, or `nullopt` when either side is empty. |
| `spread()` | `optional<double>` | `best_ask - best_bid` in native price units. Negative means crossed. |
| `bids()` | `span<const PriceLevel>` | Active bid levels, descending by price. Invalidated by mutation. |
| `asks()` | `span<const PriceLevel>` | Active ask levels, ascending by price. Invalidated by mutation. |
| `bid_depth()` | `size_t` | Number of active bid levels, in `[0, MaxLevels]`. |
| `ask_depth()` | `size_t` | Number of active ask levels, in `[0, MaxLevels]`. |
| `total_bid_qty()` | `double` | Sum of quantities across all bid levels. |
| `total_ask_qty()` | `double` | Sum of quantities across all ask levels. |
| `level_count()` | `size_t` | Total active levels (bid + ask), in `[0, 2 * MaxLevels]`. |
| `is_crossed()` | `bool` | True if best bid > best ask (anomalous state). |
| `is_locked()` | `bool` | True if best bid == best ask within epsilon. |
| `clear()` | `void` | Remove all levels from both sides. |

### MapBook

Dynamic-depth sorted book. Namespace: `eph::book`. Same query interface as ArrayBook with the following differences:

| Method | Returns | Description |
|--------|---------|-------------|
| `update_bid(price, qty)` | `void` | Insert/update bid level; price is quantized to 1e-9 before insertion. |
| `update_ask(price, qty)` | `void` | Insert/update ask level; price quantized. |
| `best_bid()` | `optional<PriceLevel>` | Highest bid, or `nullopt` if empty. |
| `best_ask()` | `optional<PriceLevel>` | Lowest ask, or `nullopt` if empty. |
| `mid_price()` | `optional<double>` | `(best_bid + best_ask) / 2`. |
| `spread()` | `optional<double>` | `best_ask - best_bid`. |
| `bids()` | `vector<PriceLevel>` | Full depth copy, descending by price. Allocates on every call. |
| `asks()` | `vector<PriceLevel>` | Full depth copy, ascending by price. Allocates on every call. |
| `top_bids(n)` | `vector<PriceLevel>` | Top N bid levels (descending). Prefer over `bids()` on the hot path. |
| `top_asks(n)` | `vector<PriceLevel>` | Top N ask levels (ascending). Prefer over `asks()` on the hot path. |
| `bid_depth()` | `size_t` | Number of active bid levels. |
| `ask_depth()` | `size_t` | Number of active ask levels. |
| `level_count()` | `size_t` | Total active levels (bid + ask). |
| `total_bid_qty()` | `double` | Sum of quantities across all bid levels. |
| `total_ask_qty()` | `double` | Sum of quantities across all ask levels. |
| `is_crossed()` | `bool` | True if best bid > best ask. |
| `is_locked()` | `bool` | True if best bid == best ask within epsilon. |
| `clear()` | `void` | Remove all levels from both sides. |

### Signal Functions

All in namespace `eph::book`. Templates accept any book type with the expected interface (`best_bid()`, `best_ask()`, `total_bid_qty()`, `total_ask_qty()`).

| Function | Returns | Description |
|----------|---------|-------------|
| `order_imbalance(book)` | `double` | Normalized buy/sell pressure: `(bid_qty - ask_qty) / (bid_qty + ask_qty)`. Range [-1, 1]. Returns 0.0 if the book is empty. |
| `weighted_mid(book)` | `optional<double>` | BBO size-weighted fair value: `(bid * ask_qty + ask * bid_qty) / (bid_qty + ask_qty)`. |
| `microprice(book)` | `optional<double>` | Top-of-book microprice (same formula as `weighted_mid`; semantically reserved for future multi-level extensions). |
| `spread_bps(book)` | `optional<double>` | Bid-ask spread in basis points: `(ask - bid) / mid * 10000`. |
| `vwap(span<PriceLevel>)` | `optional<double>` | Volume-weighted average price over a contiguous range of levels. Works with ArrayBook spans or any PriceLevel vector. |
| `depth_ratio(book)` | `double` | `total_bid_qty / total_ask_qty`. Returns 0.0 if the ask side is empty. |

### BinanceBookAdapter\<MaxLevels\>

Adapts Binance WebSocket and REST data into ArrayBook. Namespace: `eph::book`. Default `MaxLevels = 20`.

| Method | Returns | Description |
|--------|---------|-------------|
| `update_from_ticker(ticker)` | `bool` | Apply BBO from a parsed `BookTicker`; returns `false` on parse failure (non-numeric price/qty strings). |
| `load_snapshot(snapshot)` | `size_t` | Bulk-load a `DepthSnapshot` (clears book first); returns levels loaded. Stores `last_update_id` for gap detection. |
| `last_update_id()` | `int64_t` | Sequence ID from last snapshot. Incremental updates with `U <= last_update_id()` should be dropped. |
| `book()` | `ArrayBook&` | Access underlying book (const and mutable overloads). |

### ItchBookBuilder\<MaxLevels\>

Converts order-level ITCH 5.0 events into aggregated L2 price levels. Namespace: `eph::book`. Default `MaxLevels = 20`.

Supported ITCH message types: `'A'` AddOrder, `'F'` AddOrderMPID, `'E'` OrderExecuted, `'C'` OrderExecutedWithPrice, `'X'` OrderCancel, `'D'` OrderDelete, `'U'` OrderReplace. All others are silently ignored.

| Method | Returns | Description |
|--------|---------|-------------|
| `process(msg)` | `bool` | Dispatch an ITCH `MessageView` to the appropriate handler; returns `true` if the book was modified. |
| `book()` | `ArrayBook&` | Access underlying book (const and mutable overloads). |
| `order_count()` | `size_t` | Number of tracked live orders. |
| `clear()` | `void` | Clear all orders, quantity accumulators, and the book. |

## Dependencies

| Dependency | Required by | Description |
|------------|-------------|-------------|
| **eph-core** | All components | Number parsing (`parse_number.hpp`) used by BinanceBookAdapter. |
| **spdlog** | All components | Leveled logging throughout (compile-time filtered via `SPDLOG_ACTIVE_LEVEL`). |
| **eph-json** | `binance_adapter.hpp` only | Binance JSON types (`BookTicker`, `DepthSnapshot`, parser). Optional -- not needed if you only use the core book types and signals. |
| **eph-itch** | `itch_adapter.hpp` only | ITCH 5.0 message parsing (`MessageView`, field accessors). Optional -- not needed if you only use the core book types and signals. |

## Usage Examples

### Basic L2 Book with Signals

```cpp
#include <eph/book/array_book.hpp>
#include <eph/book/signals.hpp>

// Create a 20-level L2 book
eph::book::ArrayBook<20> book;

// Apply price-level updates (crypto exchange style)
book.update_bid(50000.0, 1.5);   // bid at 50000, qty 1.5
book.update_ask(50001.0, 0.8);   // ask at 50001, qty 0.8
book.update_bid(49999.0, 2.0);   // second bid level

// Query BBO
auto best_bid = book.best_bid();  // -> PriceLevel{50000.0, 1.5}
auto best_ask = book.best_ask();  // -> PriceLevel{50001.0, 0.8}
auto mid      = book.mid_price(); // -> 50000.5

// Remove a level by setting qty to 0
book.update_bid(49999.0, 0.0);   // removes the 49999 bid level

// Compute trading signals
double imbalance = eph::book::order_imbalance(book);   // [-1, 1]
auto wmid        = eph::book::weighted_mid(book);       // size-adjusted mid
auto spd         = eph::book::spread_bps(book);         // spread in bps
auto bid_vwap    = eph::book::vwap(book.bids());        // VWAP across bid depth
double dr        = eph::book::depth_ratio(book);        // bid/ask qty ratio
```

### Deep Book with MapBook

```cpp
#include <eph/book/map_book.hpp>
#include <eph/book/signals.hpp>

eph::book::MapBook book;

// Populate from a deep order book feed (no capacity limit)
for (const auto& [price, qty] : depth_feed_bids) {
    book.update_bid(price, qty);
}
for (const auto& [price, qty] : depth_feed_asks) {
    book.update_ask(price, qty);
}

// Extract top 5 levels for display
auto top5_bids = book.top_bids(5);
auto top5_asks = book.top_asks(5);

// Signals work with any book type
double imbalance = eph::book::order_imbalance(book);
auto wmid        = eph::book::weighted_mid(book);
```

### Binance Adapter

```cpp
#include <eph/book/binance_adapter.hpp>
#include <eph/book/signals.hpp>

eph::book::BinanceBookAdapter<20> adapter;

// Load initial state from REST depth snapshot (reconnection recovery)
auto snapshot = eph::json::binance::parse_depth_response(buf, len);
if (snapshot) {
    adapter.load_snapshot(*snapshot);
    // Use last_update_id() to validate subsequent incremental updates
    int64_t seq = adapter.last_update_id();
}

// Apply live BBO updates from WebSocket bookTicker stream
auto ticker = eph::json::binance::BookTicker::from(json_view);
if (ticker) {
    adapter.update_from_ticker(*ticker);
}

// Query the book and compute signals
const auto& book = adapter.book();
auto mid = book.mid_price();
auto spd = eph::book::spread_bps(book);
```

### ITCH 5.0 Adapter

```cpp
#include <eph/book/itch_adapter.hpp>
#include <eph/book/signals.hpp>

eph::book::ItchBookBuilder<20> builder;

// Feed ITCH messages from parser
for (const auto& msg : itch_messages) {
    if (builder.process(msg)) {
        // Book was modified -- recompute signals
        auto bid = builder.book().best_bid();
        auto ask = builder.book().best_ask();
        double imbalance = eph::book::order_imbalance(builder.book());
    }
}

// Inspect adapter state
std::size_t live_orders = builder.order_count();

// Reset for a new trading session
builder.clear();
```
