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
