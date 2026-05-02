# eph-net-dpdk onboarding

## What's in this module

The DPDK-side implementation of the networking concepts. Four
user-facing types:

- `DpdkTcpStream<C, EnableTls>` — per-connection TCP stream with in-place TLS.
- `DpdkUdpSocket<C>` — per-connection UDP socket with multicast helpers.
- `DpdkPoller<P>` — lcore burst-poll I/O driver.
- `Eal` — RAII EAL init/teardown.

Plus a rich internal-detail layer under `include/eph/dpdk/` (retained include
path) containing ARP / DNS / flow steering / TCP state
machine / packet templates / EAL platform primitives.

## How to read the code

Read the code in this order:

1. `include/eph/net/dpdk/eal.hpp` — trivial RAII wrapper. Warms you up to the
   `eph::net::dpdk` namespace.
2. `include/eph/net/dpdk/config.hpp` — config structs.
3. `include/eph/net/dpdk/udp_socket.hpp` — simpler than TCP; shows the `Datagram`
   concept satisfaction and multicast helpers.
4. `include/eph/net/dpdk/tcp_stream.hpp` — the interesting one. Read
   `create()`, `process_burst_()`, `send()` in order. TLS path uses
   `detail::TlsState` which wires the shared `eph::net::detail::TlsSession`
   to a DPDK byte-socket adapter.
5. `include/eph/net/dpdk/poller.hpp` — the lcore burst loop. Watch how `add()`
   erases the Pollable type into a function-pointer entry and how `poll()`
   dispatches `rte_eth_rx_burst` output through the flow-steering table.
6. `include/eph/net/dpdk/detail/mbuf_view.hpp` — the `PacketView` implementation
   with `writable_data()` support for in-place mutation.
7. `include/eph/dpdk/*` — internal detail only if you're debugging the TCP
   state machine or adding ARP / DNS / flow steering support.
