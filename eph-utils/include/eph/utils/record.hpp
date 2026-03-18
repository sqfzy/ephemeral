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
#include <numeric>
#include <print>
#include <string>
#include <vector>

#include "eph/utils/time.hpp"

namespace eph::utils {

using eph::utils::TSC;

namespace fs = std::filesystem;

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
 *
 * @example
 * @code
 * auto cycles = measure([&]{ vec.push_back(42); });
 * auto cycles2 = measure(std::sort<decltype(vec.begin())>, vec.begin(),
 * vec.end());
 * @endcode
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
 * 适合测量代码块的执行时间。
 *
 * @example
 * @code
 * uint64_t latency;
 * {
 * ScopedTSC timer(latency);
 * // 被测量的代码块
 * process_data();
 * }  // 析构时自动记录到 latency
 * std::println("Latency: {} cycles", latency);
 * @endcode
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

/**
 * @brief 高动态范围直方图
 *
 * 使用对数刻度记录延迟分布，在宽范围内保持恒定的相对精度。
 *
 * @note 非线程安全，设计用于单线程环境
 * @note 基于 Gil Tene 的 HdrHistogram 算法
 *
 * @example
 * @code
 * HdrHistogram hist(1, 3600ULL * 1e9, 3);  // 1ns 到 1 小时
 *
 * hist.record(1500);   // 记录 1.5μs
 * hist.record(25000);  // 记录 25μs
 *
 * uint64_t p99 = hist.get_value_at_percentile(99.0);
 * auto percentiles = hist.get_percentiles({50, 90, 99, 99.9});
 * @endcode
 */
class HdrHistogram {
   public:
    /**
     * @brief 默认构造函数（用于延迟初始化）
     */
    HdrHistogram() = default;

    /**
     * @brief 构造直方图
     *
     * @param lowest_trackable_value 最小可追踪值（>= 1）
     * @param highest_trackable_value 最大可追踪值（>= 2 * lowest）
     * @param significant_figures 有效数字位数（1-5，推荐 2-3）
     *
     * @throws std::invalid_argument 参数无效
     *
     * @example
     * @code
     * // 追踪 1 到 1 小时的纳秒延迟，误差 < 0.1%
     * HdrHistogram hist(1, 3600ULL * 1e9, 3);
     *
     * // 追踪 100 到 1 秒的纳秒延迟，误差 < 1%
     * HdrHistogram hist(100, 1e9, 2);
     * @endcode
     */
    explicit HdrHistogram(uint64_t lowest_trackable_value,
                          uint64_t highest_trackable_value,
                          int significant_figures = 3) {
        // === 参数校验 ===
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

        // === 计算桶结构 ===
        // 有效数字 -> 子桶数量（向上取 2 的幂）
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

        // 计算需要的桶总数
        establish_size(highest_trackable_value);

        // 预分配内存
        counts_.resize(counts_len_, 0);
    }

    /**
     * @brief 记录单个样本
     *
     * @param value 测量值（例如延迟的纳秒数或 CPU 周期数）
     * @return true 记录成功
     * @return false 值超出范围
     *
     * @note 性能：约 5-10 纳秒/次
     * @note 拒绝超出 [lowest, highest] 范围的值
     */
    bool record(uint64_t value) noexcept {
        if (value < lowest_trackable_value_ || value > highest_trackable_value_)
            [[unlikely]] {
            return false;
        }

        int32_t idx = counts_index_for(value);
        if (idx < 0 || idx >= counts_len_) [[unlikely]] {
            return false;  // 索引溢出保护
        }

        counts_[idx]++;
        total_count_++;

        // 更新最小/最大值
        min_value_ = std::min(min_value_, value);
        max_value_ = std::max(max_value_, value);

        return true;
    }

    /**
     * @brief 批量记录相同值
     *
     * @param value 测量值
     * @param count 样本数量
     * @return true 记录成功
     *
     * @note 比多次调用 record() 更高效
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

    /**
     * @brief 清空所有记录
     */
    void reset() noexcept {
        std::fill(counts_.begin(), counts_.end(), 0);
        total_count_ = 0;
        min_value_ = std::numeric_limits<uint64_t>::max();
        max_value_ = 0;
    }

