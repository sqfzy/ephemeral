# eph-sbe — onboarding

A 10-minute tour for working in `eph-sbe`.

## What this module is

A header-only, decode-only **Simple Binary Encoding (SBE)** parser. SBE is the
FIX-standard binary market-data format (Binance spot WS SBE, CME MDP 3.0, …).
The module decodes it in place — no allocation, no copy. It is the SBE analog of
`eph-itch`: a parser module that depends only on `eph-core` and never touches
networking.

If you know `eph-itch`, you already know the shape. The differences:

| `eph-itch` | `eph-sbe` |
|---|---|
| big-endian (`read_be*`) | little-endian (`read_le*`) |
| 1-byte type tag | 8-byte SBE `messageHeader` |
| fixed-layout messages | repeating groups + `varString8` |
| dispatch on a type char | dispatch on `templateId` |

## First build

```bash
xmake build -g tests           # builds eph-sbe's test targets (and the rest)
xmake run test_sbe_header
xmake run test_sbe_byte_order
xmake run test_sbe_book_ticker
xmake run bench_sbe_book_ticker
```

The module is header-only, so there is nothing to compile until a test or
benchmark `#include`s it.

## Layout

- `include/eph/sbe/byte_order.hpp` — the primitives. Start here. LE integer
  reads, signed reads, `read_var_string8`, `decode_decimal`.
- `include/eph/sbe/message_header.hpp` — `parse_header()`: the 8-byte header.
- `include/eph/sbe/parser.hpp` — `MessageView`, `parse()`, the
  `groupSize16Encoding` reader, and the `dispatch()` scaffold.
- `include/eph/sbe/binance/` — the schema layer (accessors that know field
  offsets). `book_ticker.hpp` is the worked example.
- `schemas/spot_3_2.xml` — the **authoritative** Binance spot SBE schema.
  Offsets in `book_ticker.hpp` are derived from it. **This file is the source of
  truth** — if Binance revises the schema, refresh it and re-derive offsets.

## How decoding works

1. `parse(data, len)` validates the 8-byte header and hands back a zero-copy
   `MessageView`.
2. `dispatch(view, handler)` routes by `template_id` (212 → `msg::BookTicker`).
3. `binance::for_each_ticker(view, fn)` reads the `tickers` repeating-group
   header and walks each entry with bounds checking, calling `fn(block)`.
4. `book_ticker::*` accessors read fields off `block`: decimals are
   `mantissa × 10^exponent`; optional bid/ask return `std::optional` (`nullopt`
   on the `INT64_MIN` null sentinel); `symbol` is a zero-copy `string_view`.

## Conventions to keep

- **Never** `reinterpret_cast` a packed field — use the `read_le*` helpers
  (`memcpy` + compile-time byteswap). SBE fields are unaligned; a cast is UB.
- **Bounds-check before deref** on anything length-driven (`numInGroup`,
  `varString8` length). This module parses untrusted wire data.
- **Pin to a schema.** Accessor offsets are valid for one `schemaId:version`;
  guard with `binance::is_supported(view)` and refuse mismatches rather than
  decode silently-wrong data.
- **Cite the schema** in offset comments (field ids from `spot_3_2.xml`).
- **Log on error paths** via the `sbe.parser` / `sbe.binance` loggers with
  actionable context (offset, declared vs available length).

## Adding a new Binance message

1. Add its `templateId` to `binance/schema.hpp` (`tid::`).
2. Create `binance/<msg>.hpp` with offset constants (cite `spot_3_2.xml`) and
   accessors; reuse `read_le*` / `read_var_string8` / `decode_decimal` and, for
   grouped messages, the `for_each_ticker`-style iterator pattern.
3. Add a `msg::` tag and a `case` in `dispatch()` (`parser.hpp`).
4. Add a `tests/test_sbe_<msg>.cpp` with a synthetic builder matching the
   official layout; cover multi-entry, optional null, and truncation.
