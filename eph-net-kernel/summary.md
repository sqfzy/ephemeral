# eph-net-kernel summary

## Public API surface

Namespace: `eph::net::kernel`. Header-only. Kernel-backend implementation of
the networking concepts on top of POSIX sockets + `epoll`.

### `KernelTcpStream<C, EnableTls>`

```cpp
template <class C, bool EnableTls = true>
class KernelTcpStream {
public:
    using CodecType  = C;                               // StreamCodec
    using PacketView = detail::SpanView;                // contiguous span
    using OnMessage  = std::function<void(std::span<const uint8_t>)>;

    static std::expected<std::unique_ptr<KernelTcpStream>, core::ErrorInfo>
    create(StreamConfig cfg) noexcept;

    OnMessage on_message;

    std::expected<std::size_t, core::ErrorInfo>
        send(std::span<const uint8_t> app_payload) noexcept;
    std::expected<void, core::ErrorInfo> close_gracefully() noexcept;

    bool      is_attached() const noexcept;
    TcpState  state()       const noexcept;
    int       fd()          const noexcept;

    std::uint64_t metric(eph::net::StreamMetric m) const noexcept;
};

static_assert(eph::net::Stream<KernelTcpStream<eph::codec::WsCodec>>);
```

`create()` is synchronous: it performs the TCP connect, the optional HTTP
CONNECT proxy handshake, the TLS 1.3 handshake (when `EnableTls=true`) and
the optional WebSocket RFC 6455 upgrade (when `cfg.ws.path` is non-empty)
before returning. After `KernelPoller::add(stream.get())`, RX work runs
inside `poller->poll()`.

### `KernelUdpSocket<C>`

```cpp
template <class C>
class KernelUdpSocket {
public:
    using CodecType  = C;                               // DatagramCodec
    using PacketView = detail::SpanView;
    using OnDatagram = std::function<void(std::span<const uint8_t>,
                                          const SocketAddr&)>;

    static std::expected<std::unique_ptr<KernelUdpSocket>, core::ErrorInfo>
    create(UdpConfig cfg) noexcept;

    OnDatagram on_datagram;

    std::expected<std::size_t, core::ErrorInfo>
        send_to(std::span<const uint8_t>, const SocketAddr&) noexcept;

    std::expected<void, core::ErrorInfo> join_multicast (const SocketAddr&) noexcept;
    std::expected<void, core::ErrorInfo> leave_multicast(const SocketAddr&) noexcept;
    std::expected<void, core::ErrorInfo> connect_to     (const SocketAddr&) noexcept;

    bool is_attached() const noexcept;
    int  fd()          const noexcept;

    std::uint64_t metric(eph::net::StreamMetric m) const noexcept;
};

static_assert(eph::net::Datagram<KernelUdpSocket<eph::codec::Mold64Codec>>);
```

### `KernelPoller`

```cpp
class KernelPoller {
public:
    static std::expected<std::unique_ptr<KernelPoller>, core::ErrorInfo>
    create(PollerConfig cfg = {}) noexcept;

    // KernelPollable<P> requires: p.fd(), p.poll_once_(),
    //                             p.notify_attached_(KernelPoller*),
    //                             p.notify_detached_()  (all noexcept)
    template <KernelPollable P>
    std::expected<void, core::ErrorInfo> add   (P* obj) noexcept;
    template <KernelPollable P>
    std::expected<void, core::ErrorInfo> remove(P* obj) noexcept;

    std::size_t poll() noexcept;                               // non-blocking
    std::size_t poll(std::chrono::milliseconds timeout) noexcept;

    std::size_t size()     const noexcept;   // test hook
    int         epoll_fd() const noexcept;   // test hook
};

static_assert(eph::net::Poller<KernelPoller>);
```

Internals: one `epoll_fd_` (from `epoll_create1(EPOLL_CLOEXEC)`) and one
`std::vector<PollableEntry>` where each entry is
`{void* obj; size_t(*poll_fn)(void*) noexcept; void(*detach_fn)(void*) noexcept; int fd}`.
`add()` captures the type-erased thunks at compile time and calls
`epoll_ctl(EPOLL_CTL_ADD, EPOLLIN)`; `poll()` calls `epoll_wait` (burst
capped at `min(cfg.max_events_per_wait, 256)`) and linearly resolves each
ready event's `data.ptr` back to an entry to dispatch `poll_fn`. No virtual
dispatch, no `std::function`, no heap allocation on the hot path.

The Poller's destructor calls every still-attached Pollable's `detach_fn`
before closing `epoll_fd_`, so a later `~Stream` never dereferences a dead
Poller.

### Config types (`config.hpp`)

```cpp
// Post-T3.19 layout: backend-shared concerns at the top level; kernel-only
// knobs live in a nested `kernel` substruct. The DPDK twin mirrors the same
// shape with a `dpdk` substruct in place of `kernel`.
struct StreamConfig {
    SocketAddr                           remote{};          // required
    std::chrono::milliseconds            connect_timeout{3000};
    std::size_t                          reasm_capacity{64 * 1024};
    eph::net::TlsConfig                  tls{};             // used when EnableTls=true

    // WebSocket upgrade — non-empty ws.path activates RFC 6455 handshake.
    // Sub-struct also carries `host`, `extra_headers`, `timeout`, and
    // `permessage_deflate` (true by default).
    eph::net::WsConfig                   ws{};

    // TCP keepalive (interval==0 disables). Kernel lowers this to
    // setsockopt(SO_KEEPALIVE / TCP_KEEPIDLE / TCP_KEEPINTVL / TCP_KEEPCNT).
    eph::net::KeepaliveConfig            keepalive{};

    // HTTP CONNECT proxy — populated optional routes TCP through the proxy,
    // then runs TLS / WS inside the tunnel. DPDK has no proxy field.
    std::optional<eph::net::ProxyConfig> proxy{};

    // Kernel-only knobs.
    struct Kernel {
        SocketAddr local_bind{};         // optional bind before connect()
        bool       tcp_nodelay{true};
    } kernel;
};

struct UdpConfig {
    SocketAddr  bind{};                     // local bind address
    SocketAddr  connect_to{};               // non-zero port → connected UDP
    std::size_t rcv_buf{0};                 // 0 = kernel default
    std::size_t snd_buf{0};                 // 0 = kernel default
    bool        reuse_addr{false};          // SO_REUSEADDR (multicast)
};

struct PollerConfig {
    std::size_t initial_capacity{16};       // entries_ reserve()
    int         max_events_per_wait{64};    // epoll_wait burst cap (≤ 256)
};
```

TLS selection is template-parametric (`EnableTls`), not a runtime bool —
`tls` is ignored unless `EnableTls=true`. Stream-local reconnect state was
removed on 2026-04-14; callers drive the retry loop via
`eph::utils::ExponentialBackoff` / `eph::utils::retry`.

## Dependencies

- `eph-net` (public) — concepts, `SocketAddr`, `ReconnectOrchestrator`, TLS detail
- `eph-core`, `eph-utils`, `eph-containers` (transitive)
- `spdlog`, `aws-lc` (public packages)

## See also

- `README.md`
- `CHANGELOG.md`
- `docs/ONBOARDING.md`
- `../docs/poller-guide.md`
- `../docs/architecture.md`
