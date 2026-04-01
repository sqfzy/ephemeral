# eph-book

Header-only C++23 library for L2/L3 order book maintenance and market microstructure signal computation, designed for HFT market data processing.

## Key Components

All headers are under `include/eph/book/`:

- **array_book.hpp** -- Fixed-size, zero-allocation L2 order book backed by sorted `std::array`. Cache-friendly for shallow books (5-20 levels) typical of crypto L2 feeds and equity top-of-book. Template parameter `MaxLevels` caps each side independently. Supports both explicit updates (price+qty, crypto style) and implicit updates (order-level aggregation, ITCH style).
- **map_book.hpp** -- Deep L3 order book backed by `std::map` for 1000+ price levels with O(log n) insert/delete/lookup. Bids sorted descending (`std::greater`), asks ascending, so `begin()` always yields BBO. Prices quantized to 1e-9 multiples to prevent near-duplicate keys.
- **signals.hpp** -- Pure function templates computing trading signals from any book type: order imbalance, weighted mid / microprice, spread (bps), VWAP, and depth ratio. All noexcept, allocation-free on the hot path.
- **binance_adapter.hpp** -- Bridges Binance bookTicker BBO snapshots into ArrayBook. Requires both eph-book and eph-json headers on the include path.
- **itch_adapter.hpp** -- Bridges ITCH 5.0 order-level events into aggregated L2 price levels in ArrayBook. Maintains an internal order map (order_ref -> {price, qty, side}). Requires both eph-book and eph-itch headers on the include path.

## Dependencies

- **eph-core** -- Number parsing
- **spdlog** -- Logging
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
double imbalance = eph::book::order_imbalance(book);  // [-1, 1]
```
