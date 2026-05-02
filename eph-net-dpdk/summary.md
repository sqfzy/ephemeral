# eph-net-dpdk summary

## Public API surface

Namespace: `eph::net::dpdk`. Header-only (but links DPDK libs). Backend
implementation of the network concepts on top of DPDK kernel-bypass I/O.

### `Eal` - RAII EAL wrapper

Alias for `eph::dpdk::EalGuard`. Constructed via the static `init` /
`init_raw` factories rather than a public constructor so the
expected-returning API can surface EAL-init failures (missing hugepages,
bad arguments, already-initialized). Both factories return move-only
guards that drive `rte_eal_cleanup` on destruction.

```cpp
class Eal {                                            // = EalGuard
public:
    // Recommended typed-pin path. Pre-validates and registers each
    // (lcore, cpu) pair into the process-wide pin registry BEFORE
    // calling rte_eal_init, so SMT / NUMA / IRQ / duplicate-cpu
    // policy violations surface as a typed error with no DPDK side
    // effects. See eph-net-dpdk/docs/lcore-pin-integration.md.
    static std::expected<Eal, std::string>
    init(EalConfig cfg, std::span<LcorePin const> pins,
         eph::utils::CpuPinPolicy policy = {});

    // Escape hatch: hand-assembled argv. Skips pin pre-validation so
    // callers that already drove --lcores=N@cpu themselves don't
    // double-register. See eph-net-dpdk/docs/dpdk-multiprocess.md
    // for the autojoin orchestrator's use of this path.
    static std::expected<Eal, std::string>
    init_raw(int argc, char** argv);

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
// Post-T3.19 layout: backend-shared concerns at the top level; DPDK-only
// knobs live in a nested `dpdk` substruct. Mirrors the kernel twin's
// `kernel` substruct for symmetry. The DPDK StreamConfig has NO proxy
// field — misuse on DPDK is a compile error rather than a factory-time
// reject (HTTP CONNECT is kernel-only by design).
struct StreamConfig {
    std::chrono::milliseconds            connect_timeout{3000};
    std::size_t                          reasm_capacity{256 * 1024};
    ::eph::net::TlsConfig                tls{};             // used when EnableTls=true

    // WebSocket upgrade — non-empty ws.path activates RFC 6455 handshake.
    // Sub-struct also carries `host`, `extra_headers`, `timeout`, and
    // `permessage_deflate` (true by default).
    ::eph::net::WsConfig                 ws{};

    // TCP keepalive (interval==0 disables). Lowered into
    // dpdk.wire.keepalive_interval / keepalive_probes at factory
    // time so TcpSession::tick_keepalive honours it on every poll cycle.
    ::eph::net::KeepaliveConfig          keepalive{};

    // DPDK-only knobs.
    struct Dpdk {
        ::eph::dpdk::TcpConfig wire{};   // 4-tuple, MAC, port/queue, MSS,
                                                  // recv_window, max_rx_burst
                                                  // (renamed from `legacy` in T3.19)
        ::rte_mempool*        pool{nullptr};      // mempool for TcpSession mbufs
        std::optional<uint16_t> pin_to_queue{};   // RSS+pin / FlowDirector target
        int                   pool_lcore_hint{-1}; // per-lcore mempool hint
    } dpdk;
};

struct UdpConfig {
    ::eph::dpdk::UdpConfig  legacy{};   // 4-tuple, MAC, port/queue, mempool, hw_cksum
                                        // (UDP retains the legacy substruct name —
                                        //  see config.hpp comment)
    std::optional<uint16_t> pin_to_queue{};
    int                     pool_lcore_hint{-1};
};

struct PollerConfig {
    uint16_t port_id     = 0;
    uint16_t rx_queue_id = 0;
    // Thread affinity is NOT a field — DpdkPoller does not own a thread.
    // You call poll() from your own lcore loop and pin that thread yourself.
};
```

## `PlatformConfig` (`eph::dpdk::PlatformConfig`)

