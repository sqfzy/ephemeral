# Project: eph-itch

> Header-only C++23 codec for the Nasdaq ITCH 5.0 / MoldUDP64 / SoupBinTCP /
> OUCH 5.0 protocol stack, built for zero-allocation HFT feed handling.

**Language**: C++23 | **Build**: xmake (headeronly target) | **Parent**: ephemeral_dev monorepo

---

## Table of Contents

1. Overview
2. Architecture
3. Module Map
4. Data Flow
5. Key Components
6. Entry Points and APIs
7. Dependencies
8. Testing

---

## Overview

eph-itch is the Nasdaq-protocol layer of the ephemeral_dev monorepo. It
decodes NASDAQ TotalView-ITCH 5.0 market data, iterates MoldUDP64 multicast
packets, frames SoupBinTCP streams, and builds / parses OUCH 5.0 order entry
messages.

The library is header-only: every function is inline or a template, so the
compiler can inline the entire parse path into the caller's hot loop. There
are no heap allocations on the hot path. MessageView is a small POD that
points back into the caller's receive buffer (a type tag, a pointer, and a
length), and per-message accessors return plain scalars or std::string_views
— no deserialisation, no copies.

Design priorities, roughly in order:

1. Zero copy / zero allocation on the data path.
2. Compile-time dispatch — dispatch() resolves handler overloads via switch
   + tag types; the compiler generates one branch per overload.
3. Honest error handling — std::expected<T, ParseError> everywhere, with
   three distinct failure modes (kIncomplete, kUnknownType, kTruncated).
4. Observability — named spdlog loggers per module, actionable error logs
   (offsets + lengths + message bytes), and a ParserStats counter struct
   suitable for Prometheus/StatsD export.

Consumers are expected to be feed handlers, market data recorders, and HFT
strategies written against the rest of the eph stack.

---

## Architecture

Layered, no inheritance, no runtime polymorphism. Each layer is a small set
of free functions or lightweight value types.

### Component Diagram

```
                 +---------------------------------------+
                 |           application layer          |
                 |     (feed handler, OMS, recorder)    |
                 +----------+----------------+-----------+
                            |                |
            dispatch()/     |                | build()/views
            parse_all()     |                |
                            v                v
 +----------+  +------------------+   +---------------+
 | framer.  |  |    parser.hpp    |   |   ouch.hpp    |
 |   hpp    |  |                  |   | (builders +   |
 |(ItchFramer)| parse/parse_all   |   |  zero-copy    |
 |          |  | dispatch/        |   |   views)      |
 |          |  | dispatch_all     |   +-------+-------+
 |          |  | ParserStats      |           |
 |          |  +----------+-------+           |
 +----+-----+             |                   |
      |                   v                   |
      |        +--------------------+         |
      |        |   messages.hpp     |<--------+
      |        |                    |  (read_be*/trim
      |        | 22 msg types       |   reused)
      |        | accessors          |
      |        | constants          |
      |        +--------------------+
      |
      |   +------------------+   +------------------+
      +-->|  soupbintcp.hpp  |   |  moldudp64.hpp   |
          | (TCP framer)     |   | (UDP batch iter) |
          +------------------+   +------------------+
                  |                        |
                  v                        v
            eph::net::LengthPrefixFramer (eph-core)
            eph::net::FrameError / DecodedFrame
```

messages.hpp sits at the bottom: everything above it reuses its
read_be16/32/48/64 helpers, trim() utility, and message size constants.

---

## Module Map

| Module / File | Responsibility | Key Types / Functions | Depends On |
|---|---|---|---|
| include/eph/itch.hpp | Umbrella include | - | all of the below |
| include/eph/itch/messages.hpp | ITCH 5.0 constants, endian helpers, per-message zero-copy accessors | kMaxMessageSize, read_be*, trim, add_order::price, cross_trade::shares, classification helpers | <bit>, <cstdint>, <cstring>, <string_view> |
| include/eph/itch/parser.hpp | Parse / dispatch + ParserStats | parse, parse_all, dispatch, dispatch_all, MessageView, ParserStats, ParseError | messages.hpp, spdlog, std::expected, std::format |
| include/eph/itch/framer.hpp | 2-byte big-endian length prefix framing | ItchFramer (alias) | eph-core/length_prefix_framer |
| include/eph/itch/soupbintcp.hpp | SoupBinTCP TCP framer | SoupBinTcpFramer, soupbin::k*, soupbin::is_heartbeat | eph-core/framer_concept, spdlog |
| include/eph/itch/moldudp64.hpp | MoldUDP64 UDP batch parser | MoldUDP64Header, parse_moldudp64_header, parse_moldudp64 | messages.hpp, parser.hpp, spdlog |
| include/eph/itch/ouch.hpp | OUCH 5.0 builders (inbound) + views (outbound) | EnterOrder, ReplaceOrder, CancelOrder, AcceptedView, ExecutedView, CanceledView, ReplacedView | messages.hpp, spdlog |
| tests/test_itch.cpp | ITCH message accessor + parser tests | TEST(ItchMessages,...), TEST(ItchParser,...) | eph-itch, gtest |
| tests/test_moldudp64.cpp | MoldUDP64 header + iteration tests | TEST(MoldUDP64,...) | eph-itch, gtest |
| tests/test_soupbintcp.cpp | SoupBinTCP encode/decode tests | TEST(SoupBinTcpFramer,...) | eph-itch, gtest |
| tests/test_ouch.cpp | OUCH builder + view wire-layout tests | TEST(OuchEnterOrder,...) | eph-itch, gtest |
| benchmarks/bench_itch_parse.cpp | Parse throughput + accessor latency microbenchmarks | BENCHMARK(...) | eph-itch, benchmark |
| xmake.lua | Target declaration (headeronly) + auto-discovery of tests and benchmarks | - | xmake 2.x |

