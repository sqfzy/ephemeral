# eph-itch

Header-only C++23 library for zero-copy parsing of Nasdaq ITCH 5.0 market data
and the surrounding Nasdaq protocol stack (MoldUDP64, SoupBinTCP, OUCH 5.0).
Designed for HFT feed handling with no allocations on the hot path.

## Features

- **ITCH 5.0 message parsing** — all 22 message types covered. Zero-copy field
  accessors operate directly on raw byte buffers (no deserialization into
  structs, no memcpy of payloads). `MessageView` is a small POD pointing back
  into the caller's receive buffer.
- **Compile-time tag dispatch** — `dispatch()` / `dispatch_all()` resolve
  handler overloads at compile time via `msg::` tag structs, producing one
  branch per message type with no virtual calls. Handler return values are
  forwarded; returning `bool` from a `dispatch_all` handler supports early
  stop.
- **Transport protocols** — `ItchFramer` (2-byte length prefix),
  `SoupBinTcpFramer` (TCP framer with packet type), and `parse_moldudp64()`
  (UDP multicast container iteration).
- **OUCH 5.0 order entry** — inbound builders (`EnterOrder`, `ReplaceOrder`,
  `CancelOrder`) and outbound zero-copy views (`AcceptedView`, `ExecutedView`,
  `CanceledView`, `ReplacedView`).
- **Message classification helpers** — `is_system_message()`,
  `is_order_message()`, `is_trade_message()`, `is_imbalance_message()`,
  `is_known_type()`; all `constexpr` and mutually exclusive.
- **Padded-string trim** — `eph::itch::trim()` + per-accessor `*_trimmed()`
  variants (`add_order::stock_trimmed`, `stock_directory::stock_trimmed`,
  `add_order_mpid::attribution_trimmed`, …) strip trailing spaces from the
  8-byte stock symbol / 4-byte MPID wire fields without copying.
- **Price scaling constants** — `kItchPriceDivisor` (10 000, for most ITCH
  price fields) and `kLuldPriceDivisor` (100 000 000, for LULD auction
  collars) so consumers never hard-code magic divisors.
- **Auto-derived size bounds** — `kMaxMessageSize` / `kMinMessageSize` are
  computed at compile time from the per-type `k*Size` constants via a
  `detail::kAllMessageSizes[]` table; adding a new message type updates both
  automatically.
- **Production observability** — `ParserStats` tracks throughput, error rate,
  and first-error diagnostics (`dump()`, `to_json()`, `operator-` for
  interval snapshots). Named per-module spdlog loggers (`itch.parser`,
  `itch.moldudp64`, `itch.soupbintcp`, `itch.ouch`) with actionable error
  context (offsets, lengths, message-type byte). `std::formatter`
  specialisations for `ParseError`, `MessageView`, and `ParserStats` make
  them first-class citizens of `std::format` / `std::print`.
- **Header-only** — no compiled library; just add `#include <eph/itch.hpp>`
  and link transitive dependencies (`eph-core`, `spdlog`).

All multi-byte integers are big-endian on the wire. Endian conversion is done
via `std::memcpy` + `std::byteswap`, which optimises down to a single
`bswap`/`rev` instruction on modern targets.

## Build

eph-itch is a `headeronly` xmake target inside the `ephemeral_dev` monorepo.

### Prerequisites

