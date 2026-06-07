# eph-core summary

## Public API surface

### `eph::core` — errors

```cpp
enum class Error : uint8_t {
    Ok,
    // Connection lifecycle
    ConnectFailed, Disconnected, Timeout, NotAttached,
    // TLS
    TlsHandshakeFailed, TlsRecordBad, TlsCipherFailed,
    // WebSocket
    WsHandshakeFailed, WsFrameBad, WsCloseReceived,
    // Codec / application protocol
    CodecBad, CodecOverflow,
    // I/O
    WouldBlock, BufferFull,
    // Internal
    InvalidConfig, OutOfMemory,
    // HTTP CONNECT proxy
    ProxyConnectFailed, ProxyHandshakeFailed, ProxyAuthRequired,
    // Registry / lookup lifecycle
    NotFound,
};

struct ErrorInfo {
    Error       code;
    const char* detail;            // static storage, never dangles
    // constexpr constructor, defaulted operator==
};

constexpr const char* error_name(Error) noexcept;

// Rendering: three parallel paths kept in lockstep —
//   * std::formatter<Error> / std::formatter<ErrorInfo>  (std::format path)
//   * format_as(Error) / format_as(ErrorInfo)            (spdlog bundled-fmt ADL)
//   * operator<<(ostream&, ErrorInfo const&)             (gtest / ostream path)
```

### `eph::core` — codec concepts

```cpp
class OutputBuffer {
public:
    constexpr OutputBuffer(uint8_t* base, std::size_t cap) noexcept;

    [[nodiscard]] std::expected<void, ErrorInfo>
        append(std::span<const uint8_t> src) noexcept;
    [[nodiscard]] std::expected<void, ErrorInfo>
        reserve(std::size_t n) noexcept;

    [[nodiscard]] uint8_t* writable_tail(std::size_t n) noexcept;
    constexpr void         commit(std::size_t n) noexcept;

    [[nodiscard]] constexpr std::size_t    available() const noexcept;
    [[nodiscard]] constexpr std::size_t    size()      const noexcept;
    [[nodiscard]] constexpr const uint8_t* data()      const noexcept;
};

template <class T>
concept StreamCodec = /* decode(view, out_sink) returning
                        expected<optional<Frame>, ErrorInfo>,
                        encode(out_buf, out_cap, Frame) returning
                        expected<size_t, ErrorInfo>,
                        T::Frame / T::PacketViewRef / T::max_overhead /
                        T::is_streaming */;

template <class T>
concept DatagramCodec = /* decode(dgram, out_sink, sink_fn) returning
                          expected<size_t, ErrorInfo>,
                          encode(...), same associated types */;

template <class T>
concept Codec = StreamCodec<T> || DatagramCodec<T>;
```

### `eph::core` — PacketView contract

Every Stream / Datagram implementation exposes a `PacketView` associated type
conforming to the contract below. The concept is formalised in
`include/eph/core/packet_view.hpp` — downstream code can
`static_assert(eph::core::PacketView<T>)` to get a compile-time guarantee.

```cpp
uint8_t*       writable_data() noexcept;
const uint8_t* data()          const noexcept;
size_t         length()        const noexcept;
void           trim_front(size_t n) noexcept;
void           trim_back (size_t n) noexcept;

template <class T>
concept PacketView = /* the five members above, duck-typed noexcept */;
```

Implementations in the repo: `eph::codec::SpanPacketView` (tests),
`eph::net::kernel::detail::SpanView` (kernel reassembly), and
`eph::net::dpdk::detail::MbufView` (DPDK mbuf). The DPDK one is the zero-copy
path — `writable_data()` points straight into the NIC-DMA'd mbuf so TLS
decrypts in place.

### `eph::net` — TcpState (shared)

```cpp
enum class TcpState : uint8_t {
    Closed, Listen, SynSent, SynReceived, Established,
    FinWait1, FinWait2, CloseWait, Closing, LastAck, TimeWait,
};
constexpr const char* tcp_state_name(TcpState) noexcept;
```

Defined in `eph/core/tcp_state.hpp` — both `eph-net-kernel` and `eph-net-dpdk`
share this enum to avoid ODR conflicts. `eph/core/tcp_concept.hpp` forwards to
this header and adds the `TcpTransport` concept plus a
`std::formatter<TcpState>` specialisation.

### `eph::core` — metrics

