# eph-book Order Book Guide

How to maintain L2/L3 order books and derive trading signals with the eph-book module.

## Book Types

| Book | Backing | Capacity | Allocation | Best For |
|------|---------|----------|------------|----------|
| `ArrayBook<N>` | `std::array<PriceLevel, N>` | Fixed (N per side) | Zero | Shallow L2 feeds (5-20 levels), crypto bookTicker |
| `MapBook` | `std::map<double, double>` | Unbounded | Heap (node-based) | Deep L3 feeds (1000+ levels), full orderbook |

Both types share the same update interface (`update_bid`, `update_ask`) and query interface (`best_bid`, `best_ask`, `mid_price`, `spread`), so signal functions work with either.

## ArrayBook

Fixed-size, cache-friendly L2 book. Bids sorted descending, asks sorted ascending. Best price always at index 0.

```cpp
#include "eph/book/array_book.hpp"

// 10 levels per side (default is 20)
eph::book::ArrayBook<10> book;

// Insert / update levels
book.update_bid(87245.30, 1.5);   // price, qty
book.update_ask(87255.00, 0.8);

// Remove a level (qty == 0)
book.update_bid(87240.00, 0.0);

// BBO queries
auto bid = book.best_bid();     // std::optional<PriceLevel>
auto ask = book.best_ask();     // std::optional<PriceLevel>
auto mid = book.mid_price();    // std::optional<double>
auto spd = book.spread();       // std::optional<double>

// Depth access (zero-copy spans into the internal array)
std::span<const eph::book::PriceLevel> bids = book.bids();
std::span<const eph::book::PriceLevel> asks = book.asks();

// Depth counts
size_t bid_depth = book.bid_depth();
size_t ask_depth = book.ask_depth();

// Aggregates
double total_bid = book.total_bid_qty();
double total_ask = book.total_ask_qty();

// Anomaly detection
bool crossed = book.is_crossed();  // bid > ask (anomalous)
bool locked  = book.is_locked();   // bid == ask (within epsilon)
```

**Capacity behavior**: When the book is full and a new price is worse than the worst tracked level, it is silently dropped. If the new price is better, the worst level is evicted.

## MapBook

Dynamic-depth book for L3 feeds. Same interface as ArrayBook but backed by `std::map` for arbitrary depth.

```cpp
#include "eph/book/map_book.hpp"

eph::book::MapBook book;

// Same update interface
book.update_bid(87245.30, 1.5);
book.update_ask(87255.00, 0.8);

// Same BBO queries
auto mid = book.mid_price();

// Depth returns vectors (map is node-based, no contiguous span)
std::vector<eph::book::PriceLevel> bids = book.bids();

// Top-N extraction (for display / logging)
auto top5_bids = book.top_bids(5);
auto top5_asks = book.top_asks(5);
```

**Price quantization**: MapBook snaps prices to multiples of 1e-9 before using them as map keys, preventing near-duplicate entries from different floating-point rounding paths.

## When to Use Which

Use **ArrayBook** when:
- Feed depth is bounded (L2 snapshots, bookTicker, top-of-book)
- Zero allocation is required on the hot path
- You need `std::span` access for VWAP and other signal functions

Use **MapBook** when:
- Feed depth is unbounded or very large (full L3 orderbook)
- You need `top_bids(n)` / `top_asks(n)` extraction
- Insertion/deletion frequency is high at arbitrary price levels

## Signal Generation

The `signals.hpp` header provides pure function templates that work with any book type. All are noexcept and allocation-free.

```cpp
#include "eph/book/signals.hpp"

eph::book::ArrayBook<20> book;
// ... populate book ...

// Order imbalance: [-1.0, 1.0], positive = buy pressure
double imbalance = eph::book::order_imbalance(book);

// Weighted mid price (size-adjusted fair value)
// Formula: bid * (ask_qty / total) + ask * (bid_qty / total)
auto wmid = eph::book::weighted_mid(book);     // std::optional<double>

// Microprice (identical to weighted_mid for single-level)
auto mprice = eph::book::microprice(book);      // std::optional<double>

// Spread in basis points: (ask - bid) / mid * 10000
auto sbps = eph::book::spread_bps(book);        // std::optional<double>

// Depth ratio: total_bid_qty / total_ask_qty
double dratio = eph::book::depth_ratio(book);

// VWAP over a span of price levels (works with ArrayBook spans)
auto bid_vwap = eph::book::vwap(book.bids());   // std::optional<double>
auto ask_vwap = eph::book::vwap(book.asks());
```

**VWAP with MapBook**: Since `MapBook::bids()` returns a `std::vector<PriceLevel>`, it implicitly converts to `std::span<const PriceLevel>` for use with `vwap()`:

```cpp
eph::book::MapBook deep_book;
// ... populate ...
auto levels = deep_book.bids();              // vector<PriceLevel>
auto bid_vwap = eph::book::vwap(levels);     // works via implicit span conversion
```

## Integration with eph-json

Feed parsed exchange data directly into a book:

```cpp
#include "eph/json/adapters/binance.hpp"
#include "eph/book/array_book.hpp"
#include "eph/book/signals.hpp"

eph::book::ArrayBook<5> book;

stream->on_message = [&book](std::span<const uint8_t> frame) {
    auto json = eph::json::parse(frame.data(), frame.size());
    if (!json) return;

    auto ticker = eph::json::binance::BookTicker::from(*json);
    if (!ticker) return;

    // Parse prices and update book
    auto bid     = eph::core::parse_number(ticker->bid_price);
    auto ask     = eph::core::parse_number(ticker->ask_price);
    auto bid_qty = eph::core::parse_number(ticker->bid_qty);
    auto ask_qty = eph::core::parse_number(ticker->ask_qty);

    if (bid && bid_qty) book.update_bid(*bid, *bid_qty);
    if (ask && ask_qty) book.update_ask(*ask, *ask_qty);

    // Compute signals
    auto wmid = eph::book::weighted_mid(book);
    auto imb  = eph::book::order_imbalance(book);
    // ... trading logic ...
};
```

## Snapshot Recovery (Binance REST)

After reconnect, load a depth snapshot before applying WebSocket deltas:

```cpp
#include "eph/json/adapters/binance_rest.hpp"

// Fetch snapshot via REST
auto client = eph::json::binance::BinanceRestClient({});
auto snapshot = client.get_depth("BTCUSDT", 20);
if (!snapshot) return;

// Apply snapshot to book
book.clear();
for (auto& lvl : snapshot->bids) book.update_bid(lvl.price, lvl.qty);
for (auto& lvl : snapshot->asks) book.update_ask(lvl.price, lvl.qty);

// Resume applying WebSocket deltas
```

## PriceLevel

Both book types use the same level representation:

```cpp
struct PriceLevel {
    double price = 0.0;
    double qty   = 0.0;
};
```

All signal functions operate on `PriceLevel` or on book types that expose `best_bid()`, `best_ask()`, `total_bid_qty()`, and `total_ask_qty()`.
