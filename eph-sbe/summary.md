# eph-sbe — architecture summary

Zero-copy, header-only **Simple Binary Encoding (SBE)** decoder. Decode-only;
depends only on `eph-core`. Same role in the stack as `eph-itch` / `eph-fix`: a
codec/parser module that an application composes against a networking backend.

## Layering

```
                eph-core  (ErrorInfo, concepts)
                    ▲
                 eph-sbe
   ┌───────────────┴────────────────────────────┐
   │  schema-independent core                    │
   │    byte_order.hpp   LE reads, varString8,    │
   │                     decode_decimal           │
   │    errors.hpp       ParseError               │
   │    message_header   SbeHeader, parse_header  │
   │    parser.hpp       MessageView, parse(),    │
   │                     read_group_header,       │
   │                     dispatch()               │
   ├──────────────────────────────────────────────┤
   │  schema layer (Binance spot 3:2)             │
   │    binance/schema.hpp     ids, null sentinel │
   │    binance/book_ticker.hpp accessors +       │
   │                            for_each_ticker() │
   └──────────────────────────────────────────────┘
            offsets ◄── schemas/spot_3_2.xml (vendored, authoritative)
```

Dependencies are one-directional: the schema layer includes the core; the core
never includes the schema layer (so `dispatch()` references template id 212 as a
literal to avoid a cycle).

## Data flow

```
wire bytes (one WS frame = one SBE message)
   │  eph::sbe::parse(data, len)
   ▼
MessageView { template_id, schema_id, version, block_length, data, length }
   │  dispatch(view, handler)  →  handler(msg::BookTicker{}, view)   (template 212)
   ▼
binance::for_each_ticker(view, fn):
   read groupSize16Encoding header  →  numInGroup
   for each entry (bounds-checked):
       fn(block)  →  book_ticker::{bid_price, ask_price, bid_qty, ask_qty, symbol}
       advance by  blockLength(34) + 1 + symbol_len   (varString8)
```

## Wire layout (BookTickerResponse, schema 3:2, little-endian)

```
messageHeader (8B):  blockLength u16 | templateId u16 | schemaId u16 | version u16
tickers group hdr (4B):  blockLength u16(=34) | numInGroup u16
  entry: 34B fixed block + varString8 symbol
    +0 priceExponent i8   +1 qtyExponent i8
    +2 bidPrice  i64 (optional, null=INT64_MIN)   value = mantissa × 10^exponent
   +10 bidQty    i64
   +18 askPrice  i64 (optional, null=INT64_MIN)
   +26 askQty    i64
   +34 symbol    varString8 (u8 len + UTF-8)
```

## Design notes

- **Zero-copy / zero-alloc / stateless** — every accessor returns a value or a
  `string_view` into the caller's buffer; the parser holds no state and is
  reentrant.
- **Untrusted-input safe** — all multi-byte reads use `memcpy` (unaligned-safe);
  `for_each_ticker` bounds-checks the group header, each fixed block, and each
  `varString8` length before any deref, so a forged `numInGroup` or symbol
  length cannot read past the buffer.
- **Schema-pinned** — accessors are valid for Binance spot 3:2; a non-matching
  `schema_id`/`version` is refused (`is_supported()`), never silently decoded.
- **Observability** — `sbe.parser` / `sbe.binance` spdlog loggers; malformed /
  truncated paths emit `WARN` with offset + length context.

## Extending

Add a message: a `binance/<msg>.hpp` accessor namespace, a template-id constant
in `binance/schema.hpp`, and a `case` (plus a `msg::` tag) in `dispatch()`. The
group/varString/decimal primitives are reused as-is.
