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
   multi-process section (`ProcType`, `PlatformConfig::proc_type /
   file_prefix / rx_queue_range`, and the `create_primary` /
   `create_secondary` factories). `create_secondary` runs
   `validate_config` plus the secondary-only contract (non-empty
   `file_prefix`, `rte_eth_dev_is_valid_port`), then attaches via
   `rte_mempool_lookup` and skips the primary-only port-bringup path.
   `ProcType` and its `to_eal_string` serializer live in
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

Single-NIC multi-process setups let two processes share one port's mempool
via the DPDK `--proc-type=secondary` mechanism. `Platform::create_secondary`
enforces the secondary contract synchronously and then performs the real
shared-mempool attach:

```cpp
// Recommended path: declare an MpTopology, let the library derive
// rx_queue_range / src_port windows and cross-validate via the shared
// hugepage registry. Two numbers per process — no other coordination.
eph::dpdk::PlatformConfig cfg{
    .port_id      = 0,
    .nb_rx_queues = 4,
    .proc_type    = eph::dpdk::ProcType::Secondary,   // or use create_secondary
    .file_prefix  = "hft_app",                         // must match primary's EAL --file-prefix
    .mp_topology  = eph::dpdk::MpTopology::uniform(
                        /*self_index=*/1, /*total_procs=*/2,
                        /*nb_rx_queues=*/4),
};
auto plat = eph::dpdk::Platform::create_secondary(std::move(cfg));

// Legacy hand-partitioned alternative (mp_topology empty, manual
// rx_queue_range — see "Advanced usage" in dpdk-multiprocess.md):
//   .rx_queue_range = {2, 4},   // disjoint from primary's [0, 2)
```

`create_secondary` runs `validate_config` (which polices
`rx_queue_range`: either the `{0,0}` full-range sentinel or a non-empty
sub-range bounded by `nb_rx_queues`) and the secondary-only checks
(non-empty `file_prefix`, `rte_eth_dev_is_valid_port`). It then calls
`rte_mempool_lookup("eph_mbuf_p<port>")` and skips
`rte_eth_dev_configure / rx_queue_setup / configure_rss / dev_start`
entirely — those are primary-only. Primary callers should use
`create_primary` for symmetry and call-site clarity.

Source-port partitioning across MP processes:

  * **With `mp_topology` set + `Stream::create_and_attach` /
    `Socket::create_and_attach`** — the library auto-narrows
    `find_src_port_for_queue`'s search to this process's
    `MpTopology::self().port_lo / port_hi` window via
    `Platform::self_port_range()`. Primary and secondary draw from
    disjoint segments without manual coordination.
  * **Without `mp_topology` (legacy / manual partition path)** — the
    caller is responsible: allocate disjoint sub-ranges per process
    via `cfg.dpdk.tcp_low_level.tuple.src_port` (TCP) or
    `cfg.legacy.src_port` (UDP — the `legacy` substruct is
    intentionally retained on `eph::net::dpdk::UdpConfig` per T3.19's
    TCP-only reshape scope). `eph-net-dpdk` has no global view to
    enforce disjointness on this path.

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
