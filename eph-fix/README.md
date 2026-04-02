# eph-fix

Header-only C++23 library for FIX protocol parsing, message building, session management, and order lifecycle tracking, optimized for HFT systems. Zero-copy parsing, zero-allocation hot paths, `std::expected` error handling.

Part of the **eph** ecosystem -- provides FIX 4.4 protocol support on top of `eph-core` framing and parsing primitives, designed to integrate with `eph-net` / `eph-transport` for wire-level connectivity.

## Overview

eph-fix covers the full FIX protocol stack from wire bytes to order lifecycle management:

- **Wire layer** -- Zero-copy parser and zero-allocation builder for FIX tag=value messages, plus a framer that satisfies the `eph::net::MessageFramer` concept for integration with eph-transport.
- **Session layer** -- FIX 4.4 session management with Logon/Logout handshake, Heartbeat/TestRequest, bidirectional sequence number tracking, gap detection, and SequenceReset/GapFill handling.
- **Order entry** -- Typed builders for NewOrderSingle (D), OrderCancelRequest (F), and OrderCancelReplaceRequest (G) with strongly-typed enums for Side, OrdType, and TimeInForce.
- **Execution processing** -- Zero-copy typed view over ExecutionReport (8) with fill detection, terminal state predicates, and a combined parse+validate convenience function.
- **Risk and position management** -- Per-symbol position tracking with VWAP and realized PnL, pre-trade risk checking against configurable limits, and full order state machine with automatic position integration.

All components are header-only, use stack allocation on the hot path, and return errors via `std::expected` or `std::optional` instead of exceptions.

## Key Components

All headers are under `include/eph/fix/`:

| Header | Description |
|---|---|
| **tags.hpp** | FIX tag constants (`eph::fix::tag` namespace) covering session, order entry, and market data tags. Includes `msg_type` sub-namespace for MsgType character constants. Provides `tag_name()` and `msg_type_name()` for human-readable diagnostics. |
| **parser.hpp** | Zero-copy FIX message parser. Parses `tag=value\x01` messages into a stack-allocated `BasicMessageView<MaxFields>` of `Field` structs. Typed accessors for char, int, double, bool, and UTCTimestamp values. Supports repeating group iteration via `get_group()`, `get_nth()`, and `for_each_matching()`. Includes checksum validation. |
| **builder.hpp** | Zero-allocation FIX message builder. Writes fields into a caller-provided buffer via fluent `set()` / `set_int()` / `set_double()` / `set_char()` / `set_bool()` / `set_timestamp()` / `set_decimal()` / `set_price()` / `set_raw()` methods. `finish()` prepends BeginString + BodyLength and appends CheckSum. Duplicate-tag-safe `_unique` variants. Configurable timestamp precision (seconds through nanoseconds). |
| **framer.hpp** | `BasicFixFramer<MaxBodyLength>` satisfying `eph::net::MessageFramer`. Finds message boundaries by parsing BeginString/BodyLength/CheckSum structure. Validates checksum on decode. Extracts MsgType hint for the transport layer. Default alias: `FixFramer` (1 MB max body). |
| **orders.hpp** | Typed builder wrappers for HFT order entry. `build_new_order()` (D), `build_cancel_order()` (F), `build_replace_order()` (G). Each builds a complete FIX message into a caller-owned buffer with no heap allocation. Includes `Side`, `OrdType`, and `TimeInForce` enums. |
| **execution_report.hpp** | Zero-copy typed view over ExecutionReport (8). `ExecutionReportView<MaxFields>` provides accessors for identifiers, status, fills, and convenience predicates (`is_fill()`, `is_terminal()`). `try_parse_execution_report()` combines parse + MsgType check. Includes `ExecType` and `OrdStatus` enums. |
| **session.hpp** | FIX 4.4 session layer. `FixSession` provides Logon/Logout handshake, automatic Heartbeat/TestRequest, heartbeat timeout detection, bidirectional MsgSeqNum tracking with gap detection, SequenceReset/GapFill handling, and optional ResendRequest. Thread-safe via atomics. Pluggable send callback. |
| **position.hpp** | Per-symbol position tracker. `PositionTracker` records fills and computes signed quantity, VWAP entry price, realized PnL (average-cost method), and notional exposure. Handles position flips (long-to-short crossover). |
| **risk_check.hpp** | Pre-trade risk checker. `RiskChecker` validates orders against configurable `RiskLimits`: max order qty, max order notional, per-symbol position limits, and total exposure. Returns typed `RiskRejectReason`. Limits are hot-updatable at runtime. |
| **order_manager.hpp** | Order lifecycle manager. `OrderManager` tracks orders from submission through fills/cancels with full state machine. Processes `ExecutionReportView` events and optionally forwards fills to a `PositionTracker`. Handles cancel rejects by reverting PendingCancel state. |
| **fix.hpp** | Aggregation header -- includes all of the above in one `#include`. |

