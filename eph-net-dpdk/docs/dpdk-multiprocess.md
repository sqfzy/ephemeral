# eph-net-dpdk multi-process model

`eph-net-dpdk` runs multi-process (multiple independent applications
sharing one physical NIC) via a **daemon-led** model. A long-lived
`eph-nicd` daemon owns each NIC as the DPDK primary; tenant
applications attach as DPDK secondaries. There is no "first peer
becomes primary" race, no shared `max_procs` / `file_prefix` fields
that tenants must agree on, and no tenant ever runs as primary.

This document describes the architecture. For the deployment story
(toml schema, systemd, upgrades) see
[`dpdk-daemon-deployment.md`](dpdk-daemon-deployment.md). For the
tenant-side reconnect protocol see
[`dpdk-reconnect-pattern.md`](dpdk-reconnect-pattern.md). For the
API delta from the previous autojoin model see
`eph-net-dpdk/CHANGELOG.md` (BREAKING — daemon-led Platform reshape).

## Architecture overview

```
machine
├─ Layer 1 OS:   vfio-pci binding, hugepages, cgroups CPU slicing       (eph not aware)
│
├─ Layer 2 ops:  /etc/eph/0000:01:00.1.toml                              (one file per NIC)
│                  total_queues = 16
│                  rss_key      = "auto"
│                  promiscuous  = false
│                  daemon_lcore = 6
│                  default      = true
│
├─ Layer 3 daemon:  systemd eph-nicd@0000:01:00.1.service                (DPDK primary)
│   ├─ EAL primary, --file-prefix=eph_0000_01_00_1
│   ├─ holds VFIO group fd / MMIO mappings
│   ├─ owns hugepage memzones:
│   │     - MpRegistry (16 slots, queue ownership bitmap)
│   │     - IcmpDirectory
│   │     - mempool / RX-TX rings
│   └─ IPC handlers: eph_fd_install / eph_icmp_dispatch
│
└─ Layer 4 tenants:  multiple processes coexist, mutually unaware
     ├─ strategy_a   Platform::create({ pci, queues = 4 })   → queues 0..3
     ├─ market_b     Platform::create({ pci, queues = 8 })   → queues 4..11
     └─ risk_c       Platform::create({ pci, queues = 4 })   → queues 12..15
```

Tenants share the daemon's hugepage segment via DPDK's standard
`--proc-type=secondary` mechanism. They each own a disjoint queue
range carved from the daemon-managed pool of size `total_queues`.
Hot-path code is byte-for-byte identical to a single-process
deployment — `inc_<M>` / `rr_counter` / `poll` are per-process and
never cross the daemon boundary.

## How peers discover each other

The PCI BDF is the global coordination point. Both daemon and
tenants derive the EAL `--file-prefix` deterministically from
`cfg.pci`:

```
file_prefix = "eph_" + sanitize_bdf(pci)
```

`sanitize_bdf` replaces `:` and `.` with `_` (`bdf_sanitize.hpp`).
DPDK's file-prefix names the hugepage runtime directory; secondary
attach succeeds iff a primary exists under the same prefix. Since
both sides derive the prefix from the same `pci`, no out-of-band
agreement is required:

- daemon: `Platform::serve_nic({.pci = "0000:01:00.1", ...})` →
  prefix `eph_0000_01_00_1`, calls `rte_eal_init` with
  `--proc-type=primary --file-prefix=eph_0000_01_00_1
  -a 0000:01:00.1`.
- tenant: `Platform::create({.pci = "0000:01:00.1", ...})` →
  same prefix, calls `rte_eal_init` with
  `--proc-type=secondary --file-prefix=eph_0000_01_00_1
  -a 0000:01:00.1`.

There is no pid file, no Unix socket, no D-Bus, no service-discovery
layer. The hugepage filesystem is the rendezvous.

## Resource model

