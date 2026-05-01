# DPDK single-NIC multi-process (primary + secondary)

`eph-net-dpdk` supports DPDK's official primary+secondary multi-process
model for sharing **one physical NIC** between two (or more) processes that
each need their own TX/RX path. The typical deployment is a data-plane
process plus a monitor / risk-check / mirror process, or two independent
strategies that must share a single expensive 25/100 Gbps card.

This document is the operational contract. For the design rationale see
`.artifacts/happy-mapping-pond-*` plan artifacts and
`eph-net-dpdk/CHANGELOG.md`.

---

## TL;DR — autojoin (recommended path)

Two unrelated processes share one NIC by agreeing on **only the
PCI BDF and `pcfg_template.nb_rx_queues`** — no shared file_prefix,
no manual topology, no primary-vs-secondary launch protocol.
Whoever calls `rte_eal_init` first becomes primary; the next peer
auto-attaches as secondary and CAS-claims the lowest free slot.

```cpp
#include "eph/dpdk/platform.hpp"

// Same code in BOTH binaries. Run them in any order.
auto platform = eph::dpdk::Platform::join_dynamic({
    .pci           = "0000:28:00.0",
    .pcfg_template = { .nb_rx_queues = 4 },  // any PlatformConfig field
    .lcores        = {"0,1"},                 // process-specific lcore set
});
// First peer  → primary, owns queue/port slot 0.
// Second peer → secondary, CAS-claims slot 1, owns queue/port slot 1.
//
// file_prefix derived as "eph_0000_28_00_0" (sanitized BDF) so
// both peers automatically agree on the hugepage segment name.
// max_procs derived as pcfg_template.nb_rx_queues / queues_per_proc.
//
// Platform owns EAL: ~Platform releases DPDK resources, then runs
// eal_cleanup atomically. No manual eal_cleanup() needed.
//
// Hot-path code is byte-for-byte identical to a declarative-path
// platform with the same self_index. inc_<StreamMetric::*>,
// rr_counter, ICMP / FlowDirector / Poller state are all per-process.
```

**Mental model**: `JoinDynamicConfig` only carries autojoin-specific
inputs (pci, queues_per_proc, max_procs, file_prefix override). All
PlatformConfig-level customization (`per_lcore_pools`,
`mbuf_pool_size`, `enable_promiscuous`, `port_id`, …) flows through
`pcfg_template`. **Single source of truth**: PlatformConfig.

See `examples/simple_hft_dpdk_mp_dynamic.cpp` for
a runnable end-to-end demo.

### When to use which path

| Need                                                          | Path                          |
|---------------------------------------------------------------|-------------------------------|
| Two independent binaries / teams sharing one NIC              | **autojoin** (`join_dynamic`) |
| Single team, simple uniform partition                         | **autojoin** (`join_dynamic`) |
| Asymmetric topology (one peer gets 6 queues, others get 1)    | declarative + `MpTopology::custom` |
| Tagged process roles where slot order matters semantically    | declarative + `MpTopology::uniform` (manual self_index) |
| Pre-existing EAL bootstrap you don't want join_dynamic to own | declarative                   |
| Cross-node coordinated RSS / hand-tuned partitions            | manual `rx_queue_range`       |

The autojoin path is built **on top of** the declarative path: by
the time `Platform::join_dynamic` returns, the resulting Platform's
`Impl` is byte-for-byte identical to one produced by the
declarative factories.

---

## Startup / teardown ordering

DPDK shared-hugepage multi-process imposes a strict ordering contract:

| Step | Primary | Secondary |
|------|---------|-----------|
| 1 | `eal_init(--proc-type=primary --file-prefix=X)` | — |
| 2 | `Platform::create_primary(cfg)` — creates mempool, configures+starts port | — |
| 3 | start streams / Poller loops | — |
| 4 | — | `eal_init(--proc-type=secondary --file-prefix=X)` |
| 5 | — | `Platform::create_secondary(cfg)` — `rte_mempool_lookup`, skip port bringup |
| 6 | — | start streams / Poller loops |
| 7 | — | teardown streams / Poller / `~Platform` / `eal_cleanup` |
| 8 | teardown streams / Poller / `~Platform` / `eal_cleanup` | — |

