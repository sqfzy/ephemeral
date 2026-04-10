# eph-core summary

## Public API surface (v3.3)

### `eph::core` - errors

```cpp
enum class Error : uint8_t {
    Ok, ConnectFailed, Disconnected, Timeout, NotAttached,
    TlsHandshakeFailed, TlsRecordBad, TlsCipherFailed,
    WsHandshakeFailed, WsFrameBad, WsCloseReceived,
    CodecNeedMoreData, CodecBad, CodecOverflow,
    WouldBlock, NoData, BufferFull,
    InvalidConfig, OutOfMemory,
};

struct ErrorInfo {
    Error       code;
    const char* detail;            // static storage, never dangles
};

constexpr const char* error_name(Error) noexcept;
```

### `eph::core` - codec concepts

```cpp
class OutputBuffer {
public:
    std::expected<void, ErrorInfo> append(const uint8_t*, size_t);
    std::expected<void, ErrorInfo> reserve(size_t);
    uint8_t* writable_tail(size_t);
    void     commit(size_t);
    size_t   available() const noexcept;
};

template <class T>
concept StreamCodec = /* stateful decode(view, out) returning
                        expected<optional<Frame>, ErrorInfo> */;

template <class T>
concept DatagramCodec = /* decode(dgram, out, sink) returning
                          expected<size_t, ErrorInfo> */;

template <class T>
concept Codec = StreamCodec<T> || DatagramCodec<T>;
```

### `eph::core` - PacketView contract

Every Stream / Datagram implementation exposes a `PacketView` associated type
conforming to:

```cpp
uint8_t*       writable_data() noexcept;
const uint8_t* data()          const noexcept;
size_t         length()        const noexcept;
void           trim_front(size_t n) noexcept;
void           trim_back (size_t n) noexcept;
uint64_t       arrival_tsc()   const noexcept;
```

### `eph::net` - TcpState (shared)

```cpp
enum class TcpState : uint8_t {
    Closed, Listen, SynSent, SynReceived, Established,
    FinWait1, FinWait2, CloseWait, Closing, LastAck, TimeWait,
};
constexpr const char* tcp_state_name(TcpState) noexcept;
```

Defined in `eph/core/tcp_state.hpp` - both `eph-net-kernel` and `eph-net-dpdk`
share this enum to avoid ODR conflicts.

### `eph::core` - metrics

```cpp
struct MetricTag { std::string_view key; std::string_view value; };

template <class T>
concept MetricsSink = /* push_counter / push_gauge / push_histogram / flush */;

struct NullSink { /* all methods are inline noexcept no-ops */ };
static_assert(MetricsSink<NullSink>);
```

### `eph::core` - error_traits

```cpp
template <class E>
concept ErrorEnum = /* E is an enum with ADL error_name(E) -> string_view */;

template <ErrorEnum E>
struct ErrorEnumFormatter : std::formatter<std::string_view> { /* one-liner */ };
```

### `eph::core` - legacy framer primitives (still used by parser modules)

- `MessageFramer<F>` concept - encode / decode / max_overhead.
- `DecodedFrame` - zero-copy view with payload, payload_len, msg_type, is_control, total_len.
- `FrameError` - kIncomplete / kInvalidFormat / kPayloadTooLarge.
- `LengthPrefixFramer` - 2-byte BE length prefix, <= 65 535 byte payloads.

These predate v3.3 and remain here for eph-fix / eph-itch / eph-json. The v3.3
`eph::codec::LengthPrefixCodec` is a new implementation under the new concept.

### `eph::core` - number parsing

- `parse_number(string_view) -> optional<double>` - full IEEE 754 including
  scientific notation, rejects NaN / infinity / malformed input.
- `parse_int(string_view) -> optional<int64_t>` - full int64 range including
  INT64_MIN.

### `eph::core::detail`

- `json_escape(string_view) -> string` - RFC 8259 section 7, UTF-8 pass-through.
- `base64_encode(uint8_t*, size_t) -> string` - RFC 4648 with = padding.
- `contains_control_chars(string_view) -> bool` - constexpr; used for hostname /
  header validation.

### `eph::` - version metadata

- `kVersionMajor`, `kVersionMinor`, `kVersionPatch`
- `kVersionString` - "M.m.p"
- `kVersionFull`   - "ephemeral/M.m.p"
- `version_at_least(major, minor, patch)` - consteval feature gate

## Dependencies

- `spdlog` (public)

No dependencies on any other eph-* module.
