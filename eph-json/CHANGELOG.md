# Changelog

All notable changes to `eph-json` will be documented in this file. Format loosely follows [Keep a Changelog](https://keepachangelog.com/). No versioned releases yet — everything lives under Unreleased and is grouped by the major reshuffles visible in `git log`.

## [Unreleased]

### Added
- **OKX and Bybit WebSocket adapters.** Typed `OkxPushMessage` / `OkxBookTicker` for the `{"arg":{...},"data":[{...}]}` envelope, and `BybitPushMessage` / `BybitBookTicker` for the `{"topic":..., "type":..., "data":{...}}` envelope. Both come with `inst_id_hash` / `symbol_hash` fast-path extractors for the Transport two-phase frame filter, and their own subscribe / unsubscribe message builders.
- **Binance REST client** (`BinanceRestClient`) for public read-only endpoints:
  - `get_depth(symbol, limit)` — orderbook snapshot for post-reconnect recovery. Validates `limit` against Binance's accepted set (5/10/20/50/100/500/1000/5000) before hitting the network.
  - `get_server_time()` — server clock for drift validation.
  - `parse_depth_response()` and `parse_server_time_response()` are exposed as free functions so they can be unit-tested without HTTP.
- **Binance WebSocket subscription helpers:** `ws_path`, `combined_ws_path`, `subscribe_message`, `unsubscribe_message`.
- **Binance `BookTicker` adapter** with pre-cached parsed bid/ask prices, plus `CombinedStream` wrapper and `symbol_hash` FNV-1a fast-path extractor.
- **Zero-copy JSON parser** (`parse()`, `JsonView`, `Field`, `ParseError`) targeting the 5–15 field flat objects used by crypto exchange feeds.
- **`JsonFramer`** — pass-through framer satisfying `eph::net::MessageFramer` for JSON-over-WebSocket Transport type aliases.
- **`BinanceRestConfig` ergonomics**: `validate()`, `dump()`, `to_json()`, `warnings()`, defaulted `operator==`, and a `std::formatter` specialization.
- **Full Doxygen-style API documentation** on every public function, struct, and class in `include/`, plus inline comments explaining non-obvious logic (linear-scan rationale, LUT optimizations, escape handling trade-offs, opaque-nested-value semantics).

### Changed
- **Parser performance pass:** introduced compile-time whitespace and value-terminator lookup tables, `[[likely]]` / `[[unlikely]]` branch hints on the hot path, and a dedicated `scan_string` helper. Byte-at-a-time remains faster than `memchr`-based SIMD for the short keys/values typical of exchange payloads.
- **Parser hardening:** `JsonView` internals encapsulated behind accessors, `field_at()` now bounds-checks and returns a static empty `Field` on out-of-range, and integer parsing correctly rejects `INT64_MIN` overflow cases.
- **Shared parsing primitives consolidated in `eph-core`:** `parse_int` and `parse_number` were deduplicated across modules and are now pulled from `eph::core`, removing multiple local copies from `eph-json`.
- **Loggers unified:** every adapter now uses a named `spdlog` logger (`json.binance`, `json.binance_rest`, `json.okx`, `json.bybit`), lazily initialized via the Meyers-singleton pattern.
- **Error formatting:** `ParseError` uses the shared `eph::core::ErrorEnumFormatter` (moved from the module-local namespace) so `std::format("{}", err)` works uniformly across modules.
- **Framer `encode()`** now carries `[[nodiscard]]` across all framer implementations, including `JsonFramer`.
- **Build layout:** `eph-json` now has its own modular `xmake.lua`, with tests and benchmarks discovered per-file under `tests/` and `benchmarks/` and auto-registered against `eph-test` / `eph-bench` rules.

### Fixed
- Numerous audit findings rolled up across multi-module fix passes (parser encapsulation, integer overflow edge cases, missing `[[nodiscard]]` attributes, and concurrency / protocol correctness issues in upstream modules that `eph-json` depends on).

### Removed
- Local duplicates of `parse_int` / `parse_number` — now single source of truth in `eph-core`.
- Module-local `ErrorEnumFormatter` — now in `eph::core` namespace.
