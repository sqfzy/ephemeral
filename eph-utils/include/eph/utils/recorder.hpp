/// @file recorder.hpp
/// @brief Single-threaded and concurrent performance recorders for latency benchmarking.
///
/// `Recorder` provides single-threaded latency recording with HdrHistogram
/// storage, console reporting, and JSON/CSV export.
///
/// `ConcurrentRecorder` extends this to multiple threads using per-thread
/// `thread_local` histograms with zero contention on the hot path.
/// Data from exited threads is automatically merged into a retirement buffer.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <print>
#include <string>
#include <unordered_map>
#include <vector>

#include "eph/core/detail/json_escape.hpp"
#include "eph/utils/hdr_histogram.hpp"
#include "eph/utils/time.hpp"

namespace eph::utils {

namespace fs = std::filesystem;

// ============================================================================
// Shared file-export helpers (used by Recorder and ConcurrentRecorder)
// ============================================================================

namespace recorder_detail {

/// @brief Get the current wall-clock time as a file-safe timestamp string.
/// @return Timestamp in `YYYY-MM-DD_HH-MM-SS` format.
[[nodiscard]] inline std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    return std::format("{:%Y-%m-%d_%H-%M-%S}",
                       std::chrono::floor<std::chrono::seconds>(now));
}

/// @brief Replace non-alphanumeric characters in a filename with underscores.
/// @param name Raw name to sanitize.
/// @return Sanitized string safe for use as a filename component.
[[nodiscard]] inline std::string sanitize_filename(std::string name) {
    std::replace_if(
        name.begin(), name.end(),
        [](char c) {
            // Cast to unsigned char to avoid UB with std::isalnum on
            // negative char values (e.g., UTF-8 continuation bytes).
            auto uc = static_cast<unsigned char>(c);
            return !(std::isalnum(uc) || c == '_' || c == '-');
        },
        '_');
    return name;
}

/// @brief Build a timestamped output file path: `<dir>/<sanitized_name>_<timestamp><ext>`.
/// @param name Benchmark name (will be sanitized).
/// @param dir  Output directory.
/// @param ext  File extension including the dot (e.g., ".json").
/// @return Full filesystem path for the output file.
[[nodiscard]] inline fs::path make_output_path(const std::string& name,
                                               const std::string& dir,
                                               const std::string& ext) {
    return fs::path(dir) /
           (sanitize_filename(name) + "_" + get_timestamp() + ext);
}

/// @brief Create a directory (and parents) if it does not exist.
/// @param path Directory path to ensure.
/// @return `true` if the directory exists (or was created), `false` on error.
[[nodiscard]] inline bool ensure_directory(const std::string& path) noexcept {
    try {
        // create_directories() is idempotent: returns true if it created
        // the directory, false if it already existed. Either way, the
        // directory exists afterward. Only throws on actual I/O errors.
        // Previous code had a TOCTOU race: fs::exists() + create_directories()
        // could return false if a concurrent thread created the directory
        // between the two calls (create_directories returns false for
        // "already exists" but that's not an error).
        fs::create_directories(path);
        return true;
    } catch (const fs::filesystem_error& e) {
        std::println(stderr, "Directory error: {}", e.what());
        return false;
    }
}

}  // namespace recorder_detail

// ============================================================================
// Recorder — Single-threaded performance recorder
// ============================================================================

