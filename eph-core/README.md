# eph-core

Header-only C++23 foundation library for the eph ecosystem. Provides shared concepts, error types, number parsing, message framing, metrics interfaces, and transport abstractions used across eph-net, eph-transport, eph-json, eph-fix, eph-itch, eph-book, and eph-dpdk.

This is the leaf dependency of the eph ecosystem -- no other eph-\* subproject is required.

## Key Components

All headers are under `include/eph/core/` unless otherwise noted.

- **version.hpp** (`include/eph/version.hpp`) -- Compile-time version constants and version string generation. Packed version number for compile-time comparisons, `kVersionString` ("M.m.p"), `kVersionFull` ("ephemeral/M.m.p"), and a `version_at_least()` consteval guard for downstream feature-gating.
- **error_traits.hpp** -- `ErrorEnum` concept and `ErrorEnumFormatter` base. Any enum with an ADL-visible `error_name(E) -> string_view` satisfies `ErrorEnum`. The formatter eliminates per-enum `std::formatter` boilerplate (one-liner per type). Backward-compatible aliases are provided in `eph::net::`.
- **framer_concept.hpp** -- `MessageFramer` concept defining pluggable wire-format framing for Transport. Requires `encode()`, `decode()`, and `max_overhead()`. Also defines `FrameError` (incomplete/invalid/too-large) and `DecodedFrame` (zero-copy view into the receive buffer with payload, msg_type, and control-frame flag).
- **length_prefix_framer.hpp** -- `LengthPrefixFramer` implementing `MessageFramer` with a 2-byte big-endian length prefix. Wire format: `[uint16_t length][payload]`. First payload byte is exposed as `msg_type` in `DecodedFrame`, making it suitable for ITCH and similar binary protocols. Max payload 65535 bytes.
- **tcp_concept.hpp** -- `TcpTransport` concept defining the interface for TCP backends (DPDK, io_uring, kernel sockets, loopback). Covers connection lifecycle (`connect`, `close`, `reset`), data transfer (`send`, `poll_rx`), timestamping (`last_rx_burst_tsc`), and state queries (`state`, `mss`, `is_established`). Also defines `TcpState` enum following RFC 793 active-open states.
- **transport_errors.hpp** -- Transport-layer error types extracted to eph-core so downstream modules avoid pulling in eph-net's TLS/WebSocket dependencies. Includes `SendError` (ok/too-large/not-connected/queue-full/etc.), `ConnectionError` (invalid-config/factory-failed/tls-handshake-failed/ws-upgrade-rejected/etc.), and `ConnectionErrorInfo` (structured error with code, detail string, optional HTTP status, and JSON serialization).
- **metrics_concept.hpp** -- `MetricsSink` concept for pushing metrics to external monitoring systems. Three metric types following Prometheus conventions: counter (monotonic int64), gauge (point-in-time double), histogram (distribution sample). Dimensional tagging via `MetricTag` key-value pairs. Includes `NullSink` -- a zero-cost no-op sink that compiles to nothing at -O2.
- **parse_number.hpp** -- Fast zero-allocation decimal string-to-number parsers. `parse_number(string_view) -> optional<double>` handles sign, integer, fraction, and scientific notation. `parse_int(string_view) -> optional<int64_t>` handles sign and integer with overflow-safe full int64 range (including INT64_MIN). Rejects NaN, infinity, empty strings, bare dots, and overflow.
- **detail/json_escape.hpp** -- RFC 8259 JSON string escaping utility. Handles `\"`, `\\`, control characters, and valid UTF-8 pass-through. Invalid UTF-8 bytes are escaped as `\uXXXX`. Fast path skips allocation when no escaping is needed.

## Public API Reference

### Version (`eph::`)

| Symbol | Kind | Description |
|--------|------|-------------|
| `kVersionMajor` | `constexpr int` | Major version component |
| `kVersionMinor` | `constexpr int` | Minor version component |
| `kVersionPatch` | `constexpr int` | Patch version component |
| `kVersion` | `constexpr int` | Packed version: `major*10000 + minor*100 + patch` |
| `kVersionString` | `constexpr string_view` | `"M.m.p"` version string |
| `kVersionFull` | `constexpr string_view` | `"ephemeral/M.m.p"` for User-Agent headers and logs |
| `version_at_least(major, minor, patch)` | `consteval bool` | Compile-time version guard |

### Error Traits (`eph::core::`)

| Symbol | Kind | Description |
|--------|------|-------------|
| `ErrorEnum<E>` | concept | Satisfied when `E` is an enum with ADL-visible `error_name(E) -> string_view` |
| `ErrorEnumFormatter<E>` | struct | `std::formatter` base class -- one-liner formatter registration for any `ErrorEnum` |

