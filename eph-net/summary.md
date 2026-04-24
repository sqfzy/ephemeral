# eph-net summary

## Public API surface

Namespace: `eph::net` (plus `eph::net::test`, `eph::net::posix`,
`eph::net::detail`). Header-only. Contains concepts, shared value types,
pure-compute primitives (HTTP parser, HMAC), test mocks, POSIX server
helpers, and the shared TLS / WebSocket / HTTP wire detail used by both
the kernel and DPDK backends.

### Concepts (`eph/net/concepts.hpp`)

All four require their I/O methods to be `noexcept`.

```cpp
template <class T>
concept Pollable = requires(T& t) {
    typename T::PacketView;
    { t.poll_once_() }    noexcept -> std::convertible_to<std::size_t>;
    { t.is_attached_() }  noexcept -> std::convertible_to<bool>;
    { t.native_handle() } noexcept -> std::convertible_to<void*>;
};

template <class T>
concept Stream = Pollable<T> && requires(T& t, std::span<const uint8_t> app) {
    typename T::CodecType;
    typename T::OnMessage;
    { t.send(app) }          noexcept
        -> std::same_as<std::expected<std::size_t, core::ErrorInfo>>;
    { t.close_gracefully() } noexcept
        -> std::same_as<std::expected<void, core::ErrorInfo>>;
    { t.is_attached() }      noexcept -> std::convertible_to<bool>;
    { t.state() }            noexcept -> std::same_as<TcpState>;
    { t.on_message }     -> std::convertible_to<typename T::OnMessage>;
};

template <class T>
concept Datagram = Pollable<T> && requires(T& t,
                                           std::span<const uint8_t> app,
                                           const SocketAddr& dst,
                                           const SocketAddr& mcast) {
    typename T::CodecType;
    typename T::OnDatagram;
    { t.send_to(app, dst) }      noexcept
        -> std::same_as<std::expected<std::size_t, core::ErrorInfo>>;
    { t.join_multicast(mcast) }  noexcept
        -> std::same_as<std::expected<void, core::ErrorInfo>>;
    { t.leave_multicast(mcast) } noexcept
        -> std::same_as<std::expected<void, core::ErrorInfo>>;
    { t.is_attached() }      noexcept -> std::convertible_to<bool>;
    { t.on_datagram }    -> std::convertible_to<typename T::OnDatagram>;
};

template <class T>
concept Poller = requires(T& p) {
    { p.poll() } noexcept -> std::convertible_to<std::size_t>;
};

// Finer refinement: verifies that Poller `T` accepts Pollable `Obj`.
template <class T, class Obj>
concept PollerOf = Poller<T> && Pollable<Obj> && requires(T& p, Obj* obj) {
    { p.add(obj) }    noexcept -> std::same_as<std::expected<void, core::ErrorInfo>>;
    { p.remove(obj) } noexcept -> std::same_as<std::expected<void, core::ErrorInfo>>;
};
```

A `poll(std::chrono::milliseconds)` overload is optional —
`KernelPoller` supports blocking with timeout, `DpdkPoller` does not.

Utility helpers in the same header:

```cpp
constexpr uint16_t saturate_u16(std::size_t n) noexcept;
constexpr bool     saturate_u16_clamps(std::size_t n) noexcept;
```

`saturate_u16` silently clamps to `0xFFFF`; backends use
`saturate_u16_clamps` to gate a one-shot WARN before dispatching the
clamped frame.

### Value types

- `Ipv4Addr { std::array<uint8_t, 4> octets; }` with `from_be32` /
  `to_be32` / `from_octets` / strict `parse(string_view)` (rejects legacy
  `inet_aton` short forms, out-of-range octets, whitespace, trailing
  garbage) / `to_string()` / `operator==`.
- `SocketAddr { Ipv4Addr ip; uint16_t port; }` with `to_string()` and
  `operator==`.
- `TcpState` (re-exported from `eph/core/tcp_state.hpp`) plus
  `tcp_state_name()`.

### ReconnectPolicy

```cpp
struct ReconnectPolicyConfig {
    std::chrono::milliseconds initial_backoff{100};
    std::chrono::milliseconds max_backoff{1600};
    double                    multiplier{2.0};
    double                    jitter_factor{0.25};
    uint32_t                  max_attempts{0};   // 0 = unlimited
};

class ReconnectPolicy {
    explicit ReconnectPolicy(ReconnectPolicyConfig) noexcept;
    bool                      should_reconnect() const noexcept;
    std::chrono::milliseconds next_backoff() noexcept;   // advances state
    void                      reset() noexcept;
    uint32_t                  attempts() const noexcept;
    const ReconnectPolicyConfig& config() const noexcept;
};
```

