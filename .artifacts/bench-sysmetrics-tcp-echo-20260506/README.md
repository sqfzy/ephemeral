# bench-sysmetrics-tcp-echo — README

This artifact compares **kernel TCP echo** vs **DPDK TCP echo** on system metrics
(cycles, IPC, cache misses, context switches, NIC IRQs) under identical
`lat_tcp` workload (300s, 256-byte payload, no TLS).

- See **REPORT.md** for the cross-run mean/stddev table + ASCII bars.
- Per-trial JSONs: `kernel/trial-{1,2,3}.json` and `dpdk/trial-{1,2,3}.json`.
- Raw `perf stat` outputs and /proc snapshots also archived alongside.

CPU pinning (revised — CPU 2/3/6 reserved for other workloads):
- `cpu_client=4`  (lat_tcp / lat_tcp_dpdk main thread; also DPDK RX queue
   poller for single-queue mode → queue core = upper core ✅)
- `cpu_mock=5`    (mockex echo handler)
- `eal_cores=0,1` (DPDK EAL housekeeping; not the RX poller in single-queue)

Built with: lat_tcp[_dpdk] from current HEAD, eph-nicd post-T2.3-revert.

Reproduce: see REPORT.md "复现" section.
