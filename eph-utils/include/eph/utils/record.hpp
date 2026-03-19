#pragma once

#include <sys/resource.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <print>
#include <string>
#include <vector>

#include "eph/utils/time.hpp"

namespace eph::utils {

using eph::utils::TSC;

namespace fs = std::filesystem;

// ============================================================================
// measure_tsc / ScopedTSC — 便捷计时接口
// ============================================================================

/**
 * @brief 测量函数执行时间（便捷接口）
 *
 * 执行给定的可调用对象并返回其 CPU 周期数。
 *
 * @tparam Func 可调用类型（函数、lambda、函数对象等）
 * @tparam Args 参数类型
 * @param func 要测量的函数
 * @param args 传递给函数的参数
 * @return uint64_t 执行消耗的 CPU 周期数
 *
 * @note 不捕获返回值，仅适用于测量副作用函数
 * @note 单次测量可能受噪声影响，建议多次测量取统计值
 */
template <typename Func, typename... Args>
    requires std::invocable<Func, Args...>
[[nodiscard]] inline uint64_t measure_tsc(Func&& func, Args&&... args) {
    const uint64_t start = TSC::now();
    std::invoke(std::forward<Func>(func), std::forward<Args>(args)...);
    const uint64_t end = TSC::now();
    return end - start;
}

/**
 * @brief RAII 风格的作用域计时器
 *
 * 在构造时开始计时，在析构时自动将经过的周期数写入输出变量。
 *
 * @warning 输出变量必须在计时器对象销毁前保持有效
 */
class ScopedTSC {
   public:
    explicit inline ScopedTSC(uint64_t& out_cycles)
        : out_cycles_(out_cycles), start_cycles_(TSC::now()) {}

    inline ~ScopedTSC() { out_cycles_ = TSC::now() - start_cycles_; }

    ScopedTSC(const ScopedTSC&) = delete;
    ScopedTSC& operator=(const ScopedTSC&) = delete;

   private:
    uint64_t& out_cycles_;
    uint64_t start_cycles_;
};

// ============================================================================
// HdrHistogram — 高动态范围直方图
// ============================================================================

/**
 * @brief 高动态范围直方图
 *
 * 使用对数刻度记录延迟分布，在宽范围内保持恒定的相对精度。
 * 基于 Gil Tene 的 HdrHistogram 算法。
 *
 * @note 非线程安全，设计用于单线程环境
 */
class HdrHistogram {
   public:
    HdrHistogram() = default;

    /**
     * @brief 构造直方图
     *
     * @param lowest_trackable_value 最小可追踪值（>= 1）
     * @param highest_trackable_value 最大可追踪值（>= 2 * lowest）
     * @param significant_figures 有效数字位数（1-5，推荐 2-3）
     *
     * @throws std::invalid_argument 参数无效
     */
    explicit HdrHistogram(uint64_t lowest_trackable_value,
                          uint64_t highest_trackable_value,
                          int significant_figures = 3) {
        if (lowest_trackable_value < 1) {
            throw std::invalid_argument("lowest_trackable_value must be >= 1");
        }
        if (significant_figures < 1 || significant_figures > 5) {
            throw std::invalid_argument(
                "significant_figures must be 1-5 (recommended: 2-3)");
        }
        if (highest_trackable_value < 2 * lowest_trackable_value) {
            throw std::invalid_argument(
                "highest_trackable_value must be >= 2 * "
                "lowest_trackable_value");
        }

        lowest_trackable_value_ = lowest_trackable_value;
        highest_trackable_value_ = highest_trackable_value;

        double largest_value_with_single_unit_resolution =
            2.0 * std::pow(10.0, significant_figures);
        sub_bucket_count_magnitude_ = static_cast<int>(
            std::ceil(std::log2(largest_value_with_single_unit_resolution)));
        sub_bucket_half_count_magnitude_ = sub_bucket_count_magnitude_ - 1;

        unit_magnitude_ =
            static_cast<int>(std::floor(std::log2(lowest_trackable_value)));

        sub_bucket_count_ = static_cast<int64_t>(1)
                            << sub_bucket_count_magnitude_;
        sub_bucket_half_count_ = sub_bucket_count_ >> 1;
        sub_bucket_mask_ = (sub_bucket_count_ - 1) << unit_magnitude_;

        establish_size(highest_trackable_value);
        counts_.resize(counts_len_, 0);
    }

