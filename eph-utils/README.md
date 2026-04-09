# eph-utils

Header-only C++23 foundation library for the `ephemeral_dev` low-latency
networking and trading codebase. Provides the primitives that every other
`eph-*` subproject builds on: TSC-based nanosecond timing, HDR histograms
and latency recorders, CPU topology / affinity / real-time scheduling,
huge-page allocation, cache-line alignment, a regulatory audit trail,
wall-clock helpers, EMAs, a metrics console sink, and a `getrusage`-based
system profiler.

Designed for HFT hot paths where nanosecond-level determinism matters:
every primitive is zero-allocation on the hot path, `noexcept` where it
can be, logs through `spdlog` at compile-time-filtered levels, and falls
back gracefully on non-Linux / non-x86_64 hosts.

## Layout

```
eph-utils/
├── include/eph/
│   ├── utils.hpp                  -- convenience aggregation header
│   └── utils/
│       ├── alignment.hpp          -- CACHE_LINE_SIZE, Align<T>
│       ├── audit_log.hpp          -- AuditLog<N>, AuditEntry, AuditEvent
│       ├── console_sink.hpp       -- ConsoleSink (core::MetricsSink impl)
│       ├── cpu.hpp                -- topology, set_thread_affinity,
│       │                             set_thread_realtime, cpu_relax,
│       │                             spin_for_ns
│       ├── cpu_pin.hpp            -- pin_thread_strict (isolcpus + SMT +
│       │                             NUMA + IRQ validation)
│       ├── ema.hpp                -- Ema, EmaCrossover
│       ├── hdr_histogram.hpp      -- HdrHistogram, measure_tsc, ScopedTSC,
│       │                             Stats
│       ├── hugepage.hpp           -- HugePage::make<T>, allocate,
│       │                             deallocate
│       ├── phased_timer.hpp       -- PhasedTimer (warmup + measurement)
│       ├── record.hpp             -- aggregation header (hdr + recorder +
│       │                             system_stats)
│       ├── recorder.hpp           -- Recorder, ConcurrentRecorder
│       ├── system_stats.hpp       -- SystemStats, SystemResourceStats
│       ├── time.hpp               -- TSC (rdtscp / cntvct_el0 / fallback)
│       └── timestamp.hpp          -- wall-clock helpers, ISO 8601 format
├── tests/                         -- GoogleTest unit tests (15 files)
├── benchmarks/                    -- Google Benchmark microbenchmarks (9)
└── xmake.lua                      -- build description
```

## Dependencies

- **eph-core** — `core::MetricsSink` concept (consumed by `console_sink`),
  `core::detail::json_escape` (consumed by `hdr_histogram` and `recorder`).
  Declared as a public dep so consumers transitively pick it up.
- **spdlog** — all logging (public dep, filtered at compile time via
  `SPDLOG_ACTIVE_LEVEL` set from the root `net_log_level` variable).
- **gtest** — unit tests only (`rule("eph-test")`).
- **benchmark** — Google Benchmark for `benchmarks/*.cpp`
  (`rule("eph-bench")`).

Platform-specific system headers: `<sys/mman.h>`, `<pthread.h>`, `<sched.h>`,
`<sys/resource.h>` on Linux; `<memoryapi.h>` on Windows; `<mach/*>` on
macOS. All wrapped behind `#if defined(__linux__) | __APPLE__ | _WIN32`
blocks with sensible fallbacks.

## Build

Everything is driven by xmake from the repo root:

```bash
# header-only target — the library compiles only when a consumer
# #includes it, so `xmake build eph-utils` is essentially a no-op check
xmake build eph-utils

# build + run the full unit test group
xmake build -g tests
xmake run -g tests

# a single test
xmake build test_spin_for_ns
xmake run test_spin_for_ns

# build + run a benchmark
xmake build bench_hdr_histogram
xmake run bench_hdr_histogram
```

Debug / ASan / TSan modes are wired at the project root:

```bash
xmake f -m debug && xmake build -g tests
xmake f -m asan  && xmake build -g tests
xmake f -m tsan  && xmake build -g tests
```

`native_arch=y` enables `-march=native` for benchmark targets only.

## Quick tour

### TSC timing

```cpp
#include <eph/utils/time.hpp>

eph::utils::TSC::init();               // once per process, multi-sample
                                       // median calibration, CV warning
uint64_t t0 = eph::utils::TSC::now();  // ~20 cycles on x86-64 (rdtscp)
hot_path();
uint64_t t1 = eph::utils::TSC::now();
auto ns = eph::utils::TSC::delta_ns(t0, t1);  // std::optional<double>
```

### HdrHistogram + Recorder

```cpp
#include <eph/utils/recorder.hpp>

eph::utils::Recorder rec("OrderSubmit");  // default range 1 cycle .. ~10s
for (int i = 0; i < 100'000; ++i) {
    uint64_t t0 = eph::utils::TSC::now();
    submit_order();
    (void)rec.record(eph::utils::TSC::now() - t0);
}
rec.print_report();            // tabulated console output
(void)rec.export_json();       // outputs/OrderSubmit_<ts>.json
```

