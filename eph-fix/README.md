# eph-fix

Header-only C++23 library for FIX protocol parsing, message building, session management, and order lifecycle tracking, optimized for HFT systems. Zero-copy parsing, zero-allocation hot paths, `std::expected` error handling.

## Key Components

All headers are under `include/eph/fix/`:

- **tags.hpp** -- FIX tag constants (`eph::fix::tag` namespace) covering session-level, order entry, and market data tags. Includes `msg_type` sub-namespace for MsgType character constants (single-char and multi-char). Provides `tag_name()` and `msg_type_name()` for human-readable diagnostics. Users can define additional tags as `inline constexpr uint32_t`.
- **parser.hpp** -- Zero-copy FIX message parser. Parses `tag=value\x01` messages into a stack-allocated `BasicMessageView<MaxFields>` of `Field` structs (tag + string_view). All values point into the original buffer. Typed accessors: `get()`, `get_char()`, `get_int()`, `get_double()`, `get_bool()`, `get_timestamp()`. Supports repeating group iteration via `get_nth()`, `for_each_matching()`, and `GroupView`. Includes checksum validation and `compute_checksum()` utility. Default alias: `MessageView = BasicMessageView<128>`.
- **builder.hpp** -- Zero-allocation FIX message builder. `MessageBuilder` writes fields directly into a caller-provided buffer via fluent `set()` / `set_int()` / `set_double()` / `set_char()` / `set_bool()` / `set_timestamp()` / `set_decimal()` / `set_price()` / `set_raw()` methods. `finish()` prepends BeginString + BodyLength and appends CheckSum. Duplicate-tag-safe variants (`set_unique()`, etc.) for FIX spec compliance. Supports repeating groups via `begin_group()`. Configurable timestamp precision (seconds through nanoseconds).
- **framer.hpp** -- `BasicFixFramer<MaxBodyLength>` satisfying the `eph::net::MessageFramer` concept. Finds message boundaries by parsing BeginString/BodyLength/CheckSum structure. Validates checksum on decode. Extracts MsgType hint for the transport layer. Default alias: `FixFramer = BasicFixFramer<>` (1 MB max body).
- **orders.hpp** -- Typed builder wrappers for HFT order entry. `build_new_order()` (MsgType=D), `build_cancel_order()` (MsgType=F), `build_replace_order()` (MsgType=G). Each builds a complete, finalized FIX message into a caller-owned buffer with no heap allocation. Includes `Side`, `OrdType`, and `TimeInForce` enums.
- **execution_report.hpp** -- Zero-copy typed view over parsed ExecutionReport (MsgType=8). `ExecutionReportView<MaxFields>` wraps a `BasicMessageView` with accessors for identifiers (ClOrdID, OrderID, ExecID), status (ExecType, OrdStatus), fills (LastPx, LastQty, AvgPx, CumQty, LeavesQty), and convenience predicates (`is_fill()`, `is_terminal()`). `try_parse_execution_report()` combines parse + MsgType check in one call. Includes `ExecType` and `OrdStatus` enums.
- **session.hpp** -- FIX 4.4 session layer. `FixSession` provides Logon/Logout handshake, automatic Heartbeat/TestRequest, heartbeat timeout detection, bidirectional MsgSeqNum tracking with gap detection, SequenceReset/GapFill handling, and optional ResendRequest on sequence gaps. Thread-safe via atomics. Pluggable send callback works with any transport. Configurable via `FixSessionConfig` (comp IDs, heartbeat interval, timeout factor, reset-on-logon, resend-on-gap).
- **position.hpp** -- Per-symbol position tracker. `PositionTracker` records fills and computes signed quantity, VWAP entry price, realized PnL (average-cost method), and notional exposure. Handles position flips (long-to-short crossover). Provides `total_unrealized_pnl()`, `total_realized_pnl()`, and `net_exposure()` aggregations.
- **risk_check.hpp** -- Pre-trade risk checker. `RiskChecker` validates orders against configurable `RiskLimits`: max order qty, max order notional, per-symbol position qty/notional limits, and total portfolio exposure. Returns a typed `RiskRejectReason` enum. Limits are hot-updatable at runtime.
- **order_manager.hpp** -- Order lifecycle manager. `OrderManager` tracks orders from submission through fills/cancels with full state machine (`PendingNew -> New -> PartiallyFilled -> Filled/Canceled/Rejected`). Processes `ExecutionReportView` events and optionally forwards fills to a `PositionTracker`. Handles cancel rejects by reverting PendingCancel state. Provides `active_count()` and `purge_terminal()` for housekeeping.

## Dependencies

- **eph-core** -- Framer concept (`framer_concept.hpp`), number parsing (`parse_number.hpp`)
- **spdlog** -- Logging (all components)

## Quick Start

```cpp
#include <eph/fix/parser.hpp>
#include <eph/fix/builder.hpp>
#include <eph/fix/orders.hpp>
#include <eph/fix/execution_report.hpp>

// Build a NewOrderSingle
uint8_t buf[512];
size_t len = eph::fix::build_new_order(
    buf, sizeof(buf),
    "MY_ALGO", "EXCHANGE",
    "ORD001", "AAPL",
    eph::fix::Side::Buy,
    eph::fix::OrdType::Limit,
    /*qty=*/100.0, /*price=*/150.50);

// Parse a FIX message (zero-copy)
auto result = eph::fix::parse(data, data_len);
if (result) {
    auto msg_type = result->msg_type();  // "D", "8", etc.
    auto symbol   = result->get(eph::fix::tag::Symbol);
    auto price    = result->get_double(eph::fix::tag::Price);
}

// Process an ExecutionReport
if (auto er = eph::fix::try_parse_execution_report(data, len)) {
    if (er->view.is_fill()) {
        auto px  = er->view.last_px();   // std::optional<double>
        auto qty = er->view.last_qty();  // std::optional<int64_t>
    }
}
```