**Secondary must start after primary** — `rte_mempool_lookup` fails
otherwise with "primary not running or file_prefix mismatch".

**Primary may exit before secondaries** (since fix `0b3a4aaa→067dccbc`).
Primary's `~Platform()` is gated on `MpRegistry::is_last_alive_proc()`:
when peers are still attached, `rte_eth_dev_stop` / `rte_eth_dev_close`
/ `rte_mempool_free` / `rte_eal_cleanup` all defer, leaving shared
state alive for the surviving peers. The port is physically torn down
by whichever process happens to be the **last** to call `~Platform()`.
See `dpdk-mp-teardown-protocol.md` for the full protocol and rationale
behind the gate.

`Platform::create_secondary`'s cleanup is narrowed: it does **not** call
`rte_eth_dev_stop/close` or `rte_mempool_free` — those would corrupt the
primary's port state.

---

## Primary restart semantics

* `Platform::create_primary` always **resets** the registry on entry —
  it frees any stale memzone left from a previous run, writes a fresh
  header, and CAS-claims its own slot. There is no "rejoin" mode for
  the primary; restarting the primary always invalidates secondaries.
* If you need to restart the primary, **stop every secondary first**,
  then bring up the primary, then re-attach the secondaries. A
  secondary that survives a primary restart will see a fresh registry
  and can succeed at `attach_secondary`, but its mempool / port view
  will be torn — the same hazard DPDK already documents for the
  shared mempool. There is no `epoch` / generation counter to detect
  this; the contract is operational, not enforced.
* Within a single primary's lifetime, secondaries may attach / detach
  freely — the registry's `procs[i].claimed` CAS makes "I'm the only
  one with self_index=i" mutually exclusive among live peers.
* Primary exit ≠ port stop in MP mode (since `0b3a4aaa→067dccbc`):
  primary's `~Platform()` defers shared-resource teardown when peers
  are attached. See **MP teardown protocol** below.

## MP teardown protocol (gate semantics)

Primary owns the port's lifecycle, but the port itself is shared
across every attached process. Calling `rte_eth_dev_stop` while a
secondary is mid-`rte_eth_rx_burst` writes NULL into shared
hugepage descriptor state and faults the secondary (e.g. ENA's
`ena_com_get_next_rx_cdesc` SIGSEGV). Eph defers global teardown
until the **last** attached process exits.

```
                  T=0 ──────────── T=15 ──── T=30
primary           ███████████████| ~Platform()
                                    defer (1 peer attached)
                                    skip stop/close/free/cleanup
                                    return; OS reclaims local maps

secondary 1            ████████████████████| ~Platform()
                                             is_last_alive: YES
                                             stop/close/free runs
                                             eal_cleanup runs
                                             port physically torn down
```

Gate sites (`Platform::Impl`):

* `defer_for_peers()` — single-source predicate over
  `mp_registry->is_last_alive_proc()`.
* `Impl::cleanup()` — guards `rte_eth_dev_stop` / `rte_eth_dev_close`
  / `rte_mempool_free`.
* `~Impl()` body — gates `IcmpDirectoryHandle::disable_memzone_free()`
  before that handle's field destructor would call `rte_memzone_free`.
* `MpRegistryHandle::release_()` — its own `rte_memzone_free` is
  scoped on "any peer still alive after self-clear".
* `~Platform()` — snapshot the gate **before** `impl_.reset()` so
  it can guard `rte_eal_cleanup` (which would otherwise close all
  active devices internally).

**Trade-off**: if a peer exits abnormally (`kill -9`, OOM) without
running its `~Platform()`, its slot stays claimed and the port is
never stopped — `scripts/dpdk-teardown.sh` recycles between sessions.
v2 candidate (IPC heartbeat reaper) is parked in `TODO.md`.

Full root-cause and acceptance evidence:
`eph-net-dpdk/docs/dpdk-mp-teardown-protocol.md`.

## Advanced usage: declarative topology

The path the autojoin TL;DR builds on. Use it directly when you
need precise control over `self_index`, asymmetric per-peer
specs (`MpTopology::custom`), or when you're managing EAL
bootstrap yourself (e.g. a wrapper that already calls
`eal_init`).

