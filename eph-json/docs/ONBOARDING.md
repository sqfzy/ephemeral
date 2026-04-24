# eph-json — Developer Onboarding

Short, code-grounded guide for getting productive in the `eph-json` subproject. Assumes basic familiarity with C++ and xmake.

## What is eph-json?

Header-only C++23 zero-copy JSON parser + typed exchange adapters (Binance, OKX, Bybit), plus a Binance REST client for snapshot recovery and clock sync. It sits between the network transport layer (`eph-net` / `eph-transport`) and application logic (`eph-book`, trading strategies) in the `ephemeral` HFT stack.

Read `README.md` at the subproject root for the feature overview and full API reference. Read `summary.md` for the architectural view with ASCII diagrams.

## Environment Setup

### Prerequisites

- **C++23 compiler:** GCC 14+ (the repo is validated on Amazon Linux 2023 with GCC 14) or Clang with `-std=c++23`. Uses `std::expected`, `std::format`, designated initializers throughout.
- **xmake:** https://xmake.io/. The repo is organized as a multi-module monorepo; every subproject has its own `xmake.lua`, discovered from the root `xmake.lua`.
- **System libraries:** standard C++ runtime, `spdlog` (via xmake's package system).

The parent repo `xmake.lua` pulls everything transitively — you should not need to install `spdlog`, `gtest`, or `google/benchmark` by hand.

### First build

From the parent repo root:

```bash
# One-time: configure (picks up C++23, warnings, ccache, etc.)
xmake f -c

# Build just eph-json (header-only target — essentially a no-op, but
# also verifies headers compile via a stub)
xmake build eph-json

# Build all eph-json tests and benchmarks
xmake build test_json test_binance test_okx test_bybit bench_json_parse

# Or build the entire monorepo
xmake build
```

### Verify

```bash
xmake run test_json        # parser / JsonView unit tests
xmake run test_binance     # Binance adapter tests
xmake run test_okx         # OKX adapter tests
xmake run test_bybit       # Bybit adapter tests
xmake run bench_json_parse # google/benchmark microbenchmarks
```

If all four test binaries finish green, your environment is good.

## Project Layout

```
eph-json/
├── include/eph/
│   ├── json.hpp                            # Aggregate convenience header
│   └── json/
│       ├── parser.hpp                      # Core zero-copy parser + JsonView
│       ├── framer.hpp                      # JsonFramer (pass-through MessageFramer)
│       └── adapters/
│           ├── binance.hpp                 # Binance WS adapters + helpers
│           ├── binance_depth_types.hpp     # DepthLevel / DepthSnapshot / ServerTime
│           ├── binance_rest.hpp            # BinanceRestClient (needs eph-net)
│           ├── okx.hpp                     # OKX WS adapters
│           └── bybit.hpp                   # Bybit WS adapters
├── tests/
│   ├── test_json.cpp
│   ├── test_binance.cpp
│   ├── test_okx.cpp
│   └── test_bybit.cpp
├── benchmarks/
│   └── bench_json_parse.cpp
├── README.md
├── CHANGELOG.md
├── summary.md
└── xmake.lua
```

### Key entry points

- **`eph::json::parse(data, len)`** — top-level parser. Returns `expected<JsonView, ParseError>`.
- **`eph::json::binance::BookTicker::from(JsonView)`** / **`okx::OkxBookTicker::from(...)`** / **`bybit::BybitBookTicker::from(...)`** — typed adapter factories.
- **`eph::json::binance::BookTicker::parse(data, len)`** — fused single-pass fast path for dedicated Binance `@bookTicker` subscriptions; ~2-3× faster than `from(parse(...))`, same required/optional field set.
- **`eph::json::binance::symbol_hash`** / **`okx::inst_id_hash`** / **`bybit::symbol_hash`** — raw-byte FNV-1a extractors that skip the parser entirely (hot-path deduplication).
- **`eph::json::binance::BinanceRestClient`** — REST client for `/api/v3/depth` and `/api/v3/time`.

## Architecture in 60 Seconds

- **Single-pass O(n) parser.** Scans the buffer once, emitting `Field { string_view key, string_view value, bool is_string }` into a fixed-size `std::array<Field, 32>` (configurable via `JsonView::kMaxFields`). No heap allocation.
- **Linear scan on lookup.** `JsonView::get*()` scans the array with a first-char + length pre-filter — faster than hashing for 5–15 field messages thanks to cache locality.
- **Compile-time LUTs.** `kWsLut` (whitespace) and `kValTermLut` (value terminators: `,`, `}`, whitespace) replace chained `||` checks with a single indexed load per byte.
- **Nested structures kept opaque.** Objects and arrays inside a parsed object are captured as a raw `string_view` spanning their braces/brackets; you re-parse them only if needed. Depth bounded at 64 internally.
- **Escape handling is minimal.** The scanner recognizes `\` to avoid terminating on `\"`, but does NOT decode `\n` / `\uXXXX` / etc. Field values are raw slices of the input.
- **Typed adapters.** Each exchange adapter pulls required fields from a `JsonView` via `get_string()` / `get_int()` and returns `optional<Struct>`. Binance's `BookTicker` additionally pre-parses prices into `cached_bid` / `cached_ask` to make `mid_price()` / `spread()` branch-light under hot-loop use.
- **Fused fast path (Binance bookTicker).** `BookTicker::parse(data, len)` is a specialised single-pass alternative: it walks the raw bytes once and dispatches each 1-char key directly into the target `BookTicker` field, skipping the generic `JsonView` intermediate. ~2-3× faster than `BookTicker::from(parse(data, len))` on representative payloads. Use it on dedicated `/ws/<sym>@bookTicker` subscriptions; keep `from()` for combined-stream envelopes whose outer JSON is already a `JsonView`. Unknown fields (future Binance additions, or multi-char keys like `"e"`) are silently skipped, so callers stay forward-compatible with additive schema changes.

## Daily Development

### Build

```bash
xmake build eph-json                             # header-only target check
xmake build test_json test_binance test_okx test_bybit bench_json_parse
```

### Test

```bash
xmake run test_json
xmake run test_binance
xmake run test_okx
xmake run test_bybit
```

Tests use `gtest` via the shared `eph-test` rule. Tests are auto-discovered from any `.cpp` under `tests/` by `xmake.lua`.

### Benchmark

```bash
xmake run bench_json_parse
```

Four microbenchmarks on a representative Binance `bookTicker` payload:
- `BM_JsonParse` — `parse()` alone.
- `BM_JsonParseAndExtract` — `parse()` + `BookTicker::from()`.
- `BM_BinanceBookTickerParse` — `BookTicker::parse(data, len)` fused single-pass fast path (direct competitor to `BM_JsonParseAndExtract`).
- `BM_SymbolHash` — `symbol_hash()` on raw bytes (no full parse).

**Always run benchmarks before AND after modifying the parser or any adapter.** Global user instructions require establishing a baseline, then verifying no regression before finalizing the change.

### Common Tasks

#### Add a new exchange adapter

1. Create `include/eph/json/adapters/<exchange>.hpp`.
2. Define a push-message envelope struct and one or more typed data structs with static `from(JsonView)` factories returning `optional<T>`.
3. If the exchange has a fast-path hot field (like `symbol`), add a `symbol_hash()` free function that scans raw bytes without invoking the parser.
4. Add a named `spdlog` logger in an anonymous-ish `detail::` namespace (see `binance_logger`, `okx_logger`, `bybit_logger` for the Meyers-singleton pattern).
5. Add `tests/test_<exchange>.cpp` with happy-path tests, missing-field tests, mid/spread correctness, and hash stability tests.
6. Update `README.md` and `summary.md` — API tables and module map.

#### Add a new parser feature

1. Read `parser.hpp` end-to-end before touching anything — the hot path is tightly tuned.
2. Establish a benchmark baseline: `xmake run bench_json_parse` and record the numbers.
3. Make the change. Keep branch hints (`[[likely]]` / `[[unlikely]]`) consistent with the new flow.
4. Extend `test_json.cpp` with cases exercising the new feature plus error paths and boundary conditions.
5. Re-run the benchmarks and verify no regression. If there is one, investigate before committing.

#### Debug a parse failure

- The parser itself has no logger — failures return `ParseError::{kIncomplete,kInvalidFormat,kFieldOverflow}`.
- `parse_error_name(err)` returns a string; `ParseError` is formattable via `std::format("{}", err)`.
- Adapters DO log at DEBUG level when a required field is missing — set `SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG` (repo default in debug builds) and look for lines from the `json.binance` / `json.okx` / `json.bybit` loggers.

## Code Conventions

### Naming

- **Namespaces:** `eph::json`, `eph::json::binance`, `eph::json::okx`, `eph::json::bybit`, `eph::json::binance::detail`.
- **Types:** `PascalCase` (`JsonView`, `BookTicker`, `BinanceRestClient`).
- **Enum constants:** `k`-prefixed PascalCase (`ParseError::kIncomplete`, `JsonView::kMaxFields`).
- **Free functions and members:** `snake_case` (`parse_error_name`, `get_string`, `mid_price`).
- **Compile-time constants:** `k` prefix (`kWsLut`, `kMaxFields`, `kValidDepthLimits`).

### Error handling

- **Parser and REST client:** `std::expected<T, E>`. Parser uses `ParseError` (`enum class : uint8_t`); REST client uses `std::string` for ad-hoc errors.
- **Adapter factories:** `std::optional<T>`. `from()` returns `nullopt` on missing required fields and logs the missing-field set at DEBUG level.
- **Zero exceptions** on the hot path — everything is `noexcept`-marked where feasible.

### Logging (spdlog)

- Every adapter has a named logger obtained via a `detail::xxx_logger()` Meyers-singleton.
- Use `SPDLOG_LOGGER_DEBUG(log, ...)` / `SPDLOG_LOGGER_WARN(log, ...)` / etc. — these macros compile out below `SPDLOG_ACTIVE_LEVEL`, which is set project-wide via `net_log_level` in the root `xmake.lua` (`SPDLOG_LEVEL_TRACE` in debug mode, `SPDLOG_LEVEL_INFO` otherwise).
- The parser itself has NO logger — it's on the hot path for every message and must stay allocation- and call-overhead-free.

### Commit conventions

Conventional Commits is used across the repo — prefixes seen in `git log` for this subproject:
- `feat(json): ...` — new feature
- `perf(json): ...` — performance change (must include benchmark evidence)
- `fix(json): ...` — bug fix
- `refactor(json): ...` — no behavior change
- `docs: ...` — documentation-only

## Common Pitfalls

### Lifetime of `string_view`s

Every `string_view` returned from `parse()` or an adapter's `from()` points into the original input buffer. If you drop the buffer before consuming the views, you have a use-after-free. For messages moved across threads, copy the values out first (e.g., into a `std::string` or a fixed-size `std::array<char, N>`).

### `get()` vs `get_string()`

- `get("key")` returns an empty `string_view` for BOTH missing keys and present-but-empty values.
- `get_string("key")` returns `nullopt` when the key is absent, `optional{""}` when present-but-empty.

Use `get_string()` when the distinction matters.

### OKX / Bybit double-parse

`OkxPushMessage::from()` and `OkxBookTicker::from()` each re-parse a nested string (the `arg` object for the envelope, the first array element for the ticker). The original outer buffer must still be alive for these nested parses to work. Same for `BybitBookTicker::from()`, which re-parses the nested `data` object.

### `BookTicker` vs `OkxBookTicker` / `BybitBookTicker` price caching

Only Binance's `BookTicker` caches parsed bid/ask doubles. OKX and Bybit tickers re-parse on every `mid_price()` / `spread()` call. If you need hot-loop mid/spread for those exchanges, cache the result yourself.

### `BookTicker::from(JsonView)` vs `BookTicker::parse(data, len)`

Two factories, different call sites:

- `BookTicker::from(JsonView)` — generic, works against any `JsonView`. Use this when the outer JSON has already been parsed (e.g. inside a combined-stream `data_raw`) and you want the extracted fields.
- `BookTicker::parse(data, len)` — fused single-pass over the raw bytes. Skips the `JsonView` intermediate entirely and is ~2-3× faster, but only applies when the payload is already known to be a flat bookTicker object (dedicated `/ws/<sym>@bookTicker` subscription). Do NOT pass a combined-stream wrapper to it — it would not descend into the nested `data` and would (correctly) return `nullopt`.

Pick `parse()` on dedicated hot paths, `from()` on generic JsonView flows. Both pre-populate `cached_bid` / `cached_ask` identically.

### `kMaxFields` overflow

If a message has more than 32 fields, `parse()` returns `ParseError::kFieldOverflow` and the `JsonView` is unusable. For larger flat objects, bump `JsonView::kMaxFields` — but think twice, as a larger fixed array hurts cache behavior.

## Where To Go Next

- Read `parser.hpp` end-to-end — ~400 lines, heavily commented. It is the heart of the module.
- Read `binance.hpp` for the canonical "how an adapter looks" template.
- Browse `tests/test_json.cpp` for an exhaustive catalog of parser edge cases.
- See `summary.md` for the project-wide architectural view and ASCII diagrams.
