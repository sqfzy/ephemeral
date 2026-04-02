# eph-utils

Header-only C++23 utility library for low-latency systems programming. Provides high-precision TSC timing, latency histograms, CPU topology and affinity, hugepage allocation, signal processing (EMA), regulatory audit trails, system resource profiling, and metrics sinks. Designed for HFT hot paths where nanosecond-level determinism matters.

## Key Components

All headers are under `include/eph/utils/`:

- **time.hpp** -- `TSC` class for sub-nanosecond timing via the CPU hardware timestamp counter. Reads `rdtscp` on x86-64, `cntvct_el0` on ARM64, falls back to `std::chrono::steady_clock`. Auto-calibrates against `steady_clock` with multi-sample median and CV stability check. Thread-safe initialization via `std::call_once`.
- **timestamp.hpp** -- Constexpr timestamp unit conversions (ms/us/ns, ITCH midnight-offset to epoch) and wall-clock helpers (`now_ns`, `now_ms`, `feed_latency_ns`). Includes ISO 8601 formatting for logging.
- **hdr_histogram.hpp** -- High Dynamic Range histogram (Gil Tene algorithm) for recording latency distributions with constant relative precision across a wide value range. Supports single-value record, batch record, coordinated omission correction, forward/inverse CDF queries, percentile iteration (linear and halving-distance), merge, subtract (windowed measurement), and text/JSON export. Also provides `measure_tsc()` (function timing helper), `ScopedTSC` (RAII scope timer), and `Stats` (summary struct with avg/min/max/p50/p90/p99/p99.9/stddev in nanoseconds).
- **recorder.hpp** -- `Recorder` (single-thread) and `ConcurrentRecorder` (multi-thread, `thread_local` histograms with auto-merge on thread exit) for recording TSC-based latency measurements. Outputs console reports, JSON, and CSV. `ConcurrentRecorder` uses `shared_ptr` for safe access after recorder destruction.
- **cpu.hpp** -- CPU topology detection (socket/core/thread via `/proc/cpuinfo` with ARM fallback), thread affinity pinning (`pthread_setaffinity_np` on Linux, QoS on macOS), real-time scheduling (`SCHED_FIFO`/`SCHED_RR`), CPU base frequency detection, and `cpu_relax()` spin-wait hint (`PAUSE` on x86, `YIELD` on ARM64).
- **hugepage.hpp** -- `HugePage::make<T>(args...)` allocates objects on 2 MB hugepages via `mmap(MAP_HUGETLB)` with transparent fallback to `std::aligned_alloc`. Returns `std::unique_ptr` with a custom deleter. Linux and Windows support.
- **ema.hpp** -- `Ema` (exponential moving average, O(1) per update) and `EmaCrossover` (dual-EMA golden/death cross detector). Constructable from smoothing factor alpha or period N.
- **audit_log.hpp** -- `AuditLog<Capacity>` fixed-size ring buffer for regulatory audit trails (MiFID II / Reg NMS). 64-byte cache-line-aligned `AuditEntry` records with TSC timestamps covering the full order lifecycle (NewOrder, Fill, Cancel, KillSwitch, etc.). Single-writer (`record`) and multi-writer CAS (`record_mt`) modes. Binary file flush for post-trade reporting.
- **console_sink.hpp** -- `ConsoleSink` implementing the `eph::core::MetricsSink` concept. Logs counters, gauges, and histograms as structured spdlog lines. Development/debug use; swap with `NullSink` in production.
- **system_stats.hpp** -- `SystemStats` RAII profiler capturing CPU time, page faults, context switches, RSS, and thread count via `getrusage` + `/proc`. `SystemResourceStats` supports delta computation, `dump()`, and `to_json()` for monitoring integration.
- **alignment.hpp** -- `CACHE_LINE_SIZE` constant (64) and `Align<T>` template that returns `max(alignof(T), CACHE_LINE_SIZE)` for false-sharing prevention.
- **record.hpp** -- Convenience aggregation header that includes `hdr_histogram.hpp`, `recorder.hpp`, and `system_stats.hpp`.
- **utils.hpp** -- Top-level convenience header that includes all public headers.

## Public API Reference

All symbols are in namespace `eph::utils` unless otherwise noted.