```cpp
// Primary process — declare only (self_index, total_procs).
PlatformConfig p_cfg{
    .port_id      = 0,
    .nb_rx_queues = 4,
    .enable_rss   = true,
    .proc_type    = ProcType::Primary,
    .file_prefix  = "eph_mp_demo",
    .mp_topology  = MpTopology::uniform(/*self_index=*/0,
                                        /*total_procs=*/2,
                                        /*nb_rx_queues=*/4),
};
auto platform = Platform::create_primary(std::move(p_cfg));
// Library auto-derives queues=[0,2) and src_port=[32768, 49152) for
// this process. Stream create_and_attach picks src_ports from that
// window automatically; no manual partition tables.

// Secondary process (separate binary; same file_prefix).
PlatformConfig s_cfg{
    .port_id      = 0,
    .nb_rx_queues = 4,                   // must match primary
    .proc_type    = ProcType::Secondary,
    .file_prefix  = "eph_mp_demo",
    .mp_topology  = MpTopology::uniform(/*self_index=*/1,
                                        /*total_procs=*/2,
                                        /*nb_rx_queues=*/4),
};
auto platform_sec = Platform::create_secondary(std::move(s_cfg));
// Auto-derives queues=[2,4) and src_port=[49152, 65536). The shared
// hugepage registry rejects two processes that declare the same
// self_index — silent collisions become loud cold-path errors.
```

For non-uniform layouts (e.g. one trader process gets 6 queues, the
rest 1 each), use `MpTopology::custom(self_index, {ProcSpec{...}, ...})`.

The same code with `mp_topology` left empty falls back to the legacy
"hand-partitioned `rx_queue_range` + caller-allocated src_port" path;
see "Advanced usage: manual partitioning" below if you need it.

---

## Advanced usage: manual partitioning