8. `include/eph/dpdk/platform.hpp` — bottom of the file has the
   multi-process section. The only public MP entry point is
   `Platform::create_or_join(CreateOrJoinConfig)` (see
   `include/eph/dpdk/create_or_join.hpp`): peers race on EAL init,
   the winner becomes primary and runs the internal
   `primary_bringup_()` helper (configure / start the port, build
   the mempool, write the registry), losers attach as secondaries
   via `secondary_bringup_()` (CAS-claim a registry slot, look up
   the primary's mempool by name, skip configure/start/stop/close).
   Single-process callers use `Platform::create(PlatformConfig)`
   (rejects `max_procs > 1`) or the one-shot
   `Platform::launch(PlatformConfig, EalConfig, …)` that owns EAL
   too. `ProcType` and its `to_eal_string` serializer live in
   `include/eph/dpdk/proc_type.hpp` so `platform.hpp` and `eal.hpp`
   share one source of truth.

## Getting DPDK running on the host

See `../docs/dpdk-setup.md` for the full flow:

1. Install system libdpdk (`sudo pacman -S dpdk` / `sudo apt install libdpdk-dev`).
2. Allocate hugepages (`echo 1024 > .../nr_hugepages`).
3. Bind your secondary NIC to `vfio-pci`.
4. Build with `xmake -m release` — no compiler wrapper needed; system libdpdk
   lives in an isolated `/usr/include/dpdk/` tree so the previous vcpkg path's
   aws-lc / openssl header collision no longer applies.
5. Verify with a DPDK example: `sudo xmake run simple_hft -- --pci 0000:28:00.0 --pin 0=2 --local-ip … --gateway-ip … --host …`.

The latency benchmark wrapper (`../benchmarks/latency/lat tcp --dpdk`) is the
friendliest way to verify NIC-B state transitions end-to-end.

## Running the tests

```bash
xmake build -g tests
xmake run test_dpdk_tls_handshake     # TLS regression guard
xmake run test_dpdk_tls_state         # in-place decrypt state tests
xmake run test_dpdk_udp_multicast     # multicast join/leave

# Legacy unit tests for the internal primitives (still the coverage
# source for eph::dpdk::TcpSession<> / ARP / DNS / multicast / ...)
xmake run test_tcp                    # TCP state machine
xmake run test_arp / test_dns / ...

# Full end-to-end (needs NIC_B on vfio-pci, skips cleanly otherwise)
sudo tests/integration/dpdk_e2e
```

Per-file targets are auto-globbed. `tests/legacy/` contains the preserved
unit-level DPDK primitive coverage.

## Common tasks

### Using a different codec

Change the template parameter:

```cpp
using Raw = eph::net::dpdk::DpdkTcpStream<eph::codec::RawStreamCodec, false>;
using Ws  = eph::net::dpdk::DpdkTcpStream<eph::codec::WsCodec,       true >;
```

Each instantiation is a distinct monomorphised class.

### Running without TLS

Set `EnableTls=false`. The `detail::TlsState` member becomes `std::monostate`
and the encrypt/decrypt steps vanish via `if constexpr`. No runtime cost.

### Adding a new flow-steering rule

The flow-steering surface lives in `include/eph/net/dpdk/flow_steering.hpp`.
`install_flow_rule(port_id, queue_id, tuple, FlowProtocol)` returns a
move-only RAII `FlowRule` whose destructor calls `rte_flow_destroy`.
`DpdkTcpStream::create_and_attach` / `DpdkUdpSocket::create_and_attach`
install the rule when the Platform's `dispatch_mode()` is `FlowDirector`
and move the resulting `FlowRule` into the stream/socket's `flow_rule_`
member so teardown is automatic. Poller::add() does NOT touch flow rules —
the stream owns the rule's lifetime. Unit tests are in
`tests/test_flow_steering.cpp`.

### Running as a DPDK secondary process

Single-NIC multi-process setups let N processes share one port's mempool
via the DPDK `--proc-type=secondary` mechanism. The only public entry
point is `Platform::create_or_join(CreateOrJoinConfig)` — peers race
on EAL init, the winner runs the primary bring-up and writes a shared
hugepage registry, the rest attach as secondaries by reading that
registry:

```cpp
// Zero-coordination path: every peer issues the SAME call. The library
// resolves primary-vs-secondary post-EAL via rte_eal_process_type().
eph::dpdk::CreateOrJoinConfig cfg{
    .nic = eph::dpdk::PlatformConfig{
        .port_id      = 0,
        .nb_rx_queues = 4,
        .file_prefix  = "hft_app",
        // max_procs / queues_per_proc / file_prefix are read by the
        // autojoin race when this peer resolves to primary.
        .max_procs        = 2,
        .queues_per_proc  = 2,
    },
    .extra_eal_args = {},   // optional: passthrough EAL flags
};
auto plat = eph::dpdk::Platform::create_or_join(std::move(cfg));
```

The winner runs `primary_bringup_()` internally (validate config,
`rte_eth_dev_configure / rx_queue_setup / configure_rss / dev_start`,
build the mempool named `eph_mbuf_p<port>`, publish to the registry).
Losers run `secondary_bringup_()` (CAS-claim a registry slot, read NIC
state from primary's hugepage registry / live device, skip
configure/start/stop/close, attach via `rte_mempool_lookup`).

Source-port partitioning across MP processes is the **caller's**
responsibility — `eph-net-dpdk` does not auto-allocate src_port and has
no global view to enforce disjointness. Allocate disjoint sub-ranges
per process via `cfg.dpdk.wire.tuple.src_port` (TCP) or
`cfg.dpdk.wire.src_port` (UDP — UDP mirrors TCP's `dpdk` substruct
shape post Tier-1 audit follow-up). Read your peer's port window from
`Platform::port_range()` and use it to clamp your application-side
allocator.

See also: `../docs/dpdk-multiprocess.md` for startup ordering, the
1+N partitioning table, PMD caveats, and the orchestrator script;
`examples/dpdk_mp_demo.cpp` for a runnable single-file skeleton.

### Debugging TLS handshake failures

1. Enable debug logs: `xmake f -m debug && xmake build -g tests`.
2. Run `test_dpdk_tls_handshake`. It should pass on any host (no NIC binding
   required — uses a FakeStream).
3. If the real DPDK path fails at `DpdkTcpStream::create_and_attach()`,
   check `eph::net::detail::tls_session.hpp` for the handshake state
   machine. (The older `create(cfg, poller)` overload was removed —
   its narrow subset is covered by `create_and_attach`.)
4. If you see duplicate `RSA_*` / `CRYPTO_THREADID` symbols at link time, the
   build is picking up a second openssl implementation somewhere on the
   include / link path. Check your DPDK came from system libdpdk (not vcpkg)
   and that `pkg-config --cflags libdpdk` does not point inside a vcpkg tree.

## See also

- `README.md`
- `summary.md`
- `CHANGELOG.md`
- `../docs/dpdk-setup.md`
- `../docs/poller-guide.md`
- `../docs/architecture.md`
