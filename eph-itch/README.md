# eph-itch

Header-only C++23 library for zero-copy parsing of Nasdaq ITCH 5.0 market data and related protocols (MoldUDP64, SoupBinTCP, OUCH 5.0), designed for HFT feed handling with no allocations on the hot path.

## Overview

eph-itch is the Nasdaq protocol layer of the eph ecosystem. It provides:

- **ITCH 5.0 message parsing** -- All 22 message types with zero-copy field accessors operating directly on raw byte buffers. No deserialization, no copies.
- **Type-safe dispatch** -- Compile-time tag-based visitor dispatch for zero-overhead handler overload resolution across all ITCH message types.
- **Transport protocol support** -- Framers and parsers for MoldUDP64 (multicast), SoupBinTCP (TCP), and OUCH 5.0 (order entry), covering the full Nasdaq data and order entry stack.
- **Production monitoring** -- `ParserStats` with throughput, error rate, and first-error diagnostics for integration with Prometheus/StatsD.
- **`std::formatter` specializations** -- `ParseError`, `MessageView`, and `ParserStats` are all directly usable with `std::format` and `std::print`.

All multi-byte integers are big-endian on the wire. Accessors use `std::memcpy` + `std::byteswap` for safe, portable decoding.

## Key Components

All headers are under `include/eph/itch/`:

| Header | Description |
|---|---|
| **messages.hpp** | ITCH 5.0 message type definitions, wire constants, and zero-copy field accessors. Defines all 22 message types with their on-wire sizes. Provides per-message accessor namespaces (e.g. `add_order::`, `order_executed::`, `cross_trade::`) that operate directly on raw byte pointers. Includes big-endian read helpers, string trimming, price scaling constants, message classification helpers, and compile-time `kMaxMessageSize`/`kMinMessageSize`. |
| **parser.hpp** | Zero-copy ITCH message parser. `parse()` returns a `MessageView` pointing into the receive buffer. `parse_all()` iterates consecutive messages with a callback. `dispatch()` and `dispatch_all()` provide tag-based visitor dispatch using `msg::` tag structs. Includes `ParserStats` for throughput/error monitoring. |
| **framer.hpp** | `ItchFramer` type alias over `eph::net::LengthPrefixFramer` (2-byte big-endian length prefix), satisfying the `MessageFramer` concept for use with the eph transport layer. |
| **soupbintcp.hpp** | `SoupBinTcpFramer` implementing Nasdaq's TCP transport protocol. Wire format: 2-byte big-endian length + 1-byte packet type + payload. Provides `encode()` and `decode()` methods, packet type constants in `soupbin::` namespace, and satisfies the `MessageFramer` concept. |
| **moldudp64.hpp** | MoldUDP64 multicast packet parser. `parse_moldudp64_header()` extracts the 20-byte header. `parse_moldudp64()` iterates over length-prefixed messages within a UDP payload, delivering each to a callback with its computed sequence number. |
| **ouch.hpp** | Nasdaq OUCH 5.0 order entry protocol. Inbound message builders (`EnterOrder`, `ReplaceOrder`, `CancelOrder`) serialize orders into wire format. Outbound zero-copy views (`AcceptedView`, `ExecutedView`, `CanceledView`, `ReplacedView`) provide field accessors over raw response bytes. |

The convenience header `include/eph/itch.hpp` includes all of the above in a single include.

## Public API Reference

### Endian Helpers (`eph::itch`)

| Function | Description |
|---|---|
| `read_be16(p)` | Read big-endian `uint16_t` from byte pointer |
| `read_be32(p)` | Read big-endian `uint32_t` from byte pointer |
| `read_be48(p)` | Read 6-byte big-endian timestamp into `uint64_t` |
| `read_be64(p)` | Read big-endian `uint64_t` from byte pointer |

### Utilities (`eph::itch`)