/// @brief Single-threaded latency recorder backed by an HdrHistogram.
///
/// Records TSC cycle measurements, computes percentile statistics, and
/// exports reports in console, JSON, and CSV formats. Does not collect
/// system resource data; pair with `SystemStats` if needed.
///
/// @note Default range: 1 cycle to ~10 seconds (~20 KB memory), 3
///       significant digits of precision.
///
/// @code
/// Recorder rec("VectorPushBack");
/// for (int i = 0; i < 100000; ++i) {
///     uint64_t start = TSC::now();
///     vec.push_back(i);
///     uint64_t end = TSC::now();
///     rec.record(end - start);
/// }
/// rec.print_report();
/// @endcode
class Recorder {
   public:
    /// @brief Construct a recorder.
    ///
    /// @param name            Benchmark / measurement name (must not be empty).
    /// @param lowest_cycles   Minimum trackable cycle count (default 1).
    /// @param highest_cycles  Maximum trackable cycle count. 0 (default)
    ///                        auto-computes the equivalent of 10 seconds.
    /// @param precision       HdrHistogram significant figures (default 3).
    ///
    /// @throws std::invalid_argument if `name` is empty.
    /// @throws std::runtime_error if TSC initialization fails.
    explicit Recorder(std::string name, uint64_t lowest_cycles = 1,
                      uint64_t highest_cycles = 0, int precision = 3)
        : name_(std::move(name)) {
        if (name_.empty()) {
            throw std::invalid_argument("Recorder name cannot be empty");
        }

        if (!TSC::is_initialized()) {
            if (!TSC::init()) {
                throw std::runtime_error("TSC initialization failed");
            }
        }

        // Default max cycles: 10 seconds (not 1 hour, to reduce memory usage)
        if (highest_cycles == 0) {
            highest_cycles =
                TSC::to_cycles(std::chrono::seconds(10))
                    .value_or(10ULL * 3'400'000'000);  // fallback to 3.4 GHz
        }

        highest_cycles = std::max(highest_cycles, 2 * lowest_cycles);
        histogram_ = HdrHistogram(lowest_cycles, highest_cycles, precision);
    }

    /// @brief Record a single measurement in CPU cycles.
    ///
    /// @param cycles Elapsed TSC cycles (must be > 0).
    /// @return `true` on success, `false` if the value is zero or out of
    ///         the histogram's trackable range.
    [[nodiscard]] bool record(uint64_t cycles) noexcept {
        if (cycles == 0) [[unlikely]] {
            skipped_invalid_++;
            return false;
        }

        if (!histogram_.record(cycles)) [[unlikely]] {
            skipped_overflow_++;
            return false;
        }

        count_++;
        total_cycles_ += cycles;
        min_cycles_ = std::min(min_cycles_, cycles);
        max_cycles_ = std::max(max_cycles_, cycles);

        return true;
    }

    /// @brief Record the same cycle value multiple times.
    ///
    /// @param cycles Elapsed TSC cycles (must be > 0).
    /// @param count  Number of times to record this value.
    /// @return `true` on success, `false` if the value is zero or out of range.
    [[nodiscard]] bool record_values(uint64_t cycles, uint64_t count) noexcept {
        if (count == 0) return true;
        if (cycles == 0) [[unlikely]] {
            skipped_invalid_ += count;
            return false;
        }

        if (!histogram_.record_values(cycles, count)) [[unlikely]] {
            skipped_overflow_ += count;
            return false;
        }

        count_ += count;
        // Saturate on overflow instead of wrapping to prevent corrupted
        // average calculations. This can happen with very large cycle
        // values recorded many times (e.g., cycles=10^10, count=10^9).
        if (cycles <= std::numeric_limits<uint64_t>::max() / count) [[likely]] {
            total_cycles_ += cycles * count;
        } else {
            total_cycles_ = std::numeric_limits<uint64_t>::max();
        }
        min_cycles_ = std::min(min_cycles_, cycles);
        max_cycles_ = std::max(max_cycles_, cycles);

        return true;
    }

    /// @brief Record a single latency sample in nanoseconds.
    ///
    /// Converts the ns value to TSC cycles internally via the current
    /// ns/cycle ratio, then dispatches to `record(uint64_t cycles)`. The
    /// existing cycle-based storage and `compute_stats()` path are
    /// preserved unchanged — the final output is still ns regardless of
    /// whether samples entered via `record()` or `record_ns()`.
    ///
    /// Intended for bench helpers that measure latency via
    /// `clock_gettime(MONOTONIC_RAW)` or receive ns timestamps from
    /// another process (e.g., a Python mock echo server).
    ///
    /// @param ns Elapsed time in nanoseconds.
    ///
    /// @note Precision: the ns → cycles → ns round-trip loses ≤ 1 ns
    ///       from integer truncation of the cycle count. Acceptable for
    ///       benchmarks with p50 in the microsecond range.
    ///
    /// @note Thread-safety: same as `record()` (not thread-safe; one
    ///       recorder per thread).
    void record_ns(uint64_t ns) noexcept {
        // Discard the bool return from record(): record_ns() has a void
        // contract so it can be used from hot-path code that doesn't
        // want to branch on per-sample failures. Out-of-range samples
        // are still tallied into skipped_overflow_ for diagnostics.
        (void)record(ns_to_cycles_(ns));
    }

    /// @brief Record the same ns latency value multiple times.
    ///
    /// Batched variant of `record_ns()`. Equivalent to calling
    /// `record_ns(ns)` `count` times but faster due to the existing
    /// `record_values()` batched path.
    ///
    /// @param ns    Elapsed time in nanoseconds.
    /// @param count Number of times to record this value.
    void record_ns_values(uint64_t ns, uint64_t count) noexcept {
        (void)record_values(ns_to_cycles_(ns), count);
    }

    /// @brief Merge another Recorder's histogram and statistics into this one.
    ///
    /// @param other The recorder to merge from (must have compatible histogram config).
    /// @return `true` on success, `false` if histograms are incompatible.
    [[nodiscard]] bool merge(const Recorder& other) noexcept {
        if (!histogram_.merge(other.histogram_)) {
            return false;
        }

        count_ += other.count_;
        total_cycles_ += other.total_cycles_;
        min_cycles_ = std::min(min_cycles_, other.min_cycles_);
        max_cycles_ = std::max(max_cycles_, other.max_cycles_);
        skipped_invalid_ += other.skipped_invalid_;
        skipped_overflow_ += other.skipped_overflow_;

        return true;
    }

    /// @brief Compute aggregated latency statistics from the recorded data.
    /// @return `Stats` on success, or `nullopt` if no data has been recorded
    ///         or TSC is uncalibrated.
    [[nodiscard]] std::optional<Stats> compute_stats() const noexcept {
        if (count_ == 0) return std::nullopt;

        auto ns_per_cycle = TSC::to_ns(1);
        if (!ns_per_cycle) [[unlikely]]
            return std::nullopt;

        double ratio = *ns_per_cycle;
        double avg_cyc = static_cast<double>(total_cycles_) / count_;

        auto percentiles = histogram_.get_percentiles({50.0, 90.0, 99.0, 99.9});

        return Stats{
            .name = name_,
            .count = count_,
            .avg_ns = avg_cyc * ratio,
            .min_ns = min_cycles_ * ratio,
            .max_ns = max_cycles_ * ratio,
            .p50_ns = percentiles[0] * ratio,
            .p90_ns = percentiles[1] * ratio,
            .p99_ns = percentiles[2] * ratio,
            .p999_ns = percentiles[3] * ratio,
            .stddev_ns = histogram_.get_std_deviation() * ratio,
        };
    }

    /// @brief Print a formatted latency report to stdout.
    void print_report() const {
        auto stats = compute_stats();
        if (!stats) {
            std::println(stderr, "[{}] No data recorded", name_);
            print_warnings();
            return;
        }

        std::string time_str = get_timestamp();
        std::string title = std::format(" BENCHMARK REPORT ({}) ", time_str);

        constexpr int w_name = 30;
        constexpr int w_metric = 12;
        constexpr int total_w = w_name + (w_metric * 6) + 18;

        std::println("\n{:-^{}}", title, total_w);

        std::println(
            "{:<{}} | {:>{}} | {:>{}} | {:>{}} | {:>{}} | {:>{}} | {:>{}}",
            "Task Name", w_name, "Count", w_metric, "Avg(ns)", w_metric,
            "P50(ns)", w_metric, "P99(ns)", w_metric, "Min(ns)", w_metric,
            "Max(ns)", w_metric);

        std::println("{:-^{}}", "", total_w);

        std::println(
            "{:<{}} | {:>{}} | {:>{}.1f} | {:>{}.1f} | {:>{}.1f} | "
            "{:>{}.1f} | {:>{}.1f}",
            stats->name, w_name, stats->count, w_metric, stats->avg_ns,
            w_metric, stats->p50_ns, w_metric, stats->p99_ns, w_metric,
            stats->min_ns, w_metric, stats->max_ns, w_metric);

        std::println("{:-^{}}\n", "", total_w);
        print_warnings();
    }

    /// @brief Export latency statistics as a JSON file.
    ///
    /// @param output_dir Directory for the output file (created if absent).
    /// @return `true` on success, `false` on error (logged to stderr).
    [[nodiscard]] bool export_json(const std::string& output_dir = "outputs") const {
        auto stats = compute_stats();
        if (!stats) return false;

        if (!ensure_directory(output_dir)) return false;

        auto path = make_output_path(output_dir, ".json");
        std::ofstream file(path);
        if (!file) {
            std::println(stderr, "Failed to open: {}", path.string());
            return false;
        }

        try {
            file << std::format(
                R"({{
  "name": "{}",
  "timestamp": "{}",
  "samples": {{
    "recorded": {},
    "skipped_invalid": {},
    "skipped_overflow": {}
  }},
  "latency_ns": {{
    "avg": {:.2f},
    "min": {:.2f},
    "max": {:.2f},
    "stddev": {:.2f},
    "p50": {:.2f},
    "p90": {:.2f},
    "p99": {:.2f},
    "p99_9": {:.2f}
  }},
  "histogram_memory_bytes": {}
}})",
                eph::core::detail::json_escape(stats->name), get_timestamp(),
                stats->count, skipped_invalid_,
                skipped_overflow_, stats->avg_ns, stats->min_ns, stats->max_ns,
                stats->stddev_ns, stats->p50_ns, stats->p90_ns, stats->p99_ns,
                stats->p999_ns, histogram_.get_memory_size());

