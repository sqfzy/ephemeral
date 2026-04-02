# eph-itch

Header-only C++23 library for zero-copy parsing of Nasdaq ITCH 5.0 market data and related protocols (MoldUDP64, SoupBinTCP, OUCH 5.0), designed for HFT feed handling with no allocations on the hot path.

## Key Components

All headers are under `include/eph/itch/`:

- **messages.hpp** -- ITCH 5.0 message type definitions, wire constants, and zero-copy field accessors. Defines all 22 message types with their on-wire sizes and provides per-message accessor namespaces (`add_order::`, `order_executed::`, `cross_trade::`, etc.) that operate directly on raw byte pointers. Includes big-endian read helpers (`read_be16/32/48/64`), right-padded string trimming (`trim()`), price scaling constants, message classification helpers (`is_order_message`, `is_trade_message`, `is_system_message`, `is_imbalance_message`), and compile-time `kMaxMessageSize`/`kMinMessageSize` derived from all known types.
- **parser.hpp** -- Zero-copy ITCH message parser. `parse()` takes raw bytes (after framing extraction) and returns a `MessageView` pointing back into the receive buffer. `parse_all()` iterates consecutive messages with a callback, optionally accumulating `ParserStats` (throughput, error rate, first-error diagnostics). `dispatch()` and `dispatch_all()` provide type-safe tag-based visitor dispatch using `msg::` tag structs (e.g. `msg::AddOrder`, `msg::OrderDelete`) for zero-overhead handler overload resolution. Includes `std::formatter` specializations for `ParseError`, `MessageView`, and `ParserStats`.
- **framer.hpp** -- `ItchFramer` type alias over `eph::net::LengthPrefixFramer` (2-byte big-endian length prefix), satisfying the `MessageFramer` concept for use with the eph transport layer.
- **soupbintcp.hpp** -- `SoupBinTcpFramer` implementing Nasdaq's TCP transport protocol. Wire format: 2-byte big-endian length + 1-byte packet type + payload. Provides `encode()` and `decode()` methods, packet type constants in `soupbin::` namespace (SequencedData, LoginAccepted, heartbeats, etc.), and satisfies the `MessageFramer` concept.
- **moldudp64.hpp** -- MoldUDP64 multicast packet parser. `parse_moldudp64_header()` extracts the 20-byte header (session, sequence number, message count). `parse_moldudp64()` iterates over length-prefixed messages within a UDP payload, delivering each to a callback with its computed sequence number. Handles end-of-session markers and truncation gracefully.
- **ouch.hpp** -- Nasdaq OUCH 5.0 order entry protocol. Inbound message builders: `EnterOrder::build()`, `ReplaceOrder::build()`, `CancelOrder::build()` serialize orders into wire format. Outbound zero-copy views: `AcceptedView`, `ExecutedView`, `CanceledView`, `ReplacedView` provide field accessors over raw response bytes with validity checking.

## Dependencies

- **eph-core** -- `LengthPrefixFramer` (framer.hpp), `MessageFramer` concept (soupbintcp.hpp)
- **spdlog** -- Logging (parser, soupbintcp, moldudp64, ouch)

## Quick Start

```cpp
#include <eph/itch/parser.hpp>

// Parse a single ITCH message from raw bytes (after framing extraction)
auto result = eph::itch::parse(data, len);
if (result) {
    auto& msg = *result;
    // msg.msg_type, msg.stock_locate(), msg.timestamp_ns()
}

// Type-safe dispatch with handler overloads
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
```

### MoldUDP64 multicast feed

```cpp
#include <eph/itch/moldudp64.hpp>

// Process a UDP payload containing batched ITCH messages
eph::itch::parse_moldudp64(udp_data, udp_len,
    [](const uint8_t* msg_data, uint16_t msg_len, uint64_t seq_num) {
        auto result = eph::itch::parse(msg_data, msg_len);
        if (result) { /* handle message */ }
    });
```

### OUCH 5.0 order entry

```cpp
#include <eph/itch/ouch.hpp>

// Build an order
uint8_t buf[eph::itch::ouch::EnterOrder::kSize];
eph::itch::ouch::EnterOrder::build(
    buf, "TOKEN1234     ", 'B', 100, "AAPL    ", 1500000, 0, "FIRM");

// Parse an execution report
eph::itch::ouch::ExecutedView exec(response_data, response_len);
if (exec.valid()) {
    auto shares = exec.executed_shares();
    auto price  = exec.execution_price();
    auto match  = exec.match_number();
}
```
