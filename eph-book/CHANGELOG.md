# Changelog

All notable changes to `eph-book`. Format loosely follows [Keep a Changelog](https://keepachangelog.com/). Dates reflect commit order on the `dev` branch; the crate is not yet versioned.

## [Unreleased]

### Changed
- Tests and benchmarks now live inside the subproject and are driven by the modular `xmake.lua`; they follow the same file layout as the library headers.

## Documentation

- Doxygen-style API comments added to every public type, function, and template in `include/eph/book/*.hpp`, covering `PriceLevel`, `ArrayBook`, `MapBook`, `Order`, `ItchBookBuilder`, `BinanceBookAdapter`, and all signal calculators.
- Comprehensive README regenerated with API reference tables, usage snippets, and logger-name conventions.

## Core order book

### Added
- **`ArrayBook<MaxLevels>`** — fixed-capacity, zero-allocation L2 order book with a crypto-style price-level API (`update_bid` / `update_ask`). Maintains sorted-by-best-price arrays with silent drop of levels worse than the current worst when the book is full. NaN prices are rejected and negative quantities are clamped to zero (treated as removal).
- **`MapBook`** — dynamic-depth L3 order book backed by `std::map` with `std::greater<>` on the bid side. Handles 1000+ levels; includes `top_bids(n)` / `top_asks(n)` for cheap top-of-book extraction. Prices are quantized to `1e-9` on insertion.
- BBO / depth queries across both books: `best_bid`, `best_ask`, `mid_price`, `spread`, `bid_depth`, `ask_depth`, `level_count`, `total_bid_qty`, `total_ask_qty`, `is_crossed`, `is_locked`, `clear`.
- Additional utility methods on `ArrayBook` (depth / total-qty / level-count accessors) and expanded test coverage for edge cases.

### Fixed
- `MapBook::is_crossed` now uses the same strict `>` + epsilon comparison as `ArrayBook::is_crossed`, so the two implementations agree on boundary cases.
- `MapBook` uses epsilon-tolerant price matching to avoid duplicate levels caused by floating-point rounding drift.

## Signal calculators

### Added
- `signals.hpp` with pure-function templates: `order_imbalance`, `weighted_mid`, `microprice`, `spread_bps`, `vwap`, `depth_ratio`. All generic over any book exposing the expected BBO / total-qty interface, all `noexcept`, and allocation-free on the hot path.

## Feed adapters

### Added
- **`BinanceBookAdapter<MaxLevels>`** — bridges Binance WebSocket `bookTicker` BBO updates into `ArrayBook`, parsing the string price/quantity fields via `eph::core::parse_number`.
- REST depth-snapshot loading (`load_snapshot`) for reconnection recovery, including `last_update_id` tracking so that stale incremental updates can be filtered.
- **`ItchBookBuilder<MaxLevels>`** — converts order-level ITCH 5.0 events (`AddOrder`, `AddOrderMPID`, `OrderExecuted`, `OrderExecutedWithPrice`, `OrderCancel`, `OrderDelete`, `OrderReplace`) into aggregated L2 levels, with an internal order map and per-price accumulators for O(1) incremental updates.

### Fixed
- `ItchBookBuilder` clamps executed and cancelled shares to the order's remaining quantity, guarding the book against exchange-side over-execution races. Clamps are logged at WARN level with the offending values.

## Observability and infrastructure

### Changed
- All four components migrated to named `spdlog` loggers (`book.array_book`, `book.map_book`, `book.binance_adapter`, `book.itch_adapter`) with lazy, idempotent construction — safe against duplicate-registration exceptions on repeated TU initialization.
- Log levels compile-time filtered via `SPDLOG_ACTIVE_LEVEL` inherited from the parent project.
- `BinanceBookAdapter` uses the consolidated `eph::core::parse_number` after the core module dedup refactor.
- Tests and benchmarks moved alongside the library headers under `eph-book/`, following the modular xmake layout used across all `eph` subprojects.

### Fixed
- Resolved audit findings across multiple pass rounds (40 + 28 + 18 + 33 nit/minor issues) affecting naming, logging consistency, comment accuracy, and minor correctness edges.
