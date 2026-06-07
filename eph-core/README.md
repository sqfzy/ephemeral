# eph-core

Header-only C++23 foundation for the `eph` ecosystem. Provides the error types,
the `StreamCodec` / `DatagramCodec` concepts, `OutputBuffer`, the `PacketView`
contract, the `MetricsSink` concept, plus a handful of utility primitives
(number parsers, base64, JSON escaping, string checks, a named-logger factory).
No networking dependencies — `eph-core` is the leaf of the dependency graph.

## What lives here

### Core types (`eph/core/`)

| Header | Contents |
|---|---|
| `error.hpp` | `enum class Error` + `struct ErrorInfo { Error code; const char* detail; }`. The unified error type, plus `error_name()`, `operator<<`, `format_as()`, and `std::formatter` specialisations for both `Error` and `ErrorInfo`. |
| `codec.hpp` | `concept StreamCodec` / `concept DatagramCodec` / `concept Codec`, plus the `OutputBuffer` class for codec auto-responses (span-based `append`, zero-copy `writable_tail` + `commit`). |
| `packet_view.hpp` | The formal `concept PacketView` — `writable_data()` / `data()` / `length()` / `trim_front()` / `trim_back()`. Downstream backends `static_assert(PacketView<T>)` to verify conformance at compile time. |
| `tcp_state.hpp` | `enum class TcpState` (RFC 793) + `tcp_state_name()`. Shared by kernel and DPDK backends (single definition, no ODR conflict). |
| `error_traits.hpp` | `ErrorEnum<E>` concept + `ErrorEnumFormatter<E>` one-liner `std::formatter` base. Also exports `eph::net::ErrorEnum` / `eph::net::ErrorEnumFormatter` as backward-compat aliases. |
| `metrics_concept.hpp` | `MetricTag`, `concept MetricsSink`, and `NullSink` (zero-cost, all methods no-op). |

### Version metadata (`eph/version.hpp`)

`kVersionMajor` / `kVersionMinor` / `kVersionPatch` / `kVersion` (packed int) /
`kVersionString` (`"M.m.p"`) / `kVersionFull` (`"ephemeral/M.m.p"`) /
`version_at_least(…)` (consteval). Pure compile-time constants; no runtime
globals.

### Legacy primitives still in use (`eph/core/`)

These are legacy primitives still consumed by the parser modules
(`eph-fix`, `eph-itch`, `eph-json`) and by the internal DPDK TCP session. They
remain here because extracting them would be disruptive and offers no
architectural win.

| Header | Consumers |
|---|---|
| `framer_concept.hpp` | `eph-fix`, `eph-itch`, `eph-json` framers |
| `length_prefix_framer.hpp` | `eph-itch/framer.hpp` |
| `parse_number.hpp` | `eph-json`, `eph-fix`, `eph-book` |
| `tcp_concept.hpp` | internal detail of `eph-net-dpdk` (legacy `TcpSession` layer that `DpdkTcpStream` wraps) |
| `detail/json_escape.hpp` | `eph-json` serialisation |
| `detail/base64.hpp` | `eph-net` WebSocket handshake, HTTP CONNECT Basic auth |
| `detail/string_checks.hpp` | hostname / path validation |
| `detail/logger.hpp` | `make_logger()` — lazy, thread-safe spdlog factory used by every downstream `eph-*` module |

## Building and testing

`eph-core` is header-only — the target is a no-op except for a C++23 feature
check (`std::expected` + `std::format`).

```bash
xmake build eph-core
xmake build -g tests      # builds test_codec_concept, test_error, etc.
xmake run test_error
xmake run test_codec_concept
xmake run test_parse_number
xmake run test_length_prefix_framer
```

Tests and benchmarks are auto-globbed from `tests/test_*.cpp` and
`benchmarks/bench_*.cpp`.

## Usage example

```cpp
#include <eph/core/codec.hpp>
#include <eph/core/error.hpp>

// Every codec must satisfy one of these:
static_assert(eph::core::StreamCodec<MyCodec>);
// or:
static_assert(eph::core::DatagramCodec<MyCodec>);

// Fallible APIs return expected<T, ErrorInfo>:
std::expected<size_t, eph::core::ErrorInfo> my_op() {
    if (oops) {
        return std::unexpected{eph::core::ErrorInfo{
            eph::core::Error::CodecBad, "short read"}};
    }
    return 42;
}
```

## Dependencies

- `spdlog` (public) — logging; `SPDLOG_ACTIVE_LEVEL` is set by the root
  `xmake.lua` and re-exported as a public define.

No dependencies on any other `eph-*` module. Parser modules and networking
modules depend on `eph-core`.

## See also

- `docs/architecture.md` — the concept model
- `docs/custom-codec.md` — writing a new codec
- `.artifacts/design-eph-v3.3-architecture-20260410.md` — design spec
