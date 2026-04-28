# eph-fix

Header-only C++23 library implementing the FIX 4.4 protocol for HFT systems: zero-copy parsing, zero-allocation message building, session management, execution report handling, pre-trade risk checks, and full order-lifecycle tracking.

Part of the **eph** monorepo. Builds as an `xmake` header-only target with a single public dependency: `eph-core` (for the shared `MessageFramer` concept and number-parsing primitives).

## Features

- **Zero-copy parser** — FIX `tag=value\x01` messages are parsed into a stack-allocated `BasicMessageView<MaxFields>` whose `std::string_view` values point directly into the caller's buffer. No heap allocation on the hot path.
- **Zero-allocation builder** — `MessageBuilder` writes fields directly into a caller-owned buffer, prepending `8=/9=` header and appending `10=` checksum on `finish()`.
- **Framer** — `BasicFixFramer` satisfies `eph::net::MessageFramer`, so FIX can be dropped into `eph-transport` with one using-alias.
- **Typed FIX 4.4 session** — Logon/Logout handshake, automatic Heartbeat, TestRequest probing, bidirectional `MsgSeqNum` gap detection, `SequenceReset`/`GapFill`, and optional `ResendRequest`. Thread-safe state via `std::atomic`.
- **Typed order entry** — `build_new_order`, `build_cancel_order`, `build_replace_order` with `Side`, `OrdType`, `TimeInForce` enums.
- **Execution report view** — `ExecutionReportView` with `cl_ord_id()`, `exec_type()`, `ord_status()`, `last_px()`, `is_fill()`, `is_terminal()`, plus a one-call `try_parse_execution_report()` convenience.
- **Risk and positions** — `PositionTracker` maintains per-symbol VWAP entry price, realized PnL (average-cost), and notional exposure. `RiskChecker` enforces configurable `RiskLimits`. `OrderManager` tracks orders through the full state machine and forwards fills to an attached `PositionTracker`.
- **Error handling** — `std::expected<T, ParseError>` (parser), `std::expected<void, std::string>` (session lifecycle), `std::optional<T>` (typed accessors). No exceptions in hot paths.
- **Observability** — every module owns a named `spdlog` logger (`fix.parser`, `fix.framer`, `fix.builder`, `fix.session`, `fix.orders`, `fix.execrpt`, `fix.position`, `fix.risk_check`, `fix.ordmgr`). Compile-time level via `SPDLOG_ACTIVE_LEVEL`.

## Quick Start

### Prerequisites