### time.hpp -- TSC Timing

| Symbol | Kind | Description |
|--------|------|-------------|
| `TSC::now()` | static method | Read the hardware timestamp counter (~20-cycle overhead) |
| `TSC::init(duration)` | static method | Calibrate TSC frequency against `steady_clock` (call once at startup) |
| `TSC::to_ns(cycles)` | static method | Convert TSC cycles to nanoseconds (`optional<double>`) |
| `TSC::to_cycles(ns)` | static method | Convert nanoseconds to TSC cycles (`optional<uint64_t>`) |
| `TSC::to_cycles(chrono::duration)` | static method | Convert a `std::chrono::duration` to cycles |
| `TSC::is_initialized()` | static method | Check whether calibration has completed |
| `TSC::get_ns_per_cycle()` | static method | Get the calibrated ns/cycle ratio |
| `TSC::get_calibration_cv()` | static method | Get the coefficient of variation from calibration |

### timestamp.hpp -- Timestamp Conversions

| Symbol | Kind | Description |
|--------|------|-------------|
| `ms_to_ns(int64_t)` | constexpr fn | Milliseconds since epoch to nanoseconds |
| `ns_to_ms(uint64_t)` | constexpr fn | Nanoseconds to milliseconds (truncating) |
| `us_to_ns(int64_t)` | constexpr fn | Microseconds to nanoseconds |
| `itch_ts_to_epoch_ns(ns_since_midnight, midnight_epoch_ns)` | constexpr fn | ITCH midnight-offset to epoch nanoseconds |
| `now_ns()` | inline fn | Current wall-clock time as nanoseconds since epoch |
| `now_ms()` | inline fn | Current wall-clock time as milliseconds since epoch |
| `feed_latency_ns(exchange_ts_ns)` | inline fn | Latency between exchange timestamp and now |
| `feed_latency_ns_from_ms(exchange_ts_ms)` | inline fn | Latency from a millisecond exchange timestamp |
| `format_timestamp_ns(epoch_ns)` | inline fn | Format as ISO 8601 string with nanosecond precision |
| `format_timestamp_ms(epoch_ms)` | inline fn | Format as ISO 8601 string with millisecond precision |

### hdr_histogram.hpp -- Latency Histogram and Timing Helpers

| Symbol | Kind | Description |
|--------|------|-------------|
| `measure_tsc(func, args...)` | function template | Measure CPU cycle cost of a callable |
| `ScopedTSC` | class | RAII scope timer; writes elapsed cycles to a reference on destruction |
| `HdrHistogram(lowest, highest, sig_figs)` | class | High Dynamic Range histogram (Gil Tene algorithm) |
| `HdrHistogram::record(value)` | method | Record a single sample (~5-10 ns) |
| `HdrHistogram::record_values(value, count)` | method | Batch-record the same value N times |
| `HdrHistogram::record_corrected(value, expected_interval)` | method | Record with coordinated omission correction |
| `HdrHistogram::reset()` | method | Clear all samples and statistics |
| `HdrHistogram::get_value_at_percentile(pct)` | method | Forward CDF: value at a given percentile |
| `HdrHistogram::get_percentiles(pcts)` | method | Multi-percentile query in a single scan |
| `HdrHistogram::get_percentile_at_or_below(value)` | method | Inverse CDF: percentile for a given value |
| `HdrHistogram::get_percentiles_at_or_below(values)` | method | Batch inverse CDF in a single scan |
| `HdrHistogram::get_count_between(low, high)` | method | Count samples in a value range |
| `HdrHistogram::get_mean()` | method | Arithmetic mean of all recorded values |
| `HdrHistogram::get_std_deviation()` | method | Population standard deviation |
| `HdrHistogram::get_total_count()` | method | Total recorded samples |
| `HdrHistogram::get_min_value()` / `get_max_value()` | methods | Extrema |
| `HdrHistogram::get_dropped_count()` | method | Out-of-range samples rejected |
| `HdrHistogram::empty()` | method | Check if no samples recorded |
| `HdrHistogram::merge(other)` | method | Merge another histogram into this one |
| `HdrHistogram::subtract(other)` | method | Subtract another histogram (windowed measurement) |
| `HdrHistogram::for_each_recorded_value(func)` | method | Iterate over non-empty buckets |
| `HdrHistogram::for_each_linear(step, func)` | method | Iterate in fixed-width linear steps |
| `HdrHistogram::for_each_percentile(func, ticks)` | method | Iterate at halving-distance percentile steps |
| `HdrHistogram::report(title, unit)` | method | Human-readable percentile report string |
| `HdrHistogram::to_json()` | method | JSON summary for monitoring integration |
| `HdrHistogram::output_percentile_distribution(scale)` | method | Standard HDR Histogram text format |
| `HdrHistogram::get_memory_size()` | method | Approximate memory footprint in bytes |
| `HdrHistogram::is_compatible(other)` | method | Check structural compatibility for merge/subtract |
| `Stats` | struct | Aggregated latency summary (name, count, avg/min/max/p50/p90/p99/p99.9/stddev in ns) |
| `Stats::dump()` | method | Multi-line formatted summary |
| `Stats::to_json()` | method | JSON-formatted summary |