| Resource              | Owner                | Coordination                                              |
|-----------------------|----------------------|-----------------------------------------------------------|
| NIC physical state    | daemon               | `/etc/eph/<bdf>.toml` (one file per NIC)                  |
| Hugepage segment      | daemon (primary)     | DPDK file-prefix (BDF-derived)                            |
| RX/TX queue pool      | daemon-managed       | `total_queues` cap; tenants claim contiguous sub-ranges   |
| RSS RETA              | daemon writes        | hash buckets follow claimed queues (S5 work)              |
| Mempool               | daemon (primary)     | `rte_mempool_lookup` from secondaries                     |
| ICMP directory        | daemon-managed       | cross-process Type 3 Code 4 routing (existing mechanism)  |
| FlowDirector rules    | install via daemon   | `eph_fd_install` IPC; tenant holds RAII handle            |
| CPU lcore pinning     | OS (cgroups/systemd) | eph does **not** coordinate cross-tenant; see deployment doc |

Note that **CPU pinning** is explicitly not eph's job. The library
sees only its own process's lcore pin registry; cross-tenant
disjointness is enforced via systemd cpusets, cgroups, or `taskset`.
This is the same model HFT operators use for kernel-side
co-tenancy and avoids the library duplicating OS-level facilities.

## Failure isolation

The daemon-led shape buys two important isolation properties that
the older autojoin model could not provide:

### Tenant crash → other tenants unaffected

A tenant SEGV / OOM / `kill -9` releases its claimed queue range
back to the daemon's pool (via the slot's CAS-claim release path
on tenant exit; abnormal exit recovery is the daemon's responsibility,
not other tenants'). The NIC stays up, the mempool stays intact,
the hugepage segment stays mapped. Other tenants do not observe the
crash at all.

Compare to autojoin: the first tenant to call `create_or_join`
became primary, holding the NIC port lifecycle. Its crash tore
down the port for every secondary peer.

### Daemon crash → tenants reconnect

A daemon crash invalidates the hugepage segment for every
attached tenant. Tenants observe `Error::DaemonDisconnected` (S6)
on their next `rx_burst` / `tx_burst`, and follow the reconnect
pattern documented in [`dpdk-reconnect-pattern.md`](dpdk-reconnect-pattern.md):
sleep, retry `Platform::create`, rebuild business state. systemd
revives the daemon within ~1 s via `Restart=on-failure`, so the
typical recovery window is bounded.

The daemon is a single point of failure for the NIC, by design.
This trade-off (one SPOF, isolated from tenants) is the
deliberate choice over the autojoin model's "any tenant can take
down the NIC" property.

### Pool exhaustion

If `sum(cfg.queues across tenants) > total_queues`, the latest
tenant's `Platform::create` call returns
`ErrorInfo{QueuePoolExhausted}` (S5 work; foundation commit uses a
static placeholder). Standard tenant response: surface to ops,
exit with diagnostic; ops bumps `total_queues` in the toml or
removes a low-priority tenant.

## Source-port partitioning across tenants

`eph-net-dpdk` does **not** auto-allocate source ports across
processes, and (ENA-first) does **not** predict or rebind src_port.
The `create_and_attach` paths take the source port verbatim from the
caller-supplied `cfg.dpdk.wire.tuple.src_port` (TCP) or
`cfg.dpdk.wire.src_port` (UDP). In RssPartitioned mode the caller
measures which src_port lands on the desired queue empirically
(`tools/dpdk_rss_queue_probe --finder`) and pins it.

### Library-enforced disjointness (post-2026-05-02 daemon-led model)

In the daemon-led architecture, src_port partitioning across tenants
is **library-enforced** by the wire-level registry, not the
"operator's responsibility" the pre-reshape docs warned about. The
mechanism (T2.4 in the 2026-05-05 action list — verified during
audit, found to already be implemented):