    /**
     * @brief 记录单个样本
     * @return true 记录成功，false 值超出范围
     * @note 性能：约 5-10 纳秒/次
     */
    bool record(uint64_t value) noexcept {
        if (value < lowest_trackable_value_ || value > highest_trackable_value_)
            [[unlikely]] {
            return false;
        }

        int32_t idx = counts_index_for(value);
        if (idx < 0 || idx >= counts_len_) [[unlikely]] {
            return false;
        }

        counts_[idx]++;
        total_count_++;
        min_value_ = std::min(min_value_, value);
        max_value_ = std::max(max_value_, value);

        return true;
    }

    /**
     * @brief 批量记录相同值
     */
    bool record_values(uint64_t value, uint64_t count) noexcept {
        if (count == 0) return true;
        if (value < lowest_trackable_value_ || value > highest_trackable_value_)
            [[unlikely]] {
            return false;
        }

        int32_t idx = counts_index_for(value);
        if (idx < 0 || idx >= counts_len_) [[unlikely]] {
            return false;
        }

        counts_[idx] += count;
        total_count_ += count;
        min_value_ = std::min(min_value_, value);
        max_value_ = std::max(max_value_, value);

        return true;
    }

    void reset() noexcept {
        std::fill(counts_.begin(), counts_.end(), 0);
        total_count_ = 0;
        min_value_ = std::numeric_limits<uint64_t>::max();
        max_value_ = 0;
    }

    // ========== 查询 API ==========

    [[nodiscard]] uint64_t get_value_at_percentile(
        double percentile) const noexcept {
        if (percentile < 0.0 || percentile > 100.0 || total_count_ == 0)
            [[unlikely]] {
            return 0;
        }

        double fractional_count = (percentile / 100.0) * total_count_;
        uint64_t count_at_percentile =
            static_cast<uint64_t>(std::ceil(fractional_count));

        uint64_t accumulated = 0;
        for (int32_t i = 0; i < counts_len_; ++i) {
            uint64_t count = counts_[i];
            if (count == 0) continue;

            accumulated += count;
            if (accumulated >= count_at_percentile) {
                uint64_t low = value_from_index(i);
                uint64_t high = next_non_equivalent_value(low);
                return low + (high - low) / 2;
            }
        }

        return max_value_;
    }

    /**
     * @brief 批量获取多个百分位（单次遍历）
     */
    [[nodiscard]] std::vector<uint64_t> get_percentiles(
        const std::vector<double>& percentiles) const {
        std::vector<uint64_t> results(percentiles.size(), 0);

        if (total_count_ == 0 || percentiles.empty()) return results;

        std::vector<size_t> sorted_indices(percentiles.size());
        std::iota(sorted_indices.begin(), sorted_indices.end(), 0);
        std::sort(sorted_indices.begin(), sorted_indices.end(),
                  [&](size_t a, size_t b) {
                      return percentiles[a] < percentiles[b];
                  });

        uint64_t accumulated = 0;
        size_t next_idx = 0;

        for (int32_t i = 0; i < counts_len_ && next_idx < sorted_indices.size();
             ++i) {
            uint64_t count = counts_[i];
            if (count == 0) continue;

            accumulated += count;

            while (next_idx < sorted_indices.size()) {
                size_t original_idx = sorted_indices[next_idx];
                double p = percentiles[original_idx];

                if (p < 0.0 || p > 100.0) {
                    next_idx++;
                    continue;
                }

                double fractional_count = (p / 100.0) * total_count_;
                uint64_t count_at_p =
                    static_cast<uint64_t>(std::ceil(fractional_count));

                if (accumulated >= count_at_p) {
                    uint64_t low = value_from_index(i);
                    uint64_t high = next_non_equivalent_value(low);
                    results[original_idx] = low + (high - low) / 2;
                    next_idx++;
                } else {
                    break;
                }
            }
        }

        return results;
    }

    [[nodiscard]] uint64_t get_total_count() const noexcept {
        return total_count_;
    }

    [[nodiscard]] uint64_t get_min_value() const noexcept {
        return min_value_ == std::numeric_limits<uint64_t>::max() ? 0
                                                                  : min_value_;
    }

    [[nodiscard]] uint64_t get_max_value() const noexcept { return max_value_; }

    [[nodiscard]] double get_mean() const noexcept {
        if (total_count_ == 0) return 0.0;

        uint64_t sum = 0;
        for (int32_t i = 0; i < counts_len_; ++i) {
            uint64_t count = counts_[i];
            if (count > 0) {
                uint64_t low = value_from_index(i);
                uint64_t high = next_non_equivalent_value(low);
                uint64_t mid = low + (high - low) / 2;
                sum += mid * count;
            }
        }

        return static_cast<double>(sum) / static_cast<double>(total_count_);
    }

