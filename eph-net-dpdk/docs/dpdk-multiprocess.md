# DPDK single-NIC multi-process (primary + secondary)

`eph-net-dpdk` supports DPDK's official primary+secondary multi-process
model for sharing **one physical NIC** between two (or more) processes that
each need their own TX/RX path. The typical deployment is a data-plane
process plus a monitor / risk-check / mirror process, or two independent
strategies that must share a single expensive 25/100 Gbps card.

This document is the operational contract. For the design rationale see
`eph-net-dpdk/CHANGELOG.md`.

> **API note**: the only multi-process entry point is
> `Platform::join_dynamic(JoinDynamicConfig)` (autojoin). The
> cooperative path (`Platform::attach(PlatformAttachConfig)` plus
> a declarative-primary `Platform::create(PlatformConfig)` with
> `max_procs > 1`) was removed; both `Platform::create` and
> `Platform::create_with_eal` now reject any `max_procs > 1`
> config. Single-process programs continue to use `Platform::create` /
> `Platform::create_with_eal` with `max_procs == 1` (the default).
>
> Sections below that reference `primary_bringup_` /
> `secondary_bringup_` describe the **internal lifecycle helpers**
> that `Platform::join_dynamic` delegates to. They are not part
> of the public API.

---

## TL;DR — autojoin

Two unrelated processes share one NIC by agreeing on **only the
PCI BDF and `pcfg_template.nb_rx_queues`** — no shared file_prefix,
no manual topology, no primary-vs-secondary launch protocol.
Whoever calls `rte_eal_init` first becomes primary; the next peer
auto-attaches as secondary and CAS-claims the lowest free slot.

```cpp
#include "eph/dpdk/platform.hpp"

// Same code in BOTH binaries. Run them in any order.
auto platform = eph::dpdk::Platform::join_dynamic({
    .pci            = "0000:28:00.0",
    .primary_config = {
        .nb_rx_queues    = 4,
        .max_procs       = 2,        // 2 process slots
        .queues_per_proc = 2,        // each process owns 2 RX queues
    },
    .lcores         = {"0,1"},        // process-specific lcore set
});
// First peer  → primary, owns queue/port slot 0.
// Second peer → secondary, CAS-claims slot 1, owns queue/port slot 1.
//
// file_prefix derived as "eph_0000_28_00_0" (sanitized BDF) so
// both peers automatically agree on the hugepage segment name.
// Secondary peers can leave primary_config at default — the library
// reads nb_rx_queues / max_procs from primary's registry post-EAL.
//
// Platform owns EAL: ~Platform releases DPDK resources, then runs
// eal_cleanup atomically. No manual eal_cleanup() needed.
//
// Hot-path code is byte-for-byte identical to a single-process
// platform with the same self_index. inc_<StreamMetric::*>,
// rr_counter, ICMP / FlowDirector / Poller state are all per-process.
```

**Mental model**: `JoinDynamicConfig` carries only the autojoin-
specific inputs (pci, lcores, pin policy). NIC-physical-state and
multi-process layout (`per_lcore_pools`, `mbuf_pool_size`,
`enable_promiscuous`, `port_id`, `max_procs`, `queues_per_proc`)
all live inside `primary_config` (a `PlatformConfig`). Secondary
peers ignore `primary_config` entirely — DPDK's
`rte_eal_process_type` resolves the role post-init, and the
secondary path queries the primary's registry / live NIC for any
field it needs.

For a runnable single-binary skeleton, see
[`examples/dpdk_mp_demo.cpp`](../../examples/dpdk_mp_demo.cpp).

---

## Startup / teardown ordering

DPDK shared-hugepage multi-process imposes a strict ordering contract.
With autojoin both peers run the same code; the table below describes
the steps each process performs once `Platform::join_dynamic` has
resolved its role:

| Step | Primary peer | Secondary peer |
|------|--------------|----------------|
| 1 | `Platform::join_dynamic` → eal_init wins primary lock; primary_bringup_ creates mempool, configures+starts port | — |
| 2 | start streams / Poller loops | — |
| 3 | — | `Platform::join_dynamic` → eal_init resolves to secondary; secondary_bringup_ does `rte_mempool_lookup`, skips port bringup, CAS-claims a registry slot |
| 4 | — | start streams / Poller loops |
| 5 | — | teardown streams / Poller / `~Platform` (releases EAL via owned session) |
| 6 | teardown streams / Poller / `~Platform` | — |

