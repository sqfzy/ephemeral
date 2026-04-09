# Project: eph-core

> Header-only C++23 foundation library providing the shared concepts, error types, parsers, framing primitives, and transport abstractions used by every other `eph-*` subproject.

**Language**: C++23 | **Build**: xmake (header-only target) | **License**: see repository root

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

`eph-core` sits at the bottom of the `ephemeral_dev` dependency graph. It exists to give every downstream module a single, authoritative place to pull shared primitives from — version metadata, the `ErrorEnum`/`ErrorEnumFormatter` machinery that wires every error enum into `std::format` and spdlog, the `MessageFramer` / `TcpTransport` / `MetricsSink` concepts that describe the interfaces between layers, the transport-layer error types (`SendError`, `ConnectionError`, `ConnectionErrorInfo`) that the whole stack surfaces, and the zero-allocation decimal / JSON / base64 / string-check helpers that hot paths rely on.

It is deliberately **leaf**: it has no `eph-*` dependencies and its only external dependency is spdlog for logging. Anything that would drag in OpenSSL, DPDK, io_uring, or WebSocket machinery lives in a different subproject. The rule is strict — `transport_errors.hpp` was extracted precisely so that downstream modules (`eph-fix`, `eph-itch`, `eph-dpdk`, `eph-book`) can react to `ConnectionError` values without pulling in the full `eph-net` TLS/WS surface.

It is also deliberately **small**: 11 public headers, ~1 400 lines of code (excluding comments), and every symbol is either a concept, a typed error, a parser, or a thin helper. The only concrete runtime types are `LengthPrefixFramer` (one wire format), `FakeTcpTransport` (the test double), and `NullSink` (zero-cost metrics no-op). Everything else is a shape that downstream modules implement.

Because it is header-only, `eph-core` has no runtime. Building the `eph-core` xmake target only checks that the toolchain supports C++23 `std::expected` + `std::format`. All real compilation happens when downstream consumers `#include` these headers.

---

## Architecture

`eph-core` is **not** layered — it is a flat collection of primitives. What structure it has comes from the dependency graph between headers:

- `error_traits.hpp` is the bottom of the internal graph: every typed-error header includes it for `ErrorEnumFormatter`.
- `framer_concept.hpp` depends on `error_traits.hpp` (so `FrameError` can be formatted).
- `length_prefix_framer.hpp` depends on `framer_concept.hpp` (it satisfies the concept).
- `tcp_concept.hpp` is standalone apart from std headers.
- `fake_tcp_transport.hpp` depends on `tcp_concept.hpp`.
- `transport_errors.hpp` depends on `error_traits.hpp` and `detail/json_escape.hpp` (for `to_json()`).
- `metrics_concept.hpp`, `parse_number.hpp`, `version.hpp`, and the `detail/*` headers are all standalone.

Everything is `inline` or `constexpr` so that including multiple headers in the same translation unit does not cause ODR violations, and so that the compiler can fully inline through the concepts at `-O2`.

### Component diagram

```
        +---------------------------+
        |        version.hpp        |    standalone
        +---------------------------+

        +---------------------------+
        |    error_traits.hpp       |  <-- ErrorEnum concept + formatter base
        +---------------------------+
           ^        ^         ^
           |        |         |
  +--------+--+  +--+--------+ +-------+-----------+
  | framer_   |  | transport_| | (downstream       |
  | concept.hpp|  | errors.hpp| |  error enums)    |
  +-----------+  +-----------+ +-------------------+
       ^              ^
       |              |
  +----+------------+ |
  | length_prefix_  | |   depends on json_escape for to_json()
  | framer.hpp      | v
  +-----------------+ +----> detail/json_escape.hpp

  +--------------+    +-------------------+
  | tcp_         |    | fake_tcp_         |
  | concept.hpp  |<---| transport.hpp     |
  +--------------+    +-------------------+

  +--------------+    +-------------------+
  | metrics_     |    |  parse_number.hpp |
  | concept.hpp  |    +-------------------+
  +--------------+

  +--------------+    +-------------------+
  | detail/      |    | detail/           |
  | base64.hpp   |    | string_checks.hpp |
  +--------------+    +-------------------+
```

---

## Module Map

