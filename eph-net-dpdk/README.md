# eph-net-dpdk

Header-only C++23 DPDK networking backend. Implements the `eph::net::Stream` /
`Datagram` / `Poller` concepts on top of DPDK kernel-bypass I/O. Successor to the
legacy `eph-dpdk` module (same code, new name and namespace shape).

## Types

| Class | Header | Concept |
|---|---|---|
| `eph::net::dpdk::DpdkTcpStream<C, EnableTls>` | `eph/net/dpdk/tcp_stream.hpp` | `eph::net::Stream` |
| `eph::net::dpdk::DpdkUdpSocket<C>` | `eph/net/dpdk/udp_socket.hpp` | `eph::net::Datagram` |
| `eph::net::dpdk::DpdkPoller<P>` | `eph/net/dpdk/poller.hpp` | `eph::net::Poller` |
| `eph::net::dpdk::Eal` | `eph/net/dpdk/eal.hpp` | RAII EAL init/teardown |

All four live under `namespace eph::net::dpdk` and use `eph::net::dpdk::detail::MbufView`
as their `PacketView` associated type — backed by an `rte_mbuf` with pool-managed
headroom so TLS decrypt can run in place via aws-lc's
`EVP_AEAD_CTX_open_scatter`.

## Internal detail layer

`eph-net-dpdk` wraps a rich set of low-level DPDK primitives that live under
`include/eph/dpdk/` (retained include path for historical continuity):

- `eal.hpp` — raw EAL init (the `Eal` RAII wrapper in `eph::net::dpdk` uses this)
- `tcp.hpp` — `DpdkTcpSession` (the TCP state machine `DpdkTcpStream` wraps)
- `udp.hpp` — UDP sender primitives
- `arp.hpp`, `dns.hpp` — link-layer resolution helpers
- `flow_steering.hpp` — moved to `eph/net/dpdk/flow_steering.hpp`; provides
  RSS / RTE flow rule management and the Toeplitz hash predictor that
  `DpdkTcpStream::create_and_attach` uses for queue pinning
- `packet_template.hpp` — pre-computed UDP+IP header templates
- `multicast.hpp` — multicast group management
- `net_header.hpp` — Ethernet/IP/TCP header packing
- `platform.hpp` — EAL platform wrapper

User code should only touch the `eph::net::dpdk::*` public surface. The `eph::dpdk::*`
types are implementation detail and may change without notice.

## Usage

```cpp
#include "eph/net/dpdk/eal.hpp"
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/dpdk/tcp_stream.hpp"
#include "eph/net/dpdk/udp_socket.hpp"
#include "eph/codec/ws_codec.hpp"
#include "eph/codec/mold64_codec.hpp"

namespace en = eph::net::dpdk;
namespace ec = eph::codec;

int main(int argc, char** argv) {
    en::Eal eal{argc, argv};         // RAII EAL init

    auto poller = en::DpdkPoller<>::create({
        .port_id = 0, .queue_id = 0, .lcore = 4,
    }).value();

    // TLS WebSocket order channel over DPDK TCP
    auto orders = en::DpdkTcpStream<ec::WsCodec>::create(order_cfg).value();
    orders->on_message = handle_exec_report;
    poller->add(orders.get()).value();

    // MoldUDP64 market data over DPDK UDP multicast
    auto md = en::DpdkUdpSocket<ec::Mold64Codec>::create(md_cfg).value();
    md->on_datagram = handle_itch_message;
    md->join_multicast(mcast).value();
    poller->add(md.get()).value();

    while (running) poller->poll();   // lcore burst
}
```

## Thread model

`eph-net-dpdk` uses a **one-lcore-per-Poller** model — each Poller
runs on a dedicated lcore and owns its RX queue exclusively. The
only cross-lcore interaction is ICMP dispatch (because ICMP Type 3
Code 4 may land on any RSS queue regardless of which lcore owns the
target TCP session).

```
                ┌──────────────────────┐
                │   Control thread     │
                │ (app main / setup)   │
                │                      │
                │  - Eal construction  │
                │  - Platform::create  │
                │  - ARP resolve       │
                │  - DNS lookups       │
                │  - Stream::create()  │
                │  - Poller::add()     │
                │  - install_flow_rule │
                └──────────┬───────────┘
                           │ (setup only;
                           │  no steady-state
                           │  traffic)
                           ▼
 ┌───────────────────────────────────────────────────────┐
 │                  ICMP registry (shared)               │
 │         eph::dpdk::detail::IcmpRegistry               │
 │  std::shared_ptr + std::mutex — safe across lcores    │
 └─────────┬──────────────┬──────────────┬───────────────┘
           │              │              │
           │ dispatch     │ dispatch     │ dispatch
           │ on Type 3    │ on Type 3    │ on Type 3
           │ Code 4       │ Code 4       │ Code 4
           │              │              │
           ▼              ▼              ▼
  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
  │ Lcore 1      │ │ Lcore 2      │ │ Lcore 3      │
  │ (RX queue 0) │ │ (RX queue 1) │ │ (RX queue 2) │
  │              │ │              │ │              │
  │ DpdkPoller   │ │ DpdkPoller   │ │ DpdkPoller   │
  │   ├─ TCP A   │ │   ├─ TCP B   │ │   ├─ UDP md  │
  │   ├─ TCP C   │ │   └─ TCP D   │ │   └─ …       │
  │   └─ UDP ord │ │              │ │              │
  │              │ │              │ │              │
  │ poll() burst │ │ poll() burst │ │ poll() burst │
  │ process_rx   │ │ process_rx   │ │ process_rx   │
  │ on_poll_tick │ │ on_poll_tick │ │ on_poll_tick │
  └──────────────┘ └──────────────┘ └──────────────┘
```