Used by `Platform::create`, `Platform::launch`, and (embedded as
`CreateOrJoinConfig::nic`) `Platform::create_or_join`. All fields are
*requested* values and are clamped to the NIC-reported limits at
bring-up time. The multi-process knobs at the bottom default to
single-process — `Platform::create` / `launch` reject any
`max_procs > 1` (the cooperative MP path was removed; use
`Platform::create_or_join` for multi-process).

```cpp
enum class ProcType : uint8_t { Primary, Secondary };  // resolved post-EAL by autojoin

struct PlatformConfig {
    // ── Identity ─────────────────────────────────────────────────────
    uint16_t     port_id              = 0;
    std::string_view file_prefix      = {};          // mirrors EAL --file-prefix; primary-only

    // ── NIC physical state ───────────────────────────────────────────
    uint16_t     nb_rx_queues                  = 1;
    uint16_t     nb_tx_queues                  = 1;
    uint16_t     nb_rx_desc                    = 256;
    uint16_t     nb_tx_desc                    = 512;
    uint32_t     mbuf_pool_size                = 4095;
    uint16_t     mbuf_cache_size               = 256;
    bool         enable_promiscuous            = false;
    bool         enable_rx_checksum_offload    = false;
    bool         enable_strict_rx_checksum     = false;
    int          link_timeout_ms               = 2000;
    uint16_t     per_lcore_pools               = 0;  // 0 = single shared pool

    // ── Multi-process knobs (read by autojoin primary) ───────────────
    uint8_t      max_procs                     = 1;  // 1 = single-process
    uint16_t     queues_per_proc               = 0;  // 0 = auto (nb_rx_queues / max_procs)
    // Source-port partitioning across MP processes is the caller's
    // responsibility — eph-net-dpdk does not auto-allocate src_port.

    friend bool operator==(const PlatformConfig&, const PlatformConfig&) = default;
};
```

### Public factories

```cpp
static std::expected<Platform, std::string> create        (PlatformConfig);
static std::expected<Platform, std::string> launch        (PlatformConfig, EalConfig,
                                                           std::span<LcorePin const> pins,
                                                           CpuPinPolicy policy);
static std::expected<Platform, std::string> create_or_join(CreateOrJoinConfig);
```

- `create()` — single-process. EAL must be initialized externally.
  Rejects `max_procs > 1` with a recovery hint pointing at
  `create_or_join`.
- `launch()` — single-process one-shot: builds the EAL argv from
  `EalConfig` + typed `LcorePin`s, calls `rte_eal_init`, and brings up
  the `Platform`. EAL ownership rides with the returned guard.
- `create_or_join()` — the **only** multi-process entry point. Every
  peer issues the same call; the library races on `rte_eal_init` and
  the winner becomes primary (runs the internal `primary_bringup_()`
  helper, builds the mempool named `eph_mbuf_p<port>`, publishes a
  shared hugepage registry). Losers attach as secondaries via
  `secondary_bringup_()` (CAS-claim a registry slot, look up the
  primary's mempool, skip configure/start/stop/close).

### Lifecycle and teardown

`Impl::cleanup` checks `rte_eal_process_type()` (or the cached
secondary flag synthesized inside `secondary_bringup_`): primary does
`rte_eth_dev_stop/close` + `rte_mempool_free`, secondary only zeroes
its per-process view (pollers[] + mempool pointer) so the shared port
state the primary owns is never touched. The EAL-side complement
(`EalConfig` + `build_eal_argv`) lives in `eph/dpdk/eal.hpp`; the
`launch` and `create_or_join` factories invoke it under the hood and
inject `--proc-type` / `--file-prefix` automatically.

### `proc_type.hpp` — minimal cross-cutting header

`eph::dpdk::ProcType` and `to_eal_string(ProcType)` live in
`eph/dpdk/proc_type.hpp` so both `eph/dpdk/platform.hpp` (full
Platform contract) and `eph/dpdk/eal.hpp` (`EalConfig` argv assembly
via `build_eal_argv`) share a single source of truth. Adding a new
enum value is automatically reached by every consumer; the central
serializer is a `switch` so `-Wswitch` flags missed cases at compile
time.

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
