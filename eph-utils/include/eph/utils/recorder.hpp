#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <print>
#include <string>
#include <vector>

#include "eph/utils/hdr_histogram.hpp"
#include "eph/utils/time.hpp"

namespace eph::utils {

namespace fs = std::filesystem;

// ============================================================================
// Shared file-export helpers (used by Recorder and ConcurrentRecorder)
// ============================================================================

namespace recorder_detail {

[[nodiscard]] inline std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    return std::format("{:%Y-%m-%d_%H-%M-%S}",
                       std::chrono::floor<std::chrono::seconds>(now));
}

[[nodiscard]] inline std::string sanitize_filename(std::string name) {
    std::replace_if(
        name.begin(), name.end(),
        [](char c) { return !(std::isalnum(c) || c == '_' || c == '-'); },
        '_');
    return name;
}

[[nodiscard]] inline fs::path make_output_path(const std::string& name,
                                               const std::string& dir,
                                               const std::string& ext) {
    return fs::path(dir) /
           (sanitize_filename(name) + "_" + get_timestamp() + ext);
}

[[nodiscard]] inline bool ensure_directory(const std::string& path) noexcept {
    try {
        if (!fs::exists(path)) {
            return fs::create_directories(path);
        }
        return true;
    } catch (const fs::filesystem_error& e) {
        std::println(stderr, "Directory error: {}", e.what());
        return false;
    }
}

}  // namespace recorder_detail

// ============================================================================
// Recorder — 单线程性能记录器
// ============================================================================

/**
 * @brief 单线程性能数据记录器
 *
 * 不含系统资源采集，纯延迟统计。如需系统资源统计请配合 SystemStats 使用。
 *
 * @note 默认构造范围 1ns-10s（约 ~20KB 内存），3 位有效数字精度
 *
 * @example
 * @code
 * Recorder rec("VectorPushBack");
 * for (int i = 0; i < 100000; ++i) {
 *     uint64_t start = TSC::now();
 *     vec.push_back(i);
 *     uint64_t end = TSC::now();
 *     rec.record(end - start);
 * }
 * rec.print_report();
 * @endcode
 */
class Recorder {
   public:
    /**
     * @brief 构造记录器
     *
     * @param name 基准测试名称
     * @param lowest_cycles 最小可追踪周期数（默认 1）
     * @param highest_cycles 最大可追踪周期数（默认 0 → 自动计算 10 秒对应周期）
     * @param precision 直方图精度（默认 3 位有效数字）
     */
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

        // 默认最大周期数：10 秒（而非 1 小时，减少内存占用）
        if (highest_cycles == 0) {
            highest_cycles =
                TSC::to_cycles(std::chrono::seconds(10))
                    .value_or(10ULL * 3'400'000'000);  // 回退到 3.4 GHz
        }

        highest_cycles = std::max(highest_cycles, 2 * lowest_cycles);
        histogram_ = HdrHistogram(lowest_cycles, highest_cycles, precision);
    }