## Public API Reference

### Namespace `eph::fix::tag`

**Tag constants** -- `inline constexpr uint32_t` values for FIX tags:

| Category | Tags |
|---|---|
| Session | `BeginString` (8), `BodyLength` (9), `MsgType` (35), `SenderCompID` (49), `TargetCompID` (56), `MsgSeqNum` (34), `SendingTime` (52), `CheckSum` (10), `HeartBtInt` (108), `TestReqID` (112), `ResetSeqNumFlag` (141), `NewSeqNo` (36), `PossDupFlag` (43), `GapFillFlag` (123), `BeginSeqNo` (7), `EndSeqNo` (16), `EncryptMethod` (98), `OrigSendingTime` (122), `PossResend` (97), `SenderSubID` (50), `TargetSubID` (57) |
| Order entry | `ClOrdID` (11), `OrigClOrdID` (41), `OrderID` (37), `ExecID` (17), `ExecType` (150), `OrdStatus` (39), `Symbol` (55), `Side` (54), `OrderQty` (38), `OrdType` (40), `Price` (44), `TimeInForce` (59), `TransactTime` (60), `LastPx` (31), `LastQty` (32), `CumQty` (14), `AvgPx` (6), `LeavesQty` (151), `Text` (58), `HandlInst` (21), `Account` (1), `ExDestination` (100), `StopPx` (99), `MinQty` (110), `MaxFloor` (111), `SecurityID` (48), `SecurityIDSource` (22), `CxlRejReason` (102), `OrdRejReason` (103) |
| Market data | `MDReqID` (262), `SubscriptionRequestType` (263), `MarketDepth` (264), `MDUpdateType` (265), `NoMDEntryTypes` (267), `MDEntryType` (269), `MDEntryPx` (270), `MDEntrySize` (271), `NoMDEntries` (268), `MDUpdateAction` (279), `SecurityExchange` (207) |

**Message type constants** (`eph::fix::tag::msg_type`):

| Constant | Value | Description |
|---|---|---|
| `Heartbeat` | `'0'` | Heartbeat |
| `TestRequest` | `'1'` | TestRequest |
| `ResendRequest` | `'2'` | ResendRequest |
| `Reject` | `'3'` | Session-level Reject |
| `SequenceReset` | `'4'` | SequenceReset |
| `Logout` | `'5'` | Logout |
| `Logon` | `'A'` | Logon |
| `ExecutionReport` | `'8'` | ExecutionReport |
| `OrderCancelReject` | `'9'` | OrderCancelReject |
| `NewOrderSingle` | `'D'` | NewOrderSingle |
| `OrderCancelRequest` | `'F'` | OrderCancelRequest |
| `OrderCancelReplace` | `'G'` | OrderCancelReplaceRequest |
| `MarketDataRequest` | `'V'` | MarketDataRequest |
| `MarketDataSnapshot` | `'W'` | MarketDataSnapshot/FullRefresh |
| `MarketDataIncRefresh` | `'X'` | MarketDataIncrementalRefresh |
| `TradeCaptureReport` | `"AE"` | TradeCaptureReport (string_view) |
| `TradeCaptureReportAck` | `"AR"` | TradeCaptureReportAck (string_view) |
| `PositionReport` | `"AP"` | PositionReport (string_view) |

**Lookup functions**:

| Function | Signature | Description |
|---|---|---|
| `tag_name` | `constexpr string_view tag_name(uint32_t t)` | Human-readable tag name, or `"Unknown"` |
| `msg_type_name` | `constexpr string_view msg_type_name(char mt)` | Message type name from single char |
| `msg_type_name` | `constexpr string_view msg_type_name(string_view mt)` | Message type name from string (handles multi-char) |

### Parser (`parser.hpp`)

| Type / Function | Description |
|---|---|
| `Field` | Struct: `uint32_t tag` + `string_view value` (zero-copy view into source buffer) |
| `BasicMessageView<MaxFields>` | Stack-allocated parsed message view. Default `MaxFields = 128`. |
| `MessageView` | Alias for `BasicMessageView<128>` |
| `ParseError` | Enum: `kIncomplete`, `kInvalidFormat`, `kChecksumMismatch`, `kFieldOverflow` |
| `parse_error_name(ParseError)` | Human-readable error name |
| `parse<MaxFields, MaxBodyLength>(data, len)` | Parse raw bytes -> `expected<BasicMessageView, ParseError>` |
| `compute_checksum(data, len)` | Sum of all bytes mod 256 |
| `verify_checksum(data, len)` | Verify checksum of a complete FIX message |
| `parse_tag_number(p, end)` | Parse decimal tag number, advancing pointer past `=` |
| `kDefaultMaxBodyLength` | Default max body length: 1 MB |

**`BasicMessageView<N>` methods**:

| Method | Return | Description |
|---|---|---|
| `field_count()` | `size_t` | Number of parsed fields |
| `total_len()` | `size_t` | Total consumed bytes |
| `begin_string()` | `string_view` | BeginString value (e.g. `"FIX.4.4"`) |
| `get(tag)` | `optional<string_view>` | First field with given tag |
| `has(tag)` | `bool` | Whether tag exists |
| `msg_type()` | `optional<string_view>` | Shortcut for `get(tag::MsgType)` |
| `get_char(tag)` | `optional<char>` | Single-character field value |
| `get_int(tag)` | `optional<int64_t>` | Parse as signed integer |
| `get_double(tag)` | `optional<double>` | Parse as double |
| `get_bool(tag)` | `optional<bool>` | Parse FIX boolean (Y/N) |
| `get_timestamp(tag)` | `optional<uint64_t>` | Parse UTCTimestamp to epoch nanoseconds |
| `count(tag)` | `size_t` | Number of occurrences of a tag |
| `get_nth(tag, n)` | `optional<string_view>` | Nth occurrence (0-based) |
| `for_each(fn)` | `void` | Iterate all fields: `fn(uint32_t tag, string_view value)` |
| `for_each_matching(tag, fn)` | `void` | Iterate all occurrences: `fn(string_view value)` |
| `get_group(count_tag, delim_tag, out, max)` | `RepeatingGroupView` | Extract repeating group entries |
| `dump()` | `string` | Multi-line diagnostic output |
| `to_json()` | `string` | JSON-formatted message for monitoring |
| `begin()` / `end()` | `const Field*` | Random-access iteration over fields |

**`GroupEntry`** (nested in `BasicMessageView`): same `get`, `has`, `get_char`, `get_int`, `get_double`, `get_bool`, `get_timestamp` accessors scoped to a single repeating group entry.

**`RepeatingGroupView`** (nested in `BasicMessageView`): `size()`, `empty()`, `operator[]`, range-for iteration over `GroupEntry` elements.

### Builder (`builder.hpp`)