| Symbol | Description |
|---|---|
| `trim(s)` | Remove trailing spaces from a right-padded ITCH string field |
| `kItchPriceDivisor` | `10'000.0` -- divide raw price fields by this for dollars |
| `kLuldPriceDivisor` | `100'000'000.0` -- LULD auction collar price divisor |

### Common Header Accessors (`eph::itch`)

All ITCH messages share a common 11-byte header. These functions take a pointer to the body (after the 1-byte type tag):

| Function | Description |
|---|---|
| `stock_locate(body)` | Stock locate code (2 bytes) |
| `tracking_number(body)` | Tracking number (2 bytes) |
| `timestamp_ns(body)` | Nanoseconds since midnight (6 bytes) |

### Message Type Constants (`eph::itch`)

22 message types, each with a `k<Name>` type constant (`uint8_t`) and `k<Name>Size` wire size (`size_t`):

| Type | Char | Size | Category |
|---|---|---|---|
| `kSystemEvent` | `'S'` | 12 | System |
| `kStockDirectory` | `'R'` | 39 | System |
| `kStockTradingAction` | `'H'` | 25 | System |
| `kRegSHORestriction` | `'Y'` | 20 | System |
| `kMarketParticipantPosition` | `'L'` | 26 | System |
| `kMWCBDeclineLevel` | `'V'` | 35 | System |
| `kMWCBStatus` | `'W'` | 12 | System |
| `kIPOQuotingPeriod` | `'K'` | 28 | System |
| `kLULDAuctionCollar` | `'J'` | 35 | System |
| `kOperationalHalt` | `'h'` | 21 | System |
| `kAddOrder` | `'A'` | 36 | Order |
| `kAddOrderMPID` | `'F'` | 40 | Order |
| `kOrderExecuted` | `'E'` | 31 | Order |
| `kOrderExecutedWithPrice` | `'C'` | 36 | Order |
| `kOrderCancel` | `'X'` | 23 | Order |
| `kOrderDelete` | `'D'` | 19 | Order |
| `kOrderReplace` | `'U'` | 35 | Order |
| `kNonCrossTrade` | `'P'` | 44 | Trade |
| `kCrossTrade` | `'Q'` | 40 | Trade |
| `kBrokenTrade` | `'B'` | 19 | Trade |
| `kNOII` | `'I'` | 50 | Imbalance |
| `kRPII` | `'N'` | 20 | Imbalance |

Aggregate constants: `kMaxMessageSize` (50), `kMinMessageSize` (12).

### Message Classification (`eph::itch`)

| Function | Description |
|---|---|
| `is_system_message(type)` | System/reference-data messages (events, directories, halts, collars) |
| `is_order_message(type)` | Order-lifecycle messages (add, execute, cancel, delete, replace) |
| `is_trade_message(type)` | Trade messages (non-cross, cross, broken) |
| `is_imbalance_message(type)` | NOII and RPII imbalance indicators |
| `is_known_type(type)` | Any of the 22 known ITCH 5.0 types |

### Per-Message Accessor Namespaces (`eph::itch`)

Each namespace operates on the full message pointer (byte 0 = type tag):

| Namespace | Key Accessors |
|---|---|
| `system_event::` | `event_code` |
| `stock_directory::` | `stock`, `stock_trimmed`, `market_category`, `financial_status`, `round_lot_size`, `round_lots_only`, `issue_classification`, `issue_subtype`, `authenticity`, `short_sale_threshold`, `ipo_flag`, `luld_ref_price_tier`, `etp_flag`, `etp_leverage_factor`, `inverse_indicator` |
| `add_order::` | `order_ref`, `side`, `shares`, `stock`, `stock_trimmed`, `price_raw`, `price` |
| `add_order_mpid::` | Same as `add_order::` plus `attribution`, `attribution_trimmed` |
| `order_executed::` | `order_ref`, `executed_shares`, `match_number` |
| `order_executed_price::` | `order_ref`, `executed_shares`, `match_number`, `printable`, `execution_price_raw`, `execution_price` |
| `order_cancel::` | `order_ref`, `cancelled_shares` |
| `order_delete::` | `order_ref` |
| `order_replace::` | `original_order_ref`, `new_order_ref`, `shares`, `price_raw`, `price` |
| `non_cross_trade::` | `order_ref`, `side`, `shares`, `stock`, `stock_trimmed`, `price_raw`, `price`, `match_number` |
| `cross_trade::` | `shares`, `stock`, `stock_trimmed`, `cross_price_raw`, `cross_price`, `match_number`, `cross_type` |
| `stock_trading_action::` | `stock`, `stock_trimmed`, `trading_state`, `reason` |