    /**
     * @brief 记录单次测量（周期数）
     * @return true 记录成功，false 无效值或超出范围
     */
    bool record(uint64_t cycles) noexcept {
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

    /**
     * @brief 批量记录
     */
    bool record_values(uint64_t cycles, uint64_t count) noexcept {
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
        total_cycles_ += cycles * count;
        min_cycles_ = std::min(min_cycles_, cycles);
        max_cycles_ = std::max(max_cycles_, cycles);

        return true;
    }

    /**
     * @brief 合并另一个 Recorder 的数据
     * @return true 合并成功
     */
    bool merge(const Recorder& other) noexcept {
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

    /**
     * @brief 计算统计数据
     */
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

    /**
     * @brief 打印延迟报告到控制台
     */
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

    /**
     * @brief 导出 JSON 统计数据
     */
    bool export_json(const std::string& output_dir = "outputs") const {
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
                stats->name, get_timestamp(), stats->count, skipped_invalid_,
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

    /**
     * @brief 导出 CSV 分布数据
     */
    bool export_csv(const std::string& output_dir = "outputs") const {
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

    bool export_all(const std::string& output_dir = "outputs") const {
        bool json_ok = export_json(output_dir);
        bool csv_ok = export_csv(output_dir);
        return json_ok && csv_ok;
    }

    // ========== 查询 API ==========

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] uint64_t count() const noexcept { return count_; }
    [[nodiscard]] uint64_t skipped_invalid_count() const noexcept {
        return skipped_invalid_;
    }
    [[nodiscard]] uint64_t skipped_overflow_count() const noexcept {
        return skipped_overflow_;
    }
    [[nodiscard]] bool has_data() const noexcept { return count_ > 0; }
    [[nodiscard]] const HdrHistogram& histogram() const noexcept {
        return histogram_;
    }

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
// ConcurrentRecorder — 多线程性能记录器
// ============================================================================

/**
 * @brief 多线程性能记录器
 *
 * 每个线程通过 thread_local HdrHistogram 记录数据，零竞争。
 * report/compute_stats 时自动 merge 所有线程的数据。
 *
 * 第一次调用 record() 时自动注册当前线程的 histogram。
 * 线程退出时数据自动 merge 到退役缓冲区，不会丢失。
 *
 * 使用 shared_ptr 管理共享状态，确保 thread_local 析构器
 * 在 ConcurrentRecorder 销毁后仍能安全访问共享状态。
 *
 * @note 默认构造范围 1ns-10s（约 ~20KB/线程），3 位有效数字精度
 *
 * @example
 * @code
 * ConcurrentRecorder rec("HttpLatency");
 *
 * // 在任意线程中：
 * rec.record(end - start);
 *
 * // 在主线程中（线程 join 后）：
 * rec.print_report();  // 自动 merge 所有线程数据（含已退出线程）
 * @endcode
 */
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

        // 共享状态：通过 shared_ptr 管理，线程退出时仍可安全访问
        state_ = std::make_shared<SharedState>(
            lowest_cycles, highest_cycles, precision);
    }

    ~ConcurrentRecorder() = default;

    ConcurrentRecorder(const ConcurrentRecorder&) = delete;
    ConcurrentRecorder& operator=(const ConcurrentRecorder&) = delete;
    ConcurrentRecorder(ConcurrentRecorder&&) = delete;
    ConcurrentRecorder& operator=(ConcurrentRecorder&&) = delete;

    /**
     * @brief 记录单次测量（线程安全）
     *
     * 第一次调用时自动为当前线程创建 thread_local histogram 并注册。
     * 后续调用直接写入 thread_local histogram，无竞争。
     */
    bool record(uint64_t cycles) noexcept {
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

    /**
     * @brief 批量记录相同值（线程安全）
     *
     * 等价于调用 record(cycles) count 次，但避免了重复的 min/max 比较。
     * 与 Recorder::record_values() 对齐。
     *
     * @param cycles 测量值（CPU 周期数）
     * @param count  记录次数（必须 > 0）
     * @return true 记录成功，false 值无效或超出范围
     */
    bool record_values(uint64_t cycles, uint64_t count) noexcept {
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
        local->total_cycles += cycles * count;
        local->min_cycles = std::min(local->min_cycles, cycles);
        local->max_cycles = std::max(local->max_cycles, cycles);

        return true;
    }

    /**
     * @brief 计算合并后的统计数据
     *
     * merge 所有活跃线程和已退出线程的数据。
     * 需要获取锁，不适合在热路径中调用。
     */
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

    [[nodiscard]] const std::string& name() const noexcept { return name_; }

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
    bool export_json(const std::string& output_dir = "outputs") const {
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
                stats->name, get_timestamp(),
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
    bool export_csv(const std::string& output_dir = "outputs") const {
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

    /// Export both JSON summary and CSV distribution.
    bool export_all(const std::string& output_dir = "outputs") const {
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
    // 每个线程持有的本地数据
    struct ThreadLocalData {
        HdrHistogram histogram;
        uint64_t count = 0;
        uint64_t total_cycles = 0;
        uint64_t min_cycles = std::numeric_limits<uint64_t>::max();
        uint64_t max_cycles = 0;
        uint64_t skipped_invalid = 0;
        uint64_t skipped_overflow = 0;
    };

    // 共享状态：通过 shared_ptr 管理，确保线程退出时仍可安全访问
    struct SharedState {
        uint64_t lowest_cycles;
        uint64_t highest_cycles;
        int precision;

        mutable std::mutex mutex;

        // 活跃线程的本地数据指针
        std::vector<ThreadLocalData*> active_locals;

        // 已退出线程的累积数据
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

        // 线程退出时调用：merge 数据到退役缓冲区，然后从活跃列表移除
        void retire_local(ThreadLocalData* local) noexcept {
            try {
                std::lock_guard lock(mutex);
                auto it = std::find(
                    active_locals.begin(), active_locals.end(), local);
                if (it != active_locals.end()) {
                    // 将数据 merge 到退役缓冲区 (always compatible — same ctor params)
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
                // 静默忽略，避免在析构路径中抛出
            }
        }

        // 合并结果结构
        struct MergedData {
            HdrHistogram histogram;
            uint64_t count = 0;
            uint64_t total_cycles = 0;
            uint64_t min_cycles = std::numeric_limits<uint64_t>::max();
            uint64_t max_cycles = 0;
        };

        // merge 所有数据（退役 + 活跃）— 调用方持有 mutex
        [[nodiscard]] MergedData merge_all_locked() const {
            MergedData merged;
            merged.histogram =
                HdrHistogram(lowest_cycles, highest_cycles, precision);

            // 先 merge 退役数据 (always compatible — same ctor params)
            (void)merged.histogram.merge(retired_histogram);
            merged.count = retired_count;
            merged.total_cycles = retired_total_cycles;
            merged.min_cycles = retired_min_cycles;
            merged.max_cycles = retired_max_cycles;

            // 再 merge 活跃线程数据 (always compatible — same ctor params)
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

        // 重置所有数据（退役 + 活跃）— 调用方持有 mutex
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

        // merge 所有数据（退役 + 活跃）
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

    // RAII guard：持有 shared_ptr<SharedState>，线程退出时安全 retire
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
            // shared_ptr 确保 SharedState 此时仍然存活
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
        thread_local std::unique_ptr<ThreadLocalGuard> tl_guard;
        thread_local SharedState* tl_state = nullptr;

        // 热路径：已初始化且属于同一个 SharedState，直接返回
        if (tl_state == state_.get()) [[likely]] {
            return &tl_guard->data;
        }

        // 冷路径：创建并注册
        try {
            tl_guard = std::make_unique<ThreadLocalGuard>(
                state_->lowest_cycles, state_->highest_cycles,
                state_->precision, state_);
            tl_state = state_.get();
            return &tl_guard->data;
        } catch (...) {
            return nullptr;
        }
    }
};

}  // namespace eph::utils