---

## Data Flow

The library supports three ingress paths (UDP multicast, TCP, and
application-emitted OUCH) that converge at the per-message handler.

### UDP multicast (primary market data)

```
UDP socket --> parse_moldudp64_header()  --> header (session, seq, count)
                         |
                         +-> parse_moldudp64() iterates count messages
                                                |
                                                v
                                 per-message callback(data, len, seq)
                                                |
                                                v
                                          parse() --> MessageView
                                                |
                                                v
                                      dispatch(view, handler)
                                                |
                                                v
                                   handler(msg::AddOrder{}, msg)
                                                |
                                                v
                                   add_order::price(msg), ...
```

### TCP stream (SoupBinTCP)

```
TCP socket --> SoupBinTcpFramer::decode() --> DecodedFrame
                         |
                         v
              type == kSequencedData?
                         | yes
                         v
                  parse() --> MessageView
                         |
                         v
                  (continues as above)
```

### OUCH order entry (outbound + inbound)

```
app build --> EnterOrder::build(buf, ...)       --> wire bytes
           |
           v
     SoupBinTcpFramer::encode()                 --> TCP send

app recv <-- AcceptedView / ExecutedView          <-- wire bytes
                                                    |
                                                    v
                                          SoupBinTcpFramer::decode()
```

---

## Key Components

### MessageView (parser.hpp)

File: include/eph/itch/parser.hpp
Purpose: Small POD pointing into the receive buffer (a message type byte, a
pointer, and a length). All further accessors are read via free functions in
per-message namespaces or the convenience methods on the view itself.

Interface:
```
struct MessageView {
    uint8_t        msg_type;
    const uint8_t* data;
    uint16_t       length;
    uint16_t stock_locate()    const noexcept;
    uint16_t tracking_number() const noexcept;
    uint64_t timestamp_ns()    const noexcept;
    std::string dump() const;
    std::string to_json() const;
};
```

Notes: data points at byte 0 (the type tag). The per-message accessors (e.g.
add_order::price) use data directly. Common-header accessors operate on
data + 1.

### parse() / parse_all() (parser.hpp)

File: include/eph/itch/parser.hpp
Purpose: Decode a single ITCH message or iterate a buffer of concatenated
messages.

Interface:
```
std::expected<MessageView, ParseError>
    parse(const uint8_t* data, size_t len) noexcept;
std::expected<MessageView, ParseError>
    parse(std::span<const uint8_t>) noexcept;

template <typename Fn>
size_t parse_all(const uint8_t* data, size_t len, Fn&& callback) noexcept;

template <typename Fn>
size_t parse_all(const uint8_t* data, size_t len, Fn&& callback,
                 ParserStats& stats) noexcept;
```

Notes: parse_all() stops on first error; a kIncomplete trailing remnant is
treated as a normal end-of-buffer (not counted as an error) when the
ParserStats overload is used.

### dispatch() / dispatch_all() (parser.hpp)

File: include/eph/itch/parser.hpp
Purpose: Route a parsed message to overloaded handler callables keyed on
msg:: tag structs — a zero-overhead compile-time visitor.

Interface:
```
template <typename Handler>
decltype(auto) dispatch(const MessageView& view, Handler&& handler);

template <typename Handler>
size_t dispatch_all(const uint8_t* data, size_t len, Handler&& handler);

template <typename Handler>
size_t dispatch_all(const uint8_t* data, size_t len, Handler&& handler,
                    ParserStats& stats);
```