### Parser (`eph::itch`)

| Symbol | Description |
|---|---|
| `ParseError` | Enum: `kIncomplete`, `kUnknownType`, `kTruncated` |
| `parse_error_name(e)` | Human-readable name for a `ParseError` |
| `message_size(type)` | Fixed on-wire size for a known message type (0 for unknown) |
| `message_type_name(type)` | Human-readable name for a message type byte |
| `MessageView` | Zero-copy view: `msg_type`, `data`, `length`, plus `stock_locate()`, `tracking_number()`, `timestamp_ns()`, `dump()`, `to_json()` |
| `parse(data, len)` | Parse single message -> `std::expected<MessageView, ParseError>` |
| `parse(span)` | Span overload of `parse()` |
| `parse_all(data, len, callback)` | Parse consecutive messages, callback per message. Returns bytes consumed. |
| `parse_all(data, len, callback, stats)` | Same with `ParserStats` accumulation |
| `ParserStats` | Counters: `messages_parsed`, `parse_errors`, `bytes_consumed`, first-error diagnostics. Methods: `on_message()`, `on_error()`, `reset()`, `dump()`, `to_json()`, `throughput(ns)`, `error_rate()`, `operator-` for deltas |

### Tag-Based Dispatch (`eph::itch`)

| Symbol | Description |
|---|---|
| `msg::SystemEvent`, `msg::AddOrder`, ... | Empty tag structs for compile-time message type discrimination (22 types + `msg::Unknown`) |
| `dispatch(view, handler)` | Dispatch a `MessageView` to a handler as `handler(Tag{}, raw_msg_ptr)` |
| `dispatch_all(data, len, handler)` | Parse + dispatch consecutive messages in one pass |
| `dispatch_all(data, len, handler, stats)` | Same with `ParserStats` accumulation |

### Framer (`eph::itch`)

| Symbol | Description |
|---|---|
| `ItchFramer` | Type alias for `eph::net::LengthPrefixFramer` (2-byte BE length prefix). Satisfies `MessageFramer`. |

### SoupBinTCP (`eph::itch`)

| Symbol | Description |
|---|---|
| `soupbin::kSequencedData` | `'S'` -- server->client ITCH messages |
| `soupbin::kServerHeartbeat` | `'H'` -- server heartbeat |
| `soupbin::kLoginAccepted` | `'A'` -- login accepted |
| `soupbin::kLoginRejected` | `'J'` -- login rejected |
| `soupbin::kLoginRequest` | `'L'` -- client login request |
| `soupbin::kUnsequencedData` | `'U'` -- client unsequenced data |
| `soupbin::kClientHeartbeat` | `'R'` -- client heartbeat |
| `soupbin::kLogoutRequest` | `'O'` -- client logout |
| `soupbin::kDebug` | `'+'` -- debug text |
| `soupbin::is_heartbeat(type)` | Check if packet type is a heartbeat |
| `SoupBinTcpFramer` | Framer class with `encode(out, data, len, msg_type)` and `decode(data, len)`. Max payload: 65534. Satisfies `MessageFramer`. |

### MoldUDP64 (`eph::itch`)

