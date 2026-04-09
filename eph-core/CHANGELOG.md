# Changelog

All notable changes to `eph-core` are documented in this file. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this project has not yet reached a tagged release, so the section below captures every change since the module was extracted.

## [Unreleased]

### Added
- **`contains_control_chars()` test coverage** — dedicated unit tests (`tests/test_string_checks.cpp`) exercising the constexpr hostname/path validation helper that protects against HTTP header injection, request smuggling, and log injection.
- **Production-readiness hardening R2** — additional boundary and error-path coverage across the core headers.
- **Production-readiness foundations (phase 1)** — type-system and foundation cleanups so downstream modules can rely on `eph-core` without reaching into `eph-net`.
- **Modular build** — `eph-core` now carries its own `xmake.lua`; tests and benchmarks live in per-subproject directories and are picked up automatically.
- **Shared `base64_encode()` helper** (`detail/base64.hpp`) — replaces four duplicate inline implementations scattered across the transport/WS/proxy code.
- **Shared `contains_control_chars()` helper** (`detail/string_checks.hpp`) — replaces repeated inline checks across `TransportConfig`, `SocketConfig`, and `ProxyConfig`.
- **`MetricsSink` concept + `NullSink`** (`metrics_concept.hpp`) — Prometheus-style counter / gauge / histogram interface with a zero-cost default implementation that disappears at `-O2`.
- **`version.hpp`** relocated from `eph-utils` to `eph-core` so that even the leafiest modules can log `ephemeral/M.m.p`.
- **`FakeTcpTransport`** — programmable in-memory `TcpTransport` mock for deterministic unit tests (stage RX packets, stage failures, inspect TX bytes).
- **Transport error types extracted** — `SendError`, `ConnectionError`, and `ConnectionErrorInfo` (with JSON serialization) now live in `eph-core` so that downstream modules can handle transport errors without pulling in TLS or WebSocket headers.
- **`LengthPrefixFramer`** — 2-byte big-endian length-prefix framer for binary protocols (ITCH, SBE, custom wire formats).
- **`MessageFramer` concept** with `FrameError`, `DecodedFrame` zero-copy view, and pluggable `encode()` / `decode()` / `max_overhead()` contract.
- **`TcpTransport` concept** plus the RFC 793 `TcpState` enum and `tcp_state_name()` helper.
- **`ErrorEnum` concept and `ErrorEnumFormatter`** — one-liner `std::formatter` registration for every error enum in the ecosystem.
- **`parse_number()` / `parse_int()`** — zero-allocation decimal parsers consolidated from seven (number) and four (int) duplicated implementations scattered across `eph-json`, `eph-fix`, and `eph-book`.

### Changed
- **`ms_to_ns` / `us_to_ns` overflow guard** (shared `eph-utils` follow-up, but `eph-core` benefited from the same audit pass).
- **Framer `encode()` methods** now all carry `[[nodiscard]]` so accidental drops of the byte-count return value are caught at compile time.
- **All `*_name` / `error_name` functions** are marked `[[nodiscard]]` — callers that forget to use the name get a warning instead of silent code.
- **`ErrorEnum` and `ErrorEnumFormatter` moved into `eph::core::`** with backward-compatible aliases under `eph::net::` to ease migration.
- **`error_name` return types unified to `std::string_view`** across every error enum in the project, so `ErrorEnum` concept coverage is consistent.
- **`LengthPrefixFramer` logging routed through a named spdlog logger** (`core.framer`) in line with the project-wide convention of avoiding the default logger.

### Fixed
- **Empty-payload safety** — `LengthPrefixFramer::encode()` and `decode()` reject zero-length payloads because `DecodedFrame::msg_type` is derived from `payload[0]`. Heartbeat protocols should use a 1-byte sentinel message instead.
- **`parse_int(INT64_MIN)`** — fixed by switching to unsigned accumulation so that negating the max positive magnitude no longer triggers signed-overflow UB.
- **JSON escape invalid-UTF-8 handling** — orphan continuation bytes and truncated multi-byte sequences are now escaped as `\uXXXX` rather than silently emitted.
- **Static-assert gate on C++23** — `eph-core/xmake.lua` now runs a compile-check snippet for `std::expected` + `std::format` at config time and emits an actionable error (with the `EPH_USE_GCC14` hint for Amazon Linux 2023) when the toolchain is too old.
- Numerous small audit findings across the module rolled up from the monorepo-wide "resolve N audit findings" passes.

### Documentation
- Doxygen-style `///` and `/** */` comments on every public function, type, concept, and template in `include/` (see headers for details).
- `FakeTcpTransport` class/method doc comments added in this pass (previously only some setter methods carried `///` comments).

### Removed
- Duplicated `parse_number` and `parse_int` implementations previously maintained in `eph-json`, `eph-fix`, and `eph-book` adapters — all now delegate to `eph::core::parse_number` / `eph::core::parse_int`.
- Duplicated base64 encoders in WebSocket key and HTTP proxy auth paths — all now call `eph::core::detail::base64_encode`.

---

_Generated from `git log -- eph-core/`. No tags have been cut for this subproject yet; once the first release ships, historical entries will be anchored under dated version headers._
