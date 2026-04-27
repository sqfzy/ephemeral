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

Wraps the internal `eph::dpdk::TcpSession<>` TCP state machine. TLS path runs
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

Internally: one `rte_eth_rx_burst` per tick on `(port_id, rx_queue_id)`, then
a private `lookup_by_5tuple_` linear scan over a fixed-size
`std::array<PollableEntry, kMaxConn>` (`kMaxConn = 16`) to route each mbuf to
the right `PollableEntry::process_burst_fn(void*, rte_mbuf**, uint16_t,
uint64_t)`. No virtual dispatch. Optional ICMP-Type-3-Code-4 dispatch for
path-MTU feedback is served from the un-routed fallback via
`maybe_dispatch_icmp_` + the ICMP callback closure installed by
`Stream::create_and_attach`.

### Config types (`config.hpp`)

```cpp
struct StreamConfig {
    ::eph::dpdk::TcpConfig legacy{};    // 4-tuple, MAC, port/queue, MSS, recv_window
    ::rte_mempool*        pool{nullptr};
    std::chrono::milliseconds connect_timeout{3000};
    ::eph::net::TlsConfig tls{};        // only used when EnableTls=true

    // WebSocket upgrade (empty ws_path skips)
    std::string                 ws_path{};
    std::string                 ws_host{};
    std::vector<HttpHeader>     ws_extra_headers{};
    std::chrono::milliseconds   ws_timeout{10'000};

    // Unsupported on DPDK — create() rejects non-empty proxy at factory time
    std::optional<ProxyConfig>  proxy{};

    std::size_t                 reasm_capacity{256 * 1024};
    std::optional<uint16_t>     pin_to_queue{};  // RSS+pin / FlowDirector target
};

struct UdpConfig {
    ::eph::dpdk::UdpConfig legacy{};    // 4-tuple, MAC, port/queue, mempool, hw_cksum
    std::optional<uint16_t> pin_to_queue{};
};

struct PollerConfig {
    uint16_t port_id     = 0;
    uint16_t rx_queue_id = 0;
    // Thread affinity is NOT a field — DpdkPoller does not own a thread.
    // You call poll() from your own lcore loop and pin that thread yourself.
};
```

## `PlatformConfig` (`eph::dpdk::PlatformConfig`)

Used by `Platform::create` / `create_primary` / `create_secondary`. All
fields are *requested* values and are clamped to the NIC-reported limits
at bring-up time. The multi-process fields at the bottom default to
single-process / primary semantics — pre-MP code compiles byte-for-byte
unchanged.

```cpp
enum class ProcType : uint8_t { Primary, Secondary };

struct PlatformConfig {
    uint16_t     port_id              = 0;
    uint16_t     nb_rx_queues         = 1;
    uint16_t     nb_tx_queues         = 1;
    uint16_t     nb_rx_desc           = 1024;
    uint16_t     nb_tx_desc           = 1024;
    uint32_t     mbuf_pool_size       = 8'191;   // 2^k - 1
    uint32_t     mbuf_cache_size      = 250;
    bool         enable_promiscuous   = false;
    bool         enable_rss           = false;
    bool         enable_rx_checksum_offload = false;
    bool         enable_strict_rx_checksum  = false;
    int          link_timeout_ms      = 2000;

    // ── Multi-process (primary+secondary) ────────────────────────────
    ProcType     proc_type            = ProcType::Primary;
    std::string_view file_prefix      {};          // mirrors EAL --file-prefix
    std::pair<uint16_t, uint16_t> rx_queue_range {0, 0};      // {0,0} = full range
    // Source-port partitioning across MP processes is the caller's
    // responsibility — eph-net-dpdk does not auto-allocate src_port.

    friend bool operator==(const PlatformConfig&, const PlatformConfig&) = default;
};
```

### Multi-process factories

```cpp
static std::expected<Platform, std::string> create         (const PlatformConfig&);
static std::expected<Platform, std::string> create_primary (PlatformConfig);
static std::expected<Platform, std::string> create_secondary(PlatformConfig);
```

- `create()` — original single-process factory; honours
  `config.proc_type`. Default `Primary` matches legacy behaviour
  byte-for-byte.
- `create_primary()` — forces `proc_type = Primary`, otherwise
  equivalent to `create()`. Use at call sites that also use
  `create_secondary` for clarity.
- `create_secondary()` — forces `proc_type = Secondary` and runs the
  full secondary contract: invokes `validate_config` (which polices
  `rx_queue_range`: either the `{0,0}` sentinel or a non-empty sub-range
  bounded by `nb_rx_queues`), validates non-empty `file_prefix`, then
  does `rte_eth_dev_is_valid_port` + `rte_mempool_lookup("eph_mbuf_p<port>")`
  and skips `rte_eth_dev_configure / rx_queue_setup / tx_queue_setup /
  configure_rss / dev_start / wait_link_up` entirely (primary-only
  APIs). `Impl::cleanup` branches on `config.proc_type`: primary does
  `rte_eth_dev_stop/close` + `rte_mempool_free`, secondary only zeroes
  its per-process view (pollers[] + mempool pointer) so the shared
  port state the primary owns is never touched. The EAL-side complement
  (`EalConfig` + `build_eal_argv`) lives in `eph/dpdk/eal.hpp`.

### Hot-path zero-cost surfaces

Cold getter consumed exactly once per connection by
`create_and_attach`:

```cpp
std::pair<uint16_t, uint16_t> effective_rx_queue_range() const noexcept;
```

`effective_rx_queue_range()` returns `{0, nb_rx_queues}` when the
config sentinel `{0, 0}` is set, else the caller-provided range.
The `rr_counter` target-queue allocator in both
`DpdkTcpStream::create_and_attach` and
`DpdkUdpSocket::create_and_attach` became range-aware: new algorithm
is `lo + (rr_counter.fetch_add(1, relaxed) % (hi - lo))`, with
`(lo, hi)` derived from the effective range — byte-for-byte identical
to the pre-MP `% nb_q`).

No hot path (inc_<M>, `poll`, `process_burst`, `send`, `recv`) is
modified — the MP scaffolding is entirely in cold-path construction
and teardown.

## Dependencies

- `eph-net` (public) — concepts (Stream/Datagram/Poller/Pollable), SocketAddr,
  HttpHeader, ProxyConfig, TlsConfig, ReconnectPolicy, TLS detail
- `eph-core`, `eph-utils`, `eph-containers` (transitive)
- system `libdpdk` (public, via pkg-config) — the vcpkg DPDK path was
  retired because it dragged vcpkg's bundled libssl headers into the DPDK
  TU and collided with aws-lc's
- `aws-lc` (transitive, for TLS + HMAC + CSPRNG)

## See also

- `README.md`
- `docs/ONBOARDING.md`
- `CHANGELOG.md`
- `../docs/dpdk-setup.md`
- `../docs/poller-guide.md`
- `../docs/architecture.md`