| Type / Method | Description |
|---|---|
| `MessageBuilder(buf, capacity)` | Construct builder writing into caller-owned buffer |
| `set(tag, string_view)` | Append string field (validates no embedded SOH) |
| `set_int(tag, int64_t)` | Append integer field |
| `set_double(tag, double, precision=2)` | Append floating-point field (precision 0-15) |
| `set_char(tag, char)` | Append single-character field |
| `set_bool(tag, bool)` | Append FIX boolean (Y/N) |
| `set_timestamp(tag, epoch_ns, prec)` | Append UTCTimestamp with configurable precision |
| `set_decimal(tag, string_view)` | Append exact decimal string (no float rounding) |
| `set_price(tag, mantissa, decimals)` | Append price from integer mantissa + exponent |
| `set_raw(tag, data, len)` | Append raw bytes (validates no embedded SOH) |
| `set_unique(tag, value)` | Like `set()` but rejects duplicate tags |
| `set_int_unique`, `set_double_unique`, `set_char_unique`, `set_bool_unique`, `set_timestamp_unique`, `set_raw_unique` | Duplicate-safe variants of each setter |
| `begin_group(count_tag, count)` | Write repeating group count tag |
| `finish(begin_string="FIX.4.4")` | Finalize: prepend header, append checksum. Returns total length or 0 on overflow. |
| `reset()` | Reset for reuse with same buffer |
| `has_overflow()` | Check if buffer overflowed |
| `has_tag(tag)` | Check if tag already written |
| `field_count()` | Number of body fields appended |
| `bytes_used()` | Body bytes written so far |
| `remaining_capacity()` | Approximate remaining space |
| `data()` | Pointer to finalized message (after `finish()`) |
| `size()` | Total message length (after `finish()`) |
| `as_span()` | `span<const uint8_t>` view of finalized message |
| `as_string_view()` | `string_view` of finalized message |

**`TimestampPrecision`** enum (nested): `kSeconds` (17 chars), `kMilliseconds` (21), `kMicroseconds` (24), `kNanoseconds` (27).

### Framer (`framer.hpp`)

| Type / Method | Description |
|---|---|
| `BasicFixFramer<MaxBodyLength>` | FIX message framer satisfying `eph::net::MessageFramer` |
| `FixFramer` | Alias for `BasicFixFramer<>` (1 MB max body) |
| `max_overhead()` | Always 0 -- FIX messages are self-framed |
| `encode(out, data, len, msg_type)` | Pass-through copy (FIX is already framed) |
| `decode(data, len)` | Find complete message -> `expected<DecodedFrame, FrameError>` |

### Orders (`orders.hpp`)

**Enums**:

| Enum | Values |
|---|---|
| `Side` | `Buy` (`'1'`), `Sell` (`'2'`) |
| `OrdType` | `Market` (`'1'`), `Limit` (`'2'`) |
| `TimeInForce` | `Day` (`'0'`), `GTC` (`'1'`), `IOC` (`'3'`), `FOK` (`'4'`) |

**Builder functions** -- all return bytes written (0 on error), write into caller-owned buffer:

| Function | MsgType | Key Parameters |
|---|---|---|
| `build_new_order(buf, cap, sender, target, cl_ord_id, symbol, side, ord_type, qty, price, tif, sending_time_ns)` | D | Full NewOrderSingle with optional price (Limit only) |
| `build_cancel_order(buf, cap, sender, target, cl_ord_id, orig_cl_ord_id, symbol, side)` | F | Cancel by OrigClOrdID |
| `build_replace_order(buf, cap, sender, target, cl_ord_id, orig_cl_ord_id, symbol, side, ord_type, qty, price, tif)` | G | Replace with new qty/price |

### Execution Report (`execution_report.hpp`)

**Enums**:

| Enum | Values |
|---|---|
| `ExecType` | `New`, `PartialFill`, `Fill`, `DoneForDay`, `Canceled`, `Replaced`, `PendingCancel`, `Stopped`, `Rejected`, `Suspended`, `PendingNew`, `Calculated`, `Expired`, `PendingReplace`, `Trade` |
| `OrdStatus` | `New`, `PartiallyFilled`, `Filled`, `DoneForDay`, `Canceled`, `Replaced`, `PendingCancel`, `Stopped`, `Rejected`, `Suspended`, `PendingNew`, `Calculated`, `Expired`, `PendingReplace` |

**`ExecutionReportView<MaxFields>`** -- wraps a `BasicMessageView`:

