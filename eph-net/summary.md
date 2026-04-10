# eph-net summary

## Public API surface

Namespace: `eph::net`. Header-only. Contains concepts, shared value types, test
mocks, and the shared TLS / WebSocket / HTTP wire detail used by both the kernel
and DPDK backends.

### Concepts (`eph/net/concepts.hpp`)

```cpp
template <class T>
concept Pollable = requires(T& t) {
    typename T::PacketView;
    // friend Poller invokes:
    //   size_t poll_once_()
    //   void   notify_attached_(Poller*)
    //   void   notify_detached_()
};

template <class T>
concept Stream = Pollable<T> && requires(T& t,
                                         std::span<const uint8_t> data) {
    typename T::CodecType;
    typename T::OnMessage;
    { t.send(data) }         -> std::same_as<std::expected<size_t, core::ErrorInfo>>;
    { t.close_gracefully() } -> std::same_as<std::expected<void,   core::ErrorInfo>>;
    { t.is_attached() }      -> std::same_as<bool>;
    { t.state() }            -> std::same_as<TcpState>;
    { t.on_message }         -> std::convertible_to<typename T::OnMessage>;
};

template <class T>
concept Datagram = Pollable<T> && requires(T& t,
                                           std::span<const uint8_t> data,
                                           const SocketAddr& dst,
                                           const SocketAddr& mcast) {
    typename T::CodecType;
    typename T::OnDatagram;
    { t.send_to(data, dst) }         -> std::same_as<std::expected<size_t, core::ErrorInfo>>;
    { t.join_multicast(mcast) }      -> std::same_as<std::expected<void, core::ErrorInfo>>;
    { t.leave_multicast(mcast) }     -> std::same_as<std::expected<void, core::ErrorInfo>>;
    { t.is_attached() }              -> std::same_as<bool>;
    { t.on_datagram }                -> std::convertible_to<typename T::OnDatagram>;
};

template <class T>
concept Poller = requires(T& p, Pollable auto* obj) {
    { p.add(obj) }    -> std::same_as<std::expected<void, core::ErrorInfo>>;
    { p.remove(obj) } -> std::same_as<std::expected<void, core::ErrorInfo>>;
    { p.poll() }      -> std::convertible_to<size_t>;
    // optional: p.poll(chrono::milliseconds) - KernelPoller has it, DpdkPoller doesn't
};
```

### Value types

- `Ipv4Addr { uint8_t octets[4]; }` with `parse(string_view)` and `to_string()`.
- `SocketAddr { Ipv4Addr ip; uint16_t port; }`.
- `TcpState` (re-exported from `eph/core/tcp_state.hpp`) + `tcp_state_name()`.

### ReconnectPolicy

```cpp
struct ReconnectPolicyConfig {
    std::chrono::milliseconds initial_backoff;
    std::chrono::milliseconds max_backoff;
    double                    multiplier;
    double                    jitter_factor;
    uint32_t                  max_attempts;
};

class ReconnectPolicy {
    explicit ReconnectPolicy(ReconnectPolicyConfig);
    bool                      should_reconnect() const noexcept;
    std::chrono::milliseconds next_backoff() noexcept;
    void                      reset() noexcept;
};
```

Exponential backoff with jitter. Used by both `KernelTcpStream` and `DpdkTcpStream`
for automatic reconnection.

### Test mocks (`eph::net::test`)

- `FakeStream` - satisfies `Stream`. `inject_rx(span)` / `collect_tx()` /
  `clear_tx()` / `inject_disconnect()` for tests to drive the fake.
- `FakeDatagram` - satisfies `Datagram`.
- `TestPoller<P>` - drives registered pollables synchronously on `poll()`.
  No syscalls.

### Shared detail (`eph::net::detail`)

- `TlsSession<ByteSocket>` - TLS 1.3 session wrapping aws-lc. Templated on the
  byte socket adapter provided by the backend.
- `WebSocket` - RFC 6455 frame encode / decode helpers, masking pool.
- `HttpRequest` / `HttpResponse` - minimal HTTP/1.1 parser for the WS upgrade
  handshake.

Users never reference these directly; they are pulled in by the backend
`TcpStream` implementations when `EnableTls=true`.

## Dependencies

- `eph-core` (public)
- `eph-utils` (public)
- `eph-containers` (public)
- `aws-lc` (pulled in by TLS detail)

## See also

- `README.md`
- `docs/ONBOARDING.md`
- `CHANGELOG.md`
- `../docs/architecture.md`
- `../docs/poller-guide.md`