### Framing (`eph::net::`)

| Symbol | Kind | Description |
|--------|------|-------------|
| `MessageFramer<F>` | concept | Requires `encode()`, `decode()`, `max_overhead()` for pluggable wire framing |
| `FrameError` | enum | `kIncomplete`, `kInvalidFormat`, `kPayloadTooLarge` |
| `frame_error_name(FrameError)` | function | Human-readable name for a `FrameError` value |
| `DecodedFrame` | struct | Zero-copy decoded frame: `payload`, `payload_len`, `msg_type`, `is_control`, `total_len` |
| `LengthPrefixFramer` | class | 2-byte big-endian length-prefix framer satisfying `MessageFramer` |
| `LengthPrefixFramer::max_overhead()` | static constexpr | Always returns 2 |
| `LengthPrefixFramer::kMaxPayloadLen` | static constexpr | 65535 (max uint16_t) |
| `LengthPrefixFramer::encode(out, data, len, msg_type)` | method | Encode payload with length prefix. Returns bytes written (0 on failure). |
| `LengthPrefixFramer::decode(data, len)` | method | Decode framed message. Returns `expected<DecodedFrame, FrameError>`. |

### TCP Transport (`eph::net::`)

| Symbol | Kind | Description |
|--------|------|-------------|
| `TcpTransport<T>` | concept | Interface for TCP backends: `connect`, `send`, `poll_rx`, `close`, `reset`, `mss`, `state`, `is_established`, `last_rx_burst_tsc` |
| `TcpState` | enum | RFC 793 active-open states: `Closed`, `SynSent`, `Established`, `FinWait1`, `FinWait2`, `Closing`, `TimeWait`, `CloseWait`, `LastAck` |
| `tcp_state_name(TcpState)` | function | RFC 793 name as C string (e.g., `"ESTABLISHED"`) |

### Transport Errors (`eph::net::`)

| Symbol | Kind | Description |
|--------|------|-------------|
| `SendError` | enum | `kOk`, `kMessageTooLarge`, `kNotConnected`, `kQueueFull`, `kInvalidUtf8`, `kInvalidCloseCode`, `kNullData`, `kEncryptFailed`, `kTcpSendFailed` |
| `send_error_name(SendError)` | function | Human-readable name for a `SendError` value |
| `operator!(SendError)` | function | Returns `true` when error occurred (non-kOk), enabling `if (!send(...))` idiom |
| `ConnectionError` | enum | `kInvalidConfig`, `kFactoryFailed`, `kTcpNotEstablished`, `kTlsSessionFailed`, `kTlsHandshakeFailed`, `kTlsKeyExportFailed`, `kWsUpgradeFailed`, `kWsUpgradeRejected`, `kWsAcceptInvalid` |
| `connection_error_name(ConnectionError)` | function | Human-readable name for a `ConnectionError` value |
| `ConnectionErrorInfo` | struct | Structured error: `code` (ConnectionError), `detail` (string), `http_status` (optional int) |
| `ConnectionErrorInfo::message()` | method | `"[CATEGORY] detail"` formatted string |
| `ConnectionErrorInfo::to_json()` | method | JSON-formatted error for monitoring integration |

### Metrics (`eph::core::`)

| Symbol | Kind | Description |
|--------|------|-------------|
| `MetricsSink<T>` | concept | Requires `push_counter`, `push_gauge`, `push_histogram`, `flush` |
| `MetricTag` | struct | Key-value pair (`string_view key`, `string_view value`) for dimensional labeling |
| `NullSink` | struct | Zero-cost no-op sink -- all methods compile to nothing at -O2 |
| `NullSink::push_counter(name, int64_t, tags)` | method | No-op counter |
| `NullSink::push_gauge(name, double, tags)` | method | No-op gauge |
| `NullSink::push_histogram(name, double, tags)` | method | No-op histogram |
| `NullSink::flush()` | method | No-op flush |

### Number Parsing (`eph::core::`)

| Symbol | Kind | Description |
|--------|------|-------------|
| `parse_number(string_view)` | function | Parse decimal string as `optional<double>`. Supports sign, integer, fraction, scientific notation. Zero-allocation. |
| `parse_int(string_view)` | function | Parse decimal string as `optional<int64_t>`. Overflow-safe for full int64 range including INT64_MIN. Zero-allocation. |

### JSON Escape (`eph::core::detail::`)

| Symbol | Kind | Description |
|--------|------|-------------|
| `json_escape(string_view)` | function | RFC 8259 string escaping. Fast path avoids allocation when no escaping needed. |

