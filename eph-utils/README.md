# eph-utils

Header-only C++23 foundation library for the `ephemeral_dev` low-latency
networking and trading codebase. Provides the primitives that every other
`eph-*` subproject builds on: TSC-based nanosecond timing, HDR histograms
and latency recorders, CPU topology / affinity / strict pinning / real-time
scheduling, huge-page allocation, cache-line alignment, a regulatory audit
trail, wall-clock helpers, EMAs, a two-phase bench timer, a metrics console
sink, a `getrusage`-based system profiler, HFT-grade compliance primitives
(kill switch, token-bucket rate limiter), a cooperative shutdown flag, and
a `setns(2)` helper for Linux netns-isolated test fixtures.

Designed for HFT hot paths where nanosecond-level determinism matters:
every primitive is zero-allocation on the hot path, `noexcept` where it
can be, logs through `spdlog` at compile-time-filtered levels, and falls
back gracefully on non-Linux / non-x86_64 hosts.

## Layout

```
eph-utils/
├── include/eph/
│   ├── utils.hpp                  -- convenience aggregation header
│   │                                 (see "Aggregation header" below —
│   │                                 does NOT include every module)
│   └── utils/
│       ├── alignment.hpp          -- CACHE_LINE_SIZE, Align<T>
│       ├── audit_log.hpp          -- AuditLog<N>, AuditEntry, AuditEvent
│       ├── console_sink.hpp       -- ConsoleSink (core::MetricsSink impl)
│       ├── cpu.hpp                -- topology, set_thread_affinity,
│       │                             set_thread_realtime, cpu_relax,
│       │                             spin_for_ns, pin_thread +
│       │                             CpuPinPolicy (strict isolcpus /
│       │                             SMT / NUMA / IRQ validation),
│       │                             register_external_pin (DPDK lcore
│       │                             & RT framework integration)
│       ├── ema.hpp                -- Ema, EmaCrossover
│       ├── hdr_histogram.hpp      -- HdrHistogram, measure_tsc, ScopedTSC,
│       │                             Stats
│       ├── hugepage.hpp           -- HugePage::make<T>, allocate,
│       │                             deallocate
│       ├── kill_switch.hpp        -- KillSwitch (single-fire, irreversible)
│       ├── phased_timer.hpp       -- PhasedTimer (warmup + measurement)
│       ├── rate_limiter.hpp       -- TokenBucket (weighted, thread-safe)
│       ├── record.hpp             -- aggregation header (hdr + recorder +
│       │                             system_stats)
│       ├── recorder.hpp           -- Recorder, ConcurrentRecorder
│       ├── shutdown_signal.hpp    -- g_shutdown_flag +
│       │                             install_shutdown_handlers()
│       ├── system_stats.hpp       -- SystemStats, SystemResourceStats
│       ├── time.hpp               -- TSC (rdtscp / cntvct_el0 / fallback)
│       ├── timestamp.hpp          -- wall-clock helpers, ISO 8601 format
│       └── linux/
│           └── netns.hpp          -- enter_netns() for test fixtures
├── tests/                         -- GoogleTest unit tests (22 files)
├── benchmarks/                    -- Google Benchmark microbenchmarks (9)
└── xmake.lua                      -- build description
```

### Aggregation header

`include/eph/utils.hpp` pulls in every header under `include/eph/utils/`
(`alignment`, `audit_log`, `console_sink`, `cpu`, `ema`,
`hdr_histogram`, `hugepage`, `kill_switch`, `phased_timer`,
`rate_limiter`, `record`, `recorder`, `shutdown_signal`,
`system_stats`, `time`, `timestamp`).

The only public header **not** transitively included is
`linux/netns.hpp` — it is POSIX/Linux-only and used by test fixtures
that enter a network namespace; pull it in explicitly when needed.

Note: including `kill_switch` and `rate_limiter` via the umbrella
adds the type definitions but does not install anything; they are
inert until the consumer constructs an instance. `shutdown_signal`
likewise only installs signal handlers when the consumer calls
`install_shutdown_handlers()` — including the header is safe.

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
#include <eph/utils/cpu.hpp>

eph::utils::CpuPinPolicy policy{};  // all checks on by default
if (auto r = eph::utils::pin_thread(2, "poll", policy); !r) {
    spdlog::error("{}", r.error());
    return 1;
}
```

`pin_thread` verifies the cpu is in `/sys/devices/system/cpu/isolated`,
that no SMT sibling has already been pinned from this process, that
consecutive pins stay on the same NUMA node, and (warn-only) that the
cpu doesn't have active IRQs in `/proc/interrupts`. Relaxed with
`policy.require_isolcpus = false` for dev hosts. An argument-free
`pin_thread(cpu)` overload exists for the common "no name, default
policy" case.

**For DPDK EAL lcore threads, use `eph::dpdk::pin_lcore` /
`pin_lcores` / `EalGuard::init` (in `eph-net-dpdk/include/
eph/dpdk/lcore_pin.hpp`) instead** — worker lcores are spawned *inside*
`rte_eal_init`, so the right pattern is "register the cpu pre-EAL, let
EAL do the actual setaffinity from `--lcores=N@cpu`". The dpdk path
shares this same process-wide pin registry, so a strict `pin_thread`
on a non-lcore worker continues to detect SMT / NUMA conflicts against
running EAL lcores. See
[`eph-net-dpdk/docs/lcore-pin-integration.md`](../eph-net-dpdk/docs/lcore-pin-integration.md)
for the full rationale and escape-hatch rules.

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

### Kill switch (irreversible, single-fire)

```cpp
#include <eph/utils/kill_switch.hpp>

