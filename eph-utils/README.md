# eph-utils

Header-only C++23 utility library for low-latency systems programming: high-precision timing, latency histograms, CPU topology/affinity, hugepage allocation, and system resource profiling.

## Key Components

All headers are under `include/eph/utils/`:

- **time.hpp** -- TSC (Time Stamp Counter) abstraction for sub-nanosecond timing. Auto-calibrates against `steady_clock` with multi-sample median and CV stability check. Supports x86_64 (`rdtscp`), ARM64 (`cntvct_el0`), and a `std::chrono` fallback. Thread-safe initialization via `std::call_once`.
- **timestamp.hpp** -- Constexpr timestamp unit conversions (ms/us/ns, ITCH midnight-offset to epoch) and wall-clock helpers (`now_ns`, `now_ms`, `feed_latency_ns`). Includes ISO 8601 formatting for logging.
- **hdr_histogram.hpp** -- High Dynamic Range histogram (Gil Tene algorithm) for latency distribution recording with constant relative precision across a wide value range. Supports single-value record, batch record, coordinated omission correction, forward/inverse CDF queries, percentile iteration, merge, and JSON/text export. Also provides `measure_tsc()` (function timing helper), `ScopedTSC` (RAII scope timer), and `Stats` (summary struct with avg/min/max/p50/p90/p99/p99.9/stddev in nanoseconds).
- **recorder.hpp** -- `Recorder` (single-thread) and `ConcurrentRecorder` (multi-thread, thread-local histograms with auto-merge on thread exit) for recording TSC-based latency measurements. Outputs console reports, JSON, and CSV. ConcurrentRecorder uses `shared_ptr` for safe access after recorder destruction.
- **cpu.hpp** -- CPU topology detection (socket/core/thread via `/proc/cpuinfo` with ARM fallback), thread affinity pinning (`pthread_setaffinity_np` on Linux, QoS on macOS), real-time scheduling (`SCHED_FIFO`/`SCHED_RR`), CPU base frequency detection, and `cpu_relax()` spin-wait hint (`PAUSE` on x86, `YIELD` on ARM64).
- **hugepage.hpp** -- `HugePage::make<T>(args...)` allocates objects on 2MB hugepages via `mmap(MAP_HUGETLB)` with transparent fallback to `std::aligned_alloc`. Returns `std::unique_ptr` with a custom deleter that correctly frees either memory type. Linux and Windows support.
- **ema.hpp** -- `Ema` (exponential moving average, O(1) per update) and `EmaCrossover` (dual-EMA golden/death cross detector). Constructable from smoothing factor alpha or period N.
- **audit_log.hpp** -- `AuditLog<Capacity>` fixed-size ring buffer for regulatory audit trails (MiFID II / Reg NMS). 64-byte cache-line-aligned `AuditEntry` records with TSC timestamps covering the full order lifecycle (NewOrder, Fill, Cancel, KillSwitch, etc.). Single-writer (`record`) and multi-writer CAS (`record_mt`) modes. Binary file flush for post-trade reporting.
- **console_sink.hpp** -- `ConsoleSink` implementing the `eph::core::MetricsSink` concept. Logs counters, gauges, and histograms as structured spdlog lines. Development/debug use; swap with `NullSink` in production.
- **system_stats.hpp** -- `SystemStats` RAII profiler capturing CPU time, page faults, context switches, RSS, and thread count via `getrusage` + `/proc`. `SystemResourceStats` supports delta computation, `dump()`, and `to_json()` for monitoring integration.
- **alignment.hpp** -- `CACHE_LINE_SIZE` constant (64) and `Align<T>` template that returns `max(alignof(T), CACHE_LINE_SIZE)` for false-sharing prevention.
- **record.hpp** -- Convenience aggregation header that includes `hdr_histogram.hpp`, `recorder.hpp`, and `system_stats.hpp`.

## Dependencies

- **eph-core** -- `MetricsSink` concept (`console_sink.hpp`), JSON escape utility (`hdr_histogram.hpp`, `recorder.hpp`)
- **spdlog** -- Logging throughout (compile-time filtered via `SPDLOG_ACTIVE_LEVEL`)

## Quick Start

```cpp
#include <eph/utils/time.hpp>
#include <eph/utils/recorder.hpp>

// Calibrate TSC at startup (once)
eph::utils::TSC::init();

// Pin this thread to core 2
eph::utils::set_thread_affinity(2, "worker");

// Record latency with HdrHistogram
eph::utils::Recorder rec("OrderSubmit");
for (int i = 0; i < 100'000; ++i) {
    uint64_t t0 = eph::utils::TSC::now();
    submit_order();
    uint64_t t1 = eph::utils::TSC::now();
    rec.record(t1 - t0);
}
rec.print_report();   // tabulated console output
rec.export_json();    // JSON file with full percentile stats
```