            if (!file.good()) {
                std::println(stderr, "Write error: {}", path.string());
                return false;
            }

            std::println("  JSON: {}", path.string());
            return true;

        } catch (const std::exception& e) {
            std::println(stderr, "JSON error: {}", e.what());
            return false;
        }
    }

    /// @brief Export the latency distribution as a CSV file.
    ///
    /// Each row contains `(latency_ns, count)` for one histogram bucket.
    ///
    /// @param output_dir Directory for the output file (created if absent).
    /// @return `true` on success, `false` on error (logged to stderr).
    [[nodiscard]] bool export_csv(const std::string& output_dir = "outputs") const {
        if (count_ == 0) return false;

        if (!ensure_directory(output_dir)) return false;

        auto ns_per_cycle = TSC::to_ns(1);
        if (!ns_per_cycle) [[unlikely]]
            return false;

        double ratio = *ns_per_cycle;
        auto path = make_output_path(output_dir, ".csv");
        std::ofstream file(path);
        if (!file) {
            std::println(stderr, "Failed to open: {}", path.string());
            return false;
        }

        file << "latency_ns,count\n";

        try {
            histogram_.for_each_recorded_value(
                [&](uint64_t cycles, uint64_t count) {
                    file << std::format("{:.2f},{}\n", cycles * ratio, count);
                });

            if (!file.good()) {
                std::println(stderr, "Write error: {}", path.string());
                return false;
            }

            std::println("  CSV: {}", path.string());
            return true;

        } catch (const std::exception& e) {
            std::println(stderr, "CSV error: {}", e.what());
            return false;
        }
    }

    /// @brief Export both JSON summary and CSV distribution files.
    /// @param output_dir Directory for the output files (created if absent).
    /// @return `true` if both exports succeeded.
    [[nodiscard]] bool export_all(const std::string& output_dir = "outputs") const {
        bool json_ok = export_json(output_dir);
        bool csv_ok = export_csv(output_dir);
        return json_ok && csv_ok;
    }

    // ========== Query API ==========

    /// @brief Benchmark name provided at construction.
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    /// @brief Number of successfully recorded measurements.
    [[nodiscard]] uint64_t count() const noexcept { return count_; }

    /// @brief Number of measurements skipped because the value was zero.
    [[nodiscard]] uint64_t skipped_invalid_count() const noexcept {
        return skipped_invalid_;
    }

    /// @brief Number of measurements skipped because they exceeded the histogram range.
    [[nodiscard]] uint64_t skipped_overflow_count() const noexcept {
        return skipped_overflow_;
    }

    /// @brief Check whether at least one measurement has been recorded.
    [[nodiscard]] bool has_data() const noexcept { return count_ > 0; }

    /// @brief Direct access to the underlying HdrHistogram.
    [[nodiscard]] const HdrHistogram& histogram() const noexcept {
        return histogram_;
    }

    /// @brief Reset all recorded data and counters to their initial state.
    void reset() noexcept {
        count_ = 0;
        total_cycles_ = 0;
        min_cycles_ = std::numeric_limits<uint64_t>::max();
        max_cycles_ = 0;
        skipped_invalid_ = 0;
        skipped_overflow_ = 0;
        histogram_.reset();
    }

   private:
    std::string name_;
    uint64_t count_ = 0;
    uint64_t total_cycles_ = 0;
    uint64_t min_cycles_ = std::numeric_limits<uint64_t>::max();
    uint64_t max_cycles_ = 0;
    uint64_t skipped_invalid_ = 0;
    uint64_t skipped_overflow_ = 0;

    HdrHistogram histogram_;

    [[nodiscard]] static std::string get_timestamp() {
        return recorder_detail::get_timestamp();
    }
    [[nodiscard]] static bool ensure_directory(const std::string& path) noexcept {
        return recorder_detail::ensure_directory(path);
    }
    [[nodiscard]] fs::path make_output_path(const std::string& dir,
                                            const std::string& ext) const {
        return recorder_detail::make_output_path(name_, dir, ext);
    }

    /// @brief Convert a nanosecond value to TSC cycles using the current
    ///        ns/cycle ratio.
    ///
    /// Used by `record_ns()` / `record_ns_values()` to funnel raw-ns input
    /// into the existing cycle-based histogram storage so the report path
    /// (`compute_stats()`) stays unchanged.
    ///
    /// The ratio is cached in a function-local `static` so repeated calls
    /// avoid the `TSC::to_ns(1)` atomic load + optional unwrap. C++11
    /// guarantees thread-safe initialization of function-local statics,
    /// so this is safe even if the first call races across threads.
    ///
    /// Caching assumes TSC calibration is stable for the lifetime of the
    /// process (which it is: `TSC::init()` runs once and the ratio is
    /// never recomputed). If `TSC::to_ns(1)` returns `nullopt` on the
    /// very first call (TSC not yet initialized), we fall back to a
    /// sentinel ratio of 1.0 ns/cycle — this yields cycle counts equal to
    /// the ns input, which is still within the histogram's trackable
    /// range and produces self-consistent stats (just not calibrated to
    /// wall time). The Recorder constructor already forces TSC::init(),
    /// so in practice this fallback path is never taken.
    [[nodiscard]] static uint64_t ns_to_cycles_(uint64_t ns) noexcept {
        static const double cycles_per_ns = [] {
            auto ns_per_cycle = TSC::to_ns(1);
            // Sentinel 1.0 avoids division-by-zero / nullopt branching on
            // the hot path. See comment above.
            if (!ns_per_cycle || *ns_per_cycle <= 0.0) return 1.0;
            return 1.0 / *ns_per_cycle;
        }();
        // Integer truncation: loses at most 1 cycle, which on a 3 GHz
        // machine is ≈ 0.33 ns — well within the ±1 ns round-trip budget
        // documented on record_ns().
        return static_cast<uint64_t>(static_cast<double>(ns) * cycles_per_ns);
    }

    void print_warnings() const {
        if (skipped_invalid_ > 0 || skipped_overflow_ > 0) {
            std::println(stderr, "  Warnings:");
            if (skipped_invalid_ > 0) {
                std::println(stderr, "      {} invalid values skipped",
                             skipped_invalid_);
            }
            if (skipped_overflow_ > 0) {
                std::println(stderr, "      {} values exceeded histogram range",
                             skipped_overflow_);
            }
        }
    }
};

