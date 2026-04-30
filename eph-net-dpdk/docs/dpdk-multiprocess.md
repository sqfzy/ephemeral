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

## TL;DR (recommended path)

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

Hot-path code in both processes is **identical** to the single-process
case: `inc_<StreamMetric::*>` is per-instance atomic, `rr_counter` is
process-local, and ICMP / FlowDirector / Poller state are all per-process.
The multi-process machinery lives entirely in the cold startup and
teardown paths.

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

**Secondary must start after primary and exit before primary.** A
secondary that starts before the primary sees `rte_mempool_lookup` fail
with "primary not running or file_prefix mismatch". A secondary that
outlives the primary will hold pointers into freed hugepage memory — DPDK
does not detect this and no code in `eph-net-dpdk` can guard it; the
ordering is operationally enforced.

`Platform::create_secondary`'s cleanup is narrowed: it does **not** call
`rte_eth_dev_stop/close` or `rte_mempool_free` — those would corrupt the
primary's port state.

---

## Primary restart semantics

The cross-process registry uses the same operational rule that DPDK's
shared mempool already imposes: **the primary must be the last to
start and the last to stop**. Concretely:

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

If your PMD rejects `rte_flow_create` in secondary, push rule installation
back to the primary (have the secondary send its 4-tuple to the primary
via a shared ring, and let the primary install the rule pointing to the
secondary's queue). This fallback is not currently wired in `eph-net-dpdk`
— file an issue if you hit it.

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

## What is explicitly out of scope

The current implementation deliberately does **not** support:

* Cross-process connection migration / failover
* Cross-process real-time metric aggregation (each process publishes its
  own; external aggregator is the caller's responsibility)
* Cross-process ICMP/MTU propagation (each process has its own registry)
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