    // ========== 查询 API ==========

    /**
     * @brief 获取指定百分位的值
     *
     * @param percentile 百分位（0.0 - 100.0）
     * @return uint64_t 该百分位对应的值（桶的中间值）
     *
     * @note 返回 0 表示无数据或百分位无效
     * @note 使用桶的中间值（而非最小值）以提高精度
     *
     * @example
     * @code
     * uint64_t p50 = hist.get_value_at_percentile(50.0);   // 中位数
     * uint64_t p99 = hist.get_value_at_percentile(99.0);   // P99
     * uint64_t p999 = hist.get_value_at_percentile(99.9);  // P99.9
     * @endcode
     */
    [[nodiscard]] uint64_t get_value_at_percentile(
        double percentile) const noexcept {
        if (percentile < 0.0 || percentile > 100.0 || total_count_ == 0)
            [[unlikely]] {
            return 0;
        }

        // 计算目标累计计数
        double fractional_count = (percentile / 100.0) * total_count_;
        uint64_t count_at_percentile =
            static_cast<uint64_t>(std::ceil(fractional_count));

        // 遍历桶找到目标百分位
        uint64_t accumulated = 0;
        for (int32_t i = 0; i < counts_len_; ++i) {
            uint64_t count = counts_[i];
            if (count == 0) continue;

            accumulated += count;
            if (accumulated >= count_at_percentile) {
                // 返回桶的中间值
                uint64_t low = value_from_index(i);
                uint64_t high = next_non_equivalent_value(low);
                return low + (high - low) / 2;
            }
        }

        return max_value_;
    }