| Header | Responsibility | Key types / symbols | Depends on |
|---|---|---|---|
| `eph/version.hpp` | Compile-time version metadata and feature-gate guard. | `kVersionMajor/Minor/Patch`, `kVersion`, `kVersionString`, `kVersionFull`, `version_at_least()` | std only |
| `eph/core/error_traits.hpp` | Generic `ErrorEnum` concept and `std::formatter` base. | `ErrorEnum<E>`, `ErrorEnumFormatter<E>` | std only |
| `eph/core/framer_concept.hpp` | Pluggable wire-framing contract. | `MessageFramer<F>`, `FrameError`, `DecodedFrame`, `frame_error_name()` | `error_traits.hpp` |
| `eph/core/length_prefix_framer.hpp` | 2-byte big-endian length-prefix framer implementation. | `LengthPrefixFramer` | `framer_concept.hpp`, spdlog |
| `eph/core/tcp_concept.hpp` | TCP backend contract and RFC 793 state enum. | `TcpTransport<T>`, `TcpState`, `tcp_state_name()` | std only |
| `eph/core/fake_tcp_transport.hpp` | Programmable TcpTransport test double. | `testing::FakeTcpTransport` | `tcp_concept.hpp` |
| `eph/core/transport_errors.hpp` | Transport-layer typed errors and structured info. | `SendError`, `ConnectionError`, `ConnectionErrorInfo`, `operator!(SendError)` | `error_traits.hpp`, `detail/json_escape.hpp` |
| `eph/core/metrics_concept.hpp` | Prometheus-style metrics sink contract + null impl. | `MetricsSink<T>`, `MetricTag`, `NullSink` | std only |
| `eph/core/parse_number.hpp` | Zero-allocation decimal parsers. | `parse_number()`, `parse_int()` | std only |
| `eph/core/detail/json_escape.hpp` | RFC 8259 S7 JSON string escaping with UTF-8 handling. | `detail::json_escape()` | std only |
| `eph/core/detail/base64.hpp` | RFC 4648 base64 encoder for WebSocket keys and proxy auth. | `detail::base64_encode()` | std only |
| `eph/core/detail/string_checks.hpp` | Constexpr control-char check for hostname/path validation. | `detail::contains_control_chars()` | std only |

---

## Data Flow

There is no single "data path" through `eph-core` — it is a collection of primitives consumed by downstream modules. The most interesting observable flow is the **framing and error surface that `eph-net` rides on top of**.

### Flow diagram

```
 Application                 eph-net (TransportImpl)              eph-core
 ───────────                 ───────────────────────              ────────

 send(payload)  ────────►   TcpTransport::send() ──────────────►  SendError
                                 |                                    |
                                 | on success                         |
                                 v                                    |
                            Framer::encode(out, payload, len, type)   |
                                 |                                    |
                                 | writes framed bytes                |
                                 v                                    |
                            raw bytes to wire                         |
                                                                      |
 poll()     ───────────►    TcpTransport::poll_rx(cb)                  |
                                 |                                    |
                                 | on data                            |
                                 v                                    |
                            Framer::decode(buf, len) ──────► DecodedFrame
                                                             or FrameError
                                 |                                    |
                                 | deliver payload view               |
                                 v                                    |
 on_message(payload, type) ◄─── callback                              |
                                                                      |
 on error    ◄───────────────── ConnectionErrorInfo ◄─── build info   |
                                (JSON via to_json())                  |
```

All of the named types on the right (SendError, FrameError, DecodedFrame, ConnectionErrorInfo, the concepts) live in `eph-core`. None of the boxes on the left do — `eph-core` supplies only the contracts.

---

## Key Components

### `LengthPrefixFramer`

**File**: `include/eph/core/length_prefix_framer.hpp`
**Purpose**: The only concrete `MessageFramer` implementation shipped here. Encodes payloads as `[2-byte big-endian length][payload]`; exposes the first payload byte as `msg_type` in `DecodedFrame` (this is what lets it be used as an ITCH framer where byte 0 is the message type).

**Interface**:
```cpp
static constexpr size_t max_overhead() noexcept;          // always 2
static constexpr size_t kMaxPayloadLen = 65535;
[[nodiscard]] size_t encode(uint8_t* out,
                            const uint8_t* data, size_t len,
                            uint8_t /*msg_type*/) noexcept;
[[nodiscard]] std::expected<DecodedFrame, FrameError>
decode(const uint8_t* data, size_t len) noexcept;
```

**Notes**: Zero-length payloads are rejected by both `encode()` and `decode()` because `msg_type` is derived from `payload[0]`. Heartbeat protocols should use a 1-byte sentinel. Errors on `encode()` are signalled by returning `0`, not a typed error — the doc comment flags this as a footgun callers must check.

---

### `FakeTcpTransport`