| Method | Return | Description |
|---|---|---|
| `cl_ord_id()` | `optional<string_view>` | Client order ID (tag 11) |
| `order_id()` | `optional<string_view>` | Exchange order ID (tag 37) |
| `exec_id()` | `optional<string_view>` | Execution ID (tag 17) |
| `exec_type()` | `optional<ExecType>` | Execution type (tag 150) |
| `ord_status()` | `optional<OrdStatus>` | Order status (tag 39) |
| `symbol()` | `optional<string_view>` | Symbol (tag 55) |
| `side()` | `optional<char>` | Side (tag 54) |
| `last_px()` | `optional<double>` | Last fill price (tag 31) |
| `last_qty()` | `optional<int64_t>` | Last fill quantity (tag 32) |
| `avg_px()` | `optional<double>` | Average fill price (tag 6) |
| `cum_qty()` | `optional<int64_t>` | Cumulative filled qty (tag 14) |
| `leaves_qty()` | `optional<int64_t>` | Remaining open qty (tag 151) |
| `text()` | `optional<string_view>` | Free-text / reject reason (tag 58) |
| `is_fill()` | `bool` | True if PartialFill, Fill, or Trade |
| `is_terminal()` | `bool` | True if Filled, Canceled, Rejected, Expired, or DoneForDay |

**`ParsedExecutionReport<MaxFields>`** -- owns `BasicMessageView` + `ExecutionReportView` with correct lifetime.

**`try_parse_execution_report<MaxFields>(data, len)`** -- parse + MsgType='8' check in one call -> `optional<ParsedExecutionReport>`.

### Session (`session.hpp`)

| Type | Description |
|---|---|
| `SessionState` | Enum: `kDisconnected`, `kLogonSent`, `kActive`, `kLogoutSent` |
| `session_state_name(SessionState)` | Human-readable state name |
| `FixSessionConfig` | Config struct: `sender_comp_id`, `target_comp_id`, `heartbeat_interval_sec` (default 30), `reset_seq_on_logon` (default true), `begin_string` (default `"FIX.4.4"`), `heartbeat_timeout_factor` (default 1.5), `resend_on_gap` (default false), `on_state_change` callback, `validate()` method |

**`FixSession`** -- constructed with `SendFn` callback + `FixSessionConfig`:

| Method | Return | Description |
|---|---|---|
| `logon(timeout)` | `expected<void, string>` | Send Logon, block until server responds or timeout |
| `logout(timeout)` | `expected<void, string>` | Send Logout, block until response or timeout |
| `reset()` | `void` | Reset state and sequence numbers for reconnection |
| `on_rx(data, len)` | `bool` | Process inbound message. Returns true if session-level (handled internally), false if application message. |
| `tick()` | `bool` | Periodic heartbeat check. Returns false if server is considered dead. |
| `send_app(builder)` | `bool` | Send application message (fills session headers automatically) |
| `state()` | `SessionState` | Current session state |
| `next_outbound_seq()` | `uint32_t` | Next outbound sequence number |
| `last_inbound_seq()` | `uint32_t` | Last received inbound sequence number |
| `expected_inbound_seq()` | `uint32_t` | Next expected inbound sequence number |

### Position Tracking (`position.hpp`)

**`Position`** struct: `qty` (signed), `avg_price` (VWAP), `realized_pnl`, `notional`, `trade_count`.

**`PositionTracker`**:

| Method | Return | Description |
|---|---|---|
| `on_fill(symbol, side, qty, price)` | `void` | Record a fill. Computes realized PnL on reducing fills (average-cost method). Handles position flips. |
| `get(symbol)` | `const Position&` | Get position for symbol (zero-initialized if unseen) |
| `has_position(symbol)` | `bool` | True if symbol has non-zero position |
| `positions()` | `const map&` | Full position map |
| `total_unrealized_pnl(market_prices)` | `double` | Unrealized PnL across all positions given current prices |
| `total_realized_pnl()` | `double` | Sum of realized PnL across all symbols |
| `net_exposure()` | `double` | Gross notional exposure: sum of abs(notional) |
| `clear()` | `void` | Reset all positions |

### Risk Checking (`risk_check.hpp`)

**`RiskLimits`** struct: `max_order_qty`, `max_order_notional`, `max_position_qty`, `max_position_notional`, `max_total_exposure`, `max_orders_per_second`. Zero disables a check.

**`RiskRejectReason`** enum: `kOk`, `kOrderQtyExceeded`, `kOrderNotionalExceeded`, `kPositionQtyExceeded`, `kPositionNotionalExceeded`, `kTotalExposureExceeded`, `kRateLimitExceeded`, `kInvalidInput`.

