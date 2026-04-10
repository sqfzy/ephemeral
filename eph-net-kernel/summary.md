# eph-net-kernel summary

## Public API surface

Namespace: `eph::net::kernel`. Header-only. Backend implementation of the v3.3
network concepts on top of POSIX sockets + epoll.

### `KernelTcpStream<C, EnableTls>`

```cpp
template <core::StreamCodec C, bool EnableTls = true>
class KernelTcpStream {
public:
    using CodecType  = C;
    using PacketView = detail::SpanView;
    using OnMessage  = std::function<void(const uint8_t*, uint16_t)>;

    static std::expected<std::unique_ptr<KernelTcpStream>, core::ErrorInfo>
    create(StreamConfig cfg);

    OnMessage on_message;

    std::expected<size_t, core::ErrorInfo> send(std::span<const uint8_t> data);
    std::expected<void,   core::ErrorInfo> close_gracefully();
    bool      is_attached() const noexcept;
    TcpState  state()       const noexcept;
    int       fd()          const noexcept;
};

static_assert(eph::net::Stream<KernelTcpStream<eph::codec::WsCodec>>);
```

`create()` performs TCP connect + TLS handshake + WS upgrade synchronously before
returning. Once attached to a `KernelPoller`, rx work runs on `poll()` calls.

### `KernelUdpSocket<C>`

```cpp
template <core::DatagramCodec C>
class KernelUdpSocket {
public:
    using CodecType  = C;
    using PacketView = detail::SpanView;
    using OnDatagram = std::function<void(const uint8_t*, uint16_t, const SocketAddr&)>;

    static std::expected<std::unique_ptr<KernelUdpSocket>, core::ErrorInfo>
    create(UdpConfig cfg);

    OnDatagram on_datagram;

    std::expected<size_t, core::ErrorInfo>
    send_to(std::span<const uint8_t>, const SocketAddr&);

    std::expected<void, core::ErrorInfo> join_multicast (const SocketAddr&);
    std::expected<void, core::ErrorInfo> leave_multicast(const SocketAddr&);

    // optional: constrain source
    std::expected<void, core::ErrorInfo> connect_to(const SocketAddr&);
};

static_assert(eph::net::Datagram<KernelUdpSocket<eph::codec::Mold64Codec>>);
```

### `KernelPoller`

```cpp
class KernelPoller {
public:
    static std::expected<std::unique_ptr<KernelPoller>, core::ErrorInfo>
    create(PollerConfig cfg = {});

    template <Pollable P>
    std::expected<void, core::ErrorInfo> add(P* obj);

    template <Pollable P>
    std::expected<void, core::ErrorInfo> remove(P* obj);

    size_t poll() noexcept;                             // non-blocking
    size_t poll(std::chrono::milliseconds to) noexcept; // epoll_wait(timeout)
};

static_assert(eph::net::Poller<KernelPoller>);
```

Internally: one `epoll_fd_`, one `vector<PollableEntry>` where each entry is
`{ void* obj; size_t (*poll_fn)(void*); int fd; }`. `add()` calls
`epoll_ctl(EPOLL_CTL_ADD)`; `poll()` calls `epoll_wait` and dispatches readable
fds to the matching entry's `poll_fn`. No virtual dispatch.

### Config types (`config.hpp`)

```cpp
struct StreamConfig {
    std::string host;
    uint16_t    port;
    bool        use_tls        = false;
    std::string ws_path;
    TlsConfig   tls;
    std::string bind_device;   // SO_BINDTODEVICE
    // ... ReconnectPolicyConfig, timeouts, etc.
};

struct UdpConfig {
    SocketAddr bind_addr;
    bool       reuse_addr = true;
    int        rcvbuf     = 0;
    std::string bind_device;
};

struct PollerConfig {
    int max_events = 64;      // size of epoll_event batch
};
```

## Dependencies

- `eph-net` (public) - concepts, SocketAddr, ReconnectPolicy, TLS detail
- `eph-core`, `eph-utils`, `eph-containers` (transitive)

## See also

- `README.md`
- `docs/ONBOARDING.md`
- `../docs/poller-guide.md`
- `../docs/architecture.md`
