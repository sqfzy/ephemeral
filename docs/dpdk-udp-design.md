# DPDK UDP design — deltas from kernel backend

Reference for users porting `KernelUdpSocket`-based code to the DPDK
backend, or reviewers trying to decide which backend fits a given
workload. Generated from
`eph-net-dpdk/include/eph/net/dpdk/udp_socket.hpp` and
`eph-net-kernel/include/eph/net/kernel/udp_socket.hpp`.

Both backends satisfy the same `eph::net::Datagram<T>` concept, so
application code compiled against the concept is portable. The
operational semantics differ in three meaningful ways (fixed-peer,
no broadcast, multicast + connect_to interaction); the rest is
equivalent.

---

## 1. Summary table

| Capability                          | `KernelUdpSocket`            | `DpdkUdpSocket`              |
|-------------------------------------|------------------------------|------------------------------|
| `send_to(payload, dst)`             | arbitrary `dst` per call     | `dst` must match configured peer |
| `connect_to(peer)` filter           | kernel-level connected mode  | RX-side tuple filter (TX peer fixed) |
| Unconfigured-destination send       | ✅ Yes (SOCK_DGRAM is unconnected by default) | ❌ Rejected with `InvalidConfig` |
| Broadcast (255.255.255.255)         | ✅ Via `SO_BROADCAST`        | ❌ Not supported             |
| Multicast join / leave              | ✅ `IP_ADD_MEMBERSHIP` on INADDR_ANY | ✅ NIC MAC filter via `rte_eth_dev_set_mc_addr_list` |
| Max multicast groups                | Kernel limit (~20 default)   | 8 (per-socket, hardcoded)    |
| Outbound IP fragmentation           | kernel performs if payload > MTU | rejected at API boundary (65493-byte cap) |
| Inbound IP fragment reassembly      | kernel performs               | ❌ Fragments rejected at parse layer |
| RX checksum validation              | kernel performs silently     | opt-in via `PlatformConfig::enable_rx_checksum_offload` |
| Drop-cause metrics                  | N/A (OS owns it)             | `kRxBadChecksum` / `kPacketsDropped` / `kFragmentRejected` |

---

## 2. Why fixed-peer

`DpdkUdpSocket` is built on `eph::dpdk::UdpSender`, which pre-computes
a 42-byte Ethernet + IPv4 + UDP header template at `create()` time.
`send()` then only has to copy the template + user payload + fix up
length + IP ID, skipping all per-packet header construction (~2.5 ns
baseline per the RX hot-path bench). This is the whole point of the
DPDK backend — zero-allocation, zero-branching TX hot path.

Consequences:

- The destination IP / port / MAC are **burned into the template**.
  Changing them mid-stream means throwing away the template and
  building a new one — cheaper to just construct a new socket.
- `send_to(payload, dst)` validates that `dst` equals
  `cfg.legacy.dst_{ip,port}` and rejects mismatches with
  `InvalidConfig`. This is not "we haven't implemented it yet" — it
  is the **contract**. If you need to fan out to many peers, make
  one `DpdkUdpSocket` per peer.

### 2.1 Port shape: one socket per peer

```
  Kernel backend (flexible):
                 ┌──────────────────────┐
      app ──────►│  KernelUdpSocket     │
                 │  (unconnected)       │──► sendto(A)
                 │                      │──► sendto(B)
                 │                      │──► sendto(C)
                 └──────────────────────┘

  DPDK backend (fixed-peer — one socket per destination):
                 ┌──────────────────────┐
      app ──┬───►│  DpdkUdpSocket(A)    │──► TX to A (precomputed)
            │    └──────────────────────┘
            │    ┌──────────────────────┐
            ├───►│  DpdkUdpSocket(B)    │──► TX to B
            │    └──────────────────────┘
            │    ┌──────────────────────┐
            └───►│  DpdkUdpSocket(C)    │──► TX to C
                 └──────────────────────┘
```

For the intended HFT workloads this is natural: you typically have
one market-data feed + one order channel + one heartbeat endpoint,
each a distinct socket.

---

## 3. No broadcast

The kernel backend can broadcast by setting `SO_BROADCAST` and
sending to `255.255.255.255` or a subnet broadcast address. The DPDK
backend does not expose this — there is no `SO_BROADCAST` equivalent
in DPDK-space, and the pre-computed template commits to a specific
unicast destination MAC at create time.

Workaround (if ever needed): construct the broadcast frame manually
via `eph::dpdk::net::UdpPacketTemplate::init(..., dst_mac=FF:FF:FF:FF:FF:FF, ...)`.
Not supported through the concept-conforming public API.

Rationale: HFT traffic is either unicast (orders) or multicast
(market data). Broadcast has no standing use case.

---

## 4. Multicast + connect_to interaction

Multicast on `DpdkUdpSocket`:

1. `join_multicast(group)` — derives the multicast MAC per RFC 1112
   (0x01:00:5E + low-23-bits of group IP), appends to per-socket
   MAC list (max 8), pushes to NIC via `rte_eth_dev_set_mc_addr_list`.
