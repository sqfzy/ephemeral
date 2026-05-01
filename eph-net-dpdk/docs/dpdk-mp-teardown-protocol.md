# DPDK Multi-Process Teardown Protocol Guide

> **TL;DR.** In DPDK multi-process mode, the NIC port is a *system*
> resource shared by all attached processes — its descriptor rings
> live in shared hugepage memory. Per the DPDK contract, primary owns
> the port's lifecycle, so `rte_eth_dev_stop` tears down **all**
> queues regardless of which secondary owns them. Eph's earlier
> `~Platform()` ignored this and called `rte_eth_dev_stop` /
> `rte_eal_cleanup` unconditionally on primary exit, freeing the
> shared `io_cq` rings while secondaries were still mid-`rte_eth_rx_burst`
> — they read the now-NULL `cdesc_addr.virt_addr` and SIGSEGV'd in
> `ena_com_get_next_rx_cdesc`.
>
> **Fix** (commits `0b3a4aaa` → `e7d69bd5`): primary teardown is
> now gated on `MpRegistry::is_last_alive_proc()`. If peers are
> still attached, port stop / mempool free / `eal_cleanup` /
> shared-memzone free all defer; the port stays running until the
> last attached process exits. Single-process behaviour is
> byte-equal to pre-fix.

## Scope

This protocol applies to any DPDK PMD that exports descriptor-ring
state to shared hugepage memory and treats `rte_eth_dev_stop` as a
device-wide operation — i.e. essentially every PMD. The original
SIGSEGV was reproduced on **AWS ENA / DPDK 24.11.2 / Linux
6.1 / aarch64 (Graviton)**, but the underlying protocol is
NIC-agnostic.

## The DPDK contract (what ENA does correctly)

DPDK's multi-process model partitions per-process state (poller
configuration, IPC handlers, function pointers in `rte_eth_fp_ops`)
from system-shared state (memzones, mempools, port descriptor
rings). Per the spec:

* `rte_eth_dev_configure` / `rte_eth_rx_queue_setup` / `rte_eth_dev_start`
  / `rte_eth_dev_stop` / `rte_eth_dev_close` are **primary-only**.
  PMDs typically reject these from secondaries with an early return
  (ENA's `ena_stop` does exactly this — see assembly below).
* When primary calls stop/close, the port goes away **for the entire
  process group**. There is no per-secondary-queue protection inside
  the PMD because the spec doesn't require one — it's the
  application's responsibility to stop attaching peers before primary
  destroys the port.

ENA follows this contract literally. Its `ena_stop` function:

```asm
; ena_stop  (librte_net_ena.so.25, 0x174e0):
17504:  bl   rte_eal_process_type
17508:  cbnz w0, 17694      ; if SECONDARY → log error + return -1 (no teardown)
1750c:  ...  (PRIMARY continues)
17530:  loop over ALL rx_ring[]:
           bl  ena_queue_stop(ring[q])   ; q = 0, 1, ..., nb_rx_queues-1
1757c:  loop over ALL tx_ring[]:
           bl  ena_queue_stop(ring[q])
```

`ena_queue_stop` calls `ena_com_destroy_io_queue` → `ena_com_io_queue_free`,
which writes `io_cq->cdesc_addr.virt_addr = NULL` into shared
hugepage memory:

```c
/* drivers/net/ena/base/ena_com.c, line 960–969 */
static void ena_com_io_queue_free(..., struct ena_com_io_cq *io_cq)
{
    if (io_cq->cdesc_addr.virt_addr) {
        ENA_MEM_FREE_COHERENT(..., io_cq->cdesc_addr.virt_addr, ...);
        io_cq->cdesc_addr.virt_addr = NULL;   /* visible to all peers */
    }
    ...
}
```

Because `io_cq` lives at the same VA in every attached process (via
`/dev/hugepages/eph_*map_*` mappings), this NULL is **immediately
visible** to any secondary mid-`rte_eth_rx_burst` on that queue.

## How eph violated it (pre-fix)

`Platform::Impl::cleanup()` (primary path) called
`rte_eth_dev_stop` / `rte_eth_dev_close` / `rte_mempool_free`
unconditionally as soon as primary's `~Platform()` ran. There was no
check for "are other peers still attached?" — primary exit was
treated as device-wide teardown. `Platform::~Platform()`'s
`rte_eal_cleanup` had the same problem (it internally closes any
still-active devices, dispatching to the same `ena_close` path).

In single-process mode this was fine — there are no peers, primary
exit really *is* device shutdown. In multi-process mode it was a
DPDK-protocol violation.

## Evidence chain

The following data was captured in commit `aa625b4d` and remains
factually accurate; only the *interpretation* (whether ENA was at
fault, vs. eph) was wrong in the original framing.

### Crash register state (GDB)

```
Thread 1 "lat_udp" received signal SIGSEGV, Segmentation fault.
  #0 ena_com_get_next_rx_cdesc ()        librte_net_ena.so.25.0
  #1 ena_com_rx_pkt ()                   librte_net_ena.so.25.0
  #2 eth_ena_recv_pkts ()                librte_net_ena.so.25.0
  #3 rte_eth_rx_burst (port=0, queue=1)  rte_ethdev.h:6293
  #4 eph::net::dpdk::DpdkPoller<void>::poll ()
                                         poller.hpp:398
  #5 bench::scenarios::run_lat_udp_loop ()
                                         scenarios/lat_udp_loop.hpp:194
  #6 main ()                             benchmarks/latency/udp/lat_udp.cpp:146
```

| Register | Value | Meaning |
|----------|-------|---------|
| `x0` | `0x100393800` | `io_cq*` — in shared hugepages (`0x100200000–0x100e00000`) |
| `x1` | `0x8b0` (2224) | `byte_offset = (head-1) & mask * entry_size = 139 * 16` |
| `x2` | **`0x0`** | `io_cq->cdesc_addr.virt_addr` — **NULL after primary's stop write** |
| `x3` | `0x10` (16) | `entry_size` |
| `pc` | `0xfffff5847364` | `ena_com_get_next_rx_cdesc + 36` |

Faulting instruction: `ldr w3, [x2, w1, sxtw]` — reads `NULL + 2224 = 0x8b0`.
`x1 = 2224` proves ~140 completion descriptors had been processed
before the crash, confirming RSS was delivering real traffic to the
secondary's queue. The ring was live; it became NULL because primary
zeroed `cdesc_addr.virt_addr` from another process while the secondary
was mid-burst.

### Two-condition isolation (2026-04-30 A/B)

| Primary | Secondary | Result |
|---------|-----------|--------|
| `lat_tcp_dpdk` high-rate DpdkPoller I/O | `lat_udp_dpdk` full machinery | **CRASH** |
| `lat_tcp_dpdk` high-rate DpdkPoller I/O | minimal raw `rte_eth_rx_burst` loop | NO CRASH |
| benign (Platform up, no I/O) | `lat_udp_dpdk` full machinery | NO CRASH (753 k samples) |
| benign (Platform up, no I/O) | minimal raw `rte_eth_rx_burst` loop | NO CRASH |

The crash needed both a primary that actually *exits* (high-rate
benchmark runs to completion, triggers `~Platform()`) **and** a
secondary still mid-`rte_eth_rx_burst` at that moment. Either alone
left the race window unopened.

### Verified versions

| Component | Version |
|-----------|---------|
| DPDK | 24.11.2 |
| ENA PMD | `librte_net_ena.so.25` |
| Linux | 6.1.163-186.299.amzn2023.aarch64 |
| Instance | AWS EC2 c8g.4xlarge (Graviton4) |

## The fix (commits `0b3a4aaa` → `e7d69bd5`)

Five commits implement the gate plus its supporting plumbing:

| Commit | Layer | Change |
|--------|-------|--------|
| `0b3a4aaa` | API | `MpRegistryHandle::count_alive_procs()` + `is_last_alive_proc()` lock-free scan over the slot bitmap. 5 unit tests. |
| `b4074d62` | Gate | `Platform::Impl::cleanup()` (primary) and `Platform::~Platform()` defer `rte_eth_dev_stop`/`close`/`mempool_free`/`rte_eal_cleanup` when not last alive. |
| `ef1bec67` | Gate | `MpRegistryHandle::release_()` and (via `IcmpDirectoryHandle::disable_memzone_free()`) defer their own `rte_memzone_free` so peers' `hdr_` pointers stay live. |
| `3b66ee35` | Bench | Seven `lat_*_dpdk` binaries switched from hardcoded `register_poller(0, ...)` to `env.platform.effective_rx_queue_range().first` — they now bind to the queue actually owned by their MP slot. |
| `067dccbc` | Cleanup | `Impl::defer_for_peers()` private predicate dedups the three call sites; doc-comment redundancy trimmed. |

### Gate decision (single source of truth)

```cpp
// eph-net-dpdk/include/eph/dpdk/platform.hpp (Impl)
[[nodiscard]] bool defer_for_peers() const noexcept {
    return mp_registry.has_value() &&
           !mp_registry->is_last_alive_proc();
}
```

Three sites consult it in destructor order:

1. `Impl::cleanup()` — guards `rte_eth_dev_stop` / `rte_eth_dev_close`
   / per-lcore mempool free / canonical mempool free.
2. `~Impl()` body — gates `IcmpDirectoryHandle::disable_memzone_free()`
   so the field destructor that runs next skips its `rte_memzone_free`.
3. `~Platform()` — snapshot **before** `impl_.reset()` (because
   reset clears self slot via mp_registry's dtor); guards
   `rte_eal_cleanup`.

`MpRegistry::release_()` does its own scan because it runs *after*
self has already been cleared from the bitmap — it needs "any peer
alive" rather than "exactly one alive".

### Primary / secondary teardown timeline (post-fix)

```
                        T=0 ──────────────────── T=15 ─────── T=30
primary lat_tcp_dpdk    ███████████████████████| stops bench
                                                ↓
                                                ~Platform()
                                                  defer_for_peers? YES
                                                  → skip stop/close/free
                                                  → log "1 peer attached"
                                                  → return; OS reclaims
                                                    per-process maps

secondary lat_udp_dpdk        ███████████████████████████████| stops bench
                                                              ↓
                                                              ~Platform()
                                                                defer_for_peers? NO (last)
                                                                → stop/close/free fully
                                                                → eal_cleanup runs
```

The port stays running through primary's exit; the *physical*
teardown happens at the last attached process's `~Platform()`.

## v1 trade-offs

The current gate is refcount-only — it trusts every attached process
to release its slot via `MpRegistryHandle`'s RAII destructor.

**If a peer exits abnormally** (`kill -9`, OOM, SIGSEGV in unrelated
code) the slot is never released. Subsequent primary `~Platform()`
calls then never see "is last alive" and **never stop the port**.
Hugepage / vfio state leaks until the next clean session.

**Recovery** is `scripts/dpdk-teardown.sh`, which any user already
runs between DPDK benchmark sessions to recycle hugepages. The
tradeoff was deliberate: heartbeat-based liveness checking is more
complex (background thread, pid tracking, signal semantics) and adds
a new failure surface (false-positive reaper killing a slow process).
v2 candidate is in `TODO.md`.

## v2 candidate (deferred)

IPC heartbeat + reaper:

* Each `MpRegistryHandle` writes its `getpid()` and a periodic TSC
  heartbeat into its slot.
* A background thread (in primary, or in any attached peer) scans
  slots whose heartbeat has stalled by > N seconds and probes via
  `kill(pid, 0)`. Dead slots are CAS-cleared.
* Primary's gate then sees the corrected alive count and proceeds
  with teardown.

Not implemented in v1 because:

1. The user's `dpdk-teardown.sh` already covers the practical
   recovery path.
2. Heartbeat threading introduces signal-handling complexity worth
   designing carefully.

If symptoms appear (chronic hugepage leakage, slow next-session
startup), revisit.

## How to verify the fix is in place

Two harnesses live in the repo:

* **`tests/integration/repro_ena_mp_secondary_rxburst.cpp`** — single
  binary, fork+execv, exercises secondary `rte_eth_rx_burst` against
  an idle ring. Expected exit code: **9** ("sentinel holds"). If it
  ever exits 0 (SIGSEGV under idle), the gate has regressed — either
  ENA's idle-ring fast path changed, or the gate stopped firing.
  Build via `xmake build -g repros`.

* **`/tmp/ena_mp_rootcause.sh`** — primary `lat_tcp_dpdk` + secondary
  `lat_udp_dpdk` under GDB, both 15 s, full traffic. Pre-fix:
  reliable SIGSEGV in `ena_com_get_next_rx_cdesc`. Post-fix:
  `VERDICT: NO CRASH`, secondary samples ≥ 50 k.

Companion scripts validate the semantic change (port survives
primary exit) and the multi-scenario case:

* **`/tmp/ena_mp_rootcause_primary_early.sh`** — primary 5 s +
  secondary 30 s. Validates that secondary keeps polling for 25 s
  after primary exits.
* **`/tmp/ena_mp_7proc_parallel.sh`** — 7 `lat_*_dpdk` binaries
  share one ENA NIC via RSS, queues 0-6. All 7 PASS, ~1 M total
  samples.

## See also

* `eph-net-dpdk/docs/dpdk-multiprocess.md` — broader DPDK
  multi-process startup / IPC guide; includes a teardown-protocol
  cross-reference back to this document.
* `eph-net-dpdk/CHANGELOG.md` — `[Unreleased]` section's "Fixed"
  entry summarises the same change for release notes; the
  superseded "AWS ENA PMD limitation" entry below it preserves the
  original misframed wording in commit history.
* `.artifacts/retro-20260501-ena-mp-rootcause-discovery.md` — meta-retro
  on the methodology that finally pinned the root cause and the
  cognitive flip from "vendor limitation" to "library protocol bug".
* `TODO.md` — `Resolved` section with the fix-commit list; v2
  heartbeat-reaper candidate parked.

## Maintenance note

If commit hashes have shifted (squash / rebase), `git grep
'is_last_alive_proc'` finds the live implementation in
`eph-net-dpdk/include/eph/dpdk/detail/mp_registry.hpp`; the gate
sites in `Platform::Impl` are at `Impl::defer_for_peers()` /
`Impl::cleanup()` / `~Impl()` body / `~Platform()`.