    /**
     * @brief 批量获取多个百分位
     *
     * @param percentiles 百分位列表（例如 {50.0, 90.0, 99.0, 99.9}）
     * @return std::vector<uint64_t> 对应的值
     *
     * @note 单次遍历完成，比多次调用 get_value_at_percentile() 快 5-10 倍
     *
     * @example
     * @code
     * auto ps = hist.get_percentiles({50, 90, 99, 99.9, 99.99});
     * std::println("P50={}, P90={}, P99={}", ps[0], ps[1], ps[2]);
     * @endcode
     */
    [[nodiscard]] std::vector<uint64_t> get_percentiles(
        const std::vector<double>& percentiles) const {
        std::vector<uint64_t> results(percentiles.size(), 0);

        if (total_count_ == 0 || percentiles.empty()) return results;

        // 构建排序的索引
        std::vector<size_t> sorted_indices(percentiles.size());
        std::iota(sorted_indices.begin(), sorted_indices.end(), 0);
        std::sort(sorted_indices.begin(), sorted_indices.end(),
                  [&](size_t a, size_t b) {
                      return percentiles[a] < percentiles[b];
                  });

        // 单次遍历完成所有查询
        uint64_t accumulated = 0;
        size_t next_idx = 0;

        for (int32_t i = 0; i < counts_len_ && next_idx < sorted_indices.size();
             ++i) {
            uint64_t count = counts_[i];
            if (count == 0) continue;

            accumulated += count;

            // 检查是否满足当前百分位
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

    /**
     * @brief 获取总样本数
     */
    [[nodiscard]] uint64_t get_total_count() const noexcept {
        return total_count_;
    }

    /**
     * @brief 获取最小值
     */
    [[nodiscard]] uint64_t get_min_value() const noexcept {
        return min_value_ == std::numeric_limits<uint64_t>::max() ? 0
                                                                  : min_value_;
    }

    /**
     * @brief 获取最大值
     */
    [[nodiscard]] uint64_t get_max_value() const noexcept { return max_value_; }

    /**
     * @brief 计算平均值
     *
     * @note 使用桶的中间值估计，精度受 significant_figures 限制
     */
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

    /**
     * @brief 计算标准差
     */
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

    /**
     * @brief 遍历所有记录的值
     *
     * @tparam Func 回调函数类型：void(uint64_t value, uint64_t count)
     * @param func 回调函数
     *
     * @example
     * @code
     * hist.for_each_recorded_value([](uint64_t val, uint64_t cnt) {
     * std::println("{}: {}", val, cnt);
     * });
     * @endcode
     */
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
     *
     * @param other 另一个直方图
     * @return true 合并成功
     * @return false 配置不兼容
     *
     * @note 用于跨线程或跨时间窗口聚合
     */
    bool merge(const HdrHistogram& other) noexcept {
        // 检查配置兼容性
        if (lowest_trackable_value_ != other.lowest_trackable_value_ ||
            highest_trackable_value_ != other.highest_trackable_value_ ||
            unit_magnitude_ != other.unit_magnitude_ ||
            sub_bucket_count_ != other.sub_bucket_count_) {
            return false;
        }

        // 合并计数
        for (int32_t i = 0; i < counts_len_; ++i) {
            counts_[i] += other.counts_[i];
        }

        total_count_ += other.total_count_;
        min_value_ = std::min(min_value_, other.min_value_);
        max_value_ = std::max(max_value_, other.max_value_);

        return true;
    }

    /**
     * @brief 获取内存占用（字节）
     */
    [[nodiscard]] size_t get_memory_size() const noexcept {
        return sizeof(*this) + counts_.capacity() * sizeof(uint64_t);
    }

    /**
     * @brief 检查是否兼容（用于 merge）
     */
    [[nodiscard]] bool is_compatible(const HdrHistogram& other) const noexcept {
        return lowest_trackable_value_ == other.lowest_trackable_value_ &&
               highest_trackable_value_ == other.highest_trackable_value_ &&
               unit_magnitude_ == other.unit_magnitude_ &&
               sub_bucket_count_ == other.sub_bucket_count_;
    }

   private:
    // ========== 配置参数 ==========
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

    // ========== 数据存储 ==========
    std::vector<uint64_t> counts_;
    uint64_t total_count_ = 0;
    uint64_t min_value_ = std::numeric_limits<uint64_t>::max();
    uint64_t max_value_ = 0;

    // ========== 内部方法 ==========

    /**
     * @brief 计算需要的桶数量
     */
    void establish_size(uint64_t max_value) {
        int32_t buckets_needed = get_buckets_needed_to_cover_value(max_value);
        bucket_count_ = buckets_needed;
        counts_len_ = ((buckets_needed + 1) * (sub_bucket_count_ / 2));
    }

    /**
     * @brief 计算覆盖指定值需要多少个桶
     */
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

    /**
     * @brief 计算值对应的数组索引（核心算法）
     */
    [[nodiscard]] int32_t counts_index_for(uint64_t value) const noexcept {
        int32_t bucket_idx = get_bucket_index(value);
        int32_t sub_bucket_idx = get_sub_bucket_idx(value, bucket_idx);
        return counts_index(bucket_idx, sub_bucket_idx);
    }

    /**
     * @brief 获取桶索引（基于值的幅度）
     */
    [[nodiscard]] int32_t get_bucket_index(uint64_t value) const noexcept {
        int pow2ceiling = std::bit_width(value | sub_bucket_mask_);
        return pow2ceiling - unit_magnitude_ -
               (sub_bucket_half_count_magnitude_ + 1);
    }

    /**
     * @brief 获取子桶索引
     */
    [[nodiscard]] int32_t get_sub_bucket_idx(
        uint64_t value, int32_t bucket_idx) const noexcept {
        return static_cast<int32_t>(value >> (bucket_idx + unit_magnitude_));
    }

    /**
     * @brief 组合桶索引和子桶索引
     */
    [[nodiscard]] int32_t counts_index(int32_t bucket_idx,
                                       int32_t sub_bucket_idx) const noexcept {
        int32_t bucket_base_idx = (bucket_idx + 1)
                                  << sub_bucket_half_count_magnitude_;
        int32_t offset_in_bucket = sub_bucket_idx - sub_bucket_half_count_;
        return bucket_base_idx + offset_in_bucket;
    }

    /**
     * @brief 从索引还原值（桶的最小值）
     */
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

    /**
     * @brief 获取下一个不等价的值（桶的上界）
     */
    [[nodiscard]] uint64_t next_non_equivalent_value(
        uint64_t value) const noexcept {
        return lowest_equivalent_value(value) +
               size_of_equivalent_value_range(value);
    }

    /**
     * @brief 获取等价值范围的最小值
     */
    [[nodiscard]] uint64_t lowest_equivalent_value(
        uint64_t value) const noexcept {
        int32_t bucket_idx = get_bucket_index(value);
        int32_t sub_bucket_idx = get_sub_bucket_idx(value, bucket_idx);
        return static_cast<uint64_t>(sub_bucket_idx)
               << (bucket_idx + unit_magnitude_);
    }

    /**
     * @brief 获取等价值范围的大小
     */
    [[nodiscard]] uint64_t size_of_equivalent_value_range(
        uint64_t value) const noexcept {
        int32_t bucket_idx = get_bucket_index(value);
        return static_cast<uint64_t>(1) << (unit_magnitude_ + bucket_idx);
    }
};

/**
 * @brief 性能统计数据结构
 *
 * 包含延迟统计和系统资源使用信息。
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

    long majflt;         // Major Page Faults（需要磁盘 I/O 的缺页）
    long minflt;         // Minor Page Faults（无需磁盘 I/O 的缺页）
    long nvcsw;          // Voluntary Context Switches（主动让出 CPU）
    long nivcsw;         // Involuntary Context Switches（被抢占）
    double user_cpu_s;   // User CPU time (seconds)
    double sys_cpu_s;    // System CPU time (seconds)
    double total_cpu_s;  // Total CPU time (seconds)
};

/**
 * @brief 性能数据记录器
 *
 * @example
 * @code
 * Recorder rec("VectorPushBack");
 *
 * for (int i = 0; i < 100000; ++i) {
 * uint64_t start = TSC::now();
 * vec.push_back(i);
 * uint64_t end = TSC::now();
 * rec.record(end - start);
 * }
 *
 * rec.print_report();
 * rec.export_all();
 * @endcode
 */
class Recorder {
   public:
    /**
     * @brief 构造记录器
     *
     * @param name 基准测试名称
     * @param lowest_cycles 最小可追踪周期数（默认 1）
     * @param highest_cycles 最大可追踪周期数（默认 1 小时对应的周期）
     * @param precision 直方图精度（默认 3 位有效数字）
     *
     * @throws std::runtime_error TSC 初始化失败
     * @throws std::invalid_argument 参数无效
     */
    explicit Recorder(std::string name, uint64_t lowest_cycles = 1,
                      uint64_t highest_cycles = 0, int precision = 3)
        : name_(std::move(name)) {
        if (name_.empty()) {
            throw std::invalid_argument("Recorder name cannot be empty");
        }

        // 自动初始化 TSC
        if (!TSC::is_initialized()) {
            if (!TSC::init()) {
                throw std::runtime_error("TSC initialization failed");
            }
        }

        // 计算默认的最大周期数（1 小时）
        if (highest_cycles == 0) {
            highest_cycles =
                TSC::to_cycles(std::chrono::hours(1))
                    .value_or(3600ULL * 3'400'000'000);  // 回退到 3.4 GHz
        }

        highest_cycles = std::max(highest_cycles, 2 * lowest_cycles);

        // 初始化直方图
        histogram_ = HdrHistogram(lowest_cycles, highest_cycles, precision);

        // 记录初始资源状态
        start_resource_tracking();
    }

    /**
     * @brief 记录单次测量（周期数）
     *
     * @param cycles CPU 周期数
     * @return true 记录成功
     * @return false 记录失败（无效值或超出范围）
     *
     * @note 自动拒绝：0、负数、NaN、Inf、超出直方图范围
     */
    bool record(uint64_t cycles) noexcept {
        // 基础校验
        if (cycles == 0) [[unlikely]] {
            skipped_invalid_++;
            return false;
        }

        // 记录到直方图
        if (!histogram_.record(cycles)) [[unlikely]] {
            skipped_overflow_++;
            return false;
        }

        // 更新统计
        count_++;
        total_cycles_ += cycles;
        min_cycles_ = std::min(min_cycles_, cycles);
        max_cycles_ = std::max(max_cycles_, cycles);

        return true;
    }

    /**
     * @brief 批量记录
     *
     * @param cycles 周期数
     * @param count 样本数量
     * @return true 记录成功
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
     * @brief 计算统计数据
     *
     * @return std::optional<Stats> 成功时返回统计信息
     * @note 纯 const 操作，内部动态获取当前的系统资源并计算增量
     */
    [[nodiscard]] std::optional<Stats> compute_stats() const noexcept {
        if (count_ == 0) return std::nullopt;

        auto ns_per_cycle = TSC::to_ns(1);
        if (!ns_per_cycle) [[unlikely]]
            return std::nullopt;

        double ratio = *ns_per_cycle;
        double avg_cyc = static_cast<double>(total_cycles_) / count_;

        // 批量查询百分位（性能优化）
        auto percentiles = histogram_.get_percentiles({50.0, 90.0, 99.0, 99.9});

        // 动态计算从构造/重置到现在的系统资源消耗
        rusage current_rusage{};
        // Linux 环境下支持 RUSAGE_THREAD 用于仅统计当前线程的开销，
        // 这里保持兼容标准 POSIX，使用 RUSAGE_SELF（进程级）。
        getrusage(RUSAGE_SELF, &current_rusage);

        auto time_diff = [](const timeval& t1, const timeval& t2) {
            return (t2.tv_sec - t1.tv_sec) + (t2.tv_usec - t1.tv_usec) / 1e6;
        };

        double utime_s =
            time_diff(initial_rusage_.ru_utime, current_rusage.ru_utime);
        double stime_s =
            time_diff(initial_rusage_.ru_stime, current_rusage.ru_stime);

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
            .majflt = current_rusage.ru_majflt - initial_rusage_.ru_majflt,
            .minflt = current_rusage.ru_minflt - initial_rusage_.ru_minflt,
            .nvcsw = current_rusage.ru_nvcsw - initial_rusage_.ru_nvcsw,
            .nivcsw = current_rusage.ru_nivcsw - initial_rusage_.ru_nivcsw,
            .user_cpu_s = utime_s,
            .sys_cpu_s = stime_s,
            .total_cpu_s = utime_s + stime_s,
        };
    }

    /**
     * @brief 打印报告到控制台
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

        // 定义列宽
        constexpr int w_name = 30;
        constexpr int w_metric = 12;
        constexpr int total_w = w_name + (w_metric * 6) + 18;

        std::println("\n{:-^{}}", title, total_w);

        // --- Section 1: Latency (时间延迟) ---
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

        // --- Section 2: System Resources (系统资源) ---
        std::println("{:-^{}}", " System Resources ", total_w);

        // 定义资源部分的列名
        std::println(
            "{:<{}} | {:>{}} | {:>{}} | {:>{}} | {:>{}} | {:>{}} | {:>{}}",
            "CPU Time", w_name, "User(s)", w_metric, "Sys(s)", w_metric,
            "MajFault", w_metric, "MinFault", w_metric, "VolCtx", w_metric,
            "InvCtx", w_metric);

        std::println("{:-^{}}", "", total_w);

        std::println(
            "{:<{}} | {:>{}.4f} | {:>{}.4f} | {:>{}} | {:>{}} | {:>{}} | "
            "{:>{}}",
            "Usage", w_name, stats->user_cpu_s, w_metric, stats->sys_cpu_s,
            w_metric, stats->majflt, w_metric, stats->minflt, w_metric,
            stats->nvcsw, w_metric, stats->nivcsw, w_metric);

        std::println("{:-^{}}\n", "", total_w);
        print_warnings();
    }

    /**
     * @brief 导出 JSON 统计数据
     *
     * @param output_dir 输出目录（默认 "outputs"）
     * @return true 导出成功
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
  "resources": {{
    "user_cpu_s": {:.6f},
    "sys_cpu_s": {:.6f},
    "total_cpu_s": {:.6f},
    "major_faults": {},
    "minor_faults": {},
    "voluntary_switches": {},
    "involuntary_switches": {}
  }},
  "histogram_memory_bytes": {}
}})",
                stats->name, get_timestamp(), stats->count, skipped_invalid_,
                skipped_overflow_, stats->avg_ns, stats->min_ns, stats->max_ns,
                stats->stddev_ns, stats->p50_ns, stats->p90_ns, stats->p99_ns,
                stats->p999_ns, stats->user_cpu_s, stats->sys_cpu_s,
                stats->total_cpu_s, stats->majflt, stats->minflt, stats->nvcsw,
                stats->nivcsw, histogram_.get_memory_size());

            if (!file.good()) {
                std::println(stderr, "Write error: {}", path.string());
                return false;
            }

            std::println("  ✓ JSON: {}", path.string());
            return true;

        } catch (const std::exception& e) {
            std::println(stderr, "JSON error: {}", e.what());
            return false;
        }
    }

    /**
     * @brief 导出 CSV 分布数据
     *
     * @param output_dir 输出目录（默认 "outputs"）
     * @return true 导出成功
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

            std::println("  ✓ CSV: {}", path.string());
            return true;

        } catch (const std::exception& e) {
            std::println(stderr, "CSV error: {}", e.what());
            return false;
        }
    }

    /**
     * @brief 导出所有数据（JSON + CSV）
     *
     * @param output_dir 输出目录
     * @return true 全部导出成功
     */
    bool export_all(const std::string& output_dir = "outputs") const {
        bool json_ok = export_json(output_dir);
        bool csv_ok = export_csv(output_dir);
        return json_ok && csv_ok;
    }

    // ========== 查询 API ==========

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

    /**
     * @brief 重置记录器
     */
    void reset() noexcept {
        count_ = 0;
        total_cycles_ = 0;
        min_cycles_ = std::numeric_limits<uint64_t>::max();
        max_cycles_ = 0;
        skipped_invalid_ = 0;
        skipped_overflow_ = 0;
        histogram_.reset();

        // 重新记录资源基准线
        start_resource_tracking();
    }

   private:
    // ========== 成员变量 ==========
    std::string name_;
    uint64_t count_ = 0;
    uint64_t total_cycles_ = 0;
    uint64_t min_cycles_ = std::numeric_limits<uint64_t>::max();
    uint64_t max_cycles_ = 0;
    uint64_t skipped_invalid_ = 0;
    uint64_t skipped_overflow_ = 0;

    HdrHistogram histogram_;

    // 资源使用基准线
    rusage initial_rusage_{};

    // ========== 辅助方法 ==========

    /**
     * @brief 记录初始资源状态
     */
    void start_resource_tracking() noexcept {
        getrusage(RUSAGE_SELF, &initial_rusage_);
    }

    /**
     * @brief 获取时间戳字符串
     */
    [[nodiscard]] static std::string get_timestamp() {
        auto now = std::chrono::system_clock::now();
        return std::format("{:%Y-%m-%d_%H-%M-%S}",
                           std::chrono::floor<std::chrono::seconds>(now));
    }

    /**
     * @brief 清理文件名
     */
    [[nodiscard]] static std::string sanitize_filename(std::string name) {
        std::replace_if(
            name.begin(), name.end(),
            [](char c) { return !(std::isalnum(c) || c == '_' || c == '-'); },
            '_');
        return name;
    }

    /**
     * @brief 生成输出路径
     */
    [[nodiscard]] fs::path make_output_path(const std::string& dir,
                                            const std::string& ext) const {
        return fs::path(dir) /
               (sanitize_filename(name_) + "_" + get_timestamp() + ext);
    }

    /**
     * @brief 确保目录存在
     */
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

    /**
     * @brief 打印警告信息
     */
    void print_warnings() const {
        if (skipped_invalid_ > 0 || skipped_overflow_ > 0) {
            std::println(stderr, "  ⚠️  Warnings:");
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

}  // namespace eph::utils