### Per-lcore invariants

Each lcore's Poller / Streams are **strictly single-threaded** —
no locks inside the hot path, no atomic RMW on session state.
`TcpSession` / `DpdkUdpSocket` internals are not thread-safe by
design (see `tcp.hpp` `@note Not thread-safe`).

### Cross-lcore boundary: ICMP

The only reason a packet from another lcore's RX queue reaches a
given session is the ICMP path-MTU feedback. Platform's
`icmp_registry_` maps `(src_ip, dst_ip, src_port, dst_port,
protocol)` 5-tuples to `weak_ptr<IcmpTargetHandle>`. On receipt of
ICMP Type 3 Code 4 anywhere, the owning Poller calls into the
registry (mutex-acquired); the matching handle's
`on_icmp_frag_needed` is invoked, which internally updates
`effective_mss_` via the atomic exchange — safe because that single
field is the only cross-lcore mutation.

Lifecycle ordering (registry predeceases Platform → Stream's
`IcmpTargetHandle` weak_ptr fails gracefully; session lives past
registry → handle expires cleanly on ~Registry). ASan + TSan
verified via `test_icmp_registry.cpp`.

See [`../docs/dpdk-tcp-implementation.md`](../docs/dpdk-tcp-implementation.md)
for the full TCP state machine, reorder buffer semantics, delayed-ACK,
and no-retransmit contract, and
[`../docs/dpdk-udp-design.md`](../docs/dpdk-udp-design.md) for the
UDP design deltas vs the kernel backend.

## Build requirements

DPDK PMDs need whole-archive linking. Targets that link `eph-net-dpdk` must call
`apply_dpdk_pmd_linkgroups()` (defined in the root `xmake.lua`):

```lua
target("my_hft_app")
    set_kind("binary")
    add_files("main.cpp")
    add_deps("eph-net-dpdk", "eph-codec")
    apply_dpdk_pmd_linkgroups()
```

DPDK is sourced from the distribution's system package via pkg-config
(`sudo pacman -S dpdk` on Arch, `sudo apt install libdpdk-dev` on Ubuntu, or build
from source). This isolates DPDK's headers under `/usr/include/dpdk/` so they do
not collide with aws-lc's openssl headers — the previous `/tmp/gcc14-wrap/g++`
wrapper needed by the vcpkg path is no longer required.

DPDK environment setup (hugepages, vfio-pci binding) is documented in
[`../docs/dpdk-setup.md`](../docs/dpdk-setup.md).

## Testing

`eph-net-dpdk/tests/` holds:

- `test_dpdk_tls_handshake.cpp` — regression guard for TLS
  (`DpdkTcpStream<C, true>::create()` runs a real TLS 1.3 handshake).
- `test_dpdk_tls_state.cpp` — in-place decrypt state tests.
- `test_dpdk_udp_multicast.cpp` — UDP multicast join/leave tests.
- `tests/integration/test_dpdk_e2e.cpp` — full kernel-mock → DPDK-client suite.
- `tests/legacy/` — unit tests preserved from the legacy `eph-dpdk` module. They
  cover the internal `eph::dpdk::*` primitives (ARP, DNS, flow steering, TCP
  state machine, net header helpers). These aren't "legacy" in the sense of
  "to be deleted" — they're the unit-level coverage for the internal detail
  layer the public types wrap.

```bash
xmake build -g tests
xmake run test_dpdk_tls_handshake
xmake run test_dpdk_tls_state
xmake run test_dpdk_udp_multicast

# E2E suite (requires NIC_B bound to vfio-pci; skips cleanly otherwise)
sudo tests/integration/dpdk_e2e
```

## Dependencies

- `eph-net` (public) — concepts + shared TLS / WS detail
- `eph-codec` (optional, application-layer) — for codecs
- `dpdk` (public)
- `aws-lc` (transitive via `eph-net` TLS detail)
- `eph-core`, `eph-utils`, `eph-containers` (transitive)

## See also

- [`../docs/dpdk-tcp-implementation.md`](../docs/dpdk-tcp-implementation.md) — TCP state machine, reorder buffer, delayed-ACK, no-retransmit contract
- [`../docs/dpdk-udp-design.md`](../docs/dpdk-udp-design.md) — UDP design deltas vs kernel backend (fixed-peer, no broadcast, multicast + connect_to interaction)
- [`../docs/dpdk-setup.md`](../docs/dpdk-setup.md)
- [`../docs/poller-guide.md`](../docs/poller-guide.md)
- [`../docs/architecture.md`](../docs/architecture.md)
- `.artifacts/design-eph-v3.3-architecture-20260410.md` — design spec
