#pragma once

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <numeric>
#include <print>
#include <string>
#include <vector>

#include "eph/utils/time.hpp"

namespace eph::utils {

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

    /**
     * @brief 生成格式化的百分位统计报告
     *
     * 输出标准百分位（p50/p90/p99/p99.9/p99.99）以及基本统计量。
     * 可选单位名称用于标注（如 "ns"、"us"、"cycles"）。
     *
     * @param title 报告标题
     * @param unit  数值单位名称（默认为空）
     * @return 格式化的多行报告字符串
     */
    [[nodiscard]] std::string report(std::string_view title = "Histogram",
                                     std::string_view unit = "") const {
        if (total_count_ == 0) {
            return std::format("{}: (empty, no samples recorded)\n", title);
        }

        auto vals = get_percentiles({50.0, 90.0, 99.0, 99.9, 99.99});
        std::string u = unit.empty() ? "" : std::format(" {}", unit);

        return std::format(
            "{}: {} samples\n"
            "  min   : {}{}\n"
            "  p50   : {}{}\n"
            "  p90   : {}{}\n"
            "  p99   : {}{}\n"
            "  p99.9 : {}{}\n"
            "  p99.99: {}{}\n"
            "  max   : {}{}\n"
            "  mean  : {:.1f}{}\n"
            "  stddev: {:.1f}{}\n",
            title, total_count_,
            get_min_value(), u,
            vals[0], u,
            vals[1], u,
            vals[2], u,
            vals[3], u,
            vals[4], u,
            get_max_value(), u,
            get_mean(), u,
            get_std_deviation(), u);
    }

    /// JSON-formatted histogram summary for monitoring system integration.
    /// Includes count, min, max, mean, stddev, and standard percentiles.
    /// Consistent with the to_json() pattern used by RttStats, TransportStats, etc.
    [[nodiscard]] std::string to_json() const {
        if (total_count_ == 0) {
            return "{\"count\":0}";
        }
        auto vals = get_percentiles({50.0, 90.0, 99.0, 99.9, 99.99});
        return std::format(
            "{{"
            "\"count\":{},\"min\":{},\"max\":{},"
            "\"mean\":{:.1f},\"stddev\":{:.1f},"
            "\"p50\":{},\"p90\":{},\"p99\":{},\"p999\":{},\"p9999\":{}}}",
            total_count_, get_min_value(), get_max_value(),
            get_mean(), get_std_deviation(),
            vals[0], vals[1], vals[2], vals[3], vals[4]);
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

}  // namespace eph::utils