> Most users should use `MpTopology` (above). The following sections
> describe the hand-partitioned path: callers set
> `cfg.rx_queue_range` directly and allocate src_port from disjoint
> sub-ranges by hand. This path is preserved for two cases — neither
> common in HFT:
>
>   1. Cross-node coordinated RSS, where the partition is dictated by
>      a topology larger than a single host (the library has no view
>      of the cluster, so it can't help).
>   2. Hand-tuned non-uniform layouts where you want to bypass even
>      `MpTopology::custom` and own every byte of the partition.
>
> In both cases the legacy contract still holds: the caller is
> responsible for keeping the ranges disjoint. The library will not
> detect collisions on this path.

### `PlatformConfig` multi-process fields

All three fields have safe single-process defaults. Existing call sites
that don't set any MP fields continue to work byte-for-byte.

| Field | Type | Default | Primary meaning | Secondary meaning |
|-------|------|---------|-----------------|-------------------|
| `proc_type` | `ProcType` | `Primary` | process role | process role |
| `file_prefix` | `std::string_view` | `""` | optional `--file-prefix`; empty = DPDK default | **required** non-empty, must match primary's |
| `rx_queue_range` | `{uint16_t,uint16_t}` | `{0, 0}` | sentinel `{0,0}` = `[0, nb_rx_queues)` | half-open `[lo, hi)` — must be disjoint from primary's |

`validate_config` (called by `create_primary` / `create_secondary` /
`create`) rejects:

* `rx_queue_range` with `lo >= hi` (unless it is the `{0, 0}` sentinel)
* `rx_queue_range.hi > nb_rx_queues`

Additionally, `Platform::create_secondary` rejects:

* empty `file_prefix`
* `rte_eth_dev_is_valid_port(port_id)` false (primary not running or
  file-prefix mismatch)

Source-port partitioning across MP processes is the **caller's**
responsibility — see "Source-port partitioning" section below.

---

## EAL argv: `EalConfig` / `build_eal_argv`

Hand-splicing `--proc-type` and `--file-prefix` is mistake-prone; use the
typed helper:

```cpp
EalConfig cfg{
    .program_name  = "my_secondary",
    .proc_type     = ProcType::Secondary,
    .proc_type_set = true,
    .file_prefix   = "eph_mp_demo",
    .lcores        = {"2,3"},
    .allowed_devs  = {"0000:05:00.1"},
};
auto argv_owned = build_eal_argv(cfg);   // std::vector<std::string>
std::vector<char*> argv;
for (auto& s : argv_owned) argv.push_back(s.data());
eal_init(static_cast<int>(argv.size()), argv.data());
```

Leaving `proc_type_set = false` suppresses the `--proc-type` flag entirely
(DPDK then uses `auto`, which becomes primary for the first process to
init under a given file-prefix).

---

## Source-port partitioning (caller responsibility)

`eph-net-dpdk` does **not** auto-allocate source ports. The TCP/UDP
`create_and_attach` paths take the source port from the caller-supplied
`cfg.dpdk.tcp_low_level.tuple.src_port` (TCP) or `cfg.legacy.src_port`
(UDP — the `legacy` substruct is intentionally retained on
`eph::net::dpdk::UdpConfig` per T3.19's TCP-only reshape scope) in
Software / FlowDirector mode, or rebind it to one that hashes to the
desired queue (RSS-pinned mode via `find_src_port_for_queue`). The
library has no global view across processes and cannot enforce
src_port disjointness.

In multi-process setups the caller MUST allocate src_port from a
sub-range that is disjoint from every other process sharing the NIC.
Re-using the same `(src_ip, src_port)` across processes makes
exchange-grade peers see duplicate connection 4-tuples and trigger
anti-abuse disconnects.

A typical static partition for 1 primary + N secondaries:

| Process   | `rx_queue_range` | suggested src_port range |
|-----------|------------------|--------------------------|
| primary   | `{0, 4}`         | `[32768, 40959]`         |
| sec #1    | `{4, 8}`         | `[40960, 49151]`         |
| sec #2    | `{8, 12}`        | `[49152, 57343]`         |
| sec #3    | `{12, 16}`       | `[57344, 65535]`         |

`nb_rx_queues` must be the same across all processes (secondaries see
the port the primary configured; over- or under-reporting will have
surprising effects on `DpdkPoller` bookkeeping).

---

## Common errors and how to read them

### `rte_eal_init failed (ret=-1, rte_errno=2)` — No such file or directory

Secondary attempted to attach but the primary's runtime dir doesn't
exist. Causes:

* primary not yet started
* `--file-prefix` mismatch between primary and secondary
* primary started with `--no-shconf` or on a different user's runtime dir

### `rte_mempool_lookup('eph_mbuf_p0') failed`

Primary's mempool isn't visible. Either primary hasn't reached the post-
`create_mempool` stage yet, or the two processes are using different
file-prefixes. The error message includes the expected runtime-dir path
for quick grep.

### Secondary attach succeeds, but no packets arrive

Check that `rx_queue_range` in secondary points at queues the RSS / FD
rules actually steer traffic to. Run `rte_eth_stats_get(port_id)` on both
processes — if the NIC counters show RX on queue X but the secondary owns
`[Y, Z)` with X ∉ [Y, Z), RSS/RETA isn't partitioning the way you think.

---

## PMD compatibility caveats

Secondary-mode `rte_flow_create` support is PMD-specific:

| PMD   | Secondary `rte_flow_create` | Notes |
|-------|------------------------------|-------|
| mlx5  | ✓ | full support |
| ixgbe | ✓ | full support |
| i40e  | ✓ | full support (newer builds) |
| ena   | ✓ | verified under noiommu, ena 2.x, 4 RX queues; report regressions on other configurations |
| null  | — | not applicable (PMD is simulation-only) |

If your PMD rejects `rte_flow_create` in secondary, the library now
**auto-handles** this via the FlowDir secondary-fallback IPC path
(see "FlowDir secondary fallback" below). User code is unchanged —
`Stream::create_and_attach` transparently routes through
`eph_fd_install` IPC when local install fails, and the resulting
`FlowRule`'s RAII destructor fires the matching `eph_fd_destroy`
IPC. PMD compatibility moves from caller's concern to library
detail.

---

## Example skeleton

For a single-file, runnable skeleton that you can adapt directly, see
[`examples/simple_hft_dpdk_mp.cpp`](../../examples/simple_hft_dpdk_mp.cpp).
One binary, role picked via `--role primary|secondary`. Demonstrates
`EalConfig` + `build_eal_argv`, `Platform::create_primary` /
`create_secondary`, queue-range partitioning, and the secondary
cleanup branch. Run from two terminals on the same host (primary
first, then secondary once primary logs "ready"); see the file
header for the launch commands.

---

## Running the integration test

```bash
# Needs: NIC bound to vfio-pci + ≥128 free 2 MiB hugepages + root
sudo EPH_MP_ALLOWED_DEV=0000:05:00.1 \
     EPH_MP_LCORES="0,1" \
     EPH_MP_LCORES_SEC="2,3" \
     eph-net-dpdk/tests/integration/dpdk_mp_e2e.sh
```

The script:

1. Preflight-checks binaries / vfio-pci / hugepages / DPDK idle (retries
   once after a 3-minute wait if another DPDK process is active).
2. Launches `dpdk_mp_primary` in the background; waits up to 10 s for its
   ready-file.
3. Launches `dpdk_mp_secondary` in the foreground.
4. Waits for both to exit, reports pass/fail, dumps the per-role logs on
   failure.

Exit code `77` means "environment not ready, test skipped" — CI treats it
as success.

---

## Cross-process ICMP MTU propagation

When `mp_topology` is set, ICMP Frag Needed messages that land on a
peer's RX queue are auto-forwarded to the owning process. Background:
routers pick which RX queue an ICMP Type 3 Code 4 lands on using
their own hashing — they don't know our RSS configuration. The Frag
Needed often hits a secondary's queue when the affected stream lives
in primary (or vice-versa). Without forwarding, the local
`IcmpRegistry` finds no matching target, the message is silently
dropped, and the owning stream's `effective_mss` never shrinks. The
result is a packet storm: the stream keeps sending oversized
segments, the router keeps replying with Frag Needed, both sides
churn. Hard to diagnose in production.

The library's solution is automatic and transparent to user code:

1. **Cross-process directory**: `Platform::create_*` reserves an
   extra hugepage memzone `eph_mp_icmp/<file_prefix>`, parallel to
   `eph_mp/<file_prefix>` (the existing MpRegistry). It holds a
   1024-slot POD table mapping `(4-tuple, proto) →
   owner_proc_index`. Each slot has a per-slot `std::atomic<uint32_t>
   generation` that is bumped on unregister so any in-flight forward
   carrying a stale gen is dropped at the owner side.
2. **Dual register at stream attach**: `Platform::register_icmp_target`
   now registers both in the local `IcmpRegistry` (unchanged) and in
   the cross-proc directory. The returned handle is a compound RAII
   object that releases both on destruction; the directory side is
   released first so any peer-in-flight forward observing gen=N hits
   the bumped gen=N+1 and drops stale before the local side is freed.
3. **Forward on miss**: the Poller's ICMP callback closure now does
   `local IcmpRegistry::dispatch_returns_hit(parsed) ? done :
   directory.lookup → mp_ipc_send_oneway("eph_icmp_dispatch", ...)`.
   Fire-and-forget — does not block the secondary's RX poll loop.
4. **Owner-side dispatch**: `Platform::create_*` registers an
   `eph_icmp_dispatch` rte_mp action handler. On incoming msg it
   validates magic / version / generation, rebuilds enough of the
   `ParsedIcmp` struct to feed `IcmpRegistry::dispatch`, and the
   existing per-process callback path takes over from there.

Hot path is unchanged. Every step above runs only when an ICMP Frag
Needed actually arrives — IcmpDirectory is a cold-path lookup, IPC
is a cold-path RPC, and the receiving thunk runs on DPDK's IPC
thread (not your RX poll lcore).

Degrade-on-failure: if `rte_mp_action_register` returns an error
(e.g. an EAL `--no-shconf` mode), `Platform::create_*` logs ERROR
and continues. Cross-proc forwarding then falls back to silent drop
— equivalent to pre-reshape behavior, the rest of the eph stack
keeps working.

The `IcmpDirectoryHeader` exposes four `std::atomic<uint64_t>`
counters for live monitoring:

* `ipc_msgs_sent` — incremented by the Poller closure each time a
  forward succeeds.
* `ipc_msgs_received` — incremented by the owner-side thunk on each
  arriving msg.
* `dropped_stale` — generation mismatch / version skew on a forwarded
  msg.
* `dropped_no_owner` — local miss + directory lookup found nothing
  (target not registered anywhere — may indicate a torn-down stream
  or a mistargeted ICMP).

Counters are visible to any process that has the directory attached;
read via `&platform.impl_->icmp_directory->header()->...` or, in
diagnostic code, the global `eph::dpdk::detail::g_active_icmp_directory`.

---

## FlowDir secondary fallback

When `rte_flow_create` would fail on a secondary because the PMD
doesn't allow rule installation from non-primary processes, the
library auto-routes the install request through DPDK's `rte_mp_*`
IPC to the primary, which installs the rule on the secondary's
behalf and returns an opaque handle id.

Seen from user code, `Stream::create_and_attach` continues to
"just work" — there is no API change at all. Internally:

1. **Try local install first**. The fast path is unchanged:
   secondaries that are on a PMD supporting `rte_flow_create`
   (ENA / mlx5 / ixgbe / i40e in working configurations) get a
   `LocalFlowHandle` and zero IPC overhead.
2. **Fall back through `eph_fd_install`**. If local install
   returns nullptr AND the platform is a secondary AND
   `mp_topology` is in effect, `try_install_flow_rule_via_ipc`
   sends an `FdInstallMsg` (POD: 4-tuple + target_queue + port_id
   + request_id) to primary via `rte_mp_request_sync`. Primary's
   `on_fd_install_thunk` calls `install_flow_rule` locally,
   stashes the resulting `rte_flow*` in its `RemoteFlowRulesMap`
   keyed by a synthetic 64-bit handle_id, and replies with that
   id.
3. **`FlowRule` holds a `RemoteFlowHandle`**. The `FlowRule::
   handle` variant now carries `{owner_proc, handle_id}` instead
   of a `rte_flow*`. Functionality (`valid()`, `dump()`,
   `to_json()`, `opaque_handle_id()` for telemetry) is identical
   to a local rule from the caller's perspective, modulo the
   `"origin": "remote"` JSON tag.
4. **Destruction is symmetric**. When the `FlowRule` falls out of
   scope, its destructor visit-dispatches: a `LocalFlowHandle`
   becomes `rte_flow_destroy`; a `RemoteFlowHandle` becomes an
   `eph_fd_destroy` IPC. Primary's `on_fd_destroy_thunk` looks
   up `handle_id` in `RemoteFlowRulesMap` and runs
   `rte_flow_destroy` on the primary-side `rte_flow*`.

Crash semantics:

* If a secondary dies before `eph_fd_destroy` reaches primary,
  the rule is leaked until primary teardown — `Platform::Impl`'s
  destructor calls `RemoteFlowRulesMap::destroy_all` to GC any
  surviving rules.
* If primary dies before secondary's destroy IPC, the
  `mp_ipc_request_sync` call returns `Timeout`; FlowRule's
  destructor logs WARN and continues. Secondary then exits, and
  primary's hugepage state is gone anyway, so the leak is
  bounded to that primary's process lifetime.

Telemetry: the public `FlowRule::to_json()` gains an `"origin"`
field — `"local"` / `"remote"` / `"none"` — that monitoring
pipelines can use to track how many rules are running through
the IPC fallback path. A non-zero `remote` count on an ENA host
is normally unexpected and worth investigating.

---

## What is explicitly out of scope

The current implementation deliberately does **not** support:

* Cross-process connection migration / failover
* Cross-process real-time metric aggregation (each process publishes its
  own; external aggregator is the caller's responsibility)
* `fork()`-based multi-process (DPDK official stance is "not recommended";
  `mempool` per-lcore cache semantics do not survive fork)
* Independent-primary multi-process (N processes each with their own
  `--file-prefix` and no shared mempool) — mempool sharing is the main
  reason to go MP in the first place

## See also: lcore × pin_thread integration

Whether you go single-process or multi-process, EAL's lcore pinning
needs to coexist with application-thread pinning via
`eph::utils::pin_thread`. Use `eph::dpdk::LcorePin` +
`EalGuard::init_with_pins` instead of writing `EalConfig::lcores`
strings by hand: it pre-validates the pins (SMT siblings / NUMA / IRQ),
declares the cpus to `g_pinned_cpus` so subsequent `pin_thread` calls
detect conflicts, and rolls back atomically on any failure.

See [`lcore-pin-integration.md`](lcore-pin-integration.md) for the
full API and the rationale.