**File**: `include/eph/core/fake_tcp_transport.hpp`
**Purpose**: Programmable in-memory mock satisfying the `TcpTransport` concept. Lets upper-layer code (framers, session state machines, protocol codecs) be unit-tested without opening a real socket.

**Interface** (two surfaces):
```cpp
// TcpTransport concept surface — behaves like a real transport.
connect(timeout), close(), reset(), send(data, len),
poll_rx(callback), last_rx_burst_tsc(), mss(), state(), is_established()

// Programmable test-control surface.
inject_rx(std::vector<uint8_t>) / inject_rx(const uint8_t*, size_t)
set_connect_error(string) / set_send_error(string) / set_rx_error(string)
clear_errors()
inject_disconnect()
sent_data()  // inspect everything that was sent
clear_sent_data()
```

**Notes**: Begins in `TcpState::Closed`, moves to `Established` on `connect()` success. Not thread-safe — intended for single-threaded unit tests. `mss()` is hardcoded to 1460 (standard Ethernet). `last_rx_burst_tsc()` is always 0 (no hardware timestamp).

---

### `TcpTransport` concept

**File**: `include/eph/core/tcp_concept.hpp`
**Purpose**: The contract every TCP backend (DPDK, io_uring, kernel sockets, loopback, `FakeTcpTransport`) must satisfy. Constrained via C++20 concepts so template instantiation monomorphizes to zero-overhead inlined code — the whole point of a concept-based backend is that the upper layers pay nothing for the abstraction.

**Interface**:
```cpp
template <typename T>
concept TcpTransport = requires(T& t, const void* data, size_t len,
                                std::chrono::milliseconds timeout) {
    { t.connect(timeout) } -> std::same_as<std::expected<void, std::string>>;
    { t.close() }          -> std::same_as<std::expected<void, std::string>>;
    { t.reset() }          noexcept;
    { t.send(data, len) }  -> std::same_as<std::expected<size_t, std::string>>;
    { t.last_rx_burst_tsc() } -> std::convertible_to<uint64_t>;
    { t.mss() }            -> std::convertible_to<uint16_t>;
    { t.state() }          -> std::same_as<TcpState>;
    { t.is_established() } -> std::same_as<bool>;
} && requires(T& t) {
    { t.poll_rx([](const uint8_t*, uint16_t) {}) }
        -> std::same_as<std::expected<uint16_t, std::string>>;
};
```

**Notes**: `last_rx_burst_tsc()` captures the TSC read at the earliest point after data arrives (rte_eth_rx_burst / recvmsg) — this is what lets the latency benchmarks pin down where in the stack time is being spent. The `uint16_t` payload length parameter in `poll_rx` caps frames at 65 535 bytes (jumbo frames fit comfortably; reassembly beyond that would be a breaking API change).

---

### `MessageFramer` concept + `DecodedFrame`

**File**: `include/eph/core/framer_concept.hpp`
**Purpose**: The pluggable wire-format layer. Any framer (length-prefix, WebSocket, raw, FIX, SBE) satisfies this concept and can be dropped into the transport stack.

**Interface**:
```cpp
template <typename F>
concept MessageFramer = requires(F f, uint8_t* out, const uint8_t* in,
                                  size_t len, uint8_t msg_type) {
    { f.encode(out, in, len, msg_type) } noexcept -> std::same_as<size_t>;
    { f.decode(in, len) } noexcept
        -> std::same_as<std::expected<DecodedFrame, FrameError>>;
    { F::max_overhead() } noexcept -> std::convertible_to<size_t>;
};

struct DecodedFrame {
    const uint8_t* payload;   // zero-copy view into receive buffer
    size_t         payload_len;
    uint8_t        msg_type;  // protocol-specific: WS opcode, ITCH type, ...
    bool           is_control;
    size_t         total_len; // header + payload, consumed bytes
};
```

**Notes**: `decode()` is an **instance** method (not static) to support stateful framers that keep per-connection context (SBE schema cache, protocol version negotiation). Stateless framers like `LengthPrefixFramer` incur zero overhead because the compiler eliminates the unused `this` pointer. `DecodedFrame::payload` is a zero-copy view into the original receive buffer — it is invalidated by the next `decode()` call or buffer reuse.

---

### `ErrorEnum` + `ErrorEnumFormatter`

**File**: `include/eph/core/error_traits.hpp`
**Purpose**: One-liner `std::formatter` registration for every error enum in the ecosystem. Without this, each enum would need its own 10-line formatter specialization; with it, registration is a single `template <> struct std::formatter<E> : eph::core::ErrorEnumFormatter<E> {};`.

