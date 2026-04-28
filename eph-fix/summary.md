# Project: eph-fix

> Header-only C++23 FIX 4.4 protocol library for HFT systems — zero-copy parser, zero-allocation builder, FIX session layer, execution-report view, pre-trade risk, position tracking, and order lifecycle management.

**Language**: C++23 | **Build**: xmake (header-only target) | **Dependencies**: `eph-core`, `spdlog`

---

## Table of Contents
1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Module Map](#module-map)
4. [Data Flow](#data-flow)
5. [Key Components](#key-components)
6. [Entry Points & APIs](#entry-points--apis)
7. [Dependencies](#dependencies)
8. [Testing](#testing)

---

## Overview

`eph-fix` is the FIX-protocol subproject of the `ephemeral_dev` monorepo. It
implements enough of FIX 4.4 to build, parse, frame, and session-manage order
entry and market-data messages for an HFT trading gateway, plus the in-process
plumbing (order manager, position tracker, risk checker) that a trading
application typically layers on top of the wire protocol.

Everything is header-only. The public target (`eph-fix` in `xmake.lua`) has
`set_kind("headeronly")` and exposes `include/eph/fix/*.hpp`. All hot paths
avoid heap allocation: the parser writes into a stack-allocated
`BasicMessageView`, the builder writes into a caller-owned buffer, and the
order/position maps reuse their nodes for the lifetime of a session.

The library is designed to plug into the sibling subprojects:

- `eph-core` provides `eph::net::MessageFramer` (the concept `FixFramer`
  implements) and `eph::core::parse_int` / `parse_number` (the numeric
  primitives behind the typed accessors).
- `eph-net-kernel` / `eph-net-dpdk` own the socket / DPDK transport that
  feeds raw bytes into `FixFramer::decode()` and receives
  `MessageBuilder::as_span()` for send.
- `eph-net` contributes the `Stream` / `Datagram` / `Poller` concepts
  the kernel and DPDK backends both satisfy.

Consumers interact with `eph-fix` via the aggregation header
`include/eph/fix.hpp`, which includes every module in the correct dependency
order.

---

## Architecture

The design is a layered protocol stack with a small set of orthogonal
application-layer facilities bolted to the top. Layers depend only on what
sits directly below them.

### Component Diagram

```
 +--------------------------------------------------------------+
 |                    Application code                         |
 +-------+----------------+------------------+------------------+
         |                |                  |
 +-------v------+  +------v-------+   +------v-------+
 | OrderManager |  | RiskChecker  |   |PositionTracker|
 | (state m/c)  |  |  + Limits    |<--| VWAP + PnL   |
 +-------+------+  +------+-------+   +--------------+
         |                |
 +-------v---------+  +---v----------+
 |ExecutionReport  |  |orders.hpp    |
 |      View       |  |build_new_*() |
 +-------+---------+  +------+-------+
         |                   |
         +---------+---------+
                   |
         +---------v---------+
         |    FixSession     | Logon/Logout/Heartbeat/SeqNum
         +---------+---------+
                   |
     +-------------+-------------+
     |             |             |
 +---v---+   +-----v-----+  +----v----+
 |parser |   |  builder  |  | framer  |
 +---+---+   +-----+-----+  +----+----+
     |             |             |
     +-------------+-------------+
                   |
             tags.hpp (constants, names, dispatch)
```

---

## Module Map

| Module / File | Responsibility | Key Types | Depends On |
|---|---|---|---|
| `include/eph/fix/tags.hpp` | FIX tag numbers, MsgType constants, human-readable name lookup | `eph::fix::tag::*` constants, `tag_name()`, `msg_type_name()` | `<cstdint>`, `<string_view>` |
| `include/eph/fix/parser.hpp` | Zero-copy `tag=value\x01` parser and message view | `BasicMessageView<MaxFields>`, `Field`, `ParseError`, `ParserStats`, `RepeatingGroupView`, `GroupEntry`, `dispatch()`, `msg::*` tag structs | `tags.hpp`, `eph::core::parse_int/parse_number`, `spdlog` |
| `include/eph/fix/builder.hpp` | Zero-allocation message builder with checksum and body length | `MessageBuilder`, `TimestampPrecision` | `tags.hpp`, `spdlog` |
| `include/eph/fix/framer.hpp` | Message framer satisfying `eph::net::MessageFramer` | `BasicFixFramer<MaxBodyLength>`, alias `FixFramer` | `parser.hpp`, `eph::core::framer_concept` |
| `include/eph/fix/orders.hpp` | Typed FIX order builders | `Side`, `OrdType`, `TimeInForce`, `build_new_order()`, `build_cancel_order()`, `build_replace_order()` | `builder.hpp`, `tags.hpp` |
| `include/eph/fix/execution_report.hpp` | Typed view over ExecutionReport (8) | `ExecType`, `OrdStatus`, `ExecutionReportView<MaxFields>`, `ParsedExecutionReport<MaxFields>`, `try_parse_execution_report()` | `parser.hpp`, `tags.hpp` |
| `include/eph/fix/session.hpp` | FIX 4.4 session layer (Logon/Logout/Heartbeat/SeqNum/SequenceReset) | `FixSession`, `FixSessionConfig`, `SessionState` | `builder.hpp`, `parser.hpp`, `tags.hpp`, `<atomic>` |
| `include/eph/fix/position.hpp` | Per-symbol VWAP + realized PnL tracker | `Position`, `PositionTracker` | `<unordered_map>`, `spdlog` |
| `include/eph/fix/risk_check.hpp` | Pre-trade risk checker with configurable limits | `RiskLimits`, `RiskRejectReason`, `RiskChecker` | `position.hpp`, `spdlog` |
| `include/eph/fix/order_manager.hpp` | Order lifecycle state machine | `OrderState`, `ManagedOrder`, `OrderManager` | `execution_report.hpp`, `position.hpp` |
| `include/eph/fix.hpp` | Aggregation header | — | every `include/eph/fix/*.hpp` |

---

## Data Flow

### Receive path (wire -> application)

1. The transport delivers raw bytes to the framer. `BasicFixFramer::decode()`
   looks for `"8="`, parses the `9=NNN` body length, verifies the trailing
   `10=XXX\x01` checksum field, and returns a `DecodedFrame` pointing at the
   complete message (with `msg_type` pre-extracted from tag 35).
2. `FixSession::on_rx()` calls `parse()` over those bytes and handles any
   session-level message (`0`, `1`, `2`, `4`, `5`, `A`) internally. It
   updates `expected_inbound_seq_` and fires `ResendRequest` on a detected
   gap. It returns `false` only for application messages.
3. The application hands the view to a typed helper
   (e.g. `try_parse_execution_report()`), which validates `MsgType=8` and
   constructs an `ExecutionReportView`.
4. `OrderManager::on_execution_report()` locates the tracked order by
   `ClOrdID`, applies the FIX `ExecType` -> `OrderState` transition, updates
   `filled_qty`, `avg_fill_price`, `leaves_qty`, and — if a
   `PositionTracker*` was supplied — forwards the fill to `on_fill()` for
   VWAP and realized-PnL bookkeeping.

### Send path (application -> wire)

1. The application constructs a `MessageBuilder` over a caller-owned buffer
   and fills application fields (MsgType plus e.g. Symbol, Side, Price).
2. `FixSession::send_app(builder)` injects the session header
   (`SenderCompID`, `TargetCompID`, `MsgSeqNum`, `SendingTime`), calls
   `finish()` to prepend the `8=/9=` header and append the `10=` checksum,
   and hands the bytes to the caller-supplied `SendFn`.
3. `FixFramer::encode()` is a pass-through: FIX messages are already
   self-framed by design.

### Flow Diagram

```
       RX                                           TX
  ----------                                   ----------
  wire bytes                                   application
      |                                              |
      v                                              v
  FixFramer                                    MessageBuilder
  ::decode                                     (set_*, finish)
      |                                              |
      v                                              |
  parse()  -------->  BasicMessageView               |
      |                    |                         |
      v                    v                         v
  FixSession          try_parse_*             FixSession
  ::on_rx             ExecutionReport         ::send_app
      |                    |                         |
      |                    v                         |
      |               OrderManager                   |
      |               ::on_execution_report          |
      |                    |                         |
      |                    v                         |
      |               PositionTracker                |
      |               ::on_fill                      |
      v                                              v
  (session msg)                               wire bytes
  handled internally
```

---

## Key Components

### `BasicMessageView<MaxFields>`
**File**: `include/eph/fix/parser.hpp`
**Purpose**: Zero-copy stack-allocated view of a parsed FIX message.
**Interface**:
```cpp
template <size_t MaxFields = 128>
class BasicMessageView {
    std::optional<std::string_view> get(uint32_t tag) const;
    std::optional<char>     get_char(uint32_t tag) const;
    std::optional<int64_t>  get_int(uint32_t tag) const;
    std::optional<double>   get_double(uint32_t tag) const;
    std::optional<bool>     get_bool(uint32_t tag) const;
    std::optional<uint64_t> get_timestamp(uint32_t tag) const;
    RepeatingGroupView get_group(uint32_t count_tag, uint32_t delim_tag,
                                 GroupEntry* out, size_t max) const;
    std::string dump() const;
    std::string to_json() const;
};
```
**Notes**: `fields_` is a C-style array of `MaxFields` entries — no dynamic
allocation. The view does **not** own its string data; the original wire
buffer must outlive the view. Default `MaxFields = 128` is sufficient for
order-entry traffic; increase for market-data snapshots.

### `MessageBuilder`
**File**: `include/eph/fix/builder.hpp`
**Purpose**: Assemble a FIX message into a caller-owned byte buffer with no
heap allocation.
**Interface**:
```cpp
MessageBuilder(uint8_t* buf, size_t capacity);
MessageBuilder& set(uint32_t tag, std::string_view value);
MessageBuilder& set_int(uint32_t tag, int64_t value);
MessageBuilder& set_double(uint32_t tag, double value, int precision = 2);
MessageBuilder& set_char(uint32_t tag, char value);
MessageBuilder& set_bool(uint32_t tag, bool value);
MessageBuilder& set_timestamp(uint32_t tag, uint64_t epoch_ns,
                              TimestampPrecision = kMilliseconds);
MessageBuilder& set_decimal(uint32_t tag, std::string_view decimal);
MessageBuilder& set_price(uint32_t tag, int64_t mantissa, uint8_t decimals);
size_t finish(std::string_view begin_string = "FIX.4.4");
```
**Notes**: Reserves `kHeaderReserve = 32` bytes at the front of the buffer
for `8=...\x019=NNN\x01`, then builds the body at `buf + 32`; `finish()`
back-patches the header and appends the `10=XXX\x01` checksum. `set()`
scans values for embedded `\x01` and fails the build on contamination. A
`set_*_unique` family uses `has_tag()` to reject duplicate tags.

### `BasicFixFramer<MaxBodyLength>`
**File**: `include/eph/fix/framer.hpp`
**Purpose**: Satisfy `eph::net::MessageFramer` for FIX bytes on a stream
transport.
**Interface**:
```cpp
size_t encode(uint8_t* out, const uint8_t* data, size_t len, uint8_t msg_type);
std::expected<DecodedFrame, FrameError> decode(const uint8_t* data, size_t len);
```
**Notes**: `encode()` is a pass-through — FIX messages already carry their
own length and checksum. `decode()` validates `BeginString`, parses
`BodyLength` with overflow protection, checks the `10=XXX\x01` trailer,
verifies the checksum, and extracts `MsgType` (tag 35) into
`DecodedFrame.msg_type` for the transport's dispatch logic. Default alias
`FixFramer = BasicFixFramer<>` uses a 1 MB body cap.

### `FixSession`
**File**: `include/eph/fix/session.hpp`
**Purpose**: FIX 4.4 session layer with Logon/Logout, automatic Heartbeat,
sequence tracking, and gap handling.
**Interface**:
```cpp
FixSession(SendFn, FixSessionConfig);
std::expected<void, std::string> logon (std::chrono::milliseconds = 5s);
std::expected<void, std::string> logout(std::chrono::milliseconds = 3s);
bool on_rx(const uint8_t* data, size_t len); // true = handled internally
bool tick();                                  // timer callback
bool send_app(MessageBuilder& builder);
SessionState state() const;
uint32_t     next_outbound_seq() const;
uint32_t     last_inbound_seq()  const;
uint32_t     expected_inbound_seq() const;
```
**Notes**: Thread-safe — `state_`, sequence counters, last-sent/received
timestamps, `heartbeat_interval_sec_` and `test_request_pending_` are all
`std::atomic`. The RX and TX threads coordinate through acquire/release on
`state_`. `logon()`/`logout()` spin-wait with a CPU pause hint on x86/ARM64.
The session does **not** persist sequence numbers and does not maintain an
outbound journal — `ResendRequest` from the counterparty is answered with
`SequenceReset-GapFill`.

### `ExecutionReportView<MaxFields>`
**File**: `include/eph/fix/execution_report.hpp`
**Purpose**: Typed read-only view over a parsed ExecutionReport (MsgType=8).
**Interface**:
```cpp
std::optional<std::string_view> cl_ord_id()  const;
std::optional<std::string_view> order_id()   const;
std::optional<std::string_view> exec_id()    const;
std::optional<ExecType>   exec_type()  const;
std::optional<OrdStatus>  ord_status() const;
std::optional<std::string_view> symbol() const;
std::optional<char>       side()       const;
std::optional<double>     last_px()    const;
std::optional<int64_t>    last_qty()   const;
std::optional<double>     avg_px()     const;
std::optional<int64_t>    cum_qty()    const;
std::optional<int64_t>    leaves_qty() const;
bool is_fill()     const; // PartialFill, Fill, Trade
bool is_terminal() const; // Filled, Canceled, Rejected, Expired, DoneForDay
```
**Notes**: Holds a `const&` to the underlying `BasicMessageView`, so callers
either keep both alive together or use `ParsedExecutionReport<MaxFields>`
(which owns the view and constructs the typed layer over it).
`try_parse_execution_report(data, len)` gives you a one-call wire -> view
pipeline.

### `PositionTracker`
**File**: `include/eph/fix/position.hpp`
**Purpose**: Per-symbol signed quantity, VWAP entry price, realized PnL
(average-cost), and notional exposure.
**Interface**:
```cpp
void on_fill(std::string_view symbol, char side, double qty, double price);
const Position& get(std::string_view symbol) const;
bool has_position(std::string_view symbol) const;
double total_unrealized_pnl(const std::unordered_map<std::string, double>& mkt) const;
double total_realized_pnl() const;
double net_exposure() const;
void   clear();
```
**Notes**: Uses heterogeneous lookup with a transparent `StringHash` so
queries and fills on `string_view` avoid allocating a `std::string`.
Flipping positions (e.g. selling 8 against a long of 5) is handled
correctly: the closing portion contributes to realized PnL and the
remainder opens a fresh position at the fill price. Non-finite inputs are
rejected and logged at WARN. Single-threaded by design.

### `RiskChecker`
**File**: `include/eph/fix/risk_check.hpp`
**Purpose**: Stateless pre-trade threshold checks against `RiskLimits`.
**Interface**:
```cpp
RiskChecker(RiskLimits);
RiskRejectReason check_order(std::string_view symbol, char side,
                             double qty, double price,
                             const PositionTracker& positions) const;
void set_limits(RiskLimits);
```
**Notes**: A zero threshold disables that individual check, which lets
callers turn checks on and off at runtime without reconstructing the
checker. Checks are evaluated in a fixed order and return the first
violation as a typed `RiskRejectReason`. Rate limiting is intentionally
excluded because it requires clock injection for testability — the field
lives on `RiskLimits` so a stateful rate limiter can read it externally.

### `OrderManager`
**File**: `include/eph/fix/order_manager.hpp`
**Purpose**: Track orders from submission through terminal states using a
formal state machine driven by ExecutionReport events.
**Interface**:
```cpp
bool submit(std::string cl_ord_id, std::string symbol,
            char side, double qty, double price);
template <size_t MaxFields>
bool on_execution_report(const ExecutionReportView<MaxFields>&,
                         PositionTracker* positions = nullptr);
bool on_cancel_reject(std::string_view cl_ord_id, int reason = -1);
bool mark_pending_cancel(std::string_view cl_ord_id);
const ManagedOrder* get(std::string_view cl_ord_id) const;
size_t active_count() const;
void   purge_terminal();
```
**Notes**: Computes running VWAP on partial fills with an incremental
formula and guards against `avg_fill_price` going non-finite. Cancel
rejects revert `PendingCancel` to the prior active state (`New` or
`PartiallyFilled`) so orders do not get stuck. Fill quantities > 2^53 are
rejected because `int64_t -> double` would lose precision.

---

## Entry Points & APIs

| Entrypoint | Type | Description |
|---|---|---|
| `eph::fix::parse(data, len)` | free function | Wire bytes -> `BasicMessageView` |
| `eph::fix::parse_all(data, len, cb)` | free function | Streaming multi-message parse |
| `eph::fix::parse_all(data, len, cb, stats)` | free function | Same plus `ParserStats` accumulator |
| `eph::fix::MessageBuilder` | class | Wire bytes from structured fields |
| `eph::fix::FixFramer` | class (alias) | `MessageFramer`-compliant framer |
| `eph::fix::dispatch(view, handler)` | free template | Compile-time MsgType-tagged dispatch |
| `eph::fix::build_new_order(...)` | free function | NewOrderSingle (D) |
| `eph::fix::build_cancel_order(...)` | free function | OrderCancelRequest (F) |
| `eph::fix::build_replace_order(...)` | free function | OrderCancelReplaceRequest (G) |
| `eph::fix::try_parse_execution_report(data, len)` | free template | Wire -> `ParsedExecutionReport` |
| `eph::fix::FixSession` | class | Session lifecycle, heartbeat, seq |
| `eph::fix::PositionTracker` | class | Per-symbol positions and PnL |
| `eph::fix::RiskChecker` | class | Pre-trade risk validation |
| `eph::fix::OrderManager` | class | Order lifecycle state machine |

The aggregation header `eph/fix.hpp` pulls every one of these into scope.

---

## Dependencies

### Internal (module graph)

```
               +----------+
               | tags.hpp |
               +----+-----+
                    |
       +------------+------------+
       |            |            |
  +----v----+  +----v-----+  +---v-----+
  | parser  |  | builder  |  | (direct |
  |         |  |          |  |  users) |
  +--+--+---+  +----+-----+  +---------+
     |  |           |
     |  |    +------+------+
     |  |    |      |      |
     |  |    | +----v----+ |
     |  |    | | orders  | |
     |  |    | +---------+ |
     |  |    |             |
     |  |    v             v
     |  |  framer       session <-- builder, parser, tags
     |  |
     |  +->exec_report --> parser, tags
     |             |
     |             +-> order_manager --> position
     |                           |
     |                           +---> risk_check --> position
     |
     +--> ParserStats, RepeatingGroupView (internal)
```

Every module also pulls in `spdlog` for its named logger.

### External

| Package | Purpose |
|---|---|
| `spdlog` | Per-module named loggers (`fix.parser`, `fix.builder`, `fix.framer`, `fix.session`, `fix.orders`, `fix.execrpt`, `fix.position`, `fix.risk_check`, `fix.ordmgr`) |
| `eph-core` | `eph::net::MessageFramer` concept; `eph::core::parse_int` / `parse_number` |
| `gtest` | Test runner (tests only) |

No runtime dependency on threading primitives beyond `std::atomic`,
`std::thread::yield`, and platform spin-wait intrinsics (`_mm_pause` on x86,
`yield` on ARM64).

---

## Testing

| Test Suite | Location | Coverage Focus |
|---|---|---|
| `test_fix` | `tests/test_fix.cpp` | Parser, builder, framer, tags, dispatch, repeating groups, stats — the biggest suite (~330 cases) |
| `test_fix_session` | `tests/test_fix_session.cpp` | Session lifecycle, heartbeat, sequence gap handling, SequenceReset/GapFill, concurrent RX/TX |
| `test_fix_orders` | `tests/test_fix_orders.cpp` | `build_new_order` / `build_cancel_order` / `build_replace_order` happy + edge cases |
| `test_execution_report` | `tests/test_execution_report.cpp` | `ExecutionReportView` accessors, `is_fill()`, `is_terminal()`, `try_parse_execution_report()` |
| `test_order_manager` | `tests/test_order_manager.cpp` | State transitions, fill forwarding to `PositionTracker`, cancel-reject recovery |
| `test_position` | `tests/test_position.cpp` | VWAP calculation, realized PnL on reducing fills, position flips, invalid-input rejection |
| `test_risk_check` | `tests/test_risk_check.cpp` | Each `RiskRejectReason`, threshold boundaries, disabled-check semantics |
| `bench_fix_parse` | `benchmarks/bench_fix_parse.cpp` | Parser throughput benchmark |

Key test scenarios:

- **Overflow and malformed input**: body length overflow, missing SOH,
  truncated checksum, non-digit body length, embedded `\x01` in a field
  value, `INT64_MIN` integer formatting, rounding carry in `format_double`.
- **Repeating groups**: `get_group()` truncation when the caller buffer is
  too small, empty groups (count = 0), trailing fields after the last
  delimiter.
- **Session correctness**: Logon with server-provided HeartBtInt override,
  sequence gap triggering `ResendRequest` only when `resend_on_gap` is set,
  `SequenceReset` in reset vs gap-fill modes, graceful handling of
  `PossDupFlag=Y` without bumping the expected sequence.
- **Order lifecycle**: partial fill -> partial fill -> fill transitions,
  cancel reject reverting to the correct prior state, rejected orders
  treated as terminal, fill quantities exceeding `2^53` rejected.
- **Risk boundaries**: each threshold enforced in isolation, zero =
  disabled semantics, projected exposure that accounts for reducing fills
  rather than always adding.

Every test binary is discovered automatically by the glob in `xmake.lua`:

```lua
for _, file in ipairs(os.files("tests/**.cpp")) do
    target(path.basename(file))
        add_rules("eph-test")
        add_files(file)
        add_deps("eph-fix")
end
```

Adding a new test is a one-file change.