Constructor clamps invalid configs (`multiplier <= 1.0` → 2.0,
`jitter_factor` clamped to `[0, 0.999]`, `max_backoff < initial_backoff`
raised to `initial_backoff`). Thread-local RNG keeps jitter decorrelated
across threads. Not internally synchronised — one policy per connection.
Used by both `KernelTcpStream` and `DpdkTcpStream`.

### HTTP/1.1 parser + builder (`eph/net/http.hpp`)

Incremental, zero-heap HTTP/1.1 subset for exchange REST and WS-upgrade
paths.

```cpp
struct HttpHeader { std::string_view name, value; };

struct HttpRequest {
    std::string_view              method;
    std::string_view              target;
    uint8_t                       version_minor;  // 0 or 1
    std::span<const HttpHeader>   headers;        // slice of header_storage
    std::span<const uint8_t>      body;           // empty or Content-Length sized
};

struct HttpResponse {
    uint16_t                      status_code;    // 100..599
    std::string_view              reason_phrase;
    uint8_t                       version_minor;
    std::span<const HttpHeader>   headers;
    std::span<const uint8_t>      body;
};

template <class T> struct ParseResult { T value; size_t consumed; };

inline constexpr size_t kMaxHeaderCount       = 64;
inline constexpr size_t kMaxStartLineLength   = 8192;
inline constexpr size_t kMaxHeaderLineLength  = 8192;
inline constexpr size_t kMaxBodySize          = 16 * 1024 * 1024;

std::expected<std::optional<ParseResult<HttpRequest>>, core::ErrorInfo>
parse_http_request (std::span<const uint8_t> buf,
                    std::span<HttpHeader>    header_storage) noexcept;

std::expected<std::optional<ParseResult<HttpResponse>>, core::ErrorInfo>
parse_http_response(std::span<const uint8_t> buf,
                    std::span<HttpHeader>    header_storage) noexcept;

std::expected<size_t, core::ErrorInfo>
build_http_request (uint8_t* out, size_t cap,
                    std::string_view method, std::string_view target,
                    std::span<const HttpHeader> headers,
                    std::span<const uint8_t>    body = {}) noexcept;

std::expected<size_t, core::ErrorInfo>
build_http_response(uint8_t* out, size_t cap,
                    uint16_t status_code, std::string_view reason_phrase,
                    std::span<const HttpHeader> headers,
                    std::span<const uint8_t>    body = {}) noexcept;
```

Result contract:

- `Ok(Some(...))` — complete message parsed; caller drains `consumed`
  bytes from the front of its receive buffer.
- `Ok(None)` — incomplete; need more bytes.
- `Err(CodecBad)` — protocol violation: bare LF in a header line, bare
  CR without LF, any `Transfer-Encoding` present, multiple conflicting
  `Content-Length` values, non-3-digit status code, non-numeric
  `Content-Length`, response without `Content-Length` on a bodied
  status, CRLF/NUL/whitespace injection in builder inputs, etc.
- `Err(CodecOverflow)` — hit one of the hard caps (start line, header
  line, header count, body size, builder buffer capacity).

Returned views alias the caller's input buffer and `header_storage`;
lifetimes are tied to the caller's storage. Builders do not auto-inject
`Content-Length` — the caller decides.

### HMAC-SHA256 (`eph/net/hmac.hpp`)

```cpp
class HmacSha256Key {
    explicit HmacSha256Key(std::span<const uint8_t> raw) noexcept;
    explicit HmacSha256Key(std::string_view        raw) noexcept;
    // non-copyable, noexcept-movable; dtor runs OPENSSL_cleanse
};

struct HmacSha256Tag {
    std::array<uint8_t, 32> bytes;
    size_t      to_hex(std::span<uint8_t, 64> out) const noexcept;  // hot path
    std::string to_hex() const;   // allocating convenience, NOT for hot path
};

HmacSha256Tag hmac_sha256_sign(const HmacSha256Key&,
                               std::span<const uint8_t> msg) noexcept;
HmacSha256Tag hmac_sha256_sign(const HmacSha256Key&,
                               std::string_view        msg) noexcept;
```

`HmacSha256Key` normalises per RFC 2104 §2 — keys ≤ 64 bytes are
zero-padded, longer keys are SHA-256'd. Buffer is `alignas(64)`. Static
asserts at the end of the header guarantee non-copyability,
noexcept-movability, and that `HmacSha256Tag` is exactly 32 bytes.

### HTTP CONNECT proxy (`eph/net/proxy.hpp`)

```cpp
struct ProxyConfig {
    std::string                host;
    uint16_t                   port{0};
    std::optional<std::string> basic_auth_user;
    std::optional<std::string> basic_auth_pass;
    std::chrono::milliseconds  timeout{std::chrono::seconds{10}};

    std::expected<void, core::ErrorInfo> validate() const noexcept;
};
```

