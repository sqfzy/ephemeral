# Changelog

All notable changes to `eph-itch` are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/). This project has no tagged
releases yet, so every entry is in the Unreleased section.

## [Unreleased]

### Added

- **OUCH 5.0 order entry protocol** — builders for `EnterOrder`,
  `ReplaceOrder`, `CancelOrder`, and zero-copy views `AcceptedView`,
  `ExecutedView`, `CanceledView`, `ReplacedView`.
- **MoldUDP64 multicast transport** — `parse_moldudp64_header()` and
  `parse_moldudp64()` iterate length-prefixed messages in a UDP payload,
  honour the end-of-session sentinel, and reject truncated / overflow-prone
  packets.
- **SoupBinTCP framer** — `SoupBinTcpFramer` with `encode()` / `decode()`
  covering the 3-byte length+type wire format; packet type constants
  (`kSequencedData`, `kServerHeartbeat`, `kLoginAccepted`, …) and
  `soupbin::is_heartbeat()` helper.
- **ITCH 5.0 parser and zero-copy accessors** — `parse()`, `parse_all()`,
  `MessageView`, field accessor namespaces for all 22 message types, endian
  helpers (`read_be16/32/48/64`), and `trim()` for padded strings.
- **Type-safe dispatch** — `dispatch()` / `dispatch_all()` route a
  `MessageView` to handler overloads keyed on `msg::` tag structs
  (`msg::AddOrder`, `msg::OrderDelete`, …).
- **`ParserStats`** — counters for `messages_parsed`, `parse_errors`,
  `bytes_consumed`, first-error diagnostics, plus `throughput()`,
  `error_rate()`, `dump()`, `to_json()`, and a subtraction operator for
  interval snapshots.
- **Message classification helpers** — `is_system_message()`,
  `is_order_message()`, `is_trade_message()`, `is_imbalance_message()`,
  `is_known_type()`.
- **`std::formatter` specialisations** for `ParseError`, `MessageView`, and
  `ParserStats` so they plug directly into `std::format` / `std::print`.
- **std::span overloads** for `parse()` and `parse_all()`.
- **Auto-derived size constants** — `kMaxMessageSize` / `kMinMessageSize`
  computed at compile time from the per-type size constants, so adding a new
  message type only requires touching one place.
- **`ItchFramer`** type alias over `eph::net::LengthPrefixFramer`.
- **Test suite** — four GoogleTest files (`test_itch`, `test_moldudp64`,
  `test_ouch`, `test_soupbintcp`) and one Google Benchmark
  (`bench_itch_parse`).

### Changed

- **Logger factories unified** — all modules use named spdlog loggers
  (`itch.parser`, `itch.moldudp64`, `itch.soupbintcp`, `itch.ouch`) with a
  consistent return type so loggers can be configured per-module without
  tripping over spdlog's single-registry constraint.
- **`dispatch()` pointer convention** — handlers now receive the full message
  pointer (byte 0 = type tag), matching the convention used by every
  per-message accessor namespace (`add_order::price(msg)`, etc.).
- **Build layout modularised** — `eph-itch` now owns its own `xmake.lua`,
  tests, and benchmarks rather than being declared from the top-level build.

### Fixed

- **NOII message size** corrected to 50 bytes after spec review.
- **Logging actionability** — framer and parser error paths now emit the
  relevant offsets, lengths, expected sizes, and session identifiers so
  operators can diagnose malformed feeds without attaching a debugger.
- **MoldUDP64 overflow guard** — sequence-number + message-count addition is
  now checked against `UINT64_MAX` before iterating, and the buffer is
  pre-validated against the minimum `header + 2*count` size.
- **SystemEvent layout comment** — arithmetic cleaned up to match the wire
  spec exactly.

### Removed

- Nothing removed yet (no prior releases).

### Notes

Commits span 2026-03-23 (initial parser + framer) through 2026-04-03
(modular xmake refactor). The most recent documentation-oriented commits
(`docs: add Doxygen inline documentation to all public headers`,
`docs: expand all subproject READMEs with full API reference`) predate this
changelog entry.