## Dependencies

- **spdlog** -- Logging (used by `LengthPrefixFramer`)

No dependencies on other eph-\* subprojects. eph-core is the leaf dependency of the eph ecosystem.

## Usage Examples

### Number Parsing

```cpp
#include <eph/core/parse_number.hpp>

// Parse exchange price strings (zero-allocation)
auto price = eph::core::parse_number("50123.45");   // -> optional<double>(50123.45)
auto sci   = eph::core::parse_number("1.5e10");      // -> optional<double>(1.5e10)
auto qty   = eph::core::parse_int("1000");            // -> optional<int64_t>(1000)
auto neg   = eph::core::parse_int("-9223372036854775808"); // INT64_MIN, no overflow

// Rejects malformed input
auto bad1 = eph::core::parse_number("");        // -> nullopt
auto bad2 = eph::core::parse_number("1.");      // -> nullopt (bare dot)
auto bad3 = eph::core::parse_int("99999999999999999999"); // -> nullopt (overflow)
```

### Custom Error Enums

```cpp
#include <eph/core/error_traits.hpp>
#include <format>

// Define a custom error enum satisfying ErrorEnum
enum class MyError : uint8_t { kBadInput, kTimeout };
constexpr std::string_view error_name(MyError e) noexcept {
    return e == MyError::kBadInput ? "BAD_INPUT" : "TIMEOUT";
}

// One-liner formatter registration
template <> struct std::formatter<MyError>
    : eph::core::ErrorEnumFormatter<MyError> {};

// Now usable with std::format
auto msg = std::format("failed: {}", MyError::kTimeout); // "failed: TIMEOUT"
```

### Metrics Sink

```cpp
#include <eph/core/metrics_concept.hpp>

// Use NullSink for zero-cost metrics in production hot paths
eph::core::NullSink sink;
sink.push_counter("rx_packets", 42);  // compiles to nothing at -O2

// Implement a custom sink
struct MySink {
    void push_counter(std::string_view name, int64_t val,
                      std::span<const eph::core::MetricTag> tags = {}) noexcept { /* ... */ }
    void push_gauge(std::string_view name, double val,
                    std::span<const eph::core::MetricTag> tags = {}) noexcept { /* ... */ }
    void push_histogram(std::string_view name, double val,
                        std::span<const eph::core::MetricTag> tags = {}) noexcept { /* ... */ }
    void flush() noexcept { /* ... */ }
};
static_assert(eph::core::MetricsSink<MySink>);

// Dimensional tagging
eph::core::MetricTag tags[] = {{"transport", "dpdk"}, {"symbol", "btcusdt"}};
sink.push_counter("tx_packets", 100, tags);
```

### Length-Prefix Framing

```cpp
#include <eph/core/length_prefix_framer.hpp>

eph::net::LengthPrefixFramer framer;

// Encode: payload -> [2-byte BE length][payload]
uint8_t payload[] = {'A', 0x01, 0x02};  // 'A' = ITCH message type
uint8_t wire[256];
size_t n = framer.encode(wire, payload, sizeof(payload), 0);
// wire = [0x00, 0x03, 'A', 0x01, 0x02], n = 5

// Decode: wire bytes -> DecodedFrame
auto result = framer.decode(wire, n);
if (result) {
    auto& frame = *result;
    // frame.msg_type    == 'A'  (first payload byte)
    // frame.payload_len == 3
    // frame.total_len   == 5    (2-byte header + 3-byte payload)
}
```

### Version Checking

```cpp
#include <eph/version.hpp>

// Compile-time version guard
static_assert(eph::version_at_least(1, 0, 0), "Requires ephemeral >= 1.0.0");

// Runtime version string for logging
spdlog::info("starting {}", eph::kVersionFull);  // "ephemeral/1.0.0"
```

### Transport Error Handling

```cpp
#include <eph/core/transport_errors.hpp>

// SendError with ! operator for natural failure checks
eph::net::SendError err = /* from transport.send() */;
if (!err) {
    spdlog::error("send failed: {}", err);  // uses ErrorEnumFormatter
}

// Structured connection errors with JSON serialization
eph::net::ConnectionErrorInfo info{
    .code = eph::net::ConnectionError::kWsUpgradeRejected,
    .detail = "server returned 403",
    .http_status = 403,
};
spdlog::error("{}", info);        // "[WS_UPGRADE_REJECTED] server returned 403"
auto json = info.to_json();       // {"code":"WS_UPGRADE_REJECTED","detail":"server returned 403","http_status":403}
```
