# eph-net-dpdk summary

## Public API surface

Namespace: `eph::net::dpdk`. Header-only (but links DPDK libs). Backend
implementation of the network concepts on top of DPDK kernel-bypass I/O.

### `Eal` - RAII EAL wrapper

```cpp
class Eal {
public:
    Eal(int argc, char** argv);      // calls rte_eal_init
    ~Eal();                          // calls rte_eal_cleanup
    // non-copyable, non-movable
};
```

### `DpdkTcpStream<C, EnableTls>`

```cpp
template <core::StreamCodec C, bool EnableTls = true>
class DpdkTcpStream {
public:
    using CodecType  = C;
    using PacketView = detail::MbufView;   // rte_mbuf-backed, in-place mutation
    using OnMessage  = std::function<void(const uint8_t*, uint16_t)>;

    static std::expected<std::unique_ptr<DpdkTcpStream>, core::ErrorInfo>
    create(StreamConfig cfg);

    OnMessage on_message;

    std::expected<size_t, core::ErrorInfo> send(std::span<const uint8_t> data);
    std::expected<void,   core::ErrorInfo> close_gracefully();
    bool      is_attached() const noexcept;
    TcpState  state()       const noexcept;
};

static_assert(eph::net::Stream<DpdkTcpStream<eph::codec::WsCodec>>);
```

Wraps the internal `eph::dpdk::DpdkTcpSession` TCP state machine. TLS path runs
the shared `eph::net::detail::TlsSession` through a DPDK byte-socket adapter so
aws-lc's AEAD routines can decrypt in place into the same mbuf the NIC DMA'd
into.

### `DpdkUdpSocket<C>`

```cpp
template <core::DatagramCodec C>
class DpdkUdpSocket {
public:
    using CodecType  = C;
    using PacketView = detail::MbufView;
    using OnDatagram = std::function<void(const uint8_t*, uint16_t, const SocketAddr&)>;

    static std::expected<std::unique_ptr<DpdkUdpSocket>, core::ErrorInfo>
    create(UdpConfig cfg);

    OnDatagram on_datagram;

    std::expected<size_t, core::ErrorInfo>
    send_to(std::span<const uint8_t>, const SocketAddr&);

    std::expected<void, core::ErrorInfo> join_multicast (const SocketAddr&);
    std::expected<void, core::ErrorInfo> leave_multicast(const SocketAddr&);
};

static_assert(eph::net::Datagram<DpdkUdpSocket<eph::codec::Mold64Codec>>);
```

### `DpdkPoller<P>`

```cpp
template <class P = void>  // void = heterogeneous / type-erased mode
class DpdkPoller {
public:
    static std::expected<std::unique_ptr<DpdkPoller>, core::ErrorInfo>
    create(PollerConfig cfg);

    template <Pollable T>
    std::expected<void, core::ErrorInfo> add(T* obj);

    template <Pollable T>
    std::expected<void, core::ErrorInfo> remove(T* obj);

    size_t poll() noexcept;    // lcore busy-poll, never blocks
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