### recorder.hpp -- Latency Recorders

| Symbol | Kind | Description |
|--------|------|-------------|
| `Recorder(name, lowest, highest, precision)` | class | Single-threaded latency recorder backed by HdrHistogram |
| `Recorder::record(cycles)` | method | Record a single TSC measurement |
| `Recorder::record_values(cycles, count)` | method | Batch-record the same value |
| `Recorder::merge(other)` | method | Merge another Recorder's data |
| `Recorder::compute_stats()` | method | Compute `Stats` from recorded data |
| `Recorder::print_report()` | method | Tabulated console output |
| `Recorder::export_json(dir)` | method | Export JSON with percentile stats |
| `Recorder::export_csv(dir)` | method | Export CSV with (latency_ns, count) rows |
| `Recorder::export_all(dir)` | method | Export both JSON and CSV |
| `Recorder::reset()` | method | Clear all recorded data |
| `Recorder::name()` / `count()` / `has_data()` | accessors | Query recorder state |
| `Recorder::histogram()` | accessor | Direct access to the underlying HdrHistogram |
| `ConcurrentRecorder(name, lowest, highest, precision)` | class | Multi-threaded recorder with `thread_local` histograms |
| `ConcurrentRecorder::record(cycles)` | method | Thread-safe record (zero contention) |
| `ConcurrentRecorder::record_values(cycles, count)` | method | Thread-safe batch record |
| `ConcurrentRecorder::compute_stats()` | method | Merge all threads and compute `Stats` |
| `ConcurrentRecorder::print_report()` | method | Console output with thread counts |
| `ConcurrentRecorder::export_json(dir)` | method | Merged JSON export |
| `ConcurrentRecorder::export_csv(dir)` | method | Merged CSV export |
| `ConcurrentRecorder::export_all(dir)` | method | Export both JSON and CSV |
| `ConcurrentRecorder::thread_count()` | accessor | Total threads that recorded data |

### cpu.hpp -- CPU Topology and Affinity

| Symbol | Kind | Description |
|--------|------|-------------|
| `CpuTopologyInfo` | struct | Maps a logical thread to its socket, core, and hw thread ID |
| `get_cpu_topology()` | function | Detect system CPU topology (`expected<vector<CpuTopologyInfo>, string>`) |
| `set_thread_affinity(cpu_id, name)` | function | Pin calling thread to a CPU core (`expected<void, string>`) |
| `RealtimePolicy` | enum | `Fifo` (SCHED_FIFO) or `RoundRobin` (SCHED_RR) |
| `set_thread_realtime(policy, priority, name)` | function | Switch to real-time scheduling (`expected<void, string>`) |
| `get_cpu_base_frequency()` | function | Query nominal CPU frequency in GHz (`optional<double>`) |
| `cpu_relax()` | function | Spin-wait hint (PAUSE on x86, YIELD on ARM64) |
| `std::formatter<CpuTopologyInfo>` | specialization | `std::format` support: `"socket=0 core=2 thread=4"` |

### hugepage.hpp -- Hugepage Allocation

