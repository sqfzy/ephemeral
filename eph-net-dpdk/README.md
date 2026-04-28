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

For single-NIC multi-process deployments (one primary + N secondaries
sharing the mempool), see `eph::dpdk::Platform::create_primary` /
`create_secondary` plus `eph::dpdk::EalConfig` / `build_eal_argv` in
`eph/dpdk/platform.hpp` and `eph/dpdk/eal.hpp`. The `PlatformConfig::
proc_type` / `file_prefix` / `rx_queue_range` fields default to single-
process / primary so existing code is byte-for-byte compatible. Full
contract and PMD caveats: [`docs/dpdk-multiprocess.md`](docs/dpdk-multiprocess.md).

For lcore × application-thread cpu pinning (so `pin_thread` can detect
SMT / NUMA conflicts against running EAL lcores), use the typed
`eph::dpdk::LcorePin` + `EalGuard::init_with_pins` API in
`eph/dpdk/lcore_pin.hpp` and `eph/dpdk/eal.hpp`. Full rationale and
escape-hatch rules: [`docs/lcore-pin-integration.md`](docs/lcore-pin-integration.md).

When running with RSS multi-queue dispatch (`Platform::is_rss_active() == true`),
the blocking control-plane APIs (`dns::resolve`, `arp::resolve`,
`MulticastReceiver`) need a small contract to route their replies back
to the caller's queue. DNS reverse-picks a hashed src_port, ARP hardcodes
queue 0, Multicast fail-fasts unless single-queued or FlowDirector-pinned.
Full integration story: [`docs/rss-control-plane.md`](docs/rss-control-plane.md).

## Internal detail layer

`eph-net-dpdk` wraps a rich set of low-level DPDK primitives that live under
`include/eph/dpdk/` (retained include path for historical continuity):

- `eal.hpp` — raw EAL init (the `Eal` RAII wrapper in `eph::net::dpdk` uses this)
- `tcp.hpp` — `TcpSession<ReorderSlots=64>` (the TCP state machine `DpdkTcpStream` wraps)
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
    // `Eal::init` returns a move-only RAII guard that runs rte_eal_cleanup
    // on scope exit. There is no bare `Eal(argc, argv)` ctor — the factory
    // is the only public entry so the `std::expected` error contract is
    // honoured (EAL init can legitimately fail: missing hugepages, bad
    // --vdev, etc.).
    auto eal = en::Eal::init(argc, argv).value();

    // `PollerConfig` has two fields: `port_id` and `rx_queue_id`. lcore
    // affinity is the caller's responsibility (DpdkPoller does not spawn
    // a thread of its own — you call `poll()` from your own lcore loop).
    auto poller = en::DpdkPoller<>::create({
        .port_id = 0, .rx_queue_id = 0,
    }).value();

    // TLS WebSocket order channel over DPDK TCP
    auto orders = en::DpdkTcpStream<ec::WsCodec>::create(order_cfg).value();
    orders->on_message = handle_exec_report;
    poller->add(orders.get()).value();

    // MoldUDP64 market data over DPDK UDP multicast
    auto md = en::DpdkUdpSocket<ec::Mold64Codec>::create(md_cfg).value();
    md->on_datagram = handle_itch_message;
    md->join_multicast(mcast_addr).value();
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

`eph-net-dpdk/tests/` holds the public-surface tests (top-level) plus
the preserved internal-primitive tests under `tests/legacy/`.

### Public-surface tests (top-level `tests/`)

| Binary                             | Covers                                                                  |
|------------------------------------|-------------------------------------------------------------------------|
| `test_dpdk_poller`                 | `DpdkPoller` add / remove / dispatch / ICMP fallback / src_port picker  |
| `test_dpdk_tcp_stream`             | `DpdkTcpStream` create / send / close / metric / attached state         |
| `test_dpdk_udp_socket`             | `DpdkUdpSocket` create / send_to / connect_to / metric                  |
| `test_dpdk_udp_multicast`          | UDP multicast join / leave / mcast MAC registration                     |
| `test_dpdk_tls_handshake`          | `DpdkTcpStream<C, true>::create()` real TLS 1.3 handshake               |
| `test_dpdk_tls_state`              | In-place decrypt state (`detail::TlsState`)                             |
| `test_dpdk_tls_desync`             | TLS partial-send desync latch                                           |
| `test_dpdk_ws_handshake_timeout`   | WS upgrade timeout path                                                 |
| `test_dpdk_ws_sink`                | WS codec → `DpdkTcpStream` sink integration                             |
| `test_dpdk_reasm_overflow`         | Reassembly capacity exhaustion + RX cksum TD-6 precise-mask cases       |
| `test_dpdk_fault_tolerance`        | Keepalive exhaustion / link-down / reconnect policy                     |
| `test_flow_steering`               | RSS + `install_flow_rule` / `FlowRule` RAII + Toeplitz queue predictor  |

### Integration tests (`tests/integration/`)