**`risk_reject_name(RiskRejectReason)`** -- human-readable name.

**`RiskChecker`**:

| Method | Return | Description |
|---|---|---|
| `RiskChecker(limits)` | | Construct with risk thresholds |
| `check_order(symbol, side, qty, price, positions)` | `RiskRejectReason` | Validate order against all limits. Returns first violated limit or `kOk`. |
| `set_limits(limits)` | `void` | Hot-update limits at runtime |
| `limits()` | `const RiskLimits&` | Current limits |

### Order Management (`order_manager.hpp`)

**`OrderState`** enum: `PendingNew`, `New`, `PartiallyFilled`, `Filled` (terminal), `PendingCancel`, `Canceled` (terminal), `Rejected` (terminal).

**`is_terminal(OrderState)`** -- returns true for Filled, Canceled, Rejected.

**`ManagedOrder`** struct: `cl_ord_id`, `symbol`, `side`, `orig_qty`, `price`, `filled_qty`, `avg_fill_price`, `leaves_qty`, `state`, `submit_time_ns`, `last_update_ns`.

**`OrderManager`**:

| Method | Return | Description |
|---|---|---|
| `submit(cl_ord_id, symbol, side, qty, price)` | `bool` | Register new order (call after send) |
| `on_execution_report(report, positions*)` | `bool` | Process ExecutionReport. Updates state, optionally forwards fills to PositionTracker. |
| `on_cancel_reject(cl_ord_id, reason)` | `bool` | Handle OrderCancelReject (9). Reverts PendingCancel to prior active state. |
| `get(cl_ord_id)` | `const ManagedOrder*` | Lookup order (nullptr if not found) |
| `mark_pending_cancel(cl_ord_id)` | `bool` | Mark order as PendingCancel |
| `active_count()` | `size_t` | Count of non-terminal orders |
| `orders()` | `const map&` | Full order map |
| `purge_terminal()` | `void` | Remove all terminal orders from memory |

## Dependencies

- **eph-core** -- `framer_concept.hpp` (MessageFramer concept, DecodedFrame, FrameError), `parse_number.hpp` (integer/double parsing)
- **spdlog** -- Logging throughout all components (compile-time filterable via `SPDLOG_ACTIVE_LEVEL`)

## Usage Examples

### Parse a FIX message (zero-copy)

```cpp
#include <eph/fix/parser.hpp>

auto result = eph::fix::parse(data, data_len);
if (result) {
    auto msg_type = result->msg_type();     // optional<string_view>, e.g. "D"
    auto symbol   = result->get(eph::fix::tag::Symbol);   // optional<string_view>
    auto price    = result->get_double(eph::fix::tag::Price);  // optional<double>
    auto seq_num  = result->get_int(eph::fix::tag::MsgSeqNum); // optional<int64_t>

    // Iterate all fields
    result->for_each([](uint32_t tag, std::string_view value) {
        // ...
    });
} else {
    // result.error() is ParseError -- kIncomplete means wait for more data
}
```

### Build a NewOrderSingle

```cpp
#include <eph/fix/orders.hpp>

uint8_t buf[512];
size_t len = eph::fix::build_new_order(
    buf, sizeof(buf),
    "MY_ALGO", "EXCHANGE",
    "ORD001", "AAPL",
    eph::fix::Side::Buy,
    eph::fix::OrdType::Limit,
    /*qty=*/100.0, /*price=*/150.50,
    eph::fix::TimeInForce::IOC);
// len bytes in buf[] are a complete, checksummed FIX message
```

### Build a custom FIX message

```cpp
#include <eph/fix/builder.hpp>

uint8_t buf[1024];
eph::fix::MessageBuilder b(buf, sizeof(buf));
b.set_char(eph::fix::tag::MsgType, 'D');
b.set(eph::fix::tag::SenderCompID, "SENDER");
b.set(eph::fix::tag::TargetCompID, "TARGET");
b.set(eph::fix::tag::Symbol, "MSFT");
b.set_char(eph::fix::tag::Side, '1');
b.set_double(eph::fix::tag::OrderQty, 500.0);
b.set_price(eph::fix::tag::Price, 31250, 2);  // "312.50" without float rounding
b.set_timestamp(eph::fix::tag::SendingTime, epoch_ns,
    eph::fix::MessageBuilder::TimestampPrecision::kMicroseconds);
size_t len = b.finish();  // 0 on overflow
```

