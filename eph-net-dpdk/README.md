# eph-net-dpdk

Header-only C++23 DPDK networking backend. Implements the `eph::net::Stream` /
`Datagram` / `Poller` concepts on top of DPDK kernel-bypass I/O. Introduced in
v3.3 Phase 4 as the successor to the legacy `eph-dpdk` module (same code, new
name and namespace shape).

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
- `rx_dispatcher.hpp` — legacy internal burst dispatcher; `DpdkPoller` uses similar machinery
- `arp.hpp`, `dns.hpp` — link-layer resolution helpers
- `flow_steering.hpp` — RSS + RTE flow rule management
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

Builds that combine `eph-net-dpdk` with aws-lc on hosts where vcpkg brings its own
libssl need the `/tmp/gcc14-wrap/g++` compiler wrapper to reorder `-isystem` /
`-L` flags. See the Phase 7 commit message (`c2a0ca4`) for the rationale.

DPDK environment setup (hugepages, vfio-pci binding) is documented in
[`../docs/dpdk-setup.md`](../docs/dpdk-setup.md).

## Testing

`eph-net-dpdk/tests/` holds:

- `test_dpdk_tls_handshake.cpp` — regression guard for the Phase 7 TLS
  unblocking (`DpdkTcpStream<C, true>::create()` runs a real TLS 1.3 handshake).
- `test_dpdk_tls_state.cpp` — in-place decrypt state tests.
- `test_dpdk_udp_multicast.cpp` — UDP multicast join/leave tests.
- `tests/integration/test_dpdk_e2e.cpp` — full kernel-mock → DPDK-client suite.
- `tests/legacy/` — unit tests preserved from the legacy `eph-dpdk` module. They
  cover the internal `eph::dpdk::*` primitives (ARP, DNS, flow steering, TCP
  state machine, net header helpers). These aren't "legacy" in the sense of
  "to be deleted" — they're the unit-level coverage for the internal detail
  layer the v3.3 types wrap.

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

- [`../docs/dpdk-setup.md`](../docs/dpdk-setup.md)
- [`../docs/poller-guide.md`](../docs/poller-guide.md)
- [`../docs/architecture.md`](../docs/architecture.md)
- `.artifacts/design-eph-v3.3-architecture-20260410.md` — design spec