1. The daemon's `MpTopology` carries a `[port_lo, port_hi)` window
   per process slot (held as `uint32_t` in `detail/mp_registry.hpp`'s
   `ProcSlot` so `port_hi=65536` exclusive can be expressed without
   wrap).
2. `MpTopology::valid()` enforces pairwise disjointness across slots
   *before* the primary writes the topology into the hugepage memzone
   (see `mp_topology.hpp` — the `for (size_t i; i < total_procs;
   ++i) for (size_t j = i+1; j < total_procs; ++j)` overlap pass).
   Overlapping ranges fail validation; the daemon refuses to start.
3. A tenant's `Platform::create` resolves the topology via the
   daemon's registry handshake and exposes its window via
   `Platform::port_range()`. The operator measures src_ports within
   this window (`dpdk_rss_queue_probe --finder`) and supplies them
   explicitly, so the caller-pinned src_ports stay inside the
   tenant's allocated window.
4. If the caller passes a `cfg.dpdk.wire.tuple.src_port`
   outside `Platform::port_range()`, the stream-attach path emits a
   WARN with the conflict — operator override is allowed (escape
   hatch for legacy configs) but no longer silent.

So in steady state on the post-reshape architecture, the operator
declares per-tenant `port_lo` / `port_hi` exactly once in the
daemon's `NicServiceConfig`-derived `MpTopology`, and the library
prevents collisions thereafter. The pre-reshape "hardcode the
sub-range in each tenant's bench.conf" workaround is no longer
necessary — though the convention (partition by tenant ahead of
time) remains a good operational hygiene practice.

### Manual override (legacy / hand-managed paths)

Single-process Platforms (no daemon) still have no global view; in
that mode the library can only enforce disjointness within one
process and the cross-process contract reverts to the operator-
managed convention. Code that builds `EalConfig` + `MpTopology`
by hand should call `MpTopology::valid()` before `serve_nic` to
get the pre-reshape "hardcoded sub-range" check at library level.

## PMD compatibility

Secondary-mode `rte_flow_create` support is PMD-specific:

| PMD   | Secondary `rte_flow_create` | Notes |
|-------|------------------------------|-------|
| mlx5  | yes                          | full support |
| ixgbe | yes                          | full support |
| i40e  | yes                          | full support (newer builds) |
| ena   | yes                          | verified under noiommu, ena 2.x; report regressions on other configurations |
| null  | n/a                          | simulation-only |

If your PMD rejects `rte_flow_create` in secondary, the library
auto-handles this via the FlowDir secondary-fallback IPC path
(`eph_fd_install` → daemon installs locally → returns opaque
handle). User code is unchanged: `Stream::create_and_attach`
transparently routes through IPC when local install fails, and the
`FlowRule`'s RAII destructor fires the matching `eph_fd_destroy`
IPC. PMD compatibility moves from caller's concern to library
detail.

## Cross-process ICMP MTU propagation

When traffic is RSS-distributed across queues, ICMP Frag Needed
messages from a router land on whichever queue the router happens
to hash them to — typically not the queue owning the affected TCP
session. Without forwarding, the per-tenant `IcmpRegistry` finds no
matching target, the message is silently dropped, and the owning
stream's `effective_mss` never shrinks. The result is a packet
storm: the stream keeps sending oversized segments, the router
keeps replying with Frag Needed, both sides churn.

The daemon-led model handles this transparently:

1. **Cross-process directory**: `Platform::serve_nic` reserves an
   extra hugepage memzone `eph_mp_icmp/<file_prefix>` parallel to
   `eph_mp/<file_prefix>` (the existing MpRegistry). It holds a
   1024-slot POD table mapping `(4-tuple, proto) → owner_proc_index`
   with per-slot generation counters to drop in-flight stale
   forwards.
2. **Dual register at stream attach**: tenant
   `Platform::register_icmp_target` registers both in the local
   `IcmpRegistry` and in the cross-proc directory.