| Symbol | Description |
|---|---|
| `moldudp64::kSessionLen` | 10 -- session ID field length |
| `moldudp64::kHeaderLen` | 20 -- total header length |
| `moldudp64::kEndOfSession` | `0xFFFF` -- sentinel message count |
| `MoldUDP64Header` | Parsed header: `session` (string_view), `sequence_number` (uint64_t), `message_count` (uint16_t) |
| `parse_moldudp64_header(data, len)` | Parse 20-byte header -> `std::expected<MoldUDP64Header, ParseError>` |
| `parse_moldudp64(data, len, callback)` | Iterate messages in a UDP payload, callback receives `(msg_data, msg_len, seq_num)`. Returns message count delivered. |

### OUCH 5.0 (`eph::itch::ouch`)

**Wire encoding helpers:**

| Function | Description |
|---|---|
| `write_be16(p, v)` | Write big-endian uint16_t |
| `write_be32(p, v)` | Write big-endian uint32_t |
| `write_be64(p, v)` | Write big-endian uint64_t |
| `write_padded(p, s, width)` | Write right-padded string field |

**Message type constants** (`ouch::msg_type`):

| Constant | Char | Direction |
|---|---|---|
| `kEnterOrder` | `'O'` | Client -> Nasdaq |
| `kReplaceOrder` | `'U'` | Client -> Nasdaq |
| `kCancelOrder` | `'X'` | Client -> Nasdaq |
| `kAccepted` | `'A'` | Nasdaq -> Client |
| `kExecuted` | `'E'` | Nasdaq -> Client |
| `kCanceled` | `'C'` | Nasdaq -> Client |
| `kReplaced` | `'U'` | Nasdaq -> Client |

**Inbound builders** (client -> Nasdaq):

| Builder | Size | Key Parameters |
|---|---|---|
| `EnterOrder::build(buf, token, side, shares, symbol, price, tif, firm)` | 49 | Validates side ('B'/'S'), sets defaults for display/capacity/sweep/cross |
| `ReplaceOrder::build(buf, existing_token, replacement_token, shares, price, tif)` | 47 | Replaces an existing order with new price/size |
| `CancelOrder::build(buf, token, shares)` | 19 | Cancel shares (0 = entire remaining quantity) |

**Outbound views** (Nasdaq -> client, zero-copy):

| View | Size | Key Accessors |
|---|---|---|
| `AcceptedView` | 66 | `valid()`, `timestamp()`, `token()`, `side()`, `shares()`, `symbol()`, `price()`, `time_in_force()`, `firm()`, `display()`, `order_reference()`, `capacity()`, `order_state()` |
| `ExecutedView` | 40 | `valid()`, `timestamp()`, `token()`, `executed_shares()`, `execution_price()`, `liquidity_flag()`, `match_number()` |
| `CanceledView` | 28 | `valid()`, `timestamp()`, `token()`, `decrement_shares()`, `reason()` |
| `ReplacedView` | 80 | `valid()`, `timestamp()`, `replacement_token()`, `side()`, `shares()`, `symbol()`, `price()`, `time_in_force()`, `firm()`, `display()`, `order_reference()`, `capacity()`, `int_mkt_sweep()`, `cross_type()`, `order_state()`, `previous_token()` |

## Dependencies

- **eph-core** -- `LengthPrefixFramer` (framer.hpp), `MessageFramer` concept and `FrameError`/`DecodedFrame` types (soupbintcp.hpp)
- **spdlog** -- Leveled logging throughout (parser, soupbintcp, moldudp64, ouch)

## Usage Examples

### Parse a single ITCH message

```cpp
#include <eph/itch/parser.hpp>

auto result = eph::itch::parse(data, len);
if (result) {
    auto& msg = *result;
    std::println("type={} locate={} ts={}ns",
        eph::itch::message_type_name(msg.msg_type),
        msg.stock_locate(), msg.timestamp_ns());
}
```

### Type-safe dispatch with handler overloads