- C++23 compiler (GCC 13+, Clang 17+, or equivalent)
- [xmake](https://xmake.io/) build system
- `spdlog` (resolved via xmake package manager)
- `eph-core` (sibling subproject in the same monorepo)

### Build and test from the monorepo root

```bash
# Build just this target
xmake build eph-itch

# Build and run all eph-itch tests
xmake build test_itch test_moldudp64 test_ouch test_soupbintcp
xmake run test_itch
xmake run test_moldudp64
xmake run test_ouch
xmake run test_soupbintcp

# Run the parse throughput benchmark
xmake build bench_itch_parse
xmake run bench_itch_parse
```

### Use from another xmake target

```lua
add_deps("eph-itch")
```

The target is declared `headeronly` in `xmake.lua`, re-exports `eph-core` and
`spdlog` as public dependencies, and sets `SPDLOG_ACTIVE_LEVEL` from the
parent project's `net_log_level`.

## Project layout

```
eph-itch/
├── include/eph/
│   ├── itch.hpp                # Convenience umbrella include
│   └── itch/
│       ├── messages.hpp        # 22 message types, constants, accessors
│       ├── parser.hpp          # parse, parse_all, dispatch, ParserStats
│       ├── framer.hpp          # ItchFramer (alias over LengthPrefixFramer)
│       ├── soupbintcp.hpp      # SoupBinTcpFramer + packet type constants
│       ├── moldudp64.hpp       # parse_moldudp64 + MoldUDP64Header
│       └── ouch.hpp            # OUCH 5.0 builders + views
├── tests/                      # GoogleTest unit tests (4 files)
├── benchmarks/                 # Google Benchmark bench_itch_parse
└── xmake.lua                   # Target + test/bench auto-discovery
```

## Usage

### Parse a single ITCH message

```cpp
#include <eph/itch/parser.hpp>

auto result = eph::itch::parse(data, len);
if (result) {
    const auto& msg = *result;
    std::println("type={} locate={} ts={}ns",
        eph::itch::message_type_name(msg.msg_type),
        msg.stock_locate(), msg.timestamp_ns());
}
```

### Type-safe dispatch

```cpp
#include <eph/itch/parser.hpp>

struct Handler {
    void operator()(eph::itch::msg::AddOrder, const uint8_t* msg) {
        auto ref   = eph::itch::add_order::order_ref(msg);
        auto price = eph::itch::add_order::price(msg);
        // ...
    }
    template <typename T>
    void operator()(T, const uint8_t*) { /* ignore other types */ }
};

eph::itch::ParserStats stats;
size_t consumed = eph::itch::dispatch_all(buf, buf_len, Handler{}, stats);
```

### MoldUDP64 multicast feed

```cpp
#include <eph/itch/moldudp64.hpp>
#include <eph/itch/parser.hpp>

eph::itch::parse_moldudp64(udp_data, udp_len,
    [](const uint8_t* msg_data, uint16_t msg_len, uint64_t seq_num) {
        auto r = eph::itch::parse(msg_data, msg_len);
        if (r) { /* process r->msg_type ... */ }
    });
```

### SoupBinTCP framing

```cpp
#include <eph/itch/soupbintcp.hpp>

eph::itch::SoupBinTcpFramer framer;
uint8_t wire[65537];
size_t wire_len = framer.encode(wire, payload, payload_len,
                                eph::itch::soupbin::kSequencedData);

auto frame = framer.decode(rx_buf, rx_len);
if (frame && frame->msg_type == eph::itch::soupbin::kSequencedData) {
    auto msg = eph::itch::parse(frame->payload, frame->payload_len);
    // ...
}
```

### OUCH 5.0 order entry

```cpp
#include <eph/itch/ouch.hpp>

uint8_t buf[eph::itch::ouch::EnterOrder::kSize];
eph::itch::ouch::EnterOrder::build(
    buf, "TOKEN1234     ", 'B', 100, "AAPL    ", 1500000, 0, "FIRM");

eph::itch::ouch::ExecutedView exec(response, response_len);
if (exec.valid()) {
    auto shares = exec.executed_shares();
    auto price  = exec.execution_price();
}
```

## Dependencies

- **eph-core** — `LengthPrefixFramer`, `MessageFramer` concept, `FrameError`,
  `DecodedFrame` (all consumed by the framer and SoupBinTCP headers).
- **spdlog** — leveled logging in `parser.hpp`, `soupbintcp.hpp`,
  `moldudp64.hpp`, `ouch.hpp`. Compile-time filtered via `SPDLOG_ACTIVE_LEVEL`.

## References

- Nasdaq TotalView-ITCH 5.0:
  https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification.pdf
- MoldUDP64:
  https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/moldudp64.pdf
- OUCH 5.0:
  https://www.nasdaqtrader.com/content/technicalsupport/specifications/TradingProducts/OUCH5.0.pdf

## License

See the top-level `ephemeral_dev` repository for license information.