    [[nodiscard]] double get_std_deviation() const noexcept {
        if (total_count_ == 0) return 0.0;

        double mean = get_mean();
        double sum_squared_diff = 0.0;

        for (int32_t i = 0; i < counts_len_; ++i) {
            uint64_t count = counts_[i];
            if (count > 0) {
                uint64_t low = value_from_index(i);
                uint64_t high = next_non_equivalent_value(low);
                uint64_t mid = low + (high - low) / 2;
                double diff = static_cast<double>(mid) - mean;
                sum_squared_diff += diff * diff * count;
            }
        }

        return std::sqrt(sum_squared_diff / static_cast<double>(total_count_));
    }

    template <typename Func>
    void for_each_recorded_value(Func func) const {
        for (int32_t i = 0; i < counts_len_; ++i) {
            uint64_t count = counts_[i];
            if (count > 0) {
                func(value_from_index(i), count);
            }
        }
    }

    /**
     * @brief 合并另一个直方图
     * @return true 合并成功，false 配置不兼容
     */
    bool merge(const HdrHistogram& other) noexcept {
        if (lowest_trackable_value_ != other.lowest_trackable_value_ ||
            highest_trackable_value_ != other.highest_trackable_value_ ||
            unit_magnitude_ != other.unit_magnitude_ ||
            sub_bucket_count_ != other.sub_bucket_count_) {
            return false;
        }

        for (int32_t i = 0; i < counts_len_; ++i) {
            counts_[i] += other.counts_[i];
        }

        total_count_ += other.total_count_;
        min_value_ = std::min(min_value_, other.min_value_);
        max_value_ = std::max(max_value_, other.max_value_);

        return true;
    }

    [[nodiscard]] size_t get_memory_size() const noexcept {
        return sizeof(*this) + counts_.capacity() * sizeof(uint64_t);
    }

    [[nodiscard]] bool is_compatible(const HdrHistogram& other) const noexcept {
        return lowest_trackable_value_ == other.lowest_trackable_value_ &&
               highest_trackable_value_ == other.highest_trackable_value_ &&
               unit_magnitude_ == other.unit_magnitude_ &&
               sub_bucket_count_ == other.sub_bucket_count_;
    }

   private:
    uint64_t lowest_trackable_value_ = 1;
    uint64_t highest_trackable_value_ = 2;
    int unit_magnitude_ = 0;
    int sub_bucket_count_magnitude_ = 0;
    int sub_bucket_half_count_magnitude_ = 0;
    int64_t sub_bucket_count_ = 0;
    int64_t sub_bucket_half_count_ = 0;
    int64_t sub_bucket_mask_ = 0;
    int32_t bucket_count_ = 0;
    int32_t counts_len_ = 0;

    std::vector<uint64_t> counts_;
    uint64_t total_count_ = 0;
    uint64_t min_value_ = std::numeric_limits<uint64_t>::max();
    uint64_t max_value_ = 0;

    void establish_size(uint64_t max_value) {
        int32_t buckets_needed = get_buckets_needed_to_cover_value(max_value);
        bucket_count_ = buckets_needed;
        counts_len_ = ((buckets_needed + 1) * (sub_bucket_count_ / 2));
    }

    [[nodiscard]] int32_t get_buckets_needed_to_cover_value(
        uint64_t value) const noexcept {
        int64_t smallest_untrackable_value = sub_bucket_count_
                                             << unit_magnitude_;
        int32_t buckets_needed = 1;

        while (smallest_untrackable_value <= static_cast<int64_t>(value)) {
            if (smallest_untrackable_value >
                (std::numeric_limits<int64_t>::max() / 2)) {
                return buckets_needed + 1;
            }
            smallest_untrackable_value <<= 1;
            buckets_needed++;
        }
        return buckets_needed;
    }

    [[nodiscard]] int32_t counts_index_for(uint64_t value) const noexcept {
        int32_t bucket_idx = get_bucket_index(value);
        int32_t sub_bucket_idx = get_sub_bucket_idx(value, bucket_idx);
        return counts_index(bucket_idx, sub_bucket_idx);
    }

    [[nodiscard]] int32_t get_bucket_index(uint64_t value) const noexcept {
        int pow2ceiling = std::bit_width(value | sub_bucket_mask_);
        return pow2ceiling - unit_magnitude_ -
               (sub_bucket_half_count_magnitude_ + 1);
    }

