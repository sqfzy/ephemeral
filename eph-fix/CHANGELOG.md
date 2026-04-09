# Changelog

All notable changes to `eph-fix` are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/).
This project has no tagged releases yet — everything is under `[Unreleased]`,
grouped by the development day for readability.

## [Unreleased]

### Added

#### Session layer (FIX 4.4)
- Full `FixSession` implementation: Logon/Logout handshake with configurable
  timeout, automatic `Heartbeat` sending, `TestRequest` probing when the
  counterparty goes idle, heartbeat timeout detection, bidirectional
  `MsgSeqNum` tracking with gap detection, and `SequenceReset`/`GapFill`
  handling.
- `FixSessionConfig` with `validate()`, `warnings()`, `dump()`, `to_json()`,
  defaulted equality, and a `std::formatter` specialization so session
  configuration is directly printable via `{}` in `std::format`.
- Server-provided `HeartBtInt` override on Logon response, atomic state
  transitions, and an `on_state_change` callback for lifecycle observability.
- `begin_string()` accessor on `BasicMessageView` for multi-version FIX
  dispatch.

#### Order lifecycle
- `OrderManager` with full state machine (`PendingNew` → `New` →
  `PartiallyFilled` → `Filled`, plus `PendingCancel`/`Canceled`/`Rejected`
  terminals). Consumes `ExecutionReportView` and can optionally forward fills
  to a `PositionTracker`. Handles `OrderCancelReject` by reverting
  `PendingCancel` to the appropriate active state.
- `ManagedOrder.dump()` and `to_json()` for order-state monitoring.
- `purge_terminal()` to reclaim memory held by completed orders.

#### Risk and positions
- `PositionTracker`: signed quantity, VWAP entry price (incremental formula to
  avoid catastrophic cancellation), realized PnL via the average-cost method,
  notional exposure, and correct handling of position flips (e.g. selling more
  than a long position so the remainder opens a new short).
- `RiskChecker` enforcing `RiskLimits` for max order qty, max order notional,
  per-symbol position qty/notional, and total exposure. Limits are hot-updatable
  at runtime. `RiskLimits::warnings()` surfaces likely misconfigurations (e.g.
  position limit without order-size cap).
- `Position` and `RiskLimits` both ship with `dump()` / `to_json()` / equality /
  `std::formatter` specializations.

#### Order entry and execution reports
- Typed builders `build_new_order` (D), `build_cancel_order` (F), and
  `build_replace_order` (G) with `Side`, `OrdType`, and `TimeInForce` enums,
  default `HandlInst=1`, and automatic `TransactTime`/`SendingTime`.
- `ExecutionReportView<MaxFields>` zero-copy typed accessors for every common
  ExecutionReport field, plus `is_fill()` / `is_terminal()` predicates.
- `try_parse_execution_report()` convenience combining `parse()` + MsgType
  check + view construction in one wire-to-view call.
- `ExecType` and `OrdStatus` enums with `exec_type_name()` / `ord_status_name()`
  helpers and `std::formatter` specializations.

#### Parser and builder
- `BasicMessageView` typed accessors (`get_char`, `get_int`, `get_double`,
  `get_bool`, `get_timestamp`) with overflow/format validation returning
  `std::optional`.
- `get_group()`, `get_nth()`, `for_each_matching()`, and a `GroupEntry` /
  `RepeatingGroupView` abstraction for FIX repeating groups.
- `dispatch()` — compile-time tagged-dispatch visitor for routing parsed
  messages by MsgType with zero runtime cost.
- `MessageBuilder`:
  - `set`, `set_int`, `set_double`, `set_char`, `set_bool`, `set_raw`,
    `set_timestamp`, `set_decimal`, `set_price` fluent setters.
  - `set_unique` family to enforce "tag must appear at most once" per FIX spec.
  - `set_timestamp` with selectable precision (seconds/ms/us/ns).
  - `set_decimal`/`set_price` for bit-exact decimal prices (avoids double
    round-trip precision loss on financial fields).
  - `reset()` to reuse the same builder with one buffer across many messages.
  - `has_tag()`, `has_overflow()`, `field_count()`, `bytes_used()`,
    `remaining_capacity()`, `as_span()`, `as_string_view()` accessors.
- `parse_all()` batch parser with a `ParserStats` accumulator
  (`messages_parsed`, `parse_errors`, `bytes_consumed`, first-error offset,
  `throughput()` and `error_rate()` helpers, JSON dump).