3. **Forward on miss**: tenant Poller's ICMP callback does
   `local IcmpRegistry::dispatch_returns_hit ? done :
   directory.lookup → mp_ipc_send_oneway("eph_icmp_dispatch", ...)`.
   Fire-and-forget — does not block the tenant's RX poll loop.
4. **Owner-side dispatch**: the receiving tenant's
   `eph_icmp_dispatch` rte_mp action handler validates magic /
   version / generation, rebuilds the `ParsedIcmp`, and feeds it
   into the local `IcmpRegistry::dispatch` path.

Hot path is unchanged. Every step above runs only when an ICMP
Frag Needed actually arrives.

The `IcmpDirectoryHeader` exposes `ipc_msgs_sent` /
`ipc_msgs_received` / `dropped_stale` / `dropped_no_owner` counters
for live monitoring. Read via `&platform.impl_->icmp_directory->header()->...`
or, in diagnostic code,
`eph::dpdk::detail::g_active_icmp_directory`.

## Common errors

### `rte_eal_init failed (ret=-1, rte_errno=2)` — No such file or directory

Tenant attempted to attach but no daemon is running on this NIC, or
daemon and tenant disagree on the file-prefix. Causes:

- daemon not started (`systemctl status eph-nicd@<bdf>`)
- `cfg.pci` mismatch between daemon's toml and tenant's
  `PlatformConfig::pci`
- daemon was started with `--no-shconf` or under a different
  user's runtime dir

### `rte_mempool_lookup('eph_mbuf_p0') failed`

Daemon's mempool isn't visible. Either daemon hasn't completed
bring-up yet, or the two processes are using different
file-prefixes. The error message includes the expected runtime-dir
path for quick grep.

### Tenant attach succeeds, but no packets arrive

Check that the tenant's claimed queue range falls inside the
daemon's RETA-distributed hash buckets. Run `rte_eth_stats_get`
from both daemon and tenant — if the daemon counters show RX on
queue X but the tenant owns `[Y, Z)` with X ∉ [Y, Z),
RSS/RETA isn't partitioning the way you expect. (S5 makes this
self-correcting via RETA-tracking-on-claim; until S5 ships, only
queue 0 is reliably populated.)

## Historical: the autojoin model

Prior to the daemon-led reshape (commit `b4fc89695a`,
2026-05-02), the only multi-process entry point was
`Platform::create_or_join(CreateOrJoinConfig)` — peers raced on
`rte_eal_init` and the winner became primary, running tenant
business AND owning the NIC port lifecycle. This had two recurring
problems in production:

1. Cross-binary consensus required (`nb_rx_queues`,
   `max_procs`, `queues_per_proc` had to match across all peers
   sharing a NIC).
2. Tenant-as-primary crash tore down the NIC for every other peer.

The daemon-led model removes both. The autojoin code path is gone
from the library (see `eph-net-dpdk/CHANGELOG.md`, BREAKING entry).
Migration guide: replace `Platform::create_or_join` with
`Platform::create` for tenants and stand up `eph-nicd` for the
NIC's primary role.

## See also

- [`dpdk-daemon-deployment.md`](dpdk-daemon-deployment.md) — toml
  schema, systemd unit, upgrade procedure, security notes.
- [`dpdk-reconnect-pattern.md`](dpdk-reconnect-pattern.md) — tenant
  reconnect template for daemon restarts.
- [`lcore-pin-integration.md`](lcore-pin-integration.md) —
  EAL lcore × `eph::utils::pin_thread` interaction.
- [`../../docs/cpu-no-cross-core.md`](../../docs/cpu-no-cross-core.md) —
  the unified kernel-vs-DPDK no-cross-core model; empirical RSS
  src_port→queue measurement (`dpdk_rss_queue_probe --finder`).
- `eph-net-dpdk/CHANGELOG.md` — BREAKING entry for the daemon-led
  reshape with full before/after migration table.