    [[nodiscard]] int32_t get_sub_bucket_idx(
        uint64_t value, int32_t bucket_idx) const noexcept {
        return static_cast<int32_t>(value >> (bucket_idx + unit_magnitude_));
    }

    [[nodiscard]] int32_t counts_index(int32_t bucket_idx,
                                       int32_t sub_bucket_idx) const noexcept {
        int32_t bucket_base_idx = (bucket_idx + 1)
                                  << sub_bucket_half_count_magnitude_;
        int32_t offset_in_bucket = sub_bucket_idx - sub_bucket_half_count_;
        return bucket_base_idx + offset_in_bucket;
    }

    [[nodiscard]] uint64_t value_from_index(int32_t index) const noexcept {
        int32_t bucket_idx = (index >> sub_bucket_half_count_magnitude_) - 1;
        int32_t sub_bucket_idx =
            (index & (sub_bucket_half_count_ - 1)) + sub_bucket_half_count_;

        if (bucket_idx < 0) {
            sub_bucket_idx -= sub_bucket_half_count_;
            bucket_idx = 0;
        }

        return static_cast<uint64_t>(sub_bucket_idx)
               << (bucket_idx + unit_magnitude_);
    }

    [[nodiscard]] uint64_t next_non_equivalent_value(
        uint64_t value) const noexcept {
        return lowest_equivalent_value(value) +
               size_of_equivalent_value_range(value);
    }

    [[nodiscard]] uint64_t lowest_equivalent_value(
        uint64_t value) const noexcept {
        int32_t bucket_idx = get_bucket_index(value);
        int32_t sub_bucket_idx = get_sub_bucket_idx(value, bucket_idx);
        return static_cast<uint64_t>(sub_bucket_idx)
               << (bucket_idx + unit_magnitude_);
    }

    [[nodiscard]] uint64_t size_of_equivalent_value_range(
        uint64_t value) const noexcept {
        int32_t bucket_idx = get_bucket_index(value);
        return static_cast<uint64_t>(1) << (unit_magnitude_ + bucket_idx);
    }
};

// ============================================================================
// Stats — 延迟统计数据
// ============================================================================

/**
 * @brief 延迟统计数据（不含系统资源）
 */
struct Stats {
    std::string name;
    uint64_t count;
    double avg_ns;
    double min_ns;
    double max_ns;
    double p50_ns;
    double p90_ns;
    double p99_ns;
    double p999_ns;
    double stddev_ns;
};

// ============================================================================
// SystemStats — 独立的系统资源采集（RAII）
// ============================================================================

/**
 * @brief 系统资源统计数据
 */
struct SystemResourceStats {
    long majflt;         // Major Page Faults（需要磁盘 I/O 的缺页）
    long minflt;         // Minor Page Faults（无需磁盘 I/O 的缺页）
    long nvcsw;          // Voluntary Context Switches（主动让出 CPU）
    long nivcsw;         // Involuntary Context Switches（被抢占）
    double user_cpu_s;   // User CPU time (seconds)
    double sys_cpu_s;    // System CPU time (seconds)
    double total_cpu_s;  // Total CPU time (seconds)
};

/**
 * @brief 独立的系统资源采集器（RAII）
 *
 * 构造时快照 getrusage，snapshot() 时计算差值。
 * 与 Recorder 完全无耦合，用户自行决定使用粒度。
 *
 * @example
 * @code
 * SystemStats sys_stats;
 * // ... 执行被测代码 ...
 * auto resource = sys_stats.snapshot();
 * std::println("User CPU: {:.4f}s", resource.user_cpu_s);
 * @endcode
 */
class SystemStats {
   public:
    /**
     * @brief 构造并记录初始资源状态
     * @param auto_print 析构时是否自动打印报告
     */
    explicit SystemStats(bool auto_print = false)
        : auto_print_(auto_print) {
        getrusage(RUSAGE_SELF, &initial_rusage_);
    }

    ~SystemStats() {
        if (auto_print_) {
            print_report();
        }
    }

    SystemStats(const SystemStats&) = delete;
    SystemStats& operator=(const SystemStats&) = delete;
    SystemStats(SystemStats&&) = default;
    SystemStats& operator=(SystemStats&&) = default;

    /**
     * @brief 获取从构造/重置以来的资源消耗差值
     */
    [[nodiscard]] SystemResourceStats snapshot() const noexcept {
        rusage current{};
        getrusage(RUSAGE_SELF, &current);
        return compute_delta(current);
    }