**Interface**:
```cpp
template <typename E>
concept ErrorEnum = std::is_enum_v<E> && requires(E e) {
    { error_name(e) } -> std::convertible_to<std::string_view>;
};

template <ErrorEnum E>
struct ErrorEnumFormatter : std::formatter<std::string_view> {
    auto format(E e, auto& ctx) const;  // formats as error_name(e)
};
```

**Notes**: The ADL-visible `error_name(E)` function is the extension point — every error enum in `eph-core`, `eph-net`, `eph-fix`, and `eph-itch` provides one. Backward-compatible aliases exist under `eph::net::` for legacy call sites (flagged for removal in a future major version).

---

### `parse_number` / `parse_int`

**File**: `include/eph/core/parse_number.hpp`
**Purpose**: Zero-allocation decimal parsers used by `eph-json`, `eph-fix`, and `eph-book` adapters. Consolidation of seven duplicated `parse_number` and four duplicated `parse_int` implementations that previously lived in individual adapter modules.

**Interface**:
```cpp
[[nodiscard]] std::optional<double>
parse_number(std::string_view sv) noexcept;

[[nodiscard]] std::optional<int64_t>
parse_int(std::string_view sv) noexcept;
```

**Notes**: `parse_number` handles sign, integer part, fraction, and scientific notation; it rejects NaN, infinity, empty strings, bare dots (`"1."`), bare exponents (`"1e"`), trailing characters, and overflow beyond IEEE 754 double. `parse_int` handles the full int64 range including `INT64_MIN` by accumulating in `uint64_t` and casting at the end — this avoids the signed-overflow UB that the naive signed-accumulation implementation has when parsing the minimum value.

---

### `SendError` + `ConnectionError` + `ConnectionErrorInfo`

**File**: `include/eph/core/transport_errors.hpp`
**Purpose**: The typed error surface the entire transport stack returns. Lets callers match on category for retry/abort decisions (e.g. retry on transient TCP errors, abort on TLS certificate rejection).

**Interface**:
```cpp
enum class SendError : int8_t {
    kOk=0, kMessageTooLarge=-1, kNotConnected=-2, kQueueFull=-3,
    kInvalidUtf8=-4, kInvalidCloseCode=-5, kNullData=-6,
    kEncryptFailed=-7, kTcpSendFailed=-8,
};
constexpr bool operator!(SendError e) noexcept;  // true when not kOk

enum class ConnectionError : uint8_t { /* 15 categories */ };

struct ConnectionErrorInfo {
    ConnectionError    code;
    std::string        detail;
    std::optional<int> http_status;
    std::string message() const;   // "[CATEGORY] detail"
    std::string to_json() const;   // escaped JSON
};
```

**Notes**: `operator!(SendError)` is an intentional inversion: it returns `true` on error so that `if (!send(...))` reads as "if send failed". `ConnectionErrorInfo::to_json()` uses `detail::json_escape` for safe embedding in monitoring payloads; `http_status` is only emitted when populated (so monitoring systems do not see zero-valued fields).

---

### `MetricsSink` concept + `NullSink`

**File**: `include/eph/core/metrics_concept.hpp`
**Purpose**: Prometheus-style metrics contract: counter (monotonic int64), gauge (point-in-time double), histogram (distribution sample), with `MetricTag` dimensional labels. `NullSink` is a zero-cost default.

**Interface**:
```cpp
struct MetricTag { std::string_view key; std::string_view value; };

template <typename T>
concept MetricsSink = requires(T& sink, std::string_view name,
                               std::span<const MetricTag> tags) {
    { sink.push_counter(name, int64_t{}, tags) }   -> std::same_as<void>;
    { sink.push_gauge(name, double{}, tags) }      -> std::same_as<void>;
    { sink.push_histogram(name, double{}, tags) }  -> std::same_as<void>;
    { sink.flush() } -> std::same_as<void>;
};

struct NullSink { /* all methods inline noexcept no-ops */ };
```

**Notes**: `NullSink`'s methods are all inline noexcept empty bodies — at `-O2` the compiler elides the call sites entirely. Hot paths that want "metrics disabled" performance should template on `MetricsSink` and instantiate with `NullSink`.

---

## Entry Points & APIs