For multi-threaded benchmarks use `ConcurrentRecorder`, which holds one
`thread_local` histogram per thread and merges retired threads into a
shared buffer via a `shared_ptr<SharedState>` so data is never lost even
if the recorder outlives the thread.

### CPU pinning (strict)

```cpp
#include <eph/utils/cpu_pin.hpp>

eph::utils::CpuPinPolicy policy{};  // all checks on by default
if (auto r = eph::utils::pin_thread_strict(2, "poll", policy); !r) {
    spdlog::error("{}", r.error());
    return 1;
}
```

`pin_thread_strict` verifies the cpu is in `/sys/devices/system/cpu/isolated`,
that no SMT sibling has already been pinned from this process, that
consecutive pins stay on the same NUMA node, and (warn-only) that the
cpu doesn't have active IRQs in `/proc/interrupts`. Relaxed with
`policy.require_isolcpus = false` for dev hosts.

### Huge pages

```cpp
#include <eph/utils/hugepage.hpp>

auto buf = eph::utils::HugePage::make<std::array<char, 10 * 1024 * 1024>>();
// unique_ptr<T, HugePage::Deleter<T>> — mmap(MAP_HUGETLB) on Linux,
// VirtualAlloc(MEM_LARGE_PAGES) on Windows, aligned_alloc fallback
// everywhere else. Fallback is silent, logged at WARN.
```

### Regulatory audit log

```cpp
#include <eph/utils/audit_log.hpp>

eph::utils::AuditLog<8192> audit;  // capacity must be power of 2
(void)audit.record(eph::utils::AuditEvent::NewOrder,
                   /*order_id=*/12345, /*price=*/50100.50,
                   /*qty=*/1.5, eph::utils::Side::Buy,
                   /*venue_id=*/0);
// ... later, flush to disk for compliance reporting
(void)audit.flush_to_file("/var/log/audit/session.bin");
```

64-byte cache-line-aligned entries, single-writer `record()` or
multi-writer `record_mt()` via an atomic head index and per-slot
`committed_` publication flags to avoid readers observing torn writes.

### EMA / crossover

```cpp
#include <eph/utils/ema.hpp>

eph::utils::EmaCrossover cross(5, 20);  // fast=5, slow=20
for (double price : stream) {
    switch (cross.update(price)) {
        case eph::utils::EmaCrossover::Signal::BullishCross: enter_long(); break;
        case eph::utils::EmaCrossover::Signal::BearishCross: exit_long(); break;
        case eph::utils::EmaCrossover::Signal::None: break;
    }
}
```

NaN / Inf inputs are silently rejected (state unchanged) so a single
bad tick can't poison the signal.

## Tests

15 GoogleTest files under `tests/`, one per module plus `test_version`.
All tests are `[nodiscard]`-clean and cover boundary conditions:

| Test file                 | Covers                                                   |
|---------------------------|----------------------------------------------------------|
| `test_alignment.cpp`      | `CACHE_LINE_SIZE`, `Align<T>`                            |
| `test_audit_log.cpp`      | ring wrap, multi-writer CAS, flush, dump, side display   |
| `test_console_sink.cpp`   | counter/gauge/histogram, tag quoting                     |
| `test_cpu.cpp`            | topology, affinity, `cpu_relax`, `CpuTopologyInfo` format|
| `test_cpu_pin.cpp`        | isolcpus / SMT / NUMA / IRQ checks                       |
| `test_ema.cpp`            | alpha bounds, NaN rejection, crossover edge cases        |
| `test_hdr_histogram.cpp`  | percentiles, merge/subtract, linear / percentile iter    |
| `test_hugepage.cpp`       | zero-size, fallback, destructor                          |
| `test_record.cpp`         | Stats dump/json, operator-                               |
| `test_recorder.cpp`       | record, export_json/csv, overflow saturation             |
| `test_spin_for_ns.cpp`    | busy-wait accuracy at 1 us / 10 us / 100 us              |
| `test_system_stats.cpp`   | delta, move semantics, format                            |
| `test_time.cpp`           | TSC calibration, CV, NaN/Inf edge cases, delta_ns        |
| `test_timestamp.cpp`      | ms/us/ns conversions, ISO 8601, Y2K38 guard              |
| `test_version.cpp`        | version string                                           |

## Benchmarks

9 Google Benchmark files under `benchmarks/`. Typical numbers on a
3.4 GHz x86-64 host (from the historical `bench_*` commits):

- `TSC::now()` ~6 ns
- `HdrHistogram::record()` ~5-10 ns
- `EmaCrossover::update()` ~10 ns
- `AuditLog::record()` ~15 ns (single-writer)
- `cpu_relax()` ~2 ns

Run one with `xmake build bench_<name> && xmake run bench_<name>`.

## License

See the repository root for license terms.