    /**
     * @brief 重置基准线
     */
    void reset() noexcept {
        getrusage(RUSAGE_SELF, &initial_rusage_);
    }

    /**
     * @brief 打印系统资源报告
     */
    void print_report() const {
        auto s = snapshot();

        constexpr int w_label = 30;
        constexpr int w_val = 12;
        constexpr int total_w = w_label + (w_val * 6) + 18;

        std::println("{:-^{}}", " System Resources ", total_w);
        std::println(
            "{:<{}} | {:>{}} | {:>{}} | {:>{}} | {:>{}} | {:>{}} | {:>{}}",
            "CPU Time", w_label, "User(s)", w_val, "Sys(s)", w_val,
            "MajFault", w_val, "MinFault", w_val, "VolCtx", w_val,
            "InvCtx", w_val);
        std::println("{:-^{}}", "", total_w);
        std::println(
            "{:<{}} | {:>{}.4f} | {:>{}.4f} | {:>{}} | {:>{}} | {:>{}} | "
            "{:>{}}",
            "Usage", w_label, s.user_cpu_s, w_val, s.sys_cpu_s, w_val,
            s.majflt, w_val, s.minflt, w_val, s.nvcsw, w_val,
            s.nivcsw, w_val);
        std::println("{:-^{}}\n", "", total_w);
    }

   private:
    rusage initial_rusage_{};
    bool auto_print_;

    [[nodiscard]] SystemResourceStats compute_delta(
        const rusage& current) const noexcept {
        auto time_diff = [](const timeval& t1, const timeval& t2) {
            return (t2.tv_sec - t1.tv_sec) + (t2.tv_usec - t1.tv_usec) / 1e6;
        };

        double utime_s =
            time_diff(initial_rusage_.ru_utime, current.ru_utime);
        double stime_s =
            time_diff(initial_rusage_.ru_stime, current.ru_stime);

        return SystemResourceStats{
            .majflt = current.ru_majflt - initial_rusage_.ru_majflt,
            .minflt = current.ru_minflt - initial_rusage_.ru_minflt,
            .nvcsw = current.ru_nvcsw - initial_rusage_.ru_nvcsw,
            .nivcsw = current.ru_nivcsw - initial_rusage_.ru_nivcsw,
            .user_cpu_s = utime_s,
            .sys_cpu_s = stime_s,
            .total_cpu_s = utime_s + stime_s,
        };
    }
};

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
        auto now = std::chrono::system_clock::now();
        return std::format("{:%Y-%m-%d_%H-%M-%S}",
                           std::chrono::floor<std::chrono::seconds>(now));
    }

    [[nodiscard]] static std::string sanitize_filename(std::string name) {
        std::replace_if(
            name.begin(), name.end(),
            [](char c) { return !(std::isalnum(c) || c == '_' || c == '-'); },
            '_');
        return name;
    }

    [[nodiscard]] fs::path make_output_path(const std::string& dir,
                                            const std::string& ext) const {
        return fs::path(dir) /
               (sanitize_filename(name_) + "_" + get_timestamp() + ext);
    }

    [[nodiscard]] static bool ensure_directory(
        const std::string& path) noexcept {
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
                    // 将数据 merge 到退役缓冲区
                    retired_histogram.merge(local->histogram);
                    retired_count += local->count;
                    retired_total_cycles += local->total_cycles;
                    retired_min_cycles =
                        std::min(retired_min_cycles, local->min_cycles);
                    retired_max_cycles =
                        std::max(retired_max_cycles, local->max_cycles);
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

        // merge 所有数据（退役 + 活跃）
        [[nodiscard]] MergedData merge_all() const {
            std::lock_guard lock(mutex);

            MergedData merged;
            merged.histogram =
                HdrHistogram(lowest_cycles, highest_cycles, precision);

            // 先 merge 退役数据
            merged.histogram.merge(retired_histogram);
            merged.count = retired_count;
            merged.total_cycles = retired_total_cycles;
            merged.min_cycles = retired_min_cycles;
            merged.max_cycles = retired_max_cycles;

            // 再 merge 活跃线程数据
            for (const auto* local : active_locals) {
                merged.histogram.merge(local->histogram);
                merged.count += local->count;
                merged.total_cycles += local->total_cycles;
                merged.min_cycles =
                    std::min(merged.min_cycles, local->min_cycles);
                merged.max_cycles =
                    std::max(merged.max_cycles, local->max_cycles);
            }

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