| Symbol | Kind | Description |
|--------|------|-------------|
| `HugePage::make<T>(args...)` | static method | Construct T on hugepage memory, returns `unique_ptr<T>` with custom deleter |
| `HugePage::allocate(size, alignment, is_hugepage, out_size)` | static method | Low-level raw hugepage allocation (returns `void*`) |
| `HugePage::deallocate(ptr, size, is_hugepage)` | static method | Free memory from `allocate()` |

### ema.hpp -- Exponential Moving Average

| Symbol | Kind | Description |
|--------|------|-------------|
| `Ema(alpha)` | class | EMA with smoothing factor alpha in (0, 1] |
| `Ema::from_period(N)` | static method | Construct from period (alpha = 2/(N+1)) |
| `Ema::update(value)` | method | Feed a new value, returns updated EMA |
| `Ema::value()` | accessor | Current EMA value |
| `Ema::initialized()` | accessor | Whether at least one update has occurred |
| `Ema::alpha()` | accessor | Smoothing factor |
| `Ema::reset()` | method | Reset to uninitialized state |
| `EmaCrossover(fast_period, slow_period)` | class | Dual-EMA crossover detector |
| `EmaCrossover::Signal` | enum | `None`, `BullishCross`, `BearishCross` |
| `EmaCrossover::update(price)` | method | Feed a new price, returns crossover signal |
| `EmaCrossover::fast()` / `slow()` | accessors | Current fast/slow EMA values |

### audit_log.hpp -- Regulatory Audit Trail

| Symbol | Kind | Description |
|--------|------|-------------|
| `AuditEvent` | enum | Order lifecycle events (NewOrder, Fill, Cancel, KillSwitch, etc.) |
| `audit_event_name(AuditEvent)` | constexpr fn | Human-readable event name |
| `Side` | enum | `Buy` or `Sell` |
| `AuditEntry` | struct | 64-byte cache-aligned record (TSC timestamp, order_id, price, qty, fill info) |
| `AuditEntry::dump()` | method | Format as a human-readable log line |
| `AuditLog<Capacity>` | class template | Fixed-size ring buffer (Capacity must be power of 2, default 65536) |
| `AuditLog::record(event, order_id, price, qty, side, venue_id, ...)` | method | Single-writer record (no sync) |
| `AuditLog::record_mt(event, order_id, price, qty, side, venue_id, ...)` | method | Multi-writer record (CAS spinloop) |
| `AuditLog::at(offset)` / `latest()` | methods | Access entries (0 = most recent) |
| `AuditLog::count()` / `total_count()` | methods | Current and total entry counts |
| `AuditLog::flush_to_file(path)` | method | Binary file flush for post-trade reporting |
| `AuditLog::dump(max_entries)` | method | Formatted string for logging/debugging |

### console_sink.hpp -- Metrics Console Sink

| Symbol | Kind | Description |
|--------|------|-------------|
| `ConsoleSink` | class | Satisfies `core::MetricsSink`; logs metrics as structured spdlog lines |
| `ConsoleSink::push_counter(name, value, tags)` | method | Log an integer counter |
| `ConsoleSink::push_gauge(name, value, tags)` | method | Log a floating-point gauge |
| `ConsoleSink::push_histogram(name, value, tags)` | method | Log a histogram observation |
| `ConsoleSink::flush()` | method | Flush buffered log messages |

### system_stats.hpp -- System Resource Profiling

| Symbol | Kind | Description |
|--------|------|-------------|
| `SystemResourceStats` | struct | Snapshot of CPU time, page faults, context switches, RSS, thread count |
| `SystemResourceStats::dump()` | method | Multi-line formatted report |
| `SystemResourceStats::to_json()` | method | JSON-formatted stats |
| `SystemResourceStats::operator-` | operator | Delta between two snapshots |
| `SystemStats(auto_log)` | class | RAII profiler; snapshots `getrusage` at construction |
| `SystemStats::snapshot()` | method | Compute resource delta since construction |
| `SystemStats::reset()` | method | Reset baseline to current state |
| `SystemStats::log_report()` | method | Log resource report via spdlog |
| `std::formatter<SystemResourceStats>` | specialization | `std::format` support for single-line summary |

### alignment.hpp -- Cache-Line Alignment

| Symbol | Kind | Description |
|--------|------|-------------|
| `CACHE_LINE_SIZE` | constexpr | 64 bytes (x86-64 cache line size) |
| `Align<T>` | variable template | `max(alignof(T), CACHE_LINE_SIZE)` for false-sharing prevention |