`validate()` rejects empty host, zero port, half-specified basic auth
(user XOR pass), and non-positive timeout. Consumed only by the kernel
backend via `StreamConfig::proxy`. DPDK `TcpStream::create()` returns
`Error::InvalidConfig` on any non-empty `proxy`. SOCKS5, digest / NTLM /
Kerberos auth are explicitly out of scope.

### Stream metrics (`eph/net/stream_metrics.hpp`)

```cpp
enum class StreamMetric : std::size_t {
    kBytesSent, kBytesRecv, kFramesDecoded,
    kReasmOverflows, kCodecErrors,
    kTlsCrossRecordFrames, kTlsSendDesyncs,
    kTcpResetsReceived, kTcpOutOfOrderSegments,
    kTcpReorderBufferHits, kTcpReorderBufferOverflows,
    kTcpKeepaliveProbesSent, kTcpMssNegotiationApplied,
    kIcmpFragNeededReceived, kTcpDupSegments,
    kRxSessionResets,
    kRxBadChecksum, kRxIpChecksumBad, kRxL4ChecksumBad,
    kPacketsDropped, kFragmentRejected,
    kCount
};

inline constexpr std::array<std::string_view,
                            static_cast<size_t>(StreamMetric::kCount)>
kStreamMetricNames = { "net.stream.bytes_sent", ... };

template <class Stream, eph::core::MetricsSink Sink>
void publish_metrics(const Stream& source, Sink& sink,
                     std::span<const eph::core::MetricTag> tags = {}) noexcept;
```

Duck-typed over any `Stream` exposing
`[[nodiscard]] uint64_t metric(StreamMetric) const noexcept`. Hot-path
writers (`inc_<M>()` templates) live in the backend types, not here —
this header only defines the index, names, and the reader. Names follow
OpenTelemetry `net.stream.<dimension>` style; per-backend N/A entries
stay at 0.

### POSIX server helpers (`eph/net/posix_io.hpp`, `eph/net/posix_listener.hpp`)

Namespace `eph::net::posix`. **Server-side only** — clients live in
`eph-net-kernel`. Used by mock servers and benchmark fixtures.

```cpp
bool send_all  (int fd, const void*, size_t) noexcept;
bool recv_exact(int fd, void*,       size_t) noexcept;

std::expected<int, std::string>
tcp_bind_listen(std::string_view ip, uint16_t port, int backlog = 1);

// plus UDP bind + poll-based accept helpers for mocks and fixtures
```

### Test mocks (`eph::net::test`)

- `FakeStream` — satisfies `Stream`. `inject_rx(span)` / `collect_tx()` /
  `clear_tx()` / `inject_disconnect()` give tests full drive control.
- `FakeDatagram` — satisfies `Datagram`.
- `TestPoller<P>` — synchronously drives registered pollables on each
  `poll()` call. No syscalls, no threads.

### Shared detail (`eph::net::detail`)

Internal — users never reference these directly; they are pulled in by
the backend `TcpStream` / `UdpSocket` implementations.

- `TlsSession<ByteSocket>` (`tls_session.hpp`) — TLS 1.3 session wrapping
  aws-lc's `SSL*`, templated on a byte-socket adapter supplied by the
  backend.
- `tls_record.hpp`, `tls_decryptor.hpp`, `tls_encryptor.hpp`,
  `tls_inplace.hpp`, `tls_constants.hpp` — split-out AES-GCM primitives
  so the DPDK backend can decrypt straight into the mbuf (zero-copy
  `PacketView`) without dragging the full session state machine.
- `WebSocket` (`websocket.hpp`) — RFC 6455 frame encode / decode +
  masking pool.
- `ws_handshake.hpp` — client-side `Upgrade: websocket` handshake over a
  generic ByteSink. Called transparently from
  `{Kernel,Dpdk}TcpStream::create()` when `StreamConfig::ws_path` is
  non-empty (decision D-2: config-driven rather than a separate
  `connect_async`-style entry point).
- `http_connect.hpp` — HTTP CONNECT proxy handshake over a generic
  ByteSink. Kernel backend only.

## Dependencies

- `eph-core` (public) — `Error`, `ErrorInfo`, `OutputBuffer`,
  `PacketView`, `TcpState`, `MetricsSink`.
- `eph-utils` (public) — TSC timer, HDR histogram primitives.
- `eph-containers` (public) — SPSC queues / ring buffers for the TLS
  reassembly buffer.
- `aws-lc` — pulled in by `hmac.hpp` and by `detail/tls_*.hpp`. Only link
  it in targets that instantiate a TLS-enabled stream or use
  `HmacSha256Key`.

## See also

- `README.md`
- `CHANGELOG.md`
- `docs/ONBOARDING.md`
- `../docs/architecture.md`
- `../docs/poller-guide.md`