### Process ExecutionReports

```cpp
#include <eph/fix/execution_report.hpp>

if (auto er = eph::fix::try_parse_execution_report(data, len)) {
    if (er->view.is_fill()) {
        auto px  = er->view.last_px();    // optional<double>
        auto qty = er->view.last_qty();   // optional<int64_t>
        auto sym = er->view.symbol();     // optional<string_view>
    }
    if (er->view.is_terminal()) {
        // Order reached final state -- clean up
    }
}
```

### Session management

```cpp
#include <eph/fix/session.hpp>

eph::fix::FixSession session(
    [&](const uint8_t* d, size_t l) { return transport->send(d, l); },
    {.sender_comp_id = "MY_ALGO",
     .target_comp_id = "EXCHANGE",
     .heartbeat_interval_sec = 30,
     .reset_seq_on_logon = true});

// Logon (blocks until server responds or timeout)
auto result = session.logon(std::chrono::milliseconds{5000});

// In RX callback: dispatch session vs application messages
auto on_message = [&](const uint8_t* d, size_t l, uint8_t) {
    if (!session.on_rx(d, l)) {
        // Application message -- process it
    }
};

// Periodic tick (call from event loop)
if (!session.tick()) {
    // Server is dead -- reconnect
}

// Send application message with auto-filled session headers
eph::fix::MessageBuilder b(buf, sizeof(buf));
b.set_char(eph::fix::tag::MsgType, eph::fix::tag::msg_type::NewOrderSingle);
b.set(eph::fix::tag::Symbol, "AAPL");
// ... other fields ...
session.send_app(b);

// Logout
session.logout();
```

### Full order lifecycle with risk checks

```cpp
#include <eph/fix/order_manager.hpp>
#include <eph/fix/risk_check.hpp>
#include <eph/fix/position.hpp>

eph::fix::PositionTracker positions;
eph::fix::RiskChecker risk({
    .max_order_qty = 10000,
    .max_order_notional = 1'000'000.0,
    .max_position_qty = 50000,
    .max_total_exposure = 5'000'000.0,
});
eph::fix::OrderManager orders;

// Pre-trade risk check
auto reject = risk.check_order("AAPL", '1', 100, 150.50, positions);
if (reject != eph::fix::RiskRejectReason::kOk) {
    // Order rejected: eph::fix::risk_reject_name(reject)
    return;
}

// Send order and register
// ... send via session ...
orders.submit("ORD001", "AAPL", '1', 100.0, 150.50);

// Process execution report (auto-updates positions on fills)
if (auto er = eph::fix::try_parse_execution_report(data, len)) {
    orders.on_execution_report(er->view, &positions);
}

// Query state
auto* ord = orders.get("ORD001");          // ManagedOrder*
double pnl = positions.total_realized_pnl();
double exposure = positions.net_exposure();
size_t active = orders.active_count();

// Housekeeping
orders.purge_terminal();

// Hot-update risk limits
risk.set_limits({.max_order_qty = 5000, .max_total_exposure = 2'000'000.0});
```

### Repeating groups (market data)

```cpp
#include <eph/fix/parser.hpp>

auto result = eph::fix::parse<256>(data, len);
if (result) {
    eph::fix::BasicMessageView<256>::GroupEntry entries[64];
    auto group = result->get_group(
        eph::fix::tag::NoMDEntries,    // count tag
        eph::fix::tag::MDEntryType,    // delimiter tag
        entries, 64);

    for (auto& entry : group) {
        auto type = entry.get_char(eph::fix::tag::MDEntryType);  // '0'=Bid, '1'=Offer
        auto px   = entry.get_double(eph::fix::tag::MDEntryPx);
        auto sz   = entry.get_int(eph::fix::tag::MDEntrySize);
    }
}
```
