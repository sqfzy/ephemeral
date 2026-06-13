# Changelog — eph-sbe

## 2026-06-13 — Binance WS-API + market-stream messages

### Added
- **Nested message decode** — `read_message_data()` decodes the SBE
  `messageData` composite (uint32 length + embedded self-describing message)
  into a sub-`MessageView`; `group_total_size()` skips a repeating group.
  `read_var_string16()` (uint16-prefixed varString).
- **Spot 3:2 accessors** — `WebSocketResponse(50)` envelope (status + echoed
  request id + nested result), `WebSocketSessionLogonResponse(51)`,
  `ErrorResponse(100)`, `NewOrderAckResponse(300)`, `CancelOrderResponse(305)`,
  `ExecutionReportEvent(603)`; `tid::` constants + `OrderStatus` enum.
- **spot_stream 1:0** — vendored `schemas/stream_1_0.xml`; `stream/schema.hpp`
  + `stream/best_bid_ask.hpp` (`BestBidAskStreamEvent`, id 10001).
- `dispatch()` routes 50/51/100/300/305/603/10001.
- Tests: `test_sbe_best_bid_ask`, `test_sbe_order`, `test_sbe_exec_report`
  (nested envelope, truncation, schema guard, dispatch).

## 2026-06-05 — Initial module

New header-only, decode-only Simple Binary Encoding module (sibling of
`eph-fix` / `eph-itch`, depends only on `eph-core`).

### Added

- **SBE core (schema-independent)**
  - `byte_order.hpp` — little-endian reads `read_le16/32/64`, signed
    `read_le_i8/i16/i32/i64` (two's-complement via `std::bit_cast`),
    `read_var_string8` (bounds-checked length-prefixed UTF-8), and
    `decode_decimal(mantissa, exponent)`.
  - `errors.hpp` — `ParseError` (`kIncomplete` / `kTruncated` /
    `kMalformedGroup`) + `parse_error_name`.
  - `message_header.hpp` — `SbeHeader` + `parse_header()` (the 8-byte
    `messageHeader` composite).
  - `parser.hpp` — zero-copy `MessageView` + `parse()`, `GroupHeader` +
    `read_group_header()` (`groupSize16Encoding`), and a template-id
    `dispatch()` scaffold.

- **Binance spot schema 3:2 accessors**
  - `binance/schema.hpp` — schema id=3 / version=2, `tid::kBookTicker=212`,
    optional-mantissa null sentinel (`INT64_MIN`), and `is_supported()`.
  - `binance/book_ticker.hpp` — `BookTickerResponse` (id 212) zero-copy
    accessors (decimal bid/ask price+qty, optional → `std::optional`, `symbol`
    `varString8`) and `for_each_ticker()` repeating-group iteration with full
    bounds checking. Offsets cite `schemas/spot_3_2.xml` field ids.
  - `dispatch()` routes template id 212 to `msg::BookTicker`.

- **Vendored authoritative schema** `schemas/spot_3_2.xml` (Binance spot SBE,
  id=3, version=2, littleEndian) — single source of truth for field offsets.

- **Tests** (3 GoogleTest files, 28 cases): header / `MessageView` / dispatch
  smoke, LE & signed reads incl. `INT64_MIN` null, `varString8`
  normal/empty/truncated/overrun, decimal pos/neg/zero exponent,
  `BookTickerResponse` single/multi ticker, optional null, empty group,
  truncated entry, missing group header, unsupported-schema rejection.

- **Benchmark** `bench_sbe_book_ticker` — decode-throughput baseline
  (~55 ns/ticker for the full parse + group walk + all fields + symbol on
  Graviton).

### Notes

- Decode-only; no SBE encoding, no nested groups, no other schemas.
- Corrects `docs/binance-protocols.md`, whose earlier illustrative bookTicker
  layout (flat body, fixed `/1e8`, `char[16]` symbol) did not match the official
  schema (repeating group, `mantissa × 10^exponent`, `varString8`).