| Entrypoint | Type | Description |
|---|---|---|
| `include/eph/version.hpp` | Compile-time metadata | `kVersionFull`, `version_at_least()`. |
| `include/eph/core/error_traits.hpp` | Concepts + formatter base | Gateway for every error enum to participate in `std::format`. |
| `include/eph/core/framer_concept.hpp` | Concept + types | Entry point for anything building or decoding framed wire bytes. |
| `include/eph/core/tcp_concept.hpp` | Concept + enum | Entry point for anything implementing or consuming a TCP backend. |
| `include/eph/core/transport_errors.hpp` | Typed errors | Every transport-layer error value the stack returns comes from here. |
| `include/eph/core/parse_number.hpp` | Parsers | Canonical decimal parsers for exchange field values. |
| `include/eph/core/length_prefix_framer.hpp` | Runtime class | The one concrete framer shipped in this module. |
| `include/eph/core/fake_tcp_transport.hpp` | Runtime class | Test double used by every downstream module's unit tests. |

There are **no** executables, CLIs, or library entry points beyond these headers. `eph-core` is pure vocabulary.

---

## Dependencies

### Internal (module graph)

```
transport_errors.hpp ──> detail/json_escape.hpp
transport_errors.hpp ──> error_traits.hpp
framer_concept.hpp   ──> error_traits.hpp
length_prefix_framer.hpp ──> framer_concept.hpp ──> error_traits.hpp
fake_tcp_transport.hpp   ──> tcp_concept.hpp

version.hpp              (standalone)
metrics_concept.hpp      (standalone)
parse_number.hpp         (standalone)
detail/base64.hpp        (standalone)
detail/string_checks.hpp (standalone)
```

No cycles. Every header is independently includable.

### External

| Package | Version | Purpose |
|---|---|---|
| spdlog | any (resolved by monorepo `add_requires`) | Named logger `core.framer` for `LengthPrefixFramer` diagnostics. |
| gtest | any (resolved by monorepo `add_requires`) | Unit tests only — not part of the public API surface. |
| benchmark (Google Benchmark) | optional | Microbenchmarks under `benchmarks/`. |
| libFuzzer | optional | Manual-build fuzzers under `fuzzers/`. |

`SPDLOG_ACTIVE_LEVEL` is set by the subproject `xmake.lua` from the monorepo-level `net_log_level` variable — `SPDLOG_LEVEL_TRACE` in debug, `SPDLOG_LEVEL_INFO` otherwise.

---

## Testing

| Test suite | Location | Coverage focus |
|---|---|---|
| `test_parse_number` | `tests/test_parse_number.cpp` | Happy path, scientific notation, overflow, `INT64_MIN`, malformed input (bare dots, bare exponents, trailing characters, empty strings). 391 lines — the richest test file in the module. |
| `test_length_prefix_framer` | `tests/test_length_prefix_framer.cpp` | Encode/decode round-trips, incomplete frames, invalid-format (zero-length), payload-too-large boundary, `msg_type` extraction from `payload[0]`. |
| `test_json_escape` | `tests/test_json_escape.cpp` | Escapable characters, control bytes, valid UTF-8 pass-through, invalid UTF-8 `\uXXXX` fallback, fast-path no-escape-needed case. |
| `test_base64` | `tests/test_base64.cpp` | RFC 4648 vectors, `=` padding for length % 3 in {0, 1, 2}. |
| `test_transport_errors` | `tests/test_transport_errors.cpp` | `operator!(SendError)`, `error_name` coverage, `ConnectionErrorInfo::message()` / `to_json()` (with and without `http_status`), defaulted equality. |
| `test_fake_tcp_transport` | `tests/test_fake_tcp_transport.cpp` | State transitions, staged failures, `inject_rx` queue semantics, `sent_data()` ordering, concept satisfaction. 268 lines. |
| `test_string_checks` | `tests/test_string_checks.cpp` | Printable strings, empty string, every ASCII control character, `U+007F`, bytes >= 0x80 (valid UTF-8 continuation bytes must **not** trip the check). |

Key test scenarios:

- **`parse_int("-9223372036854775808")`** — verifies the unsigned-accumulation implementation correctly produces `INT64_MIN` without signed overflow UB.
- **`LengthPrefixFramer` zero-length payload rejection** — both `encode()` and `decode()` refuse, preventing reads past `payload[0]`.
- **`json_escape` invalid-UTF-8 handling** — an orphan continuation byte (e.g. `0x80` with no lead byte) is escaped as `\u0080` rather than silently emitted.
- **`contains_control_chars("hello")`** — UTF-8 multibyte sequences must **not** be flagged; only bytes `< 0x20` and `0x7f` should.
- **`FakeTcpTransport::poll_rx` queue semantics** — single packet delivered per call, FIFO order, `rx_queue` drained on success.