- `std::span` overloads for `parse()` and `parse_all()`.
- `std::formatter` specializations for `BasicMessageView`, `ParseError`,
  and `ParserStats`.

#### Framing
- `BasicFixFramer<MaxBodyLength>` satisfying the `eph::net::MessageFramer`
  concept. Scans for the `10=XXX\x01` CheckSum field to identify message
  boundaries and validates checksum on decode. Extracts `MsgType` hint into
  `DecodedFrame.msg_type` for the transport layer.

#### Tags and diagnostics
- `eph::fix::tag` namespace with session, order entry, and market data tag
  constants, plus single- and multi-char MsgType constants (including FIX 4.4+
  extensions: `TradeCaptureReport`, `PositionReport`, etc.).
- `tag_name()` and `msg_type_name()` (both single-char and multi-char
  overloads) for human-readable diagnostics.

### Changed

- Logger factories across all modules now return `const std::shared_ptr&` or
  raw pointer to avoid repeated shared-pointer copies on the hot path.
- Parser integer parsing consolidated on top of `eph::core::parse_int` /
  `parse_number` (removes duplicated implementations and centralizes overflow
  detection).
- `[[nodiscard]]` added to every `encode()` method on framers and every
  typed accessor on the message view.
- Hot-path name lookups no longer allocate: `tag_name()` / `msg_type_name()` /
  enum `*_name()` all return `string_view` of static storage.
- JSON output escapes `"` / `\\` / control characters per RFC 8259 §7 so
  `to_json()` produces well-formed JSON for arbitrary field contents.
- Build: `xmake.lua` is now modular — each `tests/*.cpp` and `benchmarks/*.cpp`
  is picked up automatically as its own target, so adding a test no longer
  requires editing the build file.

### Fixed

- **Integer overflow in body-length parsing** — both parser and framer now
  check for overflow before each multiplication when decoding `9=NNN`,
  preventing maliciously long digit strings from wrapping past the
  `MaxBodyLength` cap.
- **Checksum field range validation** — strict `10=XXX\x01` shape check,
  rejecting missing trailing SOH and non-digit checksum values.
- **Buffer arithmetic overflow** — bounds checks in both `parse()` and
  framer `decode()` rearranged to avoid computing a sum that could wrap on
  extremely large inputs.
- **`INT64_MIN` undefined behavior** in `MessageBuilder::format_int` —
  negation is performed in `uint64_t` modular arithmetic.
- **`format_double` rounding carry** — when rounding would push the
  fractional part to its limit (e.g. `0.995` at precision 2), the integer
  part is now correctly incremented instead of emitting a malformed value.
- **Non-finite doubles** in `set_double` are rejected (sets overflow flag)
  instead of being written as `inf`/`nan`.
- **SOH contamination** — `set()` and `set_raw()` reject values that contain
  embedded `\x01`, which would otherwise silently corrupt field boundaries.
- **Duplicate tag detection** — `set_unique` family uses `has_tag()` to
  scan already-written body fields so duplicate non-repeating-group tags
  are rejected at build time.
- **Null buffer / zero capacity** — `MessageBuilder` constructor sets
  overflow state immediately if `buf == nullptr` or `capacity < 32`
  (the header reservation).
- **Data races in `FixSession`** — `FixSessionConfig` is now immutable after
  construction; shared state (`state_`, sequence counters, last-sent/recv
  timestamps, `heartbeat_interval_sec_`, `test_request_pending_`) uses atomics
  with appropriate acquire/release ordering.
- **FIX Leap seconds** — `parse_timestamp_value` no longer accepts
  `second == 60` for arbitrary dates; FIX 4.4 UTCTimestamp does not carry
  leap-second information.
- **`get_group(count=0)`** is now a valid no-op (previously ambiguous).
- **`ParserStats::operator-`** preserves `first_error_*` from the newer
  snapshot so per-interval diagnostics remain meaningful.
- **Non-finite `avg_price`** in `PositionTracker::on_fill` is detected and
  reset to the fill price, with a warning, instead of silently propagating.
- **`OrderCancelReject` while not in `PendingCancel`** — no longer forces a
  terminal state; the reject is treated as informational.
- **FIX `EncryptMethod` cap** — server-provided `HeartBtInt` values outside
  `(0, 3600]` are rejected as unreasonable.

### Removed

- Nothing. No public API has been deprecated or removed since the module was
  introduced.

---

*This changelog covers commits up to `9f5c271` (2026-04-03).*