## Dependencies

- **eph-core** -- `MetricsSink` concept (`console_sink.hpp`), JSON escape utility (`hdr_histogram.hpp`, `recorder.hpp`)
- **spdlog** -- Logging throughout (compile-time filtered via `SPDLOG_ACTIVE_LEVEL`)

## Usage Examples

### TSC Timing and Latency Recording

```cpp
#include <eph/utils/time.hpp>
#include <eph/utils/recorder.hpp>

// Calibrate TSC at startup (once, thread-safe)
eph::utils::TSC::init();

// Pin this thread to core 2 for stable measurements
eph::utils::set_thread_affinity(2, "worker");

// Record latency with HdrHistogram
eph::utils::Recorder rec("OrderSubmit");
for (int i = 0; i < 100'000; ++i) {
    uint64_t t0 = eph::utils::TSC::now();
    submit_order();
    uint64_t t1 = eph::utils::TSC::now();
    rec.record(t1 - t0);
}
rec.print_report();    // tabulated console output
rec.export_json();     // JSON file with full percentile stats
```

### Multi-Threaded Latency Recording

```cpp
#include <eph/utils/recorder.hpp>
#include <thread>
#include <vector>

eph::utils::ConcurrentRecorder rec("HttpLatency");

// Each thread records with zero contention
std::vector<std::jthread> workers;
for (int t = 0; t < 4; ++t) {
    workers.emplace_back([&rec] {
        for (int i = 0; i < 50'000; ++i) {
            uint64_t t0 = eph::utils::TSC::now();
            handle_request();
            uint64_t t1 = eph::utils::TSC::now();
            rec.record(t1 - t0);
        }
    });
}
workers.clear();  // join all

rec.print_report();  // merges all threads automatically
```

### Scoped Timing with measure_tsc and ScopedTSC

```cpp
#include <eph/utils/hdr_histogram.hpp>

// Measure a callable
uint64_t cycles = eph::utils::measure_tsc([] { do_work(); });
auto ns = eph::utils::TSC::to_ns(cycles);

// RAII scope timer
uint64_t elapsed = 0;
{
    eph::utils::ScopedTSC timer(elapsed);
    do_work();
}
// elapsed now holds TSC cycles
```

### Hugepage Allocation

```cpp
#include <eph/utils/hugepage.hpp>

// Allocate a large array on 2MB hugepages (falls back to aligned_alloc)
auto buffer = eph::utils::HugePage::make<std::array<char, 10 * 1024 * 1024>>();
```

### EMA Crossover Detection

```cpp
#include <eph/utils/ema.hpp>

eph::utils::EmaCrossover crossover(5, 20);  // fast=5, slow=20

for (double price : price_stream) {
    auto signal = crossover.update(price);
    if (signal == eph::utils::EmaCrossover::Signal::BullishCross) {
        enter_long();
    } else if (signal == eph::utils::EmaCrossover::Signal::BearishCross) {
        exit_long();
    }
}
```

### Regulatory Audit Trail

```cpp
#include <eph/utils/audit_log.hpp>

eph::utils::AuditLog<8192> audit;

audit.record(eph::utils::AuditEvent::NewOrder, /*order_id=*/12345,
             /*price=*/50100.50, /*qty=*/1.5,
             eph::utils::Side::Buy, /*venue_id=*/0);

// ... later, flush to disk for compliance reporting
audit.flush_to_file("/var/log/audit/session.bin");
```

### System Resource Profiling

```cpp
#include <eph/utils/system_stats.hpp>

eph::utils::SystemStats profiler;

run_backtest();

auto stats = profiler.snapshot();
spdlog::info("{}", stats);           // single-line summary via std::format
spdlog::info("{}", stats.dump());    // multi-line detailed report
```

### Feed Latency Measurement

```cpp
#include <eph/utils/timestamp.hpp>

// Compute feed latency from a Binance millisecond timestamp
int64_t exchange_ts_ms = msg.event_time;
int64_t latency_ns = eph::utils::feed_latency_ns_from_ms(exchange_ts_ms);

// Format for logging
std::string ts = eph::utils::format_timestamp_ns(eph::utils::now_ns());
```
