# eph-utils Benchmark History

## 2026-04-03 bench_time baseline (af80692)

| Benchmark | Time (ns) | CPU (ns) | Iterations |
|---|---|---|---|
| BM_TSCNow | 19.0 | 19.0 | 36808581 |
| BM_TSCToNs | 0.823 | 0.823 | 851122813 |
| BM_TSCToCycles | 1.67 | 1.67 | 422997931 |
| BM_TSCDeltaPair | 39.2 | 39.2 | 17870735 |

Platform: Linux aarch64, 16 cores @ 2.0 GHz, L3 36864 KiB

## 2026-04-03 bench_hugepage baseline (857b863)

| Benchmark | Time (ns) | CPU (ns) | Iterations |
|---|---|---|---|
| BM_HugePage_Make_4KB | 21789 | 21747 | 33527 |
| BM_StdUnique_Make_4KB | 44.9 | 44.9 | 15583711 |
| BM_HugePage_Make_1MB | 30135 | 30135 | 23763 |
| BM_StdUnique_Make_1MB | 6736 | 6736 | 100165 |
| BM_HugePage_Allocate_Deallocate_64KB | 4116 | 4113 | 166441 |
| BM_HugePage_SequentialAccess_4MB | 48865 | 48863 | 11682 |
| BM_StdUnique_SequentialAccess_4MB | 47509 | 47509 | 13763 |

Note: HugePage allocation uses mmap(MAP_HUGETLB) which has high syscall
overhead. The benefit is TLB miss reduction on large working sets, visible
at scale but not in this microbenchmark.

## 2026-04-03 bench_recorder baseline (0484dc9)

| Benchmark | Time (ns) | CPU (ns) | Iterations |
|---|---|---|---|
| BM_RecorderRecord | 3.38 | 3.38 | 207226087 |
| BM_RecorderRecordValues | 3.42 | 3.42 | 204772271 |
| BM_ConcurrentRecorderRecord_1Thread | 9.05 | 9.05 | 75701282 |
| BM_ConcurrentRecorderRecord_MT/threads:1 | 9.53 | 9.53 | 73418800 |
| BM_ConcurrentRecorderRecord_MT/threads:2 | 9.53 | 9.53 | 73364666 |
| BM_ConcurrentRecorderRecord_MT/threads:4 | 9.54 | 9.54 | 73346768 |

ConcurrentRecorder shows linear scaling due to zero-contention
thread_local design. Single-thread overhead is ~3x vs Recorder
(unordered_map lookup for thread-local guard).

## 2026-04-03 bench_cpu baseline (3d2744d)

| Benchmark | Time (ns) | CPU (ns) | Iterations |
|---|---|---|---|
| BM_CpuRelax | 0.357 | 0.357 | 1958293113 |
| BM_EmptyLoop | 0.357 | 0.357 | 1958508982 |

ARM YIELD instruction has zero measurable overhead vs empty loop on Graviton.

## 2026-04-03 bench_system_stats baseline (c946171)

| Benchmark | Time (ns) | CPU (ns) | Iterations |
|---|---|---|---|
| BM_SystemStatsSnapshot | 12911 | 12911 | 54169 |
| BM_SystemStatsReset | 230 | 230 | 3010891 |
| BM_SystemResourceStatsDump | 506 | 506 | 1384684 |
| BM_SystemResourceStatsToJson | 526 | 526 | 1331117 |
| BM_SystemResourceStatsFormat | 463 | 463 | 1511604 |

Snapshot is expensive (~13us) due to getrusage syscall + /proc reads.
Reset is cheap (~230ns) as it only calls getrusage once.
Formatting methods are ~500ns each (std::format overhead).

## 2026-04-03 bench_hdr_histogram extended (1ede336)

| Benchmark | Time (ns) | CPU (ns) | Iterations |
|---|---|---|---|
| BM_HdrGetPercentiles_Batch | 11753 | 11754 | 59392 |
| BM_HdrReport | 59215 | 59215 | 11773 |
| BM_HdrToJson | 72054 | 72055 | 9971 |
| BM_HdrForEachLinear_HighOffset | 7760 | 7760 | 90378 |

Batch percentile query (~12us for 5 percentiles) is ~2.5x faster than
5 individual queries. Report/JSON formatting is 60-72us (std::format
+ percentile computation). for_each_linear on high-offset data benefits
from the skip-empty-prefix optimization.

## 2026-04-03 bench_ema baseline (631ab93)

| Benchmark | Time (ns) | CPU (ns) | Iterations |
|---|---|---|---|
| BM_EmaUpdate | 5.07 | 5.07 | 138134704 |
| BM_EmaUpdate_Random | 4.84 | 4.84 | 144808656 |
| BM_EmaCrossover_Update | 4.83 | 4.83 | 145229410 |

EMA update is ~5ns per sample. Random prices do not add cache pressure
(8K buffer fits in L1). EmaCrossover (dual EMA) matches single EMA
cost due to branch-free steady-state execution.

## 2026-04-03 bench_timestamp baseline (631ab93)

| Benchmark | Time (ns) | CPU (ns) | Iterations |
|---|---|---|---|
| BM_MsToNs | 0.357 | 0.357 | 1958074443 |
| BM_NsToMs | 0.450 | 0.450 | 1554835173 |
| BM_NowNs | 31.2 | 31.2 | 22446058 |
| BM_NowMs | 31.3 | 31.3 | 22382463 |
| BM_FeedLatencyNs | 31.2 | 31.2 | 22462559 |
| BM_FormatTimestampNs | 414 | 414 | 1690856 |
| BM_FormatTimestampMs | 404 | 404 | 1733725 |

Unit conversions are sub-nanosecond (compiler constant-folds).
clock_gettime(CLOCK_REALTIME) costs ~31ns on this platform.
ISO 8601 formatting is ~400ns (gmtime_r + std::format).

## 2026-04-03 bench_audit_log extended (d15eb9f)

| Benchmark | Time (ns) | CPU (ns) | Iterations |
|---|---|---|---|
| BM_AuditEntryDump | 644 | 644 | 1086560 |

Single-entry dump formatting costs ~644ns (TSC::to_ns + std::format).