**Secondary peers must start after primary** — `rte_mempool_lookup`
fails otherwise. With autojoin you control this by launch order: the
first invocation of the binary becomes primary, subsequent
invocations attach as secondaries.

**Primary may exit before secondaries** (since fix `0b3a4aaa→067dccbc`).
Primary's `~Platform()` is gated on `MpRegistry::is_last_alive_proc()`:
when peers are still attached, `rte_eth_dev_stop` / `rte_eth_dev_close`
/ `rte_mempool_free` / `rte_eal_cleanup` all defer, leaving shared
state alive for the surviving peers. The port is physically torn down
by whichever process happens to be the **last** to call `~Platform()`.
See `dpdk-mp-teardown-protocol.md` for the full protocol and rationale
behind the gate.

The secondary's cleanup is narrowed: it does **not** call
`rte_eth_dev_stop/close` or `rte_mempool_free` — those would corrupt the
primary's port state.

---

## Primary restart semantics

* When this peer wins the EAL race and `primary_bringup_` runs, it
  always **resets** the registry on entry — it frees any stale
  memzone left from a previous run, writes a fresh header, and
  CAS-claims its own slot. There is no "rejoin" mode for the
  primary; restarting the primary always invalidates secondaries.
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

## `JoinDynamicConfig` knobs

`JoinDynamicConfig` carries the autojoin-specific inputs; everything
else flows through `primary_config` (a `PlatformConfig`) which the
peer that wins the EAL race reads when bringing up the port.

| Field                        | Type                          | Required | Notes |
|------------------------------|-------------------------------|----------|-------|
| `pci`                        | `std::string_view`            | yes      | PCI BDF (e.g. `0000:28:00.0`); file_prefix is auto-derived as `eph_<sanitized BDF>` |
| `primary_config.nb_rx_queues`| `uint16_t`                    | yes (primary) | Total RX queues primary configures; secondary peers may leave at default and read from the live NIC |
| `primary_config.max_procs`   | `uint8_t`                     | optional | Number of process slots primary opens in the registry. Default 1 means "auto-derive from nb_rx_queues / queues_per_proc"; explicit values are validated against primary's registry on the secondary side |
| `primary_config.queues_per_proc` | `uint16_t`                | optional | Queues per slot. 0 = auto-split |
| `pins`                       | `std::span<LcorePin const>`   | optional | Typed pin set (mutually exclusive with `lcores`) |
| `lcores`                     | `std::vector<std::string>`    | optional | Raw `--lcores` spec (mutually exclusive with `pins`) |
| `pin_policy`                 | `eph::utils::CpuPinPolicy`    | optional | NUMA / IRQ / duplicate-cpu policy (only consulted when `pins` is set) |
| `eal_extras`                 | `std::vector<std::string>`    | optional | Additional EAL argv tokens |
| `self_lcore_mask`            | `uint64_t`                    | optional | Lcores this process owns; published into the registry for cross-process conflict detection. 0 = opt out |

`Platform::join_dynamic` also carries optional registry checks:
when both peers explicitly set `primary_config.max_procs > 1`, the
secondary path validates that the value matches the primary's
registry total_procs and refuses to attach if they disagree.

---

## EAL argv: `EalConfig` / `build_eal_argv`

`Platform::join_dynamic` builds the EAL argv internally — callers
typically don't have to think about this layer. For escape hatches
(e.g. a unit-test harness that needs to drive `eal_init` itself),
the typed helper is:

```cpp
EalConfig cfg{
    .program_name  = "my_app",
    .proc_type     = ProcType::Auto,        // primary if first; secondary otherwise
    .proc_type_set = true,
    .file_prefix   = "eph_0000_28_00_0",
    .lcores        = {"0,1"},
    .allowed_devs  = {"0000:05:00.1"},
};
auto argv_owned = build_eal_argv(cfg);   // std::vector<std::string>
std::vector<char*> argv;
for (auto& s : argv_owned) argv.push_back(s.data());
eal_init(static_cast<int>(argv.size()), argv.data());
```

Leaving `proc_type_set = false` suppresses the `--proc-type` flag entirely.
DPDK's default is `primary` (NOT `auto`), so for the autojoin race
to resolve the second peer to secondary, callers building EAL argv
manually must use `--proc-type=auto` — that is what
`Platform::join_dynamic` emits internally.