| Binary               | Covers                                                              |
|----------------------|---------------------------------------------------------------------|
| `dpdk_e2e`           | Full kernel-mock → DPDK-client suite (all P0+P1 cases)              |
| `test_dpdk_rss_platform` | RSS / RETA / queue-pinning behaviour against a live DPDK port    |
| `dpdk_mp_primary`    | Primary half of the multi-process E2E (driven by `dpdk_mp_e2e.sh`)  |
| `dpdk_mp_secondary`  | Secondary half: attaches via shared mempool, owns its sub-range     |
| `dpdk_mp_e2e.sh`     | Coordinator script — launches primary, waits, launches secondary, asserts both see their owned queue / src_port range; skips with exit 77 if env vars absent / NIC unbound / hugepages low |

### Legacy tests (`tests/legacy/`)

Unit-level coverage for the internal `eph::dpdk::*` primitives (ARP,
DNS, flow steering, TCP state machine, net-header helpers, ICMP
registry, packet parse, packet core, multicast). Preserved from the
pre-rename `eph-dpdk` module — **not deprecated**, they are the
source of truth for the detail layer the public types wrap.

```bash
xmake build -g tests
xmake run test_dpdk_poller
xmake run test_dpdk_tls_handshake
xmake run test_dpdk_udp_multicast
xmake run test_flow_steering
# ... any of the above

# E2E suite (requires NIC_B bound to vfio-pci; skips cleanly otherwise)
sudo tests/integration/dpdk_e2e
```

## Benchmarks

`eph-net-dpdk/benchmarks/` contains Google Benchmark microbenchmarks for
the DPDK hot paths. Auto-globbed via the `eph-bench` rule; build with
`xmake build -g benchmarks`.

| Binary                    | Focus                                                        |
|---------------------------|--------------------------------------------------------------|
| `bench_rx_hot_path`       | Parser chain on the RX hot path (checksum + parse + dispatch) |
| `bench_tcp_header`        | TCP header build / checksum                                  |
| `bench_udp`               | UDP send / receive primitives                                |
| `bench_multicast`         | Multicast group membership updates                           |
| `bench_dns_codec`         | DNS reply parse                                              |
| `bench_memcpy_compare`    | `rte_memcpy` vs `std::memcpy` on mbuf-sized spans            |
| `bench_rte_ring_vs_bq`    | `rte_ring` MPMC vs alternative bounded-queue baselines       |
| `bench_matrix`            | Matrix driver (shared header)                                |

The RX hot-path baseline lives in
`.artifacts/bench-rx-hot-path-20260423.txt`. Re-run with
`scripts/check-rx-hot-path-regression.sh` (default 5% threshold, exit
code 1 on regression) — suitable as a pre-PR gate or local canary.

## Fuzzers

`eph-net-dpdk/fuzzers/` ships four libFuzzer harnesses for the
zero-heap parsers. Intentionally **outside the xmake graph** —
the default toolchain is GCC 14, but libFuzzer needs Clang ≥ 17.

| Harness              | Target                                       |
|----------------------|----------------------------------------------|
| `fuzz_dns_reply`     | `eph::dpdk::dns::detail::parse_dns_response` |
| `fuzz_arp_reply`     | `eph::dpdk::arp::parse_arp_reply`            |
| `fuzz_icmp_reply`    | `parse_icmp` + `parse_ip_header` + `is_ip_fragment` |
| `fuzz_udp_packet`    | `parse_udp_packet` + `parse_udp_from_ip` + `parse_tcp_from_ip` |

Build + run instructions are in
[`fuzzers/README.md`](./fuzzers/README.md) (each `clang++
-fsanitize=fuzzer,address,undefined` invocation + seed corpus use).
Seed corpora (`fuzzers/corpus/<harness>/`) are version-controlled;
minimized crash reproducers should be committed alongside the fix.

## Scripts

`eph-net-dpdk/scripts/` holds operational helpers:

| Script                                  | Purpose                                                        |
|-----------------------------------------|----------------------------------------------------------------|
| `dpdk-setup.sh`                         | Hugepages + vfio-pci bind (idempotent)                         |
| `dpdk-teardown.sh`                      | Reverse `dpdk-setup.sh` — restore NIC to kernel driver         |
| `check-rx-hot-path-regression.sh`       | Diff `bench_rx_hot_path` vs baseline, exit 1 on regression     |

All three are idempotent and dry-run-safe (see
[`../docs/dpdk-setup.md`](../docs/dpdk-setup.md) for the hugepages /
vfio-pci flow).

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
- [`docs/dpdk-multiprocess.md`](docs/dpdk-multiprocess.md) — single-NIC primary+secondary attach, `EalConfig` argv assembly, queue/src-port partitioning rules, PMD caveats
- [`../docs/poller-guide.md`](../docs/poller-guide.md)
- [`../docs/architecture.md`](../docs/architecture.md)
- `.artifacts/design-eph-v3.3-architecture-20260410.md` — design spec
