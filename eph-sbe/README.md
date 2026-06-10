# eph-sbe

Header-only, zero-copy **Simple Binary Encoding (SBE)** decoder for the
ephemeral HFT stack. SBE is the FIX-standard binary wire format used by
exchange market-data feeds (Binance spot WS SBE, CME MDP 3.0, …). `eph-sbe`
decodes it in place — no allocation, no copy — mirroring `eph-itch`'s style but
for SBE's little-endian wire and its repeating-group / variable-length layout.

Decode-only. Depends only on `eph-core`; never pulls in networking.

## Features

- **8-byte SBE message header** parse (`blockLength` / `templateId` / `schemaId`
  / `version`).
- **Little-endian primitives** — `read_le16/32/64`, signed `read_le_i8/…/i64`
  (two's-complement via `std::bit_cast`), all `memcpy`-based (unaligned-safe;
  no UB from `reinterpret_cast` on packed fields).
- **SBE composites** — `varString8` (length-prefixed UTF-8, bounds-checked) and
  `decode_decimal(mantissa, exponent)` (`value = mantissa × 10^exponent`).
- **Repeating groups** — `groupSize16Encoding` dimension reader + bounds-checked
  iteration.
- **Template-id `dispatch()`** scaffold — extend by adding a tag + a `case`.
- **Binance spot schema 3:2 accessors** — `BookTickerResponse` (id 212):
  per-ticker bid/ask price+qty (decimal), optional fields as `std::optional`,
  `symbol` as a zero-copy `std::string_view`, plus `for_each_ticker()`.

Field offsets are derived from the **vendored authoritative schema**
[`schemas/spot_3_2.xml`](schemas/spot_3_2.xml) (Binance spot SBE, id=3,
version=2) — the single source of truth.

## Quick start

```cpp
#include "eph/sbe.hpp"

// `frame` is the application payload from a WsCodec on_message callback.
auto view = eph::sbe::parse(frame.data(), frame.size());
if (!view) return;  // truncated / non-SBE — skip safely

namespace bt = eph::sbe::binance::book_ticker;
eph::sbe::binance::for_each_ticker(*view, [](const uint8_t* t) {
    std::optional<double> bid = bt::bid_price(t);  // nullopt if no bid
    std::optional<double> ask = bt::ask_price(t);
    std::string_view      sym = bt::symbol(t);     // zero-copy
    // ... feed strategy ...
});
```

`for_each_ticker` returns `std::expected<size_t, ParseError>` — the ticker count
on success, or `kTruncated` / `kMalformedGroup` on a malformed (or untrusted /
forged) frame. It refuses any schema other than the one it targets
(`binance::is_supported(view)`) rather than silently mis-decoding.

## Structure

```
include/eph/
  sbe.hpp                    aggregation header
  sbe/
    byte_order.hpp           little-endian + varString8 + decimal primitives
    errors.hpp               ParseError vocabulary
    message_header.hpp       SbeHeader + parse_header()
    parser.hpp               MessageView, parse(), group reader, dispatch()
    binance/
      schema.hpp             schema id/version, template ids, null sentinel
      book_ticker.hpp        BookTickerResponse accessors + for_each_ticker()
schemas/spot_3_2.xml         vendored authoritative Binance spot SBE schema
tests/                       3 GoogleTest files
benchmarks/bench_sbe_book_ticker.cpp
```

## Key entry points

| Symbol | Purpose |
|---|---|
| `eph::sbe::parse(data, len)` | Decode the SBE header → `MessageView`. |
| `eph::sbe::dispatch(view, handler)` | Route by template id to a tag overload. |
| `eph::sbe::read_var_string8(p, remaining)` | Bounds-checked `varString8`. |
| `eph::sbe::decode_decimal(mantissa, exp)` | `mantissa × 10^exp`. |
| `eph::sbe::binance::for_each_ticker(view, fn)` | Walk a `BookTickerResponse`. |
| `eph::sbe::binance::book_ticker::bid_price(block)` | Per-ticker accessors. |

## Build & test

```bash
xmake build -g tests
xmake run test_sbe_header        # header / MessageView / dispatch
xmake run test_sbe_byte_order    # LE / varString8 / decimal / group header
xmake run test_sbe_book_ticker   # BookTickerResponse decode (multi / null / truncation)
xmake run bench_sbe_book_ticker  # decode-throughput baseline
```

## Scope

Decode-only; the bundled schema covers `BookTickerResponse`. Adding another
message is local: a `binance/<msg>.hpp` accessor namespace, a template-id
constant in `schema.hpp`, and a `case` in `dispatch()`. Out of scope: SBE
encoding, nested repeating groups, and other schemas (CME MDP, etc.).