- A C++23 compiler (GCC 13+, Clang 17+ recommended)
- [xmake](https://xmake.io) 2.8+
- `spdlog` (fetched automatically by the parent `xmake.lua`)
- `eph-core` subproject (already present in this monorepo)

### Build

From the monorepo root:

```bash
xmake build eph-fix         # header-only target, nothing to link
xmake build test_fix        # main parser/builder tests
xmake build bench_fix_parse # parse throughput benchmark
```

Or build everything for the subproject:

```bash
xmake build -g eph-fix      # if grouped; otherwise iterate the test targets
```

### Run tests

```bash
xmake run test_fix
xmake run test_fix_session
xmake run test_fix_orders
xmake run test_execution_report
xmake run test_order_manager
xmake run test_position
xmake run test_risk_check
```

### Run the parse benchmark

```bash
xmake run bench_fix_parse
```

## Project Structure

```
eph-fix/
├── include/eph/
│   ├── fix.hpp                  # aggregation header
│   └── fix/
│       ├── tags.hpp             # tag / MsgType constants and name lookup
│       ├── parser.hpp           # BasicMessageView + parse() + dispatch()
│       ├── builder.hpp          # MessageBuilder
│       ├── framer.hpp           # BasicFixFramer (MessageFramer concept)
│       ├── orders.hpp           # build_new_order / cancel / replace
│       ├── execution_report.hpp # ExecutionReportView
│       ├── session.hpp          # FixSession (Logon/Logout/Heartbeat/SeqNum)
│       ├── position.hpp         # PositionTracker (VWAP, realized PnL)
│       ├── risk_check.hpp       # RiskChecker + RiskLimits
│       └── order_manager.hpp    # OrderManager (order state machine)
├── tests/                       # GoogleTest unit tests (~480 test cases)
├── benchmarks/
│   └── bench_fix_parse.cpp      # parser throughput benchmark
└── xmake.lua                    # header-only target + per-file test/bench rules
```

## Usage Examples

### Parse a FIX message

```cpp
#include "eph/fix.hpp"

const uint8_t* buf = /* wire bytes */;
auto result = eph::fix::parse(buf, len);
if (!result) {
    // result.error() is ParseError::kIncomplete / kInvalidFormat / kChecksumMismatch / kFieldOverflow
    return;
}
const auto& msg = *result;

if (auto sym = msg.get(eph::fix::tag::Symbol)) {
    // *sym is a string_view into buf — do NOT outlive buf
}
if (auto side = msg.get_char(eph::fix::tag::Side)) {
    // '1' = Buy, '2' = Sell
}
if (auto qty = msg.get_int(eph::fix::tag::OrderQty)) {
    // int64_t, overflow returns nullopt
}
```

### Build a NewOrderSingle

```cpp
#include "eph/fix.hpp"

uint8_t buf[1024];
size_t n = eph::fix::build_new_order(
    buf, sizeof(buf),
    "MY_ALGO",               // SenderCompID
    "EXCHANGE",              // TargetCompID
    "ORD-000001",            // ClOrdID
    "AAPL",                  // Symbol
    eph::fix::Side::Buy,
    eph::fix::OrdType::Limit,
    100.0,                   // qty
    150.25,                  // price
    eph::fix::TimeInForce::Day);
// n == 0 on overflow
```

### Run a session

```cpp
namespace en = eph::net::kernel;
namespace ec = eph::codec;

auto poller = en::KernelPoller::create().value();
auto stream = en::KernelTcpStream<ec::RawStreamCodec>::create(cfg).value();
poller->add(stream.get()).value();

eph::fix::FixSession session(
    [&stream](const uint8_t* d, std::size_t l) {
        // FixSession's send-fn is a `bool(const uint8_t*, size_t)`; lift
        // the stream's `expected<size_t>` into that shape.
        auto r = stream->send({d, l});
        return r.has_value();
    },
    {.sender_comp_id = "MY_ALGO", .target_comp_id = "EXCHANGE"});

stream->on_message = [&](std::span<const uint8_t> bytes) {
    if (!session.on_rx(bytes.data(), bytes.size())) {
        // application-level message — parse and handle
    }
};

if (auto r = session.logon(std::chrono::milliseconds{5000}); !r) {
    // r.error() describes the failure
}
// drive the poll loop; FixSession's heartbeats run from on_rx callbacks
while (running) poller->poll();
session.logout();
```

### Process an execution report into positions

```cpp
eph::fix::PositionTracker  positions;
eph::fix::OrderManager     orders;

orders.submit("ORD-000001", "AAPL", '1', 100.0, 150.25);

auto parsed = eph::fix::try_parse_execution_report(wire, len);
if (parsed) {
    orders.on_execution_report(parsed->view, &positions);
    const auto& pos = positions.get("AAPL");
    // pos.qty, pos.avg_price, pos.realized_pnl, pos.notional
}
```

## Design Notes

- **Header-only.** All templates are defined in headers. `xmake.lua` exposes the target as `set_kind("headeronly")`.
- **Single-threaded by default.** `PositionTracker`, `OrderManager`, and `RiskChecker` assume external serialization. `FixSession` is explicitly thread-safe via atomics because RX and TX are driven from different threads.
- **No outbound journal.** `FixSession` responds to `ResendRequest` with `SequenceReset-GapFill` instead of replaying messages. Sequence numbers reset on `logon()` when `reset_seq_on_logon=true` (the default).
- **No FIX encryption.** `EncryptMethod` is always sent as `0`.
- **No `PossDupFlag` deduplication.** Messages with `PossDupFlag=Y` are logged but still delivered to the application callback — callers implement idempotency (e.g. dedup by `ClOrdID`/`ExecID`).
- **Bounded parsing.** Maximum `BodyLength` is a template parameter (default 1 MB) on both `parse()` and `BasicFixFramer`. Field capacity (`MaxFields`) is also compile-time.

## License

No `LICENSE` file is present in the repository; licensing terms TBD.
