# AWS ENA PMD: Multi-Process secondary RX crashes under traffic

> **TL;DR.** On AWS ENA, a secondary DPDK process crashes with SIGSEGV inside
> `ena_com_get_next_rx_cdesc` when **both** of the following are true simultaneously:
> **(1)** primary is doing high-rate `DpdkPoller`-driven I/O **and** **(2)** secondary
> is driving `DpdkPoller::poll()` → `DpdkUdpSocket` receive path.
>
> **Confirmed root cause (2026-04-30):** Primary's `rte_eth_dev_stop` calls
> `ena_queue_stop` for **all** queues — including the secondary's queue — and
> `ena_com_io_queue_free` writes `io_cq->cdesc_addr.virt_addr = NULL` into shared
> hugepage memory. Secondary's concurrent `rte_eth_rx_burst(queue=1)` reads this NULL
> and faults at `ldr w3, [x2, w1, sxtw]` (`x2 = 0x0`, `x1 = 2224`, faulting address
> `0x8b0`). See "Confirmed root cause" section for GDB register evidence and source
> trace.

## Scope (precise)

|                                                  | secondary OK?    |
|--------------------------------------------------|------------------|
| `rte_mempool_lookup` / mempool & memzone access  | yes              |
| `Platform::create_secondary` / `join_dynamic` (control plane) | yes |
| IPC primitives, ICMP registry, FlowDirector fallback | yes          |
| `rte_eth_tx_burst` (broadcast-MAC, idle ring) | yes                  |
| `rte_eth_rx_burst` on idle ring (head==tail)  | yes (returns 0)      |
| raw `rte_eth_rx_burst` + live traffic, even with high-rate primary | **no crash** |
| `DpdkPoller::poll()` (secondary full machinery) + idle primary | **no crash** |
| `DpdkPoller::poll()` (secondary full machinery) + high-rate primary | **CRASH** (SIGSEGV) |

The idle-ring case works because ENA's `ena_com_get_next_rx_cdesc`
short-circuits on `head == tail` before it dereferences ring metadata
that lives in primary's heap. The crash occurs only when both the
primary's high-rate burst-poll loop and the secondary's full
`DpdkPoller`/`DpdkUdpSocket` machinery are running concurrently.

## Reference backtrace

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

## Verified versions

| Component | Version |
|-----------|---------|
| DPDK | 24.11.2 |
| ENA PMD | `librte_net_ena.so.25` (DPDK 24.11.2 in-tree) |
| Linux | 6.1.163-186.299.amzn2023.aarch64 |
| Instance | AWS EC2 c8g.4xlarge (Graviton4) |
| NIC | Amazon Elastic Network Adapter, vfio-pci-bound, PCI 0000:28:00.0 |

## Reproducers

Two complementary reproducers, intentionally split by what they cost
to run:

### `tests/integration/repro_ena_mp_secondary_rxburst.cpp` (idle-ring sentinel)