eph::utils::KillSwitch ks{[] {
    SPDLOG_ERROR("kill switch tripped — cancelling open orders");
    cancel_all();
}};
while (!ks.tripped()) { process_events(); }
ks.trip();   // idempotent; callback fires exactly once across all threads
```

By design there is **no** `reset()` / `untrip()` / `clear()` — a tripped
switch requires process restart + human review. Enforced at compile time
via `static_assert`. Thread-safe acquire-release CAS internally.

### Token-bucket rate limiter (weighted, thread-safe)

```cpp
#include <eph/utils/rate_limiter.hpp>

eph::utils::TokenBucket binance{{.capacity = 1200, .refill_per_second = 20.0}};
if (!binance.try_acquire(1))  { /* denied */ }
if (!binance.try_acquire(10)) { /* expensive endpoint denied */ }
```

Mutex-guarded (`steady_clock`-driven), fixed capacity / refill, weighted
acquire matching Binance / OKX style budgets. `try_acquire(0)` is a free
no-op; `try_acquire(weight > capacity)` fails immediately rather than
spinning forever.

### Cooperative shutdown flag

```cpp
#include <eph/utils/shutdown_signal.hpp>

eph::utils::install_shutdown_handlers();   // SIGINT + SIGTERM
while (eph::utils::g_shutdown_flag.load(std::memory_order_relaxed)) {
    run_one_iteration();
}
```

Lives at the library level so tests and operational tools share one flag
rather than each installing their own.

### Linux netns entry (test fixtures only)

```cpp
#include <eph/utils/linux/netns.hpp>

if (auto r = eph::utils::linux_::enter_netns("bench_ns"); !r) {
    spdlog::error("{}", r.error());
}
```

Used by the latency-bench host transitions and by DPDK / kernel
integration fixtures that need namespace isolation. Requires
`CAP_SYS_ADMIN` and a pre-existing `/var/run/netns/<name>` (e.g. via
`ip netns add`).

## Tests

20 GoogleTest files under `tests/`, covering every public module plus
`test_version`. All tests are `[nodiscard]`-clean and exercise boundary
conditions:

| Test file                       | Covers                                                      |
|---------------------------------|-------------------------------------------------------------|
| `test_alignment.cpp`            | `CACHE_LINE_SIZE`, `Align<T>`                               |
| `test_audit_log.cpp`            | ring wrap, multi-writer CAS, flush, dump, side display      |
| `test_console_sink.cpp`         | counter/gauge/histogram, tag quoting                        |
| `test_cpu.cpp`                  | topology, affinity, `cpu_relax`, `CpuTopologyInfo` format   |
| `test_cpu_pin.cpp`              | `pin_thread` isolcpus / SMT / NUMA / IRQ checks             |
| `test_ema.cpp`                  | alpha bounds, NaN rejection, crossover edge cases           |
| `test_hdr_histogram.cpp`        | percentiles, merge/subtract, linear / percentile iter       |
| `test_hugepage.cpp`             | zero-size, fallback, destructor                             |
| `test_kill_switch.cpp`          | single-fire idempotency, CAS under concurrent trip          |
| `test_phased_timer.cpp`         | warmup → measurement transition, uncalibrated fall-through  |
| `test_rate_limiter.cpp`         | `TokenBucket` refill, weighted acquire, capacity clamp      |
| `test_rate_limiter_edge.cpp`    | `weight=0` / `weight>capacity`, clock-backward, long idle   |
| `test_record.cpp`               | Stats dump/json, operator-                                  |
| `test_recorder.cpp`             | record, export_json/csv, overflow saturation                |
| `test_shutdown_signal.cpp`      | handler installation, flag flip on SIGTERM                  |
| `test_spin_for_ns.cpp`          | busy-wait accuracy at 1 us / 10 us / 100 us                 |
| `test_system_stats.cpp`         | delta, move semantics, format                               |
| `test_time.cpp`                 | TSC calibration, CV, NaN/Inf edge cases, delta_ns           |
| `test_timestamp.cpp`            | ms/us/ns conversions, ISO 8601, Y2K38 guard                 |
| `test_version.cpp`              | `eph::version_at_least(...)` consteval feature gate         |

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