Notes: The handler receives handler(msg::TagType{}, const uint8_t* msg)
where msg is the full message pointer. For unknown types, the tag is
msg::Unknown.

### ParserStats (parser.hpp)

File: include/eph/itch/parser.hpp
Purpose: Lightweight non-thread-safe counter struct for production
monitoring.

Interface:
```
struct ParserStats {
    uint64_t messages_parsed = 0;
    uint64_t parse_errors    = 0;
    uint64_t bytes_consumed  = 0;
    size_t   first_error_offset   = 0;
    ParseError first_error_type   = {};
    uint8_t  first_error_msg_byte = 0;

    void on_message(size_t msg_bytes) noexcept;
    void on_error(size_t offset, ParseError err, uint8_t msg_byte) noexcept;
    void reset() noexcept;
    std::string dump() const;
    std::string to_json() const;
    double throughput(uint64_t duration_ns) const noexcept;
    double error_rate() const noexcept;
    friend ParserStats operator-(const ParserStats&, const ParserStats&) noexcept;
};
```

Notes: operator- computes deltas for interval-based monitoring;
first_error_* fields are taken from the later snapshot (point-in-time
diagnostic).

### SoupBinTcpFramer (soupbintcp.hpp)

File: include/eph/itch/soupbintcp.hpp
Purpose: Encode and decode SoupBinTCP wire packets — the 3-byte-overhead
framing (uint16_t length_be + uint8_t type + payload) used for Nasdaq's TCP
feed/order entry sessions.

Interface:
```
class SoupBinTcpFramer {
    static constexpr size_t max_overhead() noexcept;        // 3
    static constexpr size_t kMaxPayloadLen = 65534;

    size_t encode(uint8_t* out, const uint8_t* data, size_t len,
                  uint8_t msg_type) noexcept;
    std::expected<eph::net::DecodedFrame, eph::net::FrameError>
        decode(const uint8_t* data, size_t len) noexcept;
};
```

Notes: Heartbeat packets (kServerHeartbeat / kClientHeartbeat) surface as
DecodedFrame{ is_control = true, payload_len = 0 }.

### parse_moldudp64() (moldudp64.hpp)

File: include/eph/itch/moldudp64.hpp
Purpose: Iterate length-prefixed messages in a MoldUDP64 UDP payload,
invoking a callback per message.

Interface:
```
template <typename Fn>
size_t parse_moldudp64(const uint8_t* data, size_t len, Fn&& callback) noexcept;
```

Notes: Returns 0 and logs INFO on end-of-session (0xFFFF). Returns 0 and
logs WARN on truncated headers, overflow-prone sequence numbers, or buffers
too small for the advertised message count.

### OUCH builders (ouch.hpp)

File: include/eph/itch/ouch.hpp
Purpose: Serialise outbound order-entry messages into wire format.

Interface:
```
struct EnterOrder {
    static constexpr size_t kSize = 49;
    static size_t build(uint8_t* buf, std::string_view token, char side,
                        uint32_t shares, std::string_view symbol,
                        uint32_t price, uint32_t time_in_force,
                        std::string_view firm) noexcept;
};
struct ReplaceOrder { static constexpr size_t kSize = 47; ... };
struct CancelOrder  { static constexpr size_t kSize = 19; ... };
```

Notes: EnterOrder validates the side byte (must be 'B' or 'S') and
hard-codes display='Y', capacity='O' (agency), intermarket sweep='N',
cross type='N'. For other combinations, build the wire bytes directly.

### OUCH views (ouch.hpp)

File: include/eph/itch/ouch.hpp
Purpose: Zero-copy read access to inbound order-entry responses.

Interface:
```
class AcceptedView { /* kSize = 66, timestamp/token/side/shares/... */ };
class ExecutedView { /* kSize = 40, executed_shares/execution_price/... */ };
class CanceledView { /* kSize = 28, decrement_shares/reason */ };
class ReplacedView { /* kSize = 80, replacement_token/previous_token/... */ };
```

Notes: Every accessor assert()s valid(). If the underlying buffer is too
small, the view's data_ is nullptr and valid() returns false — always
check before accessing fields in production code.

### Per-message accessor namespaces (messages.hpp)

File: include/eph/itch/messages.hpp
Purpose: Provide one free-function accessor per ITCH field, reading
directly from the raw message bytes.

Interface (example for AddOrder):
```
namespace eph::itch::add_order {
    uint64_t order_ref(const uint8_t* msg) noexcept;
    char side(const uint8_t* msg) noexcept;         // 'B' or 'S'
    uint32_t shares(const uint8_t* msg) noexcept;
    std::string_view stock(const uint8_t* msg) noexcept;
    std::string_view stock_trimmed(const uint8_t* msg) noexcept;
    uint32_t price_raw(const uint8_t* msg) noexcept;
    double price(const uint8_t* msg) noexcept;      // /10000
}
```