Fork+execv self; child attaches as secondary, calls `tx_burst` once and
`rx_burst` twice (own queue + primary's queue) against idle rings. Single
binary, no mockex, no external traffic.

This reproducer exits **9** ("sentinel holds") on ENA / DPDK 24.11.2 —
**expected**. No traffic lands on the secondary's queue so the idle-ring
fast-path fires and there is no crash.

It is preserved as a **regression sentinel for the idle-ring path**:
if it ever exits 0 (SIGSEGV under idle), that is new evidence worth
chasing. Build via `xmake build -g repros`.

### `diag/ena-mp-isolation-2` branch (with traffic, heavyweight)

The actual crash trigger requires **real traffic landing on the
secondary's queue**. The branch `diag/ena-mp-isolation-2` carries:

1. `DpdkBenchEnv::create_via_autojoin` (cherry-picked from the
   originally hard-reset commit `d1e5fb97`).
2. An `EPH_LAT_AUTOJOIN_*` envvar diversion in
   `benchmarks/latency/core/dpdk_env.hpp` that routes lat scenarios
   through `Platform::join_dynamic` instead of declarative bring-up.
3. An `EPH_LAT_DECLARATIVE_SECONDARY=1` switch (also envvar-gated)
   that further routes the secondary through
   `Platform::create_with_eal(proc_type=Secondary)` so we can A/B
   the autojoin vs declarative paths under identical traffic.
4. Two scripts: `/tmp/ena_mp_isolation.sh` (autojoin secondary) and
   `/tmp/ena_mp_isolation_step2.sh` (declarative secondary).

Both scripts spawn a primary (`lat_tcp_dpdk`) + a secondary
(`lat_udp_dpdk` under `gdb -batch`) against independent kernel
mockex instances on NIC_A. Both reliably reproduce the SIGSEGV with
identical stacks — that's the experimental basis for "this is a real
ENA limitation, not eph autojoin specific".

This branch is **not merged to main** by design. It re-introduces
diagnostic-only code paths (envvar-gated bring-up branches) that
should not exist in the production benchmarks.

## Isolation log (post-mortem)

Originally the parallel-bench v1 retro and TODO recorded this as
"ENA MP secondary RX starvation under primary load" and later as
"ENA PMD MP secondary `rx_burst` is fundamentally broken". Both
phrasings were **overclaimed** based on a single observation that
bundled four confounders (autojoin vs declarative path /
`DpdkPoller::poll` machinery vs raw rx_burst / mockex traffic vs
idle / temporary hand-written autojoin envvars).

The 2026-04-30 isolation experiment ran two independent A/B axes:

**Phase 1 — autojoin vs declarative (same traffic)**

| Test | bring-up | traffic? | Result |
|------|----------|----------|--------|
| Minimal repro (current file) | autojoin (`join_dynamic`) | none | exit 9 (no crash) |
| `ena_mp_isolation.sh` step 1 | autojoin | yes (mockex echo) | SIGSEGV |
| `ena_mp_isolation.sh` step 2 | declarative | yes (mockex echo) | SIGSEGV (identical stack) |

Conclusion: both bring-up paths produce identical results. Traffic is
what matters — but a further A/B revealed traffic alone is not the
full story.

**Phase 2 — two-condition A/B (2026-04-30, scripts on `diag/ena-mp-isolation-2`)**

| Primary | Secondary | Result |
|---------|-----------|--------|
| `lat_tcp_dpdk` high-rate DpdkPoller I/O | `lat_udp_dpdk` full machinery | **CRASH** (baseline) |
| `lat_tcp_dpdk` high-rate DpdkPoller I/O | minimal raw `rte_eth_rx_burst` loop | NO CRASH |
| benign (Platform up, no I/O) | `lat_udp_dpdk` full machinery | NO CRASH (753k samples, 50k/s) |
| benign (Platform up, no I/O) | minimal raw `rte_eth_rx_burst` loop | NO CRASH |

**Conclusion (precise, two conditions required):** The crash requires
**both** simultaneously:
1. Primary doing high-rate `DpdkPoller`-driven I/O — `Stream::send` +
   `DpdkPoller::poll` burst loop (as in `lat_tcp_dpdk` + mockex).
2. Secondary driving `DpdkPoller::poll()` → `DpdkUdpSocket` receive path
   (as in `lat_udp_dpdk`).

Neither condition alone is sufficient. Raw `rte_eth_rx_burst` in the
secondary does not trigger the crash even under heavy primary load and
live traffic. An idle primary does not trigger the crash even when the
secondary is running the full `DpdkPoller`/`DpdkUdpSocket` machinery.

## Confirmed root cause (2026-04-30 GDB analysis)

### Crash register state

Captured via `gdb -batch -ex "handle SIGSEGV stop print" -ex "run" -ex "info registers"` on
`lat_udp_dpdk` as secondary process (PID 1550440, queue=1):

| Register | Value | Meaning |
|----------|-------|---------|
| `x0` | `0x100393800` | `io_cq*` — in shared hugepages (`0x100200000–0x100e00000`) |
| `x1` | `0x8b0` (2224) | `byte_offset = (head-1) & mask * entry_size = 139 * 16` |
| `x2` | **`0x0`** | `io_cq->cdesc_addr.virt_addr` — **NULL** (root cause) |
| `x3` | `0x10` (16) | `entry_size` (still in register from `ldrb w3, [x0, #149]`) |
| `pc` | `0xfffff5847364` | `ena_com_get_next_rx_cdesc + 36` |

Faulting instruction: `ldr w3, [x2, w1, sxtw]` — reads from `NULL + 2224 = 0x8b0`.

`x1 = 2224` is computed from `head ≈ 140` (meaning ~140 completion descriptors were
processed, confirming RSS delivered real traffic to queue 1). At that same moment,
`cdesc_addr.virt_addr` (at `io_cq + 8`) was **already NULL**, though head was non-zero —
the ring-buffer pointer had been torn down while the hardware head counter kept advancing.

### Mechanical cause: primary tears down ALL queues on stop

`rte_eth_dev_stop(port=0)` (called by primary's `~Platform()`) dispatches to
`ena_stop(dev)` in the ENA PMD. The function's first instruction checks the process type:

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

There is no per-queue ownership check: **primary stops every queue, including queue 1
which is exclusively owned by the secondary**. The per-process guard (`cbnz`) only
prevents a _secondary caller_ from stopping the device; it does not protect secondary-owned
queues from a _primary caller_'s stop.

`ena_queue_stop(ring[1])` calls `ena_com_destroy_io_queue`, which calls
`ena_com_io_queue_free(ena_dev, io_sq, io_cq)`:

```c
/* drivers/net/ena/base/ena_com.c, line 960–969 */
static void ena_com_io_queue_free(..., struct ena_com_io_cq *io_cq)
{
    if (io_cq->cdesc_addr.virt_addr) {
        ENA_MEM_FREE_COHERENT(..., io_cq->cdesc_addr.virt_addr, ...);
        io_cq->cdesc_addr.virt_addr = NULL;   /* ← writes NULL into shared hugepage */
    }
    ...
}
```

Because `io_cq` lives in **shared hugepage memory** (same VA mapped in both processes via
`/dev/hugepages/eph_0000_28_00_0map_*`), this store of `NULL` is immediately visible to
the secondary. If secondary calls `rte_eth_rx_burst(0, 1, ...)` at or after this point,
`eth_ena_recv_pkts` reads `cdesc_addr.virt_addr = NULL` and crashes at the
`ldr w3, [x2, w1, sxtw]` load.

### Why both conditions are required

| Condition | Effect |
|-----------|--------|
| Primary high-rate DpdkPoller I/O | Primary's 15 s benchmark completes → `~Platform()` → `rte_eth_dev_stop` → writes NULL to queue 1's `virt_addr` in shared hugepage |
| Secondary full DpdkUdpSocket + DpdkPoller | Secondary has real traffic flowing to queue 1, runs for its full 15 s duration. When primary teardown races with secondary's `rte_eth_rx_burst(queue=1)` call, the NULL load triggers the SIGSEGV |

**Idle primary (no `rte_eth_dev_stop` while secondary is alive):** if primary stays
running past secondary's completion window (or exits before starting its full teardown
sequence), the race window never opens — no crash.

**Raw `rte_eth_rx_burst` secondary (no real traffic, short lifetime):** the
`repro_ena_mp_secondary_rxburst` sentinel exits its fixed short run before primary's
teardown fires — no race window.

### Summary

The bug is in the ENA PMD's `ena_stop`: it guards against _secondary callers_ but does
not guard against _primary callers destroying secondary-owned queues_. A primary
`rte_eth_dev_stop` nullifies `io_cq->cdesc_addr.virt_addr` for **all** queues in shared
hugepage memory, with no synchronisation against concurrently-polling secondary processes.
This is an ENA PMD limitation; the DPDK spec permits PMDs to not support secondary I/O
burst, but ENA's failure mode is silent memory corruption rather than a documented
`ENOTSUP`.

## Recovery

### Single-process N-lcore (recommended; what eph already ships)

`benchmarks/latency/lat_multi_dpdk.cpp` and the generic template
`examples/simple_hft_dpdk_rss.cpp` use one EAL session, one
`Platform`, N RX queues, N pollers, N lcores via
`rte_eal_remote_launch`. The sole owner of the I/O burst APIs is
primary; secondaries never touch them; ENA's limitation is bypassed
entirely while RX still scales across cores via RSS.

### Different NIC

Most Intel PMDs (`net_i40e`, `net_ice`, `net_ixgbe`) and Mellanox
(`net_mlx5`) export per-queue I/O state via memzones and support
secondary `rx_burst` / `tx_burst`. If your application architecture
genuinely requires multi-process I/O sharing, deploy on one of those
instead of ENA.

## Upstream status

Not currently filed against DPDK upstream. The behaviour is
consistent with DPDK's "PMDs MAY support secondary I/O" contract —
ENA's choice is a documented absence of feature, not a regression.
The fact that empty-ring secondary `rx_burst` is safe but
populated-ring is unsafe suggests the per-queue state could be
exported via memzone with bounded effort; if a future ENA PMD
release does so, the `diag/ena-mp-isolation-2` reproducers will
stop crashing.
