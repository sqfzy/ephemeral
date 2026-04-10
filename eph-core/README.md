# eph-core

Header-only C++23 foundation for the `eph` ecosystem. Provides the v3.3 error types,
the `StreamCodec` / `DatagramCodec` concepts, `OutputBuffer`, the `PacketView`
contract, plus a handful of utility primitives (number parsers, base64, JSON
escaping, string checks). No networking dependencies — `eph-core` is the leaf of the
dependency graph.

## What lives here

### v3.3 core types (`eph/core/`)

| Header | Contents |
|---|---|
| `error.hpp` | `enum class Error` + `struct ErrorInfo { Error code; const char* detail; }`. The unified v3.3 error type. |
| `codec.hpp` | `concept StreamCodec` / `concept DatagramCodec` / `concept Codec`, plus the `OutputBuffer` class for codec auto-responses. |
| `packet_view.hpp` | The `PacketView` contract definition — `writable_data()` / `data()` / `length()` / `trim_front()` / `trim_back()` / `arrival_tsc()`. |
| `tcp_state.hpp` | `enum class TcpState` (RFC 793) + `tcp_state_name()`. Shared by kernel and DPDK backends. |
| `error_traits.hpp` | `ErrorEnum<E>` concept + `ErrorEnumFormatter<E>` one-liner `std::formatter` base. |
| `metrics_concept.hpp` | `MetricsSink<T>` concept + `NullSink` (zero-cost). |

### Legacy primitives still in use (`eph/core/`)

These predate the v3.3 refactor and are still consumed by the parser modules
(`eph-fix`, `eph-itch`, `eph-json`). They remain here because extracting them would
be disruptive and offers no architectural win.

| Header | Consumers |
|---|---|
| `framer_concept.hpp` | `eph-fix`, `eph-itch`, `eph-json` framers |
| `length_prefix_framer.hpp` | `eph-itch/framer.hpp` |
| `parse_number.hpp` | `eph-json`, `eph-fix`, `eph-book` |
| `tcp_concept.hpp` | internal detail of `eph-net-dpdk` (legacy TCP session layer that the v3.3 `DpdkTcpStream` wraps) |
| `detail/json_escape.hpp` | `eph-json` serialisation |
| `detail/base64.hpp` | `eph-net` WebSocket handshake |
| `detail/string_checks.hpp` | hostname / path validation |

### Version metadata (`eph/version.hpp`)

`kVersionMajor` / `kVersionMinor` / `kVersionPatch` / `kVersionString` /
`kVersionFull` / `version_at_least(…)`. Pure compile-time constants.

## Building and testing

`eph-core` is header-only — the target is a no-op except for a C++23 feature check.

```bash
xmake build eph-core
xmake build -g tests      # builds test_codec_concept, test_error, etc.
xmake run test_error
xmake run test_codec_concept
```

Tests are auto-globbed from `tests/test_*.cpp`.

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

- `spdlog` (public) — logging; `SPDLOG_ACTIVE_LEVEL` is set by the root `xmake.lua`.

No dependencies on any other `eph-*` module. Parser modules and networking modules
depend on `eph-core`.

## See also

- `docs/architecture.md` — the v3.3 concept model
- `docs/custom-codec.md` — writing a new codec
- `.artifacts/design-eph-v3.3-architecture-20260410.md` — design spec
