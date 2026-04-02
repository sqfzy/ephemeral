# eph-core

Header-only C++23 foundation library for the eph ecosystem. Provides shared concepts, error types, number parsing, message framing, and metrics interfaces used across eph-net, eph-json, eph-fix, eph-itch, eph-book, and eph-dpdk.

## Key Components

All headers are under `include/eph/core/`:

- **error_traits.hpp** -- `ErrorEnum` concept and `ErrorEnumFormatter` base. Any enum with an ADL-visible `error_name(E) -> string_view` satisfies `ErrorEnum`. The formatter eliminates per-enum `std::formatter` boilerplate (one-liner per type). Backward-compatible aliases are provided in `eph::net::`.
- **framer_concept.hpp** -- `MessageFramer` concept defining pluggable wire-format framing for Transport. Requires `encode()`, `decode()`, and `max_overhead()`. Also defines `FrameError` (incomplete/invalid/too-large) and `DecodedFrame` (zero-copy view into the receive buffer with payload, msg_type, and control-frame flag).
- **length_prefix_framer.hpp** -- `LengthPrefixFramer` implementing `MessageFramer` with a 2-byte big-endian length prefix. Wire format: `[uint16_t length][payload]`. First payload byte is exposed as `msg_type` in `DecodedFrame`, making it suitable for ITCH and similar binary protocols. Max payload 65535 bytes.
- **tcp_concept.hpp** -- `TcpTransport` concept defining the interface for TCP backends (DPDK, io_uring, kernel sockets, loopback). Covers connection lifecycle (`connect`, `close`, `reset`), data transfer (`send`, `poll_rx`), timestamping (`last_rx_burst_tsc`), and state queries (`state`, `mss`, `is_established`). Also defines `TcpState` enum following RFC 793 active-open states.
- **transport_errors.hpp** -- Transport-layer error types extracted to eph-core so downstream modules avoid pulling in eph-net's TLS/WebSocket dependencies. Includes `SendError` (ok/too-large/not-connected/queue-full/etc.), `ConnectionError` (invalid-config/factory-failed/tls-handshake-failed/ws-upgrade-rejected/etc.), and `ConnectionErrorInfo` (structured error with code, detail string, optional HTTP status, and JSON serialization).
- **metrics_concept.hpp** -- `MetricsSink` concept for pushing metrics to external monitoring systems. Three metric types following Prometheus conventions: counter (monotonic int64), gauge (point-in-time double), histogram (distribution sample). Dimensional tagging via `MetricTag` key-value pairs. Includes `NullSink` -- a zero-cost no-op sink that compiles to nothing at -O2.
- **parse_number.hpp** -- Fast zero-allocation decimal string-to-number parsers. `parse_number(string_view) -> optional<double>` handles sign, integer, fraction, and scientific notation. `parse_int(string_view) -> optional<int64_t>` handles sign and integer with overflow-safe full int64 range (including INT64_MIN). Rejects NaN, infinity, empty strings, bare dots, and overflow.
- **detail/json_escape.hpp** -- RFC 8259 JSON string escaping utility. Handles `\"`, `\\`, control characters, and valid UTF-8 pass-through. Invalid UTF-8 bytes are escaped as `\uXXXX`. Fast path skips allocation when no escaping is needed.

## Dependencies

- **spdlog** -- Logging (used by `LengthPrefixFramer`)

No dependencies on other eph-* subprojects. This is the leaf dependency of the eph ecosystem.

## Quick Start

```cpp
#include <eph/core/parse_number.hpp>
#include <eph/core/error_traits.hpp>
#include <eph/core/metrics_concept.hpp>

// Parse exchange price strings (zero-allocation)
auto price = eph::core::parse_number("50123.45");   // -> optional<double>(50123.45)
auto qty   = eph::core::parse_int("1000");           // -> optional<int64_t>(1000)

// Define a custom error enum satisfying ErrorEnum
enum class MyError : uint8_t { kBadInput, kTimeout };
constexpr std::string_view error_name(MyError e) noexcept {
    return e == MyError::kBadInput ? "BAD_INPUT" : "TIMEOUT";
}

// One-liner formatter registration
template <> struct std::formatter<MyError>
    : eph::core::ErrorEnumFormatter<MyError> {};

// Use NullSink for zero-cost metrics in production hot paths
eph::core::NullSink sink;
sink.push_counter("rx_packets", 42);  // compiles to nothing at -O2
```