---

## Source-port partitioning (caller responsibility)

`eph-net-dpdk` does **not** auto-allocate source ports. The TCP/UDP
`create_and_attach` paths take the source port from the caller-supplied
`cfg.dpdk.tcp_low_level.tuple.src_port` (TCP) or
`cfg.dpdk.udp_low_level.src_port` (UDP — UDP now mirrors TCP's `dpdk`
substruct shape post Tier-1 audit follow-up) in Software / FlowDirector
mode, or rebind it to one that hashes to the desired queue (RSS-pinned
mode via `find_src_port_for_queue`). The library has no global view
across processes and cannot enforce src_port disjointness.

In multi-process setups the caller MUST allocate src_port from a
sub-range that is disjoint from every other process sharing the NIC.
Re-using the same `(src_ip, src_port)` across processes makes
exchange-grade peers see duplicate connection 4-tuples and trigger
anti-abuse disconnects.

With `Platform::join_dynamic`, `MpTopology::uniform` synthesizes
disjoint `[port_lo, port_hi)` windows automatically from
`(self_index, total_procs)` and `find_src_port_for_queue`
narrows its candidate space to each peer's window. Callers that
construct streams via `create_and_attach` therefore inherit a
correct partition without manual coordination — the ranges below
are what the library auto-derives for the typical 4-process layout:

| `self_index` | `rx_queue_range` | port range          |
|--------------|------------------|---------------------|
| 0 (primary)  | `{0, 4}`         | `[32768, 40959]`    |
| 1            | `{4, 8}`         | `[40960, 49151]`    |
| 2            | `{8, 12}`        | `[49152, 57343]`    |
| 3            | `{12, 16}`       | `[57344, 65535]`    |

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
[`examples/dpdk_mp_demo.cpp`](../../examples/dpdk_mp_demo.cpp). One
binary, no `--role` flag — both peers run with the same args and
`Platform::join_dynamic` decides the role. Demonstrates `EalConfig`
+ `build_eal_argv` flowing through the autojoin path, queue-range
partitioning, and the secondary cleanup branch. Run from two
terminals on the same host (the first to launch becomes primary);
see the file header for the launch commands.

---

## Running the integration tests

```bash
# Needs: NIC bound to vfio-pci + ≥128 free 2 MiB hugepages + root
sudo EPH_MP_ALLOWED_DEV=0000:05:00.1 \
     EPH_MP_LCORES="0" \
     EPH_MP_LCORES_SEC="1" \
     eph-net-dpdk/tests/integration/dpdk_mp_dynamic_e2e.sh
```

The script:

1. Preflight-checks binaries / vfio-pci / hugepages / DPDK idle (retries
   once after a 3-minute wait if another DPDK process is active).
2. Launches `dpdk_mp_dynamic_primary` (which calls
   `Platform::join_dynamic` and asserts it auto-resolved as primary)
   in the background; waits up to 10 s for its ready-file.
3. Launches `dpdk_mp_dynamic_secondary` (same `join_dynamic` call,
   asserts it resolved as secondary) in the foreground.
4. Waits for both to exit, reports pass/fail, dumps the per-role logs on
   failure.

Exit code `77` means "environment not ready, test skipped" — CI treats it
as success.

A second integration suite,
`dpdk_mp_dynamic_tcp_handshake_e2e.sh`, drives both peers through a
real TCP connect to a kernel echo mock spawned inside primary —
the acceptance gate for `reshape/rss-aware-connect`.

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

1. **Cross-process directory**: `Platform::join_dynamic` reserves an
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
4. **Owner-side dispatch**: `Platform::join_dynamic` registers an
   `eph_icmp_dispatch` rte_mp action handler. On incoming msg it
   validates magic / version / generation, rebuilds enough of the
   `ParsedIcmp` struct to feed `IcmpRegistry::dispatch`, and the
   existing per-process callback path takes over from there.

Hot path is unchanged. Every step above runs only when an ICMP Frag
Needed actually arrives — IcmpDirectory is a cold-path lookup, IPC
is a cold-path RPC, and the receiving thunk runs on DPDK's IPC
thread (not your RX poll lcore).

Degrade-on-failure: if `rte_mp_action_register` returns an error
(e.g. an EAL `--no-shconf` mode), `Platform::join_dynamic` logs ERROR
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