Notes: Every namespace takes msg = pointer to the full message (byte 0 is
the type tag). The 11-byte common header accessors (stock_locate,
tracking_number, timestamp_ns) take body = msg + 1. This asymmetry is
documented in messages.hpp near the common-header block.

---

## Entry Points and APIs

| Entrypoint | Type | Description |
|---|---|---|
| eph::itch::parse | Function | Parse single ITCH message (ptr+len or span) |
| eph::itch::parse_all | Function template | Parse consecutive ITCH messages with per-message callback |
| eph::itch::dispatch / dispatch_all | Function template | Tag-based visitor over a MessageView |
| eph::itch::ParserStats | Type | Counter struct for observability |
| eph::itch::ItchFramer | Type alias | 2-byte length prefix framer |
| eph::itch::SoupBinTcpFramer | Class | SoupBinTCP encode/decode |
| eph::itch::parse_moldudp64 | Function template | Iterate messages in a MoldUDP64 UDP payload |
| eph::itch::parse_moldudp64_header | Function | Parse the 20-byte MoldUDP64 header only |
| eph::itch::ouch::EnterOrder::build (etc.) | Static function | OUCH inbound message builders |
| eph::itch::ouch::AcceptedView (etc.) | Class | OUCH outbound zero-copy views |

---

## Dependencies

### Internal (module graph)

```
                       +--------------+
                       | messages.hpp |
                       +-------+------+
                               |
        +----------------------+--------------------------+
        v                      v                          v
 +--------------+       +--------------+           +------------+
 |  parser.hpp  |<------| moldudp64.hpp|           |  ouch.hpp  |
 +------+-------+       +--------------+           +------------+
        |
        |             +------------------+
        v             |  soupbintcp.hpp  |
 +--------------+     +--------+---------+
 |  framer.hpp  |              |
 +------+-------+              |
        |                      |
        v                      v
  eph-core: LengthPrefixFramer, framer_concept (FrameError, DecodedFrame)
```

### External

| Package  | Used In | Purpose |
|---|---|---|
| spdlog | parser.hpp, soupbintcp.hpp, moldudp64.hpp, ouch.hpp | Named per-module leveled logging (SPDLOG_LOGGER_* macros, SPDLOG_ACTIVE_LEVEL compile-time filter) |
| eph-core | framer.hpp, soupbintcp.hpp | LengthPrefixFramer, MessageFramer concept, FrameError, DecodedFrame |
| gtest | tests/*.cpp | Unit tests |
| benchmark | benchmarks/bench_itch_parse.cpp | Microbenchmarks |

---

## Testing

| Suite | Location | Coverage Focus |
|---|---|---|
| test_itch | tests/test_itch.cpp | ITCH 5.0 parser + every per-message accessor namespace; happy / error paths |
| test_moldudp64 | tests/test_moldudp64.cpp | MoldUDP64 header parse, iteration, end-of-session, truncation |
| test_soupbintcp | tests/test_soupbintcp.cpp | SoupBinTCP encode/decode, heartbeats, length boundary conditions |
| test_ouch | tests/test_ouch.cpp | OUCH inbound builders + outbound view field offsets |
| bench_itch_parse | benchmarks/bench_itch_parse.cpp | parse() + accessor throughput and latency |

Key test scenarios exercised across the suite:

- All 22 ITCH message type constants match their ASCII characters and
  declared sizes.
- parse() returns kIncomplete for empty buffers, kUnknownType for
  unrecognised type bytes, kTruncated when len < message_size(type).
- parse_all() handles concatenated buffers and a trailing partial
  (kIncomplete) message without treating the remnant as an error when
  called with ParserStats.
- ItchFramer and SoupBinTcpFramer both satisfy the MessageFramer concept
  (enforced via static_assert at the bottom of each header).
- MoldUDP64 end-of-session (message_count == 0xFFFF) returns 0 and does
  not invoke the callback.
- OUCH EnterOrder::build rejects side bytes other than 'B'/'S'.
- OUCH outbound views return valid() == false on undersized buffers.

To run the full suite:

```
xmake build test_itch test_moldudp64 test_ouch test_soupbintcp
xmake run test_itch
xmake run test_moldudp64
xmake run test_ouch
xmake run test_soupbintcp
```

The cross-module integration test `test_itch_adapter` in
`tests/integration/test_itch_adapter.cpp` exercises
`eph::book::ItchBookBuilder<N>::process` against every wired ITCH
message type — it lives at the repo-root level so it can link both
`eph-itch` and `eph-book` without forcing an `eph-book` dep on the
in-module test build.
