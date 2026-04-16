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

## Getting DPDK running on the host

See `../docs/dpdk-setup.md` for the full flow:

1. Install system libdpdk (`sudo pacman -S dpdk` / `sudo apt install libdpdk-dev`).
2. Allocate hugepages (`echo 1024 > .../nr_hugepages`).
3. Bind your secondary NIC to `vfio-pci`.
4. Build with `xmake -m release` — no compiler wrapper needed; system libdpdk
   lives in an isolated `/usr/include/dpdk/` tree so the previous vcpkg path's
   aws-lc / openssl header collision no longer applies.
5. Verify with a DPDK example: `sudo xmake run simple_hft_dpdk -- --host …`.

The latency benchmark wrapper (`../benchmarks/latency/lat tcp --dpdk`) is the
friendliest way to verify NIC-B state transitions end-to-end.

## Running the tests

```bash
xmake build -g tests
xmake run test_dpdk_tls_handshake     # TLS regression guard
xmake run test_dpdk_tls_state         # in-place decrypt state tests
xmake run test_dpdk_udp_multicast     # multicast join/leave

# Legacy unit tests for the internal primitives (still the coverage
# source for eph::dpdk::DpdkTcpSession / RxDispatcher / ARP / DNS / ...)
xmake run test_tcp                    # TCP state machine
xmake run test_reactor                # legacy reactor (internal detail)
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

Edit `include/eph/dpdk/flow_steering.hpp`. The `FlowSteeringTable` owns a set
of `rte_flow` rules keyed by 5-tuple; the `DpdkPoller::add()` path inserts
into it. Unit tests are in `tests/legacy/test_flow_steering.cpp`.

### Debugging TLS handshake failures

1. Enable debug logs: `xmake f -m debug && xmake build -g tests`.
2. Run `test_dpdk_tls_handshake`. It should pass on any host (no NIC binding
   required — uses a FakeStream).
3. If the real DPDK path fails at `DpdkTcpStream::create()`, check
   `eph::net::detail::tls_session.hpp` for the handshake state machine.
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