```cpp
#include <eph/itch/parser.hpp>

struct MyHandler {
    void operator()(eph::itch::msg::AddOrder, const uint8_t* msg) {
        auto ref   = eph::itch::add_order::order_ref(msg);
        auto price = eph::itch::add_order::price(msg);
        auto side  = eph::itch::add_order::side(msg);
    }
    void operator()(eph::itch::msg::OrderDelete, const uint8_t* msg) {
        auto ref = eph::itch::order_delete::order_ref(msg);
    }
    template <typename T>
    void operator()(T, const uint8_t*) { /* ignore other types */ }
};

// Parse + dispatch a buffer of consecutive messages in one pass
eph::itch::ParserStats stats;
size_t consumed = eph::itch::dispatch_all(buf, buf_len, MyHandler{}, stats);
std::println("throughput: {:.0f} msg/s, error rate: {:.4f}",
    stats.throughput(elapsed_ns), stats.error_rate());
```

### MoldUDP64 multicast feed

```cpp
#include <eph/itch/moldudp64.hpp>
#include <eph/itch/parser.hpp>

// Process a UDP payload containing batched ITCH messages
eph::itch::parse_moldudp64(udp_data, udp_len,
    [](const uint8_t* msg_data, uint16_t msg_len, uint64_t seq_num) {
        auto result = eph::itch::parse(msg_data, msg_len);
        if (result) {
            // result->msg_type, result->stock_locate(), etc.
        }
    });

// Or just parse the header for gap detection
auto hdr = eph::itch::parse_moldudp64_header(udp_data, udp_len);
if (hdr) {
    if (hdr->message_count == eph::itch::moldudp64::kEndOfSession) {
        // session ended
    }
}
```

### SoupBinTCP framing

```cpp
#include <eph/itch/soupbintcp.hpp>

eph::itch::SoupBinTcpFramer framer;

// Encode an ITCH message into SoupBinTCP wire format
uint8_t wire_buf[65537];
size_t wire_len = framer.encode(wire_buf, payload, payload_len,
                                eph::itch::soupbin::kSequencedData);

// Decode a packet from the receive buffer
auto frame = framer.decode(rx_buf, rx_len);
if (frame) {
    if (frame->msg_type == eph::itch::soupbin::kSequencedData) {
        auto result = eph::itch::parse(frame->payload, frame->payload_len);
        // process ITCH message
    } else if (frame->is_control) {
        // heartbeat -- no payload
    }
}
```

### OUCH 5.0 order entry

```cpp
#include <eph/itch/ouch.hpp>

// Build an enter-order message
uint8_t buf[eph::itch::ouch::EnterOrder::kSize];
eph::itch::ouch::EnterOrder::build(
    buf, "TOKEN1234     ", 'B', 100, "AAPL    ", 1500000, 0, "FIRM");

// Build a replace-order message
uint8_t rbuf[eph::itch::ouch::ReplaceOrder::kSize];
eph::itch::ouch::ReplaceOrder::build(
    rbuf, "TOKEN1234     ", "TOKEN5678     ", 200, 1510000, 0);

// Build a cancel-order message (0 = cancel all remaining shares)
uint8_t cbuf[eph::itch::ouch::CancelOrder::kSize];
eph::itch::ouch::CancelOrder::build(cbuf, "TOKEN1234     ", 0);

// Parse an execution report
eph::itch::ouch::ExecutedView exec(response_data, response_len);
if (exec.valid()) {
    auto shares = exec.executed_shares();
    auto price  = exec.execution_price();
    auto match  = exec.match_number();
    auto flag   = exec.liquidity_flag();
}

// Parse an order-accepted confirmation
eph::itch::ouch::AcceptedView accepted(response_data, response_len);
if (accepted.valid()) {
    auto token = accepted.token();
    auto ref   = accepted.order_reference();
    auto state = accepted.order_state();  // 'L'=live, 'D'=dead
}
```