```cpp
struct MetricTag {
    std::string_view key;
    std::string_view value;
};

template <class T>
concept MetricsSink = requires(T& sink,
    std::string_view name, std::span<const MetricTag> tags) {
    { sink.push_counter  (name, int64_t{}, tags) } noexcept -> std::same_as<void>;
    { sink.push_gauge    (name, double{},  tags) } noexcept -> std::same_as<void>;
    { sink.push_histogram(name, double{},  tags) } noexcept -> std::same_as<void>;
    { sink.flush()                               } noexcept -> std::same_as<void>;
};

struct NullSink { /* all methods are inline noexcept no-ops */ };
static_assert(MetricsSink<NullSink>);
```

For a logging sink (`eph::utils::ConsoleSink`), see `eph-utils`. For the net
backends' own counters, see `eph::net::StreamMetric` / `publish_metrics` in
`eph-net`.

### `eph::core` — error_traits

```cpp
template <class E>
concept ErrorEnum = std::is_enum_v<E> && requires(E e) {
    { error_name(e) } noexcept -> std::convertible_to<std::string_view>;
};

template <ErrorEnum E>
struct ErrorEnumFormatter : std::formatter<std::string_view> { /* one-liner */ };

// Backward-compat aliases so pre-refactor call sites keep compiling:
namespace eph::net {
    template <class E> concept ErrorEnum = eph::core::ErrorEnum<E>;
    template <eph::core::ErrorEnum E>
        using ErrorEnumFormatter = eph::core::ErrorEnumFormatter<E>;
}
```

Any parser-module error enum (`FrameError`, `FixError`, `ItchError`, `JsonError`,
`BookError`) that provides an ADL `error_name()` automatically satisfies this —
a one-liner `std::formatter` specialisation is then enough to format it.

### `eph::core` — legacy framer primitives (still used by parser modules)

- `MessageFramer<F>` concept — `encode` / `decode` / static `max_overhead()`.
- `DecodedFrame` — zero-copy view with `payload`, `payload_len`, `msg_type`,
  `is_control`, `total_len`.
- `FrameError` — `kIncomplete` / `kInvalidFormat` / `kPayloadTooLarge`.
- `LengthPrefixFramer` — 2-byte BE length prefix, 1..65 535 byte payloads.
  Zero-length payloads are rejected because byte 0 is exposed as `msg_type`.

These legacy primitives remain here for `eph-fix` / `eph-itch` / `eph-json`.
`eph::codec::LengthPrefixCodec` is the current implementation under the
`StreamCodec` concept.

### `eph::net` — legacy TcpTransport concept

`eph/core/tcp_concept.hpp` defines `concept TcpTransport` — the contract the
internal DPDK `TcpSession` layer satisfies (`connect` / `send` / `poll_rx` /
`close` / `reset` / `mss` / `state` / `is_established` / `last_rx_burst_tsc`).
Public code uses `DpdkTcpStream<Codec>` which wraps a `TcpSession`; callers
never see this concept directly.

### `eph::core` — number parsing

- `parse_number(string_view) -> optional<double>` — full IEEE 754 including
  scientific notation. Rejects NaN, infinity, bare dot, bare exponent,
  trailing garbage.
- `parse_int(string_view) -> optional<int64_t>` — full int64 range including
  `INT64_MIN`. Uses unsigned arithmetic internally to avoid signed-overflow UB.

### `eph::core::detail`

- `json_escape(string_view) -> string` — RFC 8259 §7; valid UTF-8 passes
  through, invalid continuation bytes are `\uXXXX`-escaped.
- `base64_encode(uint8_t*, size_t) -> string` — RFC 4648 with `=` padding.
  Pure C++, no OpenSSL.
- `contains_control_chars(string_view) -> bool` — constexpr; used for
  hostname / header / path validation.
- `make_logger(const char* name) -> spdlog::logger*` — lazy, thread-safe
  named-logger factory used by `length_prefix_framer` and by every downstream
  `eph-*` module that needs a named logger.

### `eph::` — version metadata

- `kVersionMajor`, `kVersionMinor`, `kVersionPatch` — the three components.
- `kVersion` — packed integer `major*10000 + minor*100 + patch` for
  compile-time comparison (requires minor / patch < 100).
- `kVersionString` — `"M.m.p"` (string_view over compile-time storage).
- `kVersionFull`   — `"ephemeral/M.m.p"`.
- `version_at_least(major, minor, patch)` — consteval feature gate.

## Dependencies

- `spdlog` (public)

No dependencies on any other `eph-*` module.
