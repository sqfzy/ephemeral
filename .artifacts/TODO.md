# TODO

## ~~FIX Session Layer~~ ✅ Done

Implemented in `eph-fix/include/eph/fix/session.hpp` (commit `51aa64f`).
Covers: Logon/Logout, Heartbeat/TestRequest (tag 112), sequence gap detection,
ResendRequest, SequenceReset-GapFill, PossDupFlag, server HeartBtInt override,
heartbeat timeout detection. 20 tests.

Remaining: sequence number persistence to disk (not needed for market data — reset on reconnect).

## eph-sbe Module

SBE (Simple Binary Encoding) zero-copy decode framework as a standalone module, parallel to eph-fix/eph-itch. Would provide constexpr schema definition + codegen, not just hand-written offsets.

Current state: `docs/binance-protocols.md` has a manual constexpr offset template (ITCH-style). Works but doesn't scale to schema evolution.

Trigger: build if multiple users need SBE schema codegen. Until then, example-level manual offsets suffice.

## Ed25519 API Key Signing

Binance SBE streams require Ed25519 API key authentication. This is auth-layer concern, not transport-layer.

Current state: not implemented. Users manage API keys externally and pass via `extra_headers`.

## TLS 1.3 KeyUpdate

RFC 8446 §4.6.3 key rotation for long-lived connections (>16M TLS records ~ >100GB data). Current code warns at 90% sequence exhaustion but doesn't trigger KeyUpdate — reconnect is the workaround.

Estimated effort: 20h. Low priority unless users run single connections for days.

## Integration Test Harness

Docker-compose setup with a mock WebSocket/TLS server for CI integration tests. Current tests use `WsMockTcpTransport` (in-process mock) — no real TLS/WS handshake coverage.

Estimated effort: 8h+ plus ongoing maintenance.
