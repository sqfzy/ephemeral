# eph-core

Header-only C++23 foundation library for the `eph` ecosystem. Provides the shared concepts, error types, number parsers, message-framing primitives, metrics interfaces, and transport abstractions used across `eph-net`, `eph-transport`, `eph-json`, `eph-fix`, `eph-itch`, `eph-book`, and `eph-dpdk`.

`eph-core` is the **leaf dependency** of the ecosystem — no other `eph-*` subproject is required. Its only external dependency is [spdlog](https://github.com/gabime/spdlog) for logging.

## Features

- **Version metadata** — compile-time packed version, `"M.m.p"` / `"ephemeral/M.m.p"` strings, and a `consteval version_at_least()` guard.
- **Generic error-name infrastructure** — `ErrorEnum` concept + `ErrorEnumFormatter` one-liner `std::formatter` base, so every error enum gets `std::format` support with no boilerplate.
- **Pluggable wire framing** — `MessageFramer` concept, `DecodedFrame` zero-copy view, `FrameError` codes, and a production-ready `LengthPrefixFramer` (2-byte big-endian length prefix, payload max 65 535 bytes).
- **TCP transport concept** — `TcpTransport<T>` concept defining the contract every backend (DPDK, io_uring, kernel sockets, loopback) must satisfy, plus a `TcpState` enum following RFC 793.
- **Transport error types** — `SendError`, `ConnectionError`, and the structured `ConnectionErrorInfo` (with JSON serialization) extracted here so downstream modules avoid pulling in TLS/WebSocket dependencies.
- **Metrics sink concept** — `MetricsSink` (counter/gauge/histogram + tags) with a zero-cost `NullSink` implementation that compiles to nothing at `-O2`.
- **Fast number parsing** — zero-allocation `parse_number` (double, incl. scientific notation) and `parse_int` (full int64 range, incl. `INT64_MIN`), used by JSON/FIX/book adapters.
- **Shared detail helpers** — `json_escape` (RFC 8259 §7), `base64_encode` (RFC 4648), and `contains_control_chars` for hostname/path validation.
- **`FakeTcpTransport`** — programmable TcpTransport mock for deterministic unit testing.

## Quick Start

### Prerequisites

- **Compiler**: GCC ≥ 13 or Clang ≥ 17 (C++23 `std::expected` / `std::format` required).
  On Amazon Linux 2023: `sudo dnf install gcc14-g++ && export EPH_USE_GCC14=1`.
- **Build system**: [xmake](https://xmake.io/)
- **spdlog**: pulled in automatically by xmake as a required package at the monorepo root.

### Build

This subproject is built from the parent `ephemeral_dev` workspace. From the monorepo root:

```bash
# Build the header-only target
xmake build eph-core

# Build all eph-core tests
xmake build test_parse_number test_length_prefix_framer test_json_escape \
            test_base64 test_transport_errors test_fake_tcp_transport \
            test_string_checks
```

Because `eph-core` is header-only, `xmake build eph-core` is effectively a no-op (it just verifies C++23 support via an `on_config` snippet). The real compilation happens when downstream targets include its headers.

### Test

```bash
# Run all tests that belong to this subproject
xmake run test_parse_number
xmake run test_length_prefix_framer
xmake run test_json_escape
xmake run test_base64
xmake run test_transport_errors
xmake run test_fake_tcp_transport
xmake run test_string_checks
```

### Benchmark

```bash
xmake run bench_parse_number
```

## Project Structure

```
eph-core/
├── include/eph/
│   ├── version.hpp                     # compile-time version constants
│   └── core/
│       ├── error_traits.hpp            # ErrorEnum concept + ErrorEnumFormatter
│       ├── framer_concept.hpp          # MessageFramer, DecodedFrame, FrameError
│       ├── length_prefix_framer.hpp    # 2-byte big-endian length framer
│       ├── tcp_concept.hpp             # TcpTransport concept + TcpState
│       ├── transport_errors.hpp        # SendError, ConnectionError(Info)
│       ├── metrics_concept.hpp         # MetricsSink concept + NullSink
│       ├── parse_number.hpp            # parse_number / parse_int
│       ├── fake_tcp_transport.hpp      # FakeTcpTransport test double
│       └── detail/
│           ├── base64.hpp              # base64_encode (RFC 4648)
│           ├── json_escape.hpp         # json_escape (RFC 8259 §7)
│           └── string_checks.hpp       # contains_control_chars
├── tests/                              # gtest-based unit tests (one per header)
├── benchmarks/                         # Google Benchmark microbenchmarks
├── fuzzers/                            # libFuzzer harnesses
└── xmake.lua                           # build rules (header-only target)
```

## Public API Reference

All symbols are header-only and live under `eph::`, `eph::core::`, `eph::net::`, `eph::core::detail::`, or `eph::net::testing::`. See the Doxygen comments in each header for parameter semantics and error conditions.

### `eph::` — version metadata (`eph/version.hpp`)

| Symbol | Kind | Description |
|---|---|---|
| `kVersionMajor` / `kVersionMinor` / `kVersionPatch` | `constexpr int` | Individual version components. |
| `kVersion` | `constexpr int` | Packed `major*10000 + minor*100 + patch`. |
| `kVersionString` | `constexpr string_view` | `"M.m.p"` (built at compile time). |
| `kVersionFull` | `constexpr string_view` | `"ephemeral/M.m.p"` — for User-Agent headers and log preambles. |
| `version_at_least(major, minor, patch)` | `consteval bool` | Compile-time feature-gate guard for downstream code. |

### `eph::core::` — error traits (`error_traits.hpp`)

| Symbol | Kind | Description |
|---|---|---|
| `ErrorEnum<E>` | concept | Satisfied when `E` is an enum with an ADL-visible `error_name(E) -> string_view`. |
| `ErrorEnumFormatter<E>` | struct | `std::formatter<string_view>`-derived base that formats the enum as its `error_name()`. |

Backward-compatible aliases `eph::net::ErrorEnum` and `eph::net::ErrorEnumFormatter` are provided (slated for removal in a future major version).

### `eph::net::` — framing (`framer_concept.hpp`, `length_prefix_framer.hpp`)

| Symbol | Kind | Description |
|---|---|---|
| `MessageFramer<F>` | concept | Requires `encode()`, `decode()`, `max_overhead()`. `decode()` is an instance method so stateful framers can keep per-connection context. |
| `FrameError` | enum | `kIncomplete`, `kInvalidFormat`, `kPayloadTooLarge`. |
| `frame_error_name(FrameError)` | function | Human-readable name. |
| `DecodedFrame` | struct | Zero-copy view: `payload`, `payload_len`, `msg_type`, `is_control`, `total_len`. `payload` is valid only until the next `decode()` or buffer reuse. |
| `LengthPrefixFramer` | class | 2-byte big-endian length prefix; satisfies `MessageFramer`. |
| `LengthPrefixFramer::kMaxPayloadLen` | `static constexpr size_t` | `65535`. |
| `LengthPrefixFramer::max_overhead()` | `static constexpr` | Always `2`. |
| `LengthPrefixFramer::encode(out, data, len, msg_type)` | method | Writes `[len hi][len lo][payload]`. Returns `2 + len`, or `0` on invalid args. Rejects empty payloads (msg_type is derived from `payload[0]`). |
| `LengthPrefixFramer::decode(data, len)` | method | Returns `std::expected<DecodedFrame, FrameError>`. `msg_type` is set to `payload[0]`. |

### `eph::net::` — TCP transport (`tcp_concept.hpp`, `fake_tcp_transport.hpp`)

| Symbol | Kind | Description |
|---|---|---|
| `TcpTransport<T>` | concept | Required methods: `connect(timeout)`, `close()`, `reset()` noexcept, `send(data, len)`, `poll_rx(callback)`, `last_rx_burst_tsc()`, `mss()`, `state()`, `is_established()`. |
| `TcpState` | enum | RFC 793 active-open states: `Closed`, `SynSent`, `Established`, `FinWait1`, `FinWait2`, `Closing`, `TimeWait`, `CloseWait`, `LastAck`. |
| `tcp_state_name(TcpState)` | function | RFC 793 name as a C string (`"ESTABLISHED"`, etc.). |
| `std::formatter<TcpState>` | specialization | Formats as the RFC 793 name. |
| `testing::FakeTcpTransport` | class | Programmable mock satisfying `TcpTransport`. Stage RX data (`inject_rx`), stage failures (`set_connect_error` / `set_send_error` / `set_rx_error`), inspect TX data (`sent_data()`), simulate disconnect (`inject_disconnect()`). Single-threaded. |

### `eph::net::` — transport errors (`transport_errors.hpp`)

| Symbol | Kind | Description |
|---|---|---|
| `SendError` | enum `int8_t` | `kOk=0`, `kMessageTooLarge`, `kNotConnected`, `kQueueFull`, `kInvalidUtf8`, `kInvalidCloseCode`, `kNullData`, `kEncryptFailed`, `kTcpSendFailed`. |
| `send_error_name(SendError)` | function | Human-readable name (`"OK"`, `"QUEUE_FULL"`, …). |
| `operator!(SendError)` | function | `true` when `e != kOk`, enabling the `if (!send(...))` failure idiom. |
| `ConnectionError` | enum `uint8_t` | 15 categories covering config / factory / TCP / TLS / WebSocket / DNS / ARP / proxy / platform failures. |
| `connection_error_name(ConnectionError)` | function | Human-readable name. |
| `ConnectionErrorInfo` | struct | `code` + `detail` string + optional `http_status`. Provides `message()` → `"[CATEGORY] detail"` and `to_json()` → monitoring-friendly JSON (escaped). Defaulted `operator==`. |
| `std::formatter<SendError>` / `std::formatter<ConnectionError>` / `std::formatter<ConnectionErrorInfo>` | specializations | All registered via `ErrorEnumFormatter` or direct override. |

### `eph::core::` — metrics (`metrics_concept.hpp`)

| Symbol | Kind | Description |
|---|---|---|
| `MetricTag` | struct | `{string_view key; string_view value;}` — views into caller-owned storage. |
| `MetricsSink<T>` | concept | Requires `push_counter(name, int64_t, tags)`, `push_gauge(name, double, tags)`, `push_histogram(name, double, tags)`, `flush()`. |
| `NullSink` | struct | All methods are inline noexcept no-ops. Compiles to zero instructions at `-O2`. `static_assert(MetricsSink<NullSink>)`. |

### `eph::core::` — number parsing (`parse_number.hpp`)

| Symbol | Kind | Description |
|---|---|---|
| `parse_number(string_view) -> optional<double>` | function | Sign + integer + fraction + scientific notation. Rejects NaN, infinity, empty strings, bare dots (`"1."`, `"."`), bare exponents (`"1e"`), trailing characters, and overflow beyond IEEE 754 double. |
| `parse_int(string_view) -> optional<int64_t>` | function | Sign + integer, full int64 range including `INT64_MIN`. Uses unsigned arithmetic internally to avoid signed-overflow UB. |

### `eph::core::detail::` — internal helpers

| Symbol | Header | Description |
|---|---|---|
| `json_escape(string_view) -> string` | `detail/json_escape.hpp` | RFC 8259 §7 escaping with UTF-8 pass-through and invalid-byte `\uXXXX` fallback. Fast path: a single scan returns an unmodified copy when no escaping is needed. |
| `base64_encode(const uint8_t*, size_t) -> string` / `base64_encode(const string&)` | `detail/base64.hpp` | RFC 4648 encoder with `=` padding. Standalone (no OpenSSL), intended for small payloads like WebSocket keys and proxy auth tokens. |
| `contains_control_chars(string_view) -> bool` | `detail/string_checks.hpp` | Constexpr check for ASCII control characters (`U+0000`–`U+001F`, `U+007F`). Used to reject inputs that could enable HTTP header injection / request smuggling / log injection. Bytes `≥ 0x80` are *not* flagged (valid UTF-8). |

## Dependencies

- **spdlog** (public) — logging. `SPDLOG_ACTIVE_LEVEL` is set by the monorepo `xmake.lua` (`SPDLOG_LEVEL_TRACE` in debug builds, `SPDLOG_LEVEL_INFO` otherwise).

No dependencies on other `eph-*` subprojects.

## Usage Examples

### Number parsing

```cpp
#include <eph/core/parse_number.hpp>

// Zero-allocation parses of exchange field values.
auto px  = eph::core::parse_number("50123.45");   // optional<double>(50123.45)
auto sci = eph::core::parse_number("1.5e10");      // optional<double>(1.5e10)
auto qty = eph::core::parse_int("1000");           // optional<int64_t>(1000)
auto min = eph::core::parse_int("-9223372036854775808"); // INT64_MIN, no overflow

// Malformed input is rejected.
auto bad1 = eph::core::parse_number("");    // nullopt
auto bad2 = eph::core::parse_number("1.");  // nullopt — bare dot
auto bad3 = eph::core::parse_int("99999999999999999999"); // nullopt — overflow
```

### Custom error enums

```cpp
#include <eph/core/error_traits.hpp>
#include <format>

enum class MyError : uint8_t { kBadInput, kTimeout };
constexpr std::string_view error_name(MyError e) noexcept {
    return e == MyError::kBadInput ? "BAD_INPUT" : "TIMEOUT";
}

// One-liner std::formatter registration.
template <> struct std::formatter<MyError>
    : eph::core::ErrorEnumFormatter<MyError> {};

auto msg = std::format("failed: {}", MyError::kTimeout); // "failed: TIMEOUT"
```

### Length-prefix framing

```cpp
#include <eph/core/length_prefix_framer.hpp>

eph::net::LengthPrefixFramer framer;

// Encode: [len_hi][len_lo][payload...]
uint8_t payload[] = {'A', 0x01, 0x02};   // 'A' = ITCH AddOrder msg type
uint8_t wire[256];
size_t n = framer.encode(wire, payload, sizeof(payload), /*msg_type=*/0);
// wire = {0x00, 0x03, 'A', 0x01, 0x02}, n == 5

// Decode: bytes -> DecodedFrame (zero-copy view into `wire`).
if (auto r = framer.decode(wire, n); r) {
    auto& f = *r;
    // f.msg_type    == 'A'
    // f.payload_len == 3
    // f.total_len   == 5
}
```

### Transport error handling

```cpp
#include <eph/core/transport_errors.hpp>
#include <spdlog/spdlog.h>

auto err = /* from transport.send(...) */;
if (!err) {                        // operator!(SendError) returns true on non-OK
    spdlog::error("send failed: {}", err);  // uses ErrorEnumFormatter
}

eph::net::ConnectionErrorInfo info{
    .code        = eph::net::ConnectionError::kWsUpgradeRejected,
    .detail      = "server returned 403",
    .http_status = 403,
};
spdlog::error("{}", info);       // "[WS_UPGRADE_REJECTED] server returned 403"
auto json = info.to_json();      // {"code":"WS_UPGRADE_REJECTED","detail":"server returned 403","http_status":403}
```

### Unit-testing against `FakeTcpTransport`

```cpp
#include <eph/core/fake_tcp_transport.hpp>

eph::net::testing::FakeTcpTransport tcp;
auto ok = tcp.connect(std::chrono::milliseconds{0});   // immediately Established
assert(tcp.is_established());

// Stage an incoming packet and poll for it.
std::vector<uint8_t> pkt{'h', 'i'};
tcp.inject_rx(std::move(pkt));
tcp.poll_rx([](const uint8_t* data, uint16_t len) { /* ... */ });

// Verify what was sent upstream.
(void)tcp.send("ping", 4);
assert(tcp.sent_data().size() == 1);
```

### Version checks

```cpp
#include <eph/version.hpp>

static_assert(eph::version_at_least(1, 0, 0), "requires ephemeral >= 1.0.0");

spdlog::info("starting {}", eph::kVersionFull);  // "ephemeral/1.0.0"
```

## License

See the repository root `LICENSE` (no separate LICENSE file ships in this subproject).
