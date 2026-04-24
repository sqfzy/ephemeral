# Project: eph-utils

> Header-only C++23 foundation library for the `ephemeral_dev` low-latency
> networking / trading monorepo: TSC timing, HDR histograms, CPU pinning,
> huge pages, audit trail, and other hot-path primitives.

**Language**: C++23 (header-only) | **Build**: xmake | **Logging**: spdlog
| **Tests**: GoogleTest | **Bench**: Google Benchmark

---

## Table of Contents
1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Module Map](#module-map)
4. [Data Flow](#data-flow)
5. [Key Components](#key-components)
6. [Entry Points & APIs](#entry-points--apis)
7. [Dependencies](#dependencies)
8. [Testing](#testing)

---

## Overview

`eph-utils` is the shared foundation library consumed by every other
`eph-*` subproject in the `ephemeral_dev` monorepo (`eph-core`,
`eph-containers`, `eph-codec`, `eph-net`, `eph-net-kernel`,
`eph-net-dpdk`, `eph-fix`, `eph-itch`, `eph-json`, `eph-book`). It sits
one layer above `eph-core` (which owns the pure concepts and error
types) and provides the platform-specific primitives that
latency-sensitive code needs: reading the hardware timestamp counter,
computing HDR latency histograms, pinning threads to isolated cores,
allocating on huge pages, building regulatory audit trails, measuring
system resource usage, running two-phase bench timers, tripping an
irreversible kill switch, gating throughput with a token bucket,
coordinating cooperative shutdown, and entering a Linux network
namespace from test fixtures.

The library is header-only — `xmake.lua` declares it as
`set_kind("headeronly")` with `add_includedirs("include", { public =
true })` and pulls `spdlog` in as a public package. Consumers get the
symbols by `#include <eph/utils/...>`; nothing needs to be linked.
Every translation unit that uses logging gets its level filtered at
compile time via `SPDLOG_ACTIVE_LEVEL` set from the root-level
`net_log_level` variable (`SPDLOG_LEVEL_TRACE` in debug, `_INFO` in
release).

The API surface is organised by concern: one header per primitive
(`time.hpp`, `cpu.hpp`, `hugepage.hpp`, ...), plus two aggregation
headers. `utils.hpp` pulls in the long-standing core set (`alignment`,
`audit_log`, `console_sink`, `cpu`, `ema`, `hugepage`, `record`,
`recorder`, `system_stats`, `time`, `timestamp`); `record.hpp` groups
the three recording-related ones. The newer additions —
`kill_switch.hpp`, `rate_limiter.hpp`, `phased_timer.hpp`,
`shutdown_signal.hpp`, and `linux/netns.hpp` — are **not** transitively
pulled in by `utils.hpp`; `#include` them explicitly (trading-semantics
primitives and POSIX-only helpers stay opt-in). Prefer per-header
includes for build time regardless.

---

## Architecture

Classic loosely-coupled header library: each module is independent
except for a few explicit edges (recording depends on timing,
`cpu.hpp::spin_for_ns` depends on `time.hpp`, `audit_log.hpp` depends on
`time.hpp`). Global state is limited to: `TSC`'s `call_once`
calibration, `cpu.hpp`'s process-wide pinned-cpu registry (serving
`pin_thread`), and `shutdown_signal.hpp`'s `g_shutdown_flag` (opt-in
header, only touched when a consumer calls
`install_shutdown_handlers()`).

### Component Diagram

```
                +----------------------------+
                |     eph/utils.hpp          |
                | (aggregates the core set:  |
                |  alignment, audit_log,     |
                |  console_sink, cpu, ema,   |
                |  hugepage, record,         |
                |  recorder, system_stats,   |
                |  time, timestamp)          |
                +----------------------------+
                              |
   +-------+-------+-------+--+----+-------+-------+-------+-------+
   v       v       v       v               v       v       v       v
 align   audit  console   cpu             ema    huge-  system  time.hpp
 ment    _log   _sink     .hpp           .hpp    page   _stats  (TSC)
 0deps   deps:  deps:     deps:         0deps   deps:   0deps   deps:
         time   core      +time                 logs            logs
         +core  +logs     +logs
                          (hosts
                           pin_thread +
                           CpuPinPolicy)

        time.hpp (TSC)                          record.hpp
          ^                                     (aggregator)
          |                                     includes:
        hdr_histogram / recorder                hdr_histogram
          (nanos <-> cycles, percentile,        recorder
           per-thread merge, JSON/CSV)          system_stats

  timestamp.hpp (wall-clock, ISO8601): 0 internal deps

 Opt-in headers — NOT pulled in by utils.hpp:

   kill_switch.hpp       (std::atomic + std::function, self-contained)
   rate_limiter.hpp      (TokenBucket, mutex + steady_clock)
   phased_timer.hpp      (deps: time.hpp)
   shutdown_signal.hpp   (std::atomic + <csignal>)
   linux/netns.hpp       (POSIX <fcntl.h>/<sched.h>/<unistd.h>)
```

All modules emit logs via a per-module spdlog logger name
(`utils.tsc`, `utils.cpu`, `utils.hugepage`, `utils.ema`, ...), created
lazily inside a `detail::xxx_logger()` helper. This keeps logger
creation out of the hot path while letting operators filter by
subsystem.

---

## Module Map

| Module / File                      | Responsibility                                                                                 | Key Types                                                                  | Depends On                                    |
|------------------------------------|-------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------|-----------------------------------------------|
| `alignment.hpp`                    | `CACHE_LINE_SIZE`, `Align<T>` for false-sharing prevention                                     | `Align<T>`                                                                 | `<cstddef>`                                   |
| `time.hpp`                         | TSC calibration and reads; ns <-> cycle conversion                                             | `TSC`                                                                      | spdlog, `<atomic>`, intrinsics                |
| `timestamp.hpp`                    | Constexpr unit conversions, wall-clock `now_ns/ms`, ISO 8601 format                            | -                                                                          | `<ctime>`, `<format>`                         |
| `cpu.hpp`                          | Topology, affinity, real-time scheduling, `cpu_relax`, `spin_for_ns`, **strict `pin_thread`**  | `CpuTopologyInfo`, `RealtimePolicy`, `CpuPinPolicy`                        | spdlog, pthread, `<filesystem>`, `time.hpp`   |
| `hugepage.hpp`                     | `mmap(MAP_HUGETLB)` + fallback allocator                                                        | `HugePage`, `HugePage::Deleter<T>`                                         | spdlog, `<sys/mman.h>`                        |
| `ema.hpp`                          | O(1) EMA + dual-EMA crossover detector with NaN/Inf guards                                     | `Ema`, `EmaCrossover`, `EmaCrossover::Signal`                              | spdlog                                        |
| `audit_log.hpp`                    | Fixed-size ring buffer audit trail with single- and multi-writer modes                         | `AuditEvent`, `Side`, `AuditEntry`, `AuditLog<Capacity>`                   | spdlog, `time.hpp`                            |
| `console_sink.hpp`                 | `core::MetricsSink` impl that logs structured metric lines                                     | `ConsoleSink`                                                              | spdlog, `eph/core/metrics_concept.hpp`        |
| `system_stats.hpp`                 | RAII `getrusage` profiler with delta / JSON / `std::format` support                            | `SystemResourceStats`, `SystemStats`                                       | spdlog, `<sys/resource.h>`                    |
| `hdr_histogram.hpp`                | Gil Tene HDR histogram + `measure_tsc` + `ScopedTSC` + `Stats` struct                          | `HdrHistogram`, `ScopedTSC`, `Stats`, `measure_tsc`                        | `eph/core/detail/json_escape.hpp`, `time.hpp` |
| `recorder.hpp`                     | `Recorder` + `ConcurrentRecorder` (TSC -> HDR -> JSON/CSV)                                     | `Recorder`, `ConcurrentRecorder`                                           | `hdr_histogram.hpp`, `time.hpp`               |
| `record.hpp`                       | Aggregation header (hdr + recorder + system_stats)                                             | -                                                                          | the three above                               |
| `phased_timer.hpp`                 | Two-phase TSC deadline timer (warmup + measurement window)                                     | `PhasedTimer`                                                              | `time.hpp`                                    |
| `kill_switch.hpp`                  | Irreversible single-fire safety primitive for HFT risk / compliance                            | `KillSwitch`                                                               | spdlog, `<atomic>`, `<functional>`            |
| `rate_limiter.hpp`                 | Thread-safe token-bucket rate limiter, weighted, `steady_clock`-driven                         | `TokenBucket`, `TokenBucket::Config`                                       | spdlog, `<chrono>`, `<mutex>`                 |
| `shutdown_signal.hpp`              | Process-wide SIGINT/SIGTERM cooperative shutdown flag                                          | `g_shutdown_flag`, `install_shutdown_handlers()`                           | `<atomic>`, `<csignal>`                       |
| `linux/netns.hpp`                  | `setns(CLONE_NEWNET)` into `/var/run/netns/<name>` for test fixtures                           | `linux_::enter_netns(name)`                                                | `<fcntl.h>`, `<sched.h>`, `<unistd.h>`, `<expected>` |
| `utils.hpp`                        | Top-level aggregation of the core set (not the opt-in headers — see "Aggregation header")     | -                                                                          | 11 modules; see aggregator note               |

---

## Data Flow

The canonical hot path is: read TSC -> run the code under test -> read
TSC again -> record the delta into a per-thread histogram -> merge ->
format as JSON/CSV/console.

### Flow Diagram

```
 startup                                hot path                          teardown
 ----------                             ----------                        ---------

 TSC::init() ---> do_init_() ----+
                                 |
        rdtscp vs steady_clock   |
        5 samples, median        |
        CV check (<1% stable)    |
        publish ns_per_cycle_    |
            (atomic release)     |
                                 v
                        initialized_=true
                                 |
                                 |
 thread_local guard ----> register_local (one-time)
                                 |
                                 |
                    Loop:  TSC::now() -- rdtscp / cntvct -----> t0
                           [code under test]
                           TSC::now() -------------------------> t1
                                 |
                                 v
                           Recorder::record(t1 - t0)
                                 |
                                 v
                           HdrHistogram.record(cycles)
                                 |                         TSC::to_ns(cycles)
                                 v                          (1 x multiply)
                           counts_[counts_index_for(cycles)]++
                                 |
                                 v
 (thread exits)  ----> retire_local() merges into
                       retired_histogram (mutex)
                                                           compute_stats()
                                                           merge_all()
                                                           (retired + active)
                                                                  |
                                                                  v
                                                           get_percentiles(...)
                                                           apply ns_per_cycle_
                                                           -> Stats
                                                                  |
                                                                  v
                                                      print_report / export_json
                                                      / export_csv to outputs/
```

The key design choice: cycle deltas are stored as-is in the histogram,
and conversion to nanoseconds happens only at report time. This keeps
`record()` at the cost of one `std::min`, one `std::max`, one `bit_width`
and one array increment — no floating-point multiplication on the hot
path.

---

## Key Components

### `TSC`

**File**: `include/eph/utils/time.hpp`
**Purpose**: Nanosecond-precision timing based on the CPU hardware
timestamp counter. Reads `rdtscp` on x86-64, `cntvct_el0` on ARM64, falls
back to `std::chrono::steady_clock` elsewhere.
**Interface**:
```cpp
static bool     init(std::chrono::milliseconds = 200ms);
static uint64_t now() noexcept;                       // ~20 cycles
static std::optional<double>   to_ns(uint64_t cycles);
static std::optional<uint64_t> to_cycles(double ns);
static std::optional<uint64_t> to_cycles(chrono::duration d);
static std::optional<double>   delta_ns(uint64_t start, uint64_t end);
static bool                    is_initialized() noexcept;
static std::optional<double>   get_ns_per_cycle() noexcept;
static std::optional<double>   get_calibration_cv() noexcept;
```
**Notes**:
- `init()` is guarded by `std::call_once`; subsequent calls are cheap.
- Calibration takes 5 samples and uses the median, rejecting outliers.
- Emits an ERROR log if the CV exceeds 1% — the result is still
  published, but caller is warned the clock may be unstable.
- Checks `/proc/cpuinfo` for `constant_tsc` / `nonstop_tsc` /
  `tsc_reliable` flags and warns if missing.
- `to_cycles` rejects NaN via `!(ns >= 0)` — a direct `< 0` check would
  let NaN through (all NaN comparisons are false).

### `HdrHistogram`

**File**: `include/eph/utils/hdr_histogram.hpp`
**Purpose**: Gil Tene's HDR Histogram algorithm — records value
distributions over a wide dynamic range (e.g. 1 ns to 10 s) with
constant relative precision (default 3 significant figures).
**Interface**:
```cpp
HdrHistogram(uint64_t lowest, uint64_t highest, int sig_figs = 3);
bool record(uint64_t value);                            // ~5-10 ns
bool record_values(uint64_t value, uint64_t count);
bool record_corrected(uint64_t value, uint64_t expected_interval);
uint64_t             get_value_at_percentile(double p);
std::vector<uint64_t> get_percentiles(std::vector<double>);
double               get_percentile_at_or_below(uint64_t value);
bool merge(const HdrHistogram& other);
bool subtract(const HdrHistogram& other);
void for_each_recorded_value(Func);
void for_each_linear(uint64_t step, Func);
void for_each_percentile(Func, int ticks_per_half = 5);
std::string report(title, unit);
std::string to_json();
std::string output_percentile_distribution(double scale);
```
**Notes**:
- Not thread-safe. Use `ConcurrentRecorder` for multi-threaded cases.
- `record_corrected` implements Gil Tene's coordinated-omission
  correction: for a periodic workload, a stall produces one high sample
  and hides the samples that would have been taken during the stall —
  this method back-fills them.
- `subtract()` validates that no bucket underflows before mutating, so
  a failed call leaves the histogram untouched.
- Memory use is bounded by `kMaxCountsLen = 10'000'000`; wider ranges
  or higher precision trigger `std::invalid_argument`.

### `Recorder` / `ConcurrentRecorder`

**File**: `include/eph/utils/recorder.hpp`
**Purpose**: Wrap an HdrHistogram with TSC-aware recording, Stats
computation, console reports, and JSON/CSV export. `Recorder` is
single-threaded; `ConcurrentRecorder` maintains one `thread_local`
histogram per thread.
**Interface** (same shape for both):
```cpp
explicit Recorder(std::string name,
                  uint64_t lowest_cycles = 1,
                  uint64_t highest_cycles = 0, // auto = ~10s
                  int precision = 3);

bool record(uint64_t cycles);
bool record_values(uint64_t cycles, uint64_t count);
std::optional<Stats> compute_stats() const;
void print_report() const;
bool export_json(const std::string& dir = "outputs");
bool export_csv (const std::string& dir = "outputs");
bool export_all (const std::string& dir = "outputs");
void reset();
```
**Notes**:
- Constructors ensure TSC is initialized (call `TSC::init()` on demand)
  and throw `std::runtime_error` on failure.
- `record_values` saturates at `UINT64_MAX` on `cycles * count` overflow
  rather than silently wrapping — corrupted averages were a real bug.
- `ConcurrentRecorder` uses `std::shared_ptr<SharedState>` so the
  `thread_local` retirement hook can safely merge data into the shared
  buffer even if the `ConcurrentRecorder` instance has already been
  destroyed. Each thread holds an `unordered_map<SharedState*, ...>`
  keyed by recorder, so interleaved use of multiple recorders from the
  same thread doesn't thrash the guard.
- `compute_and_reset()` does the merge + reset atomically under one
  lock acquisition, avoiding the lost-sample gap of separate
  `compute_stats()` + `reset()` calls.

### `CpuPinPolicy` / `pin_thread`

**File**: `include/eph/utils/cpu.hpp` (the strict pin API lives in the
same header as topology / affinity — no separate `cpu_pin.hpp`)
**Purpose**: Low-latency and HFT workloads need more than bare affinity
— the pinned core should also be isolated from the scheduler, not share
an SMT sibling with another pinned thread, stay on the same NUMA node
as peers, and ideally be free of NIC IRQ storms. `pin_thread` enforces
all of that.
**Interface**:
```cpp
struct CpuPinPolicy {
    bool require_isolcpus            = true;
    bool require_no_sibling_conflict = true;
    bool require_same_numa           = true;
    bool warn_irq_overlap            = true;
};
std::expected<void, std::string>
pin_thread(int cpu, std::string_view name, CpuPinPolicy = {});
std::expected<void, std::string>
pin_thread(int cpu);                                // nameless, default policy
void reset_pin_registry_for_tests() noexcept;
```
**Notes**:
- Maintains a process-wide `std::set<int>` of already-pinned cpus,
  guarded by a mutex, so SMT / NUMA checks are cross-thread aware.
- isolcpus check parses `/sys/devices/system/cpu/isolated`.
- SMT check parses `/sys/.../cpuN/topology/thread_siblings_list`.
- NUMA check probes `/sys/.../cpuN/node*` up to node 63.
- IRQ check parses `/proc/interrupts`, warns only — doesn't fail the
  call (rebinding NIC IRQs typically needs root).
- After `pthread_setaffinity_np` it calls `pthread_getaffinity_np` and
  verifies the mask is exactly `{cpu}` — catches silent quota failures.

### `HugePage`

**File**: `include/eph/utils/hugepage.hpp`
**Purpose**: Allocate long-lived hot-path objects on 2 MB huge pages to
reduce TLB misses. Transparent fallback to `std::aligned_alloc` on
failure, so callers don't have to handle the no-huge-pages case.
**Interface**:
```cpp
template <class T, class... Args>
static auto make(Args&&... args);  // unique_ptr<T, Deleter<T>>

static void* allocate(size_t size, size_t alignment,
                      bool& is_hugepage,
                      size_t& out_allocated_size) noexcept;
static void  deallocate(void* ptr, size_t size, bool is_hugepage) noexcept;
```
**Notes**:
- Linux: `mmap(MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB)`; on failure
  falls back to `std::aligned_alloc` and logs a WARN.
- Windows: `VirtualAlloc(MEM_LARGE_PAGES)`, gated on
  `GetLargePageMinimum`.
- Other platforms go straight to `std::aligned_alloc`.
- `Deleter<T>` captures both `size` (rounded up to the huge-page
  boundary so `munmap` gets the right length) and `is_hugepage` (to
  pick the right release path), and calls `ptr->~T()` before freeing.
- Zero-size allocations return `nullptr` rather than undefined behavior.

### `AuditLog<Capacity>`

**File**: `include/eph/utils/audit_log.hpp`
**Purpose**: Regulatory audit trail (MiFID II / Reg NMS) — every order
lifecycle event gets a TSC-timestamped entry in a fixed 64-byte slot.
**Interface**:
```cpp
enum class AuditEvent : uint8_t { NewOrder, ..., KillSwitch };
enum class Side : uint8_t { Buy = 1, Sell = 2 };
struct alignas(64) AuditEntry { ...; std::string dump() const; };

template <size_t Capacity = 65536>
  requires (std::has_single_bit(Capacity))
class AuditLog {
    bool record   (AuditEvent, order_id, price, qty, Side, venue);
    bool record_mt(AuditEvent, order_id, price, qty, Side, venue);
    const AuditEntry* at(size_t offset) const;   // 0 = newest
    const AuditEntry* latest() const;
    size_t flush_to_file(std::string_view path) const;
    std::string dump(size_t max_entries = 20) const;
};
```
**Notes**:
- `Capacity` must be a power of two (enforced via `std::has_single_bit`
  concept).
- Single-writer `record()` asserts in debug that it's always called
  from the same thread — accidental cross-thread use is a silent data
  race otherwise.
- Multi-writer `record_mt()` uses `fetch_add` on the head index, then
  publishes the slot via a per-slot `committed_[idx]` atomic flag so
  readers can distinguish "being written" from "fully written".
- Wrap-around is logged exactly once per full wrap to avoid flooding
  hot-path logs.

### `SystemStats`

**File**: `include/eph/utils/system_stats.hpp`
**Purpose**: RAII wrapper around `getrusage` — captures CPU user/sys
time, major/minor page faults, voluntary/involuntary context switches,
peak and current RSS, and thread count. Deltas computed in
`snapshot()`.
**Notes**: POSIX-only (static_assert at top of file). `std::formatter`
specialization produces a single-line summary for logging. Move
semantics correctly transfer the auto-log responsibility and clear the
source's flag to avoid duplicate logs on destruction.

### `KillSwitch`

**File**: `include/eph/utils/kill_switch.hpp`
**Purpose**: Irreversible, single-fire safety primitive for HFT
risk / compliance (per Phase 9 decision D-3).
**Interface**:
```cpp
explicit KillSwitch(std::function<void()> on_trip = nullptr) noexcept;
[[nodiscard]] bool tripped() const noexcept;  // acquire load
void trip() noexcept;                         // idempotent, CAS-guarded
```
**Notes**:
- Non-copyable, non-movable.
- Callback fires **exactly once** on the thread that wins the
  compare-exchange; subsequent `trip()` calls are no-ops.
- Compile-time enforced via three `static_assert`s: there is no
  `reset()`, no `untrip()`, no `clear()`. A tripped switch requires
  process restart + human review.
- Callback must not throw — an escaping exception calls `std::terminate`
  via the `noexcept` boundary.

### `TokenBucket`

**File**: `include/eph/utils/rate_limiter.hpp`
**Purpose**: Thread-safe weighted rate limiter for per-venue / global
throughput governance (per Phase 9 decision D-4).
**Interface**:
```cpp
struct Config { uint32_t capacity; double refill_per_second; };
explicit TokenBucket(Config cfg) noexcept;
[[nodiscard]] bool try_acquire(uint32_t weight = 1) noexcept;
[[nodiscard]] double available_tokens() const noexcept;
[[nodiscard]] uint32_t capacity() const noexcept;
[[nodiscard]] double   refill_rate() const noexcept;
```
**Notes**:
- Non-copyable, non-movable (contains a `std::mutex`).
- `steady_clock`-driven refill — deliberately not TSC, because rate
  limits operate on sub-second budgets and TSC calibration / hot-plug
  concerns don't apply.
- `weight == 0` is a free no-op (consistent with "zero cost costs zero
  tokens"); `weight > capacity` fails immediately rather than spinning.
- Clock-backward and zero-elapsed paths skip the refill step rather
  than rewinding `last_refill_`.

### `PhasedTimer`

**File**: `include/eph/utils/phased_timer.hpp`
**Purpose**: Two-phase TSC deadline timer for benchmarks that need a
warmup window (cold-cache / power-state transient) followed by a
measurement window, both checked without syscalls.
**Interface**:
```cpp
void start(std::chrono::nanoseconds warmup,
           std::chrono::nanoseconds measurement) noexcept;
[[nodiscard]] bool is_warmup()  const noexcept;
[[nodiscard]] bool is_running() const noexcept;
```
**Notes**:
- POD — trivially copyable / movable.
- Not thread-safe; each thread needs its own instance.
- Requires `TSC::init()` before `start()`. Uncalibrated TSC collapses
  both windows to 0 cycles so `is_running()` returns `false` immediately
  rather than running forever.

### Shutdown signal

**File**: `include/eph/utils/shutdown_signal.hpp`
**Purpose**: Process-wide cooperative SIGINT/SIGTERM flag.
**Interface**:
```cpp
extern std::atomic<bool> g_shutdown_flag;          // default = true
void install_shutdown_handlers() noexcept;         // SIGINT + SIGTERM
```
**Notes**:
- Hot loops poll `g_shutdown_flag.load(std::memory_order_relaxed)`.
- Handlers use plain `std::signal`; consumers needing `sigaction`
  semantics (mask, restart) should install their own.
- `install_shutdown_handlers` is idempotent.

### `linux_::enter_netns`

**File**: `include/eph/utils/linux/netns.hpp`
**Purpose**: Move the calling thread into a named network namespace
(`/var/run/netns/<name>`) via `setns(2) + CLONE_NEWNET`. Used by
bench / integration fixtures that need netns isolation.
**Interface**:
```cpp
[[nodiscard]] std::expected<void, std::string>
enter_netns(std::string_view name);
```
**Notes**:
- Requires `CAP_SYS_ADMIN`.
- Namespace must pre-exist (e.g. `ip netns add <name>`); `open()`
  failure returns a diagnostic that says so.
- No umount / unshare helper — callers that need to fall back stash
  `/proc/self/ns/net` themselves.

---

## Entry Points & APIs

| Entrypoint                                   | Type          | Description                                                                     |
|----------------------------------------------|---------------|---------------------------------------------------------------------------------|
| `#include <eph/utils.hpp>`                   | aggregator    | Pulls in the core set (see "Aggregation header" in Overview)                    |
| `TSC::init()`                                | init-once     | Must run before any `to_ns`/`to_cycles` call                                    |
| `TSC::now()`                                 | hot path      | 20-cycle hardware timestamp                                                     |
| `Recorder::record(cycles)`                   | hot path      | Record a single latency sample                                                  |
| `ConcurrentRecorder::record(c)`              | hot path      | Zero-contention per-thread record                                               |
| `HugePage::make<T>(args...)`                 | factory       | Construct `T` on huge-page memory                                               |
| `pin_thread(cpu, name, policy)`              | init          | Strict pinning with isolcpus / SMT / NUMA / IRQ topology validation             |
| `AuditLog<N>::record(...)`                   | hot path      | Single-writer audit entry                                                       |
| `AuditLog<N>::record_mt(...)`                | hot path      | CAS multi-writer audit entry                                                    |
| `spin_for_ns(n)`                             | hot path      | Busy-wait ~n ns via TSC + `cpu_relax`                                           |
| `ConsoleSink::push_counter(...)` / `_gauge` / `_histogram` | sink | `core::MetricsSink` implementation with tag-escaping                            |
| `SystemStats::snapshot()`                    | sampling      | `getrusage` delta                                                               |
| `PhasedTimer::start(warmup, measure)`        | bench         | Arm warmup + measurement deadlines in TSC cycles                                |
| `KillSwitch::trip()` / `::tripped()`         | compliance    | Irreversible single-fire safety flag; `noexcept`                                |
| `TokenBucket::try_acquire(weight)`           | control       | Weighted non-blocking rate gate; `noexcept`                                     |
| `install_shutdown_handlers()` / `g_shutdown_flag` | control   | Process-wide SIGINT/SIGTERM cooperative flag                                    |
| `linux_::enter_netns(name)`                  | fixture       | Move calling thread into a named Linux netns (test / bench only)                |

---

## Dependencies

### Internal (module graph within eph-utils)

```
                 alignment.hpp   timestamp.hpp
                    (0 dep)        (0 dep)

                       time.hpp ----+
                       (TSC)        |
                          ^         |
                          |         |
                     +----+---+-----+-----+-------+
                     |        |           |       |
                 cpu.hpp   audit_log  hdr_histogram  phased_timer
                 (uses     .hpp       .hpp           .hpp
                 time for             ^
                 spin_for_ns;         |
                 hosts pin_thread  recorder.hpp
                 +CpuPinPolicy)    (Recorder, ConcurrentRecorder)
                                       ^
                                       |
                                   record.hpp (aggregator)
                                   includes hdr_histogram,
                                   recorder, system_stats

 ema.hpp, hugepage.hpp, console_sink.hpp, kill_switch.hpp,
 rate_limiter.hpp, shutdown_signal.hpp, linux/netns.hpp —
 all independent of the time.hpp / record.hpp chain.

 console_sink depends on eph/core/metrics_concept.hpp (MetricsSink).
 hdr_histogram / recorder depend on eph/core/detail/json_escape.hpp.

 utils.hpp aggregates the core set (alignment, audit_log, console_sink,
 cpu, ema, hugepage, record, recorder, system_stats, time, timestamp).
 kill_switch / rate_limiter / phased_timer / shutdown_signal / netns
 are opt-in — include them explicitly.
```

### External

| Package                   | Version  | Purpose                                   |
|---------------------------|----------|-------------------------------------------|
| spdlog                    | any      | All leveled logging (public dep)          |
| eph-core                  | sibling  | `core::MetricsSink`, `core::detail::json_escape` |
| gtest                     | any      | Unit tests (`rule("eph-test")`)           |
| benchmark                 | any      | Microbenchmarks (`rule("eph-bench")`)     |

### System headers (conditionally included)

| Header                              | Used by                       | Platform          |
|-------------------------------------|-------------------------------|-------------------|
| `<immintrin.h>`, `<x86intrin.h>`    | `time.hpp`, `cpu.hpp`         | x86-64            |
| `<arm_neon.h>`                      | `time.hpp`                    | ARM64             |
| `<sched.h>`, `<pthread.h>`          | `cpu.hpp`, `linux/netns.hpp`  | Linux             |
| `<filesystem>`                      | `cpu.hpp` (isolcpus/SMT/NUMA) | Linux             |
| `<fcntl.h>`, `<unistd.h>`           | `linux/netns.hpp`             | Linux             |
| `<csignal>`                         | `shutdown_signal.hpp`         | POSIX             |
| `<sys/mman.h>`                      | `hugepage.hpp`                | Linux             |
| `<mach/mach.h>`                     | `cpu.hpp` (realtime QoS)      | macOS             |
| `<memoryapi.h>`                     | `hugepage.hpp`                | Windows           |
| `<sys/resource.h>`                  | `system_stats.hpp`            | POSIX             |

---

## Testing

| Test Suite                        | Location                                | Coverage Focus                                                   |
|-----------------------------------|-----------------------------------------|------------------------------------------------------------------|
| `test_alignment`                  | `tests/test_alignment.cpp`              | Compile-time `Align<T>` values                                   |
| `test_audit_log`                  | `tests/test_audit_log.cpp`              | Ring wrap, CAS multi-writer, dump, flush_to_file                 |
| `test_console_sink`               | `tests/test_console_sink.cpp`           | Counter / gauge / histogram formatting, tag quoting              |
| `test_cpu`                        | `tests/test_cpu.cpp`                    | Topology parsing, affinity, `CpuTopologyInfo` format             |
| `test_cpu_pin`                    | `tests/test_cpu_pin.cpp`                | `pin_thread` — isolcpus / SMT / NUMA / IRQ checks                |
| `test_ema`                        | `tests/test_ema.cpp`                    | alpha bounds, NaN/Inf rejection, crossover edge cases            |
| `test_hdr_histogram`              | `tests/test_hdr_histogram.cpp`          | Percentiles, merge, subtract, coordinated omission               |
| `test_hugepage`                   | `tests/test_hugepage.cpp`               | Zero-size guard, fallback, deleter                               |
| `test_kill_switch`                | `tests/test_kill_switch.cpp`            | Single-fire idempotency, CAS, concurrent trip, callback-once     |
| `test_phased_timer`               | `tests/test_phased_timer.cpp`           | Warmup → measurement transition, uncalibrated-TSC fall-through   |
| `test_rate_limiter`               | `tests/test_rate_limiter.cpp`           | `TokenBucket` refill, weighted acquire, capacity clamp           |
| `test_rate_limiter_edge`          | `tests/test_rate_limiter_edge.cpp`      | `weight=0` / `weight>capacity`, clock-backward, long idle        |
| `test_record`                     | `tests/test_record.cpp`                 | `Stats::dump` / `to_json` / `operator-`                          |
| `test_recorder`                   | `tests/test_recorder.cpp`               | Record, merge, JSON / CSV export, overflow saturation            |
| `test_shutdown_signal`            | `tests/test_shutdown_signal.cpp`        | Handler installation, flag flip on SIGTERM                       |
| `test_spin_for_ns`                | `tests/test_spin_for_ns.cpp`            | 1us / 10us / 100us busy-wait accuracy                            |
| `test_system_stats`               | `tests/test_system_stats.cpp`           | Delta, move semantics, format                                    |
| `test_time`                       | `tests/test_time.cpp`                   | TSC calibration, CV, NaN/Inf edge cases                          |
| `test_timestamp`                  | `tests/test_timestamp.cpp`              | ms / us / ns conversions, ISO 8601, Y2K38 guard                  |
| `test_version`                    | `tests/test_version.cpp`                | `eph::version_at_least(...)` consteval feature gate              |

Key test scenarios:
- **Boundary values**: zero-size HugePage allocate, zero-cycle Recorder,
  empty histogram reports, NaN inputs to `TSC::to_cycles` /
  `Ema::update` / `HdrHistogram::output_percentile_distribution`.
- **Overflow saturation**: `Recorder::record_values` with cycles * count
  exceeding `UINT64_MAX` must saturate, not wrap.
- **Race-adjacent**: `AuditLog::record_mt` under concurrent writers
  verified to never expose partially-written entries to readers.
- **TOCTOU**: `recorder_detail::ensure_directory` no longer races
  between `fs::exists` and `fs::create_directories`.
- **Format edge cases**: `format_timestamp_ms` with negative epoch ms
  uses Euclidean division for the correct "1969-12-31T23:59:59.999Z"
  result.

Historical production hardening (from git log): NaN rejection in
`TSC::to_cycles` and `HdrHistogram::output_percentile_distribution`,
`HdrHistogram::subtract` now accounts for `dropped_count_`,
`SystemStats` move semantics reset `auto_log_` on the source to avoid
duplicate logs, `ConsoleSink` quotes tag values containing special
characters, `sanitize_filename` casts to unsigned char before
`std::isalnum` to avoid UB on UTF-8 continuation bytes.
