# eph-net-dpdk summary

## Public API surface

Namespace: `eph::net::dpdk`. Header-only (but links DPDK libs). Backend
implementation of the network concepts on top of DPDK kernel-bypass I/O.

### `Eal` - RAII EAL wrapper

Alias for `eph::dpdk::EalGuard`. Constructed via the static `init` factory
rather than a public constructor so the expected-returning API can surface
EAL-init failures (missing hugepages, bad arguments, already-initialized).

```cpp
class Eal {                                            // = EalGuard
public:
    static std::expected<Eal, std::string> init(int argc, char** argv);
    ~Eal();                                            // rte_eal_cleanup
    Eal(Eal&&) noexcept;                               // move-only
    Eal& operator=(Eal&&) noexcept;
    explicit operator bool() const noexcept;           // initialized?
    int args_consumed() const noexcept;                // argv entries EAL ate
};
```

### `DpdkTcpStream<C, EnableTls>`

```cpp
template <class C, bool EnableTls = true>     // C satisfies eph::core::StreamCodec
class DpdkTcpStream {
public:
    using CodecType  = C;
    using PacketView = detail::MbufView;       // rte_mbuf-backed, in-place mutation
    using OnMessage  = std::function<void(std::span<const uint8_t>)>;

    static std::expected<std::unique_ptr<DpdkTcpStream>, core::ErrorInfo>
    create(StreamConfig cfg) noexcept;

    static std::expected<std::unique_ptr<DpdkTcpStream>, core::ErrorInfo>
    create_and_attach(StreamConfig cfg, ::eph::dpdk::Platform& platform) noexcept;

    OnMessage on_message;

    std::expected<size_t, core::ErrorInfo> send(std::span<const uint8_t> data) noexcept;
    std::expected<void,   core::ErrorInfo> close_gracefully() noexcept;
    bool      is_attached()        const noexcept;
    TcpState  state()              const noexcept;
    bool      is_tls_send_desynced() const noexcept;
    uint64_t  metric(StreamMetric) const noexcept;
};

static_assert(eph::net::Stream<DpdkTcpStream<eph::codec::WsCodec>>);
```

Wraps the internal `eph::dpdk::DpdkTcpSession` TCP state machine. TLS path runs
the shared `eph::net::detail::TlsSession` through a DPDK byte-socket adapter so
aws-lc's AEAD routines can decrypt in place into the same mbuf the NIC DMA'd
into.

### `DpdkUdpSocket<C>`

```cpp
template <class C>                             // C satisfies eph::core::DatagramCodec
class DpdkUdpSocket {
public:
    using CodecType  = C;
    using PacketView = detail::MbufView;
    using OnDatagram = std::function<void(std::span<const uint8_t>, const SocketAddr&)>;

    static std::expected<std::unique_ptr<DpdkUdpSocket>, core::ErrorInfo>
    create(UdpConfig cfg) noexcept;

    static std::expected<std::unique_ptr<DpdkUdpSocket>, core::ErrorInfo>
    create_and_attach(UdpConfig cfg, ::eph::dpdk::Platform& platform) noexcept;

    OnDatagram on_datagram;

    std::expected<size_t, core::ErrorInfo>
    send_to(std::span<const uint8_t>, const SocketAddr&) noexcept;

    std::expected<void, core::ErrorInfo> join_multicast (const SocketAddr&) noexcept;
    std::expected<void, core::ErrorInfo> leave_multicast(const SocketAddr&) noexcept;
    std::expected<void, core::ErrorInfo> connect_to     (const SocketAddr&) noexcept;
    uint64_t metric(StreamMetric) const noexcept;
};

static_assert(eph::net::Datagram<DpdkUdpSocket<eph::codec::Mold64Codec>>);
```

### `DpdkPoller<P>`

```cpp
template <class P = void>  // void = heterogeneous / type-erased mode
class DpdkPoller {
public:
    static std::expected<std::unique_ptr<DpdkPoller>, core::ErrorInfo>
    create(PollerConfig cfg = {}) noexcept;

    template <DpdkPollable T>
    std::expected<void, core::ErrorInfo> add(T* obj) noexcept;

    template <DpdkPollable T>
    std::expected<void, core::ErrorInfo> remove(T* obj) noexcept;

    size_t poll() noexcept;                    // lcore busy-poll, never blocks

    // ICMP Frag Needed path-MTU feedback (TCP streams)
    void     set_icmp_callback(IcmpFragNeededCallback cb) noexcept;
    uint64_t icmp_frag_needed_dispatched() const noexcept;

    // Source-port picker for client-side TCP connects
    std::expected<uint16_t, core::ErrorInfo>
    pick_src_port(uint32_t src_ip, uint32_t dst_ip, uint16_t dst_port,
                  uint16_t range_begin = 32768, uint16_t range_end = 60999,
                  uint16_t preferred = 0) const noexcept;
};

static_assert(eph::net::Poller<DpdkPoller<>>);
```

Internally: one `rte_eth_rx_burst` per tick on `(port_id, queue_id)`, then
`FlowSteeringTable::lookup(5_tuple)` to route each mbuf to the right
`PollableEntry::process_burst_fn(void*, rte_mbuf**, uint16_t, uint64_t)`. No
virtual dispatch.

### Config types (`config.hpp`)

```cpp
struct StreamConfig {
    std::string      remote_host;
    uint16_t         remote_port;
    SocketAddr       local_addr;
    eph::net::TlsConfig tls;        // only used when EnableTls=true
    std::string      ws_path;
    ReconnectPolicyConfig reconnect;
    // ... DPDK-specific knobs (mempool, mbuf sizes)
};

struct UdpConfig {
    SocketAddr bind_addr;
    // ... mempool config
};

struct PollerConfig {
    uint16_t port_id  = 0;
    uint16_t queue_id = 0;
    uint16_t lcore    = 4;
    size_t   max_conn = 1024;
};
```

## Dependencies

- `eph-net` (public) - concepts, SocketAddr, ReconnectPolicy, TLS detail
- `eph-core`, `eph-utils`, `eph-containers` (transitive)
- `dpdk` (public, via vcpkg or system)
- `aws-lc` (transitive, for TLS)

## See also

- `README.md`
- `docs/ONBOARDING.md`
- `CHANGELOG.md`
- `../docs/dpdk-setup.md`
- `../docs/poller-guide.md`
- `../docs/architecture.md`