// ============================================================================
// ConcurrentRecorder — Multi-threaded performance recorder
// ============================================================================

/// @brief Concurrent (multi-threaded) latency recorder.
///
/// Each thread records into a `thread_local` HdrHistogram with zero
/// contention on the hot path. When a thread exits, its data is
/// automatically merged into a shared retirement buffer so no samples
/// are lost.
///
/// `compute_stats()` and `print_report()` merge all active and retired
/// thread data on demand (requires the internal mutex).
///
/// Shared state is managed via `shared_ptr`, so `thread_local`
/// destructors can safely access it even after the `ConcurrentRecorder`
/// itself is destroyed.
///
/// @note Default range: 1 cycle to ~10 seconds (~20 KB per thread), 3
///       significant digits.
///
/// @code
/// ConcurrentRecorder rec("HttpLatency");
///
/// // From any thread:
/// rec.record(end - start);
///
/// // From the main thread (after join):
/// rec.print_report();  // merges all threads, including exited ones
/// @endcode
class ConcurrentRecorder {
   public:
    explicit ConcurrentRecorder(std::string name, uint64_t lowest_cycles = 1,
                                uint64_t highest_cycles = 0,
                                int precision = 3)
        : name_(std::move(name)) {
        if (name_.empty()) {
            throw std::invalid_argument(
                "ConcurrentRecorder name cannot be empty");
        }

        if (!TSC::is_initialized()) {
            if (!TSC::init()) {
                throw std::runtime_error("TSC initialization failed");
            }
        }

        if (highest_cycles == 0) {
            highest_cycles =
                TSC::to_cycles(std::chrono::seconds(10))
                    .value_or(10ULL * 3'400'000'000);
        }

        highest_cycles = std::max(highest_cycles, 2 * lowest_cycles);

        // Shared state: managed via shared_ptr so threads can safely access it after exit
        state_ = std::make_shared<SharedState>(
            lowest_cycles, highest_cycles, precision);
    }

    ~ConcurrentRecorder() = default;

    ConcurrentRecorder(const ConcurrentRecorder&) = delete;
    ConcurrentRecorder& operator=(const ConcurrentRecorder&) = delete;
    ConcurrentRecorder(ConcurrentRecorder&&) = delete;
    ConcurrentRecorder& operator=(ConcurrentRecorder&&) = delete;

    /// @brief Record a single measurement (thread-safe, zero contention).
    ///
    /// On first call from a new thread, lazily creates and registers a
    /// `thread_local` histogram. Subsequent calls write directly to that
    /// histogram with no synchronization.
    ///
    /// @param cycles Elapsed TSC cycles (must be > 0).
    /// @return `true` on success, `false` if the value is zero or out of range.
    [[nodiscard]] bool record(uint64_t cycles) noexcept {
        auto* local = get_or_create_local();
        if (!local) [[unlikely]] return false;

        if (cycles == 0) [[unlikely]] {
            local->skipped_invalid++;
            return false;
        }

        if (!local->histogram.record(cycles)) [[unlikely]] {
            local->skipped_overflow++;
            return false;
        }

        local->count++;
        local->total_cycles += cycles;
        local->min_cycles = std::min(local->min_cycles, cycles);
        local->max_cycles = std::max(local->max_cycles, cycles);

        return true;
    }

    /// @brief Record the same cycle value multiple times (thread-safe).
    ///
    /// Equivalent to calling `record(cycles)` `count` times but avoids
    /// repeated min/max comparisons. Mirrors `Recorder::record_values()`.
    ///
    /// @param cycles Elapsed TSC cycles (must be > 0).
    /// @param count  Number of times to record this value.
    /// @return `true` on success, `false` if the value is zero or out of range.
    [[nodiscard]] bool record_values(uint64_t cycles, uint64_t count) noexcept {
        if (count == 0) return true;

        auto* local = get_or_create_local();
        if (!local) [[unlikely]] return false;

        if (cycles == 0) [[unlikely]] {
            local->skipped_invalid += count;
            return false;
        }

        if (!local->histogram.record_values(cycles, count)) [[unlikely]] {
            local->skipped_overflow += count;
            return false;
        }

        local->count += count;
        // Saturate on overflow to prevent corrupted average calculations.
        if (cycles <= std::numeric_limits<uint64_t>::max() / count) [[likely]] {
            local->total_cycles += cycles * count;
        } else {
            local->total_cycles = std::numeric_limits<uint64_t>::max();
        }
        local->min_cycles = std::min(local->min_cycles, cycles);
        local->max_cycles = std::max(local->max_cycles, cycles);

        return true;
    }

    /// @brief Compute merged latency statistics across all threads.
    ///
    /// Merges data from all active and retired threads under the internal
    /// mutex. Not suitable for hot-path use due to lock acquisition.
    ///
    /// @return `Stats` on success, or `nullopt` if no data has been recorded.
    [[nodiscard]] std::optional<Stats> compute_stats() const {
        auto merged = state_->merge_all();
        if (merged.count == 0) return std::nullopt;

        auto ns_per_cycle = TSC::to_ns(1);
        if (!ns_per_cycle) [[unlikely]]
            return std::nullopt;

        double ratio = *ns_per_cycle;
        double avg_cyc =
            static_cast<double>(merged.total_cycles) / merged.count;

        auto percentiles =
            merged.histogram.get_percentiles({50.0, 90.0, 99.0, 99.9});

        return Stats{
            .name = name_,
            .count = merged.count,
            .avg_ns = avg_cyc * ratio,
            .min_ns = merged.min_cycles * ratio,
            .max_ns = merged.max_cycles * ratio,
            .p50_ns = percentiles[0] * ratio,
            .p90_ns = percentiles[1] * ratio,
            .p99_ns = percentiles[2] * ratio,
            .p999_ns = percentiles[3] * ratio,
            .stddev_ns = merged.histogram.get_std_deviation() * ratio,
        };
    }

    /// @brief Print a formatted latency report to stdout, including thread counts.
    void print_report() const {
        auto stats = compute_stats();
        if (!stats) {
            std::println(stderr, "[{}] No data recorded", name_);
            return;
        }

        auto now = std::chrono::system_clock::now();
        std::string time_str = std::format(
            "{:%Y-%m-%d_%H-%M-%S}",
            std::chrono::floor<std::chrono::seconds>(now));
        std::string title = std::format(" BENCHMARK REPORT ({}) ", time_str);

        constexpr int w_name = 30;
        constexpr int w_metric = 12;
        constexpr int total_w = w_name + (w_metric * 6) + 18;

        auto [active, retired] = state_->thread_counts();

        std::println("\n{:-^{}}", title, total_w);
        std::println("  Threads: {} active, {} retired", active, retired);

        std::println(
            "{:<{}} | {:>{}} | {:>{}} | {:>{}} | {:>{}} | {:>{}} | {:>{}}",
            "Task Name", w_name, "Count", w_metric, "Avg(ns)", w_metric,
            "P50(ns)", w_metric, "P99(ns)", w_metric, "Min(ns)", w_metric,
            "Max(ns)", w_metric);

        std::println("{:-^{}}", "", total_w);

        std::println(
            "{:<{}} | {:>{}} | {:>{}.1f} | {:>{}.1f} | {:>{}.1f} | "
            "{:>{}.1f} | {:>{}.1f}",
            stats->name, w_name, stats->count, w_metric, stats->avg_ns,
            w_metric, stats->p50_ns, w_metric, stats->p99_ns, w_metric,
            stats->min_ns, w_metric, stats->max_ns, w_metric);

        std::println("{:-^{}}\n", "", total_w);
    }

    /// @brief Benchmark name provided at construction.
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    /// @brief Total number of threads that have recorded data (active + retired).
    [[nodiscard]] size_t thread_count() const {
        auto [active, retired] = state_->thread_counts();
        return active + retired;
    }

    /// Export merged statistics as JSON to a file.
    ///
    /// Merges all thread-local data (active + retired), then writes
    /// the same JSON format as Recorder::export_json(). Thread counts
    /// and skipped samples are included for multi-threaded diagnostics.
    ///
    /// @param output_dir  Directory for the output file (created if absent)
    /// @return true on success, false on error (logged to stderr)
    [[nodiscard]] bool export_json(const std::string& output_dir = "outputs") const {
        auto stats = compute_stats();
        if (!stats) return false;

        if (!ensure_directory(output_dir)) return false;

        auto path = make_output_path(output_dir, ".json");
        std::ofstream file(path);
        if (!file) {
            std::println(stderr, "Failed to open: {}", path.string());
            return false;
        }

        auto [active, retired] = state_->thread_counts();
        auto [skipped_invalid, skipped_overflow] = merged_skipped_counts();

        try {
            file << std::format(
                R"({{
  "name": "{}",
  "timestamp": "{}",
  "threads": {{
    "active": {},
    "retired": {}
  }},
  "samples": {{
    "recorded": {},
    "skipped_invalid": {},
    "skipped_overflow": {}
  }},
  "latency_ns": {{
    "avg": {:.2f},
    "min": {:.2f},
    "max": {:.2f},
    "stddev": {:.2f},
    "p50": {:.2f},
    "p90": {:.2f},
    "p99": {:.2f},
    "p99_9": {:.2f}
  }}
}})",
                eph::core::detail::json_escape(stats->name), get_timestamp(),
                active, retired,
                stats->count, skipped_invalid, skipped_overflow,
                stats->avg_ns, stats->min_ns, stats->max_ns,
                stats->stddev_ns, stats->p50_ns, stats->p90_ns,
                stats->p99_ns, stats->p999_ns);

