# eph-book

Header-only C++23 library for L2/L3 order book maintenance and market microstructure signal computation, designed for HFT market data processing.

## Key Components

All headers are under `include/eph/book/`:

- **array_book.hpp** -- Fixed-size, zero-allocation L2 order book backed by sorted `std::array`. Cache-friendly for shallow books (5-20 levels) typical of crypto L2 feeds and equity top-of-book. Template parameter `MaxLevels` caps each side independently (default 20). Supports both explicit updates (price+qty, crypto style) and implicit updates (order-level aggregation, ITCH style). Defines the shared `PriceLevel` struct used throughout the library.
- **map_book.hpp** -- Deep L3 order book backed by `std::map` for 1000+ price levels with O(log n) insert/delete/lookup. Bids sorted descending (`std::greater`), asks ascending, so `begin()` always yields BBO. Prices quantized to 1e-9 multiples to prevent near-duplicate keys. Includes `top_bids(n)` / `top_asks(n)` extraction for display/logging.
- **signals.hpp** -- Pure function templates computing trading signals from any book type: order imbalance, weighted mid / microprice, spread (bps), VWAP, and depth ratio. All noexcept, allocation-free on the hot path.
- **binance_adapter.hpp** -- Bridges Binance bookTicker BBO snapshots and REST depth snapshots into ArrayBook. `update_from_ticker()` applies live BBO updates; `load_snapshot()` bulk-loads a `DepthSnapshot` for reconnection recovery and tracks `last_update_id` for sequence validation. Requires both eph-book and eph-json headers on the include path.
- **itch_adapter.hpp** -- Bridges ITCH 5.0 order-level events into aggregated L2 price levels in ArrayBook. Maintains an internal order map (`order_ref -> {price, qty, side}`) with incremental per-price-level quantity tracking for O(1) book updates. Handles AddOrder, AddOrderMPID, OrderExecuted, OrderExecutedWithPrice, OrderCancel, OrderDelete, and OrderReplace messages. Requires both eph-book and eph-itch headers on the include path.

The convenience header `include/eph/book.hpp` includes both `array_book.hpp` and `map_book.hpp`.

## Public API

### Types

| Type | Header | Description |
|------|--------|-------------|
| `PriceLevel` | `array_book.hpp` | `{double price, double qty}` pair representing one side of a book level |
| `Order` | `itch_adapter.hpp` | `{double price, double remaining_qty, char side}` per-order state for ITCH aggregation |

### ArrayBook\<MaxLevels\>

Fixed-capacity sorted L2 book. Namespace: `eph::book`.

| Method | Returns | Description |
|--------|---------|-------------|
| `update_bid(price, qty)` | `void` | Insert/update bid level; qty=0 removes. NaN prices rejected, negative qty clamped to 0 |
| `update_ask(price, qty)` | `void` | Insert/update ask level; same semantics as `update_bid` |
| `best_bid()` | `optional<PriceLevel>` | Highest bid, or nullopt if empty |
| `best_ask()` | `optional<PriceLevel>` | Lowest ask, or nullopt if empty |
| `mid_price()` | `optional<double>` | (best_bid + best_ask) / 2 |
| `spread()` | `optional<double>` | best_ask - best_bid |
| `bids()` | `span<const PriceLevel>` | Active bid levels, descending by price |
| `asks()` | `span<const PriceLevel>` | Active ask levels, ascending by price |
| `bid_depth()` / `ask_depth()` | `size_t` | Number of active levels per side |
| `total_bid_qty()` / `total_ask_qty()` | `double` | Sum of quantities across all levels per side |
| `level_count()` | `size_t` | Total active levels (bid + ask) |
| `is_crossed()` | `bool` | True if best bid > best ask (anomalous) |
| `is_locked()` | `bool` | True if best bid == best ask within epsilon |
| `clear()` | `void` | Remove all levels from both sides |

### MapBook

Dynamic-depth sorted book. Same query interface as ArrayBook, plus:

| Method | Returns | Description |
|--------|---------|-------------|
| `top_bids(n)` | `vector<PriceLevel>` | Top N bid levels (descending) |
| `top_asks(n)` | `vector<PriceLevel>` | Top N ask levels (ascending) |
| `bids()` / `asks()` | `vector<PriceLevel>` | Full depth copy (node-based map cannot return span) |

### Signal Functions

All in namespace `eph::book`. Templates accept any book type with the expected interface.

| Function | Returns | Description |
|----------|---------|-------------|
| `order_imbalance(book)` | `double` | Normalized buy/sell pressure in [-1, 1] |
| `weighted_mid(book)` | `optional<double>` | BBO size-weighted fair value estimate |
| `microprice(book)` | `optional<double>` | Top-of-book microprice (same formula as weighted_mid) |
| `spread_bps(book)` | `optional<double>` | Bid-ask spread in basis points |
| `vwap(span<PriceLevel>)` | `optional<double>` | Volume-weighted average price over depth levels |
| `depth_ratio(book)` | `double` | total_bid_qty / total_ask_qty (0.0 if ask side empty) |

### BinanceBookAdapter\<MaxLevels\>

| Method | Returns | Description |
|--------|---------|-------------|
| `update_from_ticker(ticker)` | `bool` | Apply BBO from a parsed `BookTicker`; returns false on parse failure |
| `load_snapshot(snapshot)` | `size_t` | Bulk-load a `DepthSnapshot` (clears book first); returns levels loaded |
| `last_update_id()` | `int64_t` | Sequence ID from last snapshot for gap detection |
| `book()` | `ArrayBook&` | Access underlying book (const and mutable overloads) |

### ItchBookBuilder\<MaxLevels\>

| Method | Returns | Description |
|--------|---------|-------------|
| `process(msg)` | `bool` | Dispatch an ITCH `MessageView` to the appropriate handler; returns true if book modified |
| `book()` | `ArrayBook&` | Access underlying book (const and mutable overloads) |
| `order_count()` | `size_t` | Number of tracked live orders |
| `clear()` | `void` | Clear all orders and reset the book |

## Dependencies

- **eph-core** -- Number parsing (`parse_number.hpp`)
- **spdlog** -- Logging (all components)
- **eph-json** (optional) -- Required only when using `binance_adapter.hpp`
- **eph-itch** (optional) -- Required only when using `itch_adapter.hpp`

## Quick Start

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

// Compute trading signals
double imbalance = eph::book::order_imbalance(book);       // [-1, 1]
auto wmid        = eph::book::weighted_mid(book);           // size-adjusted mid
auto spd         = eph::book::spread_bps(book);             // spread in bps
auto bid_vwap    = eph::book::vwap(book.bids());            // VWAP across bid depth
double dr        = eph::book::depth_ratio(book);            // bid/ask qty ratio
```

### Binance Adapter

```cpp
#include <eph/book/binance_adapter.hpp>

eph::book::BinanceBookAdapter<20> adapter;

// From a WebSocket bookTicker message:
auto ticker = eph::json::binance::BookTicker::from(json_view);
if (ticker) adapter.update_from_ticker(*ticker);

// From a REST depth snapshot (reconnection recovery):
auto snapshot = eph::json::binance::parse_depth_response(buf, len);
if (snapshot) adapter.load_snapshot(*snapshot);

// Access the book
const auto& book = adapter.book();
```

### ITCH Adapter

```cpp
#include <eph/book/itch_adapter.hpp>

eph::book::ItchBookBuilder<20> builder;

// Feed ITCH messages from parser
for (const auto& msg : itch_messages) {
    builder.process(msg);
}

// Query resulting L2 book
auto bid = builder.book().best_bid();
auto ask = builder.book().best_ask();
```