2. The NIC admits frames with those destination MACs into the RX
   queue.
3. `DpdkPoller` dispatches the frame to a registered Pollable whose
   5-tuple matches.

Now, `connect_to(peer)` **does not** disable multicast reception in
the usual sense. It latches an **RX-side source filter** in the
socket: `process_burst_` drops any mbuf whose source address does
not match `connected_peer_`. Multicast packets carry whichever peer
IP actually sourced them on the multicast group — usually the feed
publisher's unicast IP.

### 4.1 The subtle case

If you:
- Call `connect_to(publisher_ip, publisher_port)` AND
- Call `join_multicast(group)`

... then only multicast packets whose `ip.src_addr == publisher_ip`
pass through the filter. This is **usually what you want** for a
market-data feed where you know the publisher's IP. If the feed has
multiple publishers (A/B feed failover), `connect_to` will drop the
backup publisher's traffic.

```
   feed publisher A (src=10.0.0.100)  ────► multicast group ─────┐
   feed publisher B (src=10.0.0.101)  ────► multicast group ─────┤
                                                                 │
                                                                 ▼
                                                       DpdkUdpSocket
                                                       (joined group,
                                                        connect_to(A))
                                                                 │
                                                                 ▼
                                             A frames delivered to on_datagram
                                             B frames dropped + kPacketsDropped++
```

If you want both publishers → don't call `connect_to`. The 5-tuple
Poller routing is enough to send the group traffic to the right
socket, and `on_datagram` will fire for both publishers — each
callback carries the source address as its second argument so you
can distinguish them yourself.

Multicast is idempotent: joining the same group twice is a no-op,
and leaving a group you never joined is a no-op.

---

## 5. Outbound payload cap

DPDK backend rejects `send_to` payloads > 65493 bytes at the API
boundary (65535 max IP frame minus 42 Ethernet+IP+UDP header bytes;
see `udp_socket.hpp` `kMaxUdpPayload`). This is a hard cap — the
kernel backend would silently fragment at IP level, but DPDK sets
the Don't-Fragment bit and does not implement outbound fragmentation.

If you need to send > 65493 bytes, fragment at the application layer
(your codec's problem, not the socket's). In practice HFT payloads
are always < 1500 bytes; this limit exists to surface misuse fast
rather than tolerate it.

---

## 6. RX observability

Inbound drop attribution is explicit and counted in `StreamMetric`:

| Counter                 | When it fires |
|-------------------------|---------------|
| `kRxBadChecksum`        | NIC marked `RTE_MBUF_F_RX_IP_CKSUM_BAD` or `RTE_MBUF_F_RX_L4_CKSUM_BAD` — requires `PlatformConfig::enable_rx_checksum_offload=true` |
| `kFragmentRejected`     | IPv4 MF=1 or non-zero offset (any fragment) |
| `kPacketsDropped`       | Catch-all: non-IPv4 ethertype, truncated, bad IHL, multi-segment mbuf, UDP/IP length mismatch, `connect_to` source-filter mismatch |
| `kCodecErrors`          | Codec returned error after successful parse |

Kernel backend exposes none of these — the OS silently drops
malformed packets before userspace. DPDK backend surfaces them so
operators can distinguish "NIC is flipping bits" from "wrong peer
sending to us" from "codec bug".

---

## 7. When to pick which

**Pick `KernelUdpSocket` when**:
- Throughput / latency requirements fit inside OS stack overhead
  (typical < 50 kHz / > 10 µs budget).
- You need arbitrary-destination `send_to`, broadcast, or transparent
  kernel-side fragmentation.
- You don't want to manage EAL / hugepages / vfio-pci binding.

**Pick `DpdkUdpSocket` when**:
- You need sub-µs send latency or > 1 Mpps ingress.
- Your destinations are fixed at application start (one socket per
  peer is acceptable).
- You can configure the NIC for kernel bypass (vfio-pci, IOMMU,
  hugepages).
- Observability on "why was this packet dropped" matters.

Both satisfy `eph::net::Datagram<T>`; application code conformant
to the concept is portable — only the factory and construction
config differ. See `docs/architecture.md` for the concept model.

---

## See also

- [`../eph-net-dpdk/README.md`](../eph-net-dpdk/README.md) — module
  entry point + thread model
- [`dpdk-tcp-implementation.md`](dpdk-tcp-implementation.md) — TCP
  state machine, reorder buffer, keepalive, ICMP feedback
- [`architecture.md`](architecture.md) — `Stream` / `Datagram` /
  `Poller` concept model
- [`poller-guide.md`](poller-guide.md) — how the Poller drives
  `process_burst_`
- [`dpdk-setup.md`](dpdk-setup.md) — hugepages, vfio-pci, EAL
- `eph-net-dpdk/include/eph/net/dpdk/udp_socket.hpp` — source of truth
- `eph-net-kernel/include/eph/net/kernel/udp_socket.hpp` — kernel
  counterpart