            if (!file.good()) {
                std::println(stderr, "Write error: {}", path.string());
                return false;
            }

            std::println("  JSON: {}", path.string());
            return true;

        } catch (const std::exception& e) {
            std::println(stderr, "JSON error: {}", e.what());
            return false;
        }
    }

    /// Export merged histogram distribution as CSV.
    ///
    /// Merges all thread-local histograms, then writes each recorded
    /// bucket as a (latency_ns, count) row — same format as Recorder::export_csv().
    ///
    /// @param output_dir  Directory for the output file (created if absent)
    /// @return true on success, false on error (logged to stderr)
    [[nodiscard]] bool export_csv(const std::string& output_dir = "outputs") const {
        auto merged = state_->merge_all();
        if (merged.count == 0) return false;

        if (!ensure_directory(output_dir)) return false;

        auto ns_per_cycle = TSC::to_ns(1);
        if (!ns_per_cycle) [[unlikely]]
            return false;

        double ratio = *ns_per_cycle;
        auto path = make_output_path(output_dir, ".csv");
        std::ofstream file(path);
        if (!file) {
            std::println(stderr, "Failed to open: {}", path.string());
            return false;
        }

        file << "latency_ns,count\n";

        try {
            merged.histogram.for_each_recorded_value(
                [&](uint64_t cycles, uint64_t count) {
                    file << std::format("{:.2f},{}\n", cycles * ratio, count);
                });

            if (!file.good()) {
                std::println(stderr, "Write error: {}", path.string());
                return false;
            }

            std::println("  CSV: {}", path.string());
            return true;

        } catch (const std::exception& e) {
            std::println(stderr, "CSV error: {}", e.what());
            return false;
        }
    }

    /// @brief Export both JSON summary and CSV distribution files.
    /// @param output_dir Directory for the output files (created if absent).
    /// @return `true` if both exports succeeded.
    [[nodiscard]] bool export_all(const std::string& output_dir = "outputs") const {
        bool json_ok = export_json(output_dir);
        bool csv_ok = export_csv(output_dir);
        return json_ok && csv_ok;
    }

    /// Atomically compute stats and reset all data in one operation.
    ///
    /// Returns the statistics for the current measurement window, then
    /// resets all histograms (active + retired) under the same lock.
    /// No data is lost between the merge and reset — unlike the separate
    /// compute_stats() + reset() pattern which has a gap where samples
    /// are either lost or double-counted.
    ///
    /// Ideal for periodic monitoring dashboards that need non-overlapping
    /// measurement windows.
    ///
    /// @return Stats for the completed window, or nullopt if no data was recorded
    ///
    /// @warning Not thread-safe with concurrent record() calls on the same
    ///          thread-local data. Call from a dedicated reporting thread,
    ///          ideally when recording threads are idle or between bursts.
    ///          Samples recorded concurrently with this call may be partially
    ///          included in the returned stats or silently dropped.
    [[nodiscard]] std::optional<Stats> compute_and_reset() {
        auto merged = state_->merge_all_and_reset();
        if (merged.count == 0) return std::nullopt;

        auto ns_per_cycle = TSC::to_ns(1);
        if (!ns_per_cycle) [[unlikely]]
            return std::nullopt;

        double ratio = *ns_per_cycle;
        double avg_cyc =
            static_cast<double>(merged.total_cycles) / merged.count;

        auto percentiles =
            merged.histogram.get_percentiles({50.0, 90.0, 99.0, 99.9});

        return Stats{
            .name = name_,
            .count = merged.count,
            .avg_ns = avg_cyc * ratio,
            .min_ns = merged.min_cycles * ratio,
            .max_ns = merged.max_cycles * ratio,
            .p50_ns = percentiles[0] * ratio,
            .p90_ns = percentiles[1] * ratio,
            .p99_ns = percentiles[2] * ratio,
            .p999_ns = percentiles[3] * ratio,
            .stddev_ns = merged.histogram.get_std_deviation() * ratio,
        };
    }

    /// Reset all accumulated data across all threads (active and retired).
    ///
    /// Useful for windowed measurement: call compute_stats(), then reset()
    /// to start a fresh measurement interval.
    ///
    /// @warning Not thread-safe with concurrent record() calls — call from
    ///          a single thread (typically the reporting thread) when no
    ///          other threads are actively recording.
    void reset() noexcept {
        std::lock_guard lock(state_->mutex);
        state_->reset_all_locked();
    }

   private:
    // Per-thread local data
    struct ThreadLocalData {
        HdrHistogram histogram;
        uint64_t count = 0;
        uint64_t total_cycles = 0;
        uint64_t min_cycles = std::numeric_limits<uint64_t>::max();
        uint64_t max_cycles = 0;
        uint64_t skipped_invalid = 0;
        uint64_t skipped_overflow = 0;
    };

    // Shared state: managed via shared_ptr to ensure safe access when threads exit
    struct SharedState {
        uint64_t lowest_cycles;
        uint64_t highest_cycles;
        int precision;

        mutable std::mutex mutex;

        // Pointers to active thread-local data
        std::vector<ThreadLocalData*> active_locals;

        // Accumulated data from retired (exited) threads
        HdrHistogram retired_histogram;
        uint64_t retired_count = 0;
        uint64_t retired_total_cycles = 0;
        uint64_t retired_min_cycles = std::numeric_limits<uint64_t>::max();
        uint64_t retired_max_cycles = 0;
        uint64_t retired_skipped_invalid = 0;
        uint64_t retired_skipped_overflow = 0;
        size_t retired_thread_count = 0;

        SharedState(uint64_t low, uint64_t high, int prec)
            : lowest_cycles(low),
              highest_cycles(high),
              precision(prec),
              retired_histogram(low, high, prec) {}

        void register_local(ThreadLocalData* local) {
            std::lock_guard lock(mutex);
            active_locals.push_back(local);
        }

        // Called on thread exit: merge data into retirement buffer and remove from active list
        void retire_local(ThreadLocalData* local) noexcept {
            try {
                std::lock_guard lock(mutex);
                auto it = std::find(
                    active_locals.begin(), active_locals.end(), local);
                if (it != active_locals.end()) {
                    // Merge data into retirement buffer (always compatible — same ctor params)
                    (void)retired_histogram.merge(local->histogram);
                    retired_count += local->count;
                    retired_total_cycles += local->total_cycles;
                    retired_min_cycles =
                        std::min(retired_min_cycles, local->min_cycles);
                    retired_max_cycles =
                        std::max(retired_max_cycles, local->max_cycles);
                    retired_skipped_invalid += local->skipped_invalid;
                    retired_skipped_overflow += local->skipped_overflow;
                    retired_thread_count++;

                    active_locals.erase(it);
                }
            } catch (...) {
                // Silently ignore to avoid throwing on destructor path
            }
        }

        // Merged result structure
        struct MergedData {
            HdrHistogram histogram;
            uint64_t count = 0;
            uint64_t total_cycles = 0;
            uint64_t min_cycles = std::numeric_limits<uint64_t>::max();
            uint64_t max_cycles = 0;
        };

        // Merge all data (retired + active) — caller holds mutex
        [[nodiscard]] MergedData merge_all_locked() const {
            MergedData merged;
            merged.histogram =
                HdrHistogram(lowest_cycles, highest_cycles, precision);

            // First merge retired data (always compatible — same ctor params)
            (void)merged.histogram.merge(retired_histogram);
            merged.count = retired_count;
            merged.total_cycles = retired_total_cycles;
            merged.min_cycles = retired_min_cycles;
            merged.max_cycles = retired_max_cycles;

            // Then merge active thread data (always compatible — same ctor params)
            for (const auto* local : active_locals) {
                (void)merged.histogram.merge(local->histogram);
                merged.count += local->count;
                merged.total_cycles += local->total_cycles;
                merged.min_cycles =
                    std::min(merged.min_cycles, local->min_cycles);
                merged.max_cycles =
                    std::max(merged.max_cycles, local->max_cycles);
            }

            return merged;
        }

        // Reset all data (retired + active) — caller holds mutex
        void reset_all_locked() noexcept {
            for (auto* local : active_locals) {
                local->histogram.reset();
                local->count = 0;
                local->total_cycles = 0;
                local->min_cycles = std::numeric_limits<uint64_t>::max();
                local->max_cycles = 0;
                local->skipped_invalid = 0;
                local->skipped_overflow = 0;
            }
            retired_histogram.reset();
            retired_count = 0;
            retired_total_cycles = 0;
            retired_min_cycles = std::numeric_limits<uint64_t>::max();
            retired_max_cycles = 0;
            retired_skipped_invalid = 0;
            retired_skipped_overflow = 0;
        }

        // Merge all data (retired + active)
        [[nodiscard]] MergedData merge_all() const {
            std::lock_guard lock(mutex);
            return merge_all_locked();
        }

        /// Atomically merge all data and reset — single lock acquisition.
        ///
        /// Returns the merged stats from all threads, then resets all
        /// active and retired histograms. No data is lost between the
        /// merge and reset (unlike separate compute_stats()+reset()).
        [[nodiscard]] MergedData merge_all_and_reset() {
            std::lock_guard lock(mutex);
            auto merged = merge_all_locked();
            reset_all_locked();
            return merged;
        }

        [[nodiscard]] std::pair<size_t, size_t> thread_counts() const {
            std::lock_guard lock(mutex);
            return {active_locals.size(), retired_thread_count};
        }
    };

    // RAII guard: holds shared_ptr<SharedState>, safely retires on thread exit
    struct ThreadLocalGuard {
        ThreadLocalData data;
        std::shared_ptr<SharedState> state;

        ThreadLocalGuard(uint64_t low, uint64_t high, int prec,
                         std::shared_ptr<SharedState> s)
            : state(std::move(s)) {
            data.histogram = HdrHistogram(low, high, prec);
            state->register_local(&data);
        }

        ~ThreadLocalGuard() {
            // shared_ptr ensures SharedState is still alive at this point
            state->retire_local(&data);
        }

        ThreadLocalGuard(const ThreadLocalGuard&) = delete;
        ThreadLocalGuard& operator=(const ThreadLocalGuard&) = delete;
    };

    std::string name_;
    std::shared_ptr<SharedState> state_;

    [[nodiscard]] static std::string get_timestamp() {
        return recorder_detail::get_timestamp();
    }
    [[nodiscard]] static bool ensure_directory(const std::string& path) noexcept {
        return recorder_detail::ensure_directory(path);
    }
    [[nodiscard]] fs::path make_output_path(const std::string& dir,
                                            const std::string& ext) const {
        return recorder_detail::make_output_path(name_, dir, ext);
    }

    /// Sum skipped counts across all active and retired thread-local data.
    [[nodiscard]] std::pair<uint64_t, uint64_t> merged_skipped_counts() const {
        std::lock_guard lock(state_->mutex);
        uint64_t invalid = state_->retired_skipped_invalid;
        uint64_t overflow = state_->retired_skipped_overflow;
        for (const auto* local : state_->active_locals) {
            invalid += local->skipped_invalid;
            overflow += local->skipped_overflow;
        }
        return {invalid, overflow};
    }

    ThreadLocalData* get_or_create_local() noexcept {
        // Each ConcurrentRecorder has its own SharedState. Multiple recorders
        // used from the same thread each get an independent ThreadLocalGuard,
        // keyed by SharedState pointer. This avoids the data-loss problem
        // where interleaving calls to different recorders from one thread
        // would repeatedly retire+recreate the single thread-local guard.
        using GuardMap = std::unordered_map<
            SharedState*, std::unique_ptr<ThreadLocalGuard>>;
        thread_local GuardMap tl_guards;

        auto* key = state_.get();
        auto it = tl_guards.find(key);
        if (it != tl_guards.end()) [[likely]] {
            return &it->second->data;
        }

        // Cold path: create and register
        try {
            auto guard = std::make_unique<ThreadLocalGuard>(
                state_->lowest_cycles, state_->highest_cycles,
                state_->precision, state_);
            auto* data = &guard->data;
            tl_guards.emplace(key, std::move(guard));
            return data;
        } catch (...) {
            return nullptr;
        }
    }
};

}  // namespace eph::utils
