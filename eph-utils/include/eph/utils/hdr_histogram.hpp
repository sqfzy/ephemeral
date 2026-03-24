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
            ++dropped_count_;
            return false;
        }

        int32_t idx = counts_index_for(value);
        if (idx < 0 || idx >= counts_len_) [[unlikely]] {
            ++dropped_count_;
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
            dropped_count_ += count;
            return false;
        }

        int32_t idx = counts_index_for(value);
        if (idx < 0 || idx >= counts_len_) [[unlikely]] {
            dropped_count_ += count;
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
        dropped_count_ = 0;
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

    /// Inverse CDF: return the percentile (0.0–100.0) at which a given value falls.
    ///
    /// Answers "what percentage of recorded samples are at or below this value?"
    /// Useful for SLA verification: `get_percentile_at_or_below(10000)` returns
    /// the percentage of latencies ≤ 10µs (if recording in nanoseconds).
    ///
    /// @param value  The value to query
    /// @return Percentile in [0.0, 100.0], or 0.0 if no samples recorded
    ///
    /// @note HDR Histogram quantizes values into buckets. A bucket that spans
    ///       [low, low+range) is counted as "at or below" the query value if
    ///       the bucket's low bound ≤ value. This matches the forward CDF
    ///       semantics: get_value_at_percentile() returns bucket midpoints.
    [[nodiscard]] double get_percentile_at_or_below(uint64_t value) const noexcept {
        if (total_count_ == 0) return 0.0;

        uint64_t accumulated = 0;
        for (int32_t i = 0; i < counts_len_; ++i) {
            uint64_t count = counts_[i];
            if (count == 0) continue;

            uint64_t bucket_value = value_from_index(i);
            if (bucket_value > value) break;

            accumulated += count;
        }

        return (100.0 * static_cast<double>(accumulated)) /
               static_cast<double>(total_count_);
    }

    /// Batch inverse CDF: return percentiles for multiple values in a single scan.
    ///
    /// More efficient than calling get_percentile_at_or_below() in a loop,
    /// as it traverses the histogram only once.
    ///
    /// @param values  Values to query (need not be sorted)
    /// @return Percentiles in [0.0, 100.0], one per input value
    [[nodiscard]] std::vector<double> get_percentiles_at_or_below(
        const std::vector<uint64_t>& values) const {
        std::vector<double> results(values.size(), 0.0);
        if (total_count_ == 0 || values.empty()) return results;

        // Sort indices by value for single-pass scan
        std::vector<size_t> sorted_indices(values.size());
        std::iota(sorted_indices.begin(), sorted_indices.end(), 0);
        std::sort(sorted_indices.begin(), sorted_indices.end(),
                  [&](size_t a, size_t b) {
                      return values[a] < values[b];
                  });

        uint64_t accumulated = 0;
        size_t next_idx = 0;
        double inv_total = 100.0 / static_cast<double>(total_count_);

        for (int32_t i = 0; i < counts_len_ && next_idx < sorted_indices.size(); ++i) {
            uint64_t count = counts_[i];
            if (count == 0) continue;

            uint64_t bucket_value = value_from_index(i);

            // Emit results for all query values < current bucket_value
            while (next_idx < sorted_indices.size()) {
                size_t orig = sorted_indices[next_idx];
                if (values[orig] < bucket_value) {
                    results[orig] = static_cast<double>(accumulated) * inv_total;
                    next_idx++;
                } else {
                    break;
                }
            }

            accumulated += count;

            // Emit results for query values == current bucket_value
            while (next_idx < sorted_indices.size()) {
                size_t orig = sorted_indices[next_idx];
                if (values[orig] <= bucket_value) {
                    results[orig] = static_cast<double>(accumulated) * inv_total;
                    next_idx++;
                } else {
                    break;
                }
            }
        }

        // Remaining values > max: they encompass all samples → 100%
        while (next_idx < sorted_indices.size()) {
            results[sorted_indices[next_idx]] = 100.0;
            next_idx++;
        }

        return results;
    }

    [[nodiscard]] uint64_t get_total_count() const noexcept {
        return total_count_;
    }

    /// Number of samples rejected because they fell outside the trackable range.
    /// Non-zero values indicate the histogram range should be widened.
    [[nodiscard]] uint64_t get_dropped_count() const noexcept {
        return dropped_count_;
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

    /// Percentile entry returned by for_each_percentile().
    struct PercentileEntry {
        uint64_t value;              ///< Value at this percentile (bucket midpoint)
        double   percentile;         ///< Percentile in [0.0, 100.0]
        uint64_t total_count_to_val; ///< Cumulative count at or below this value
        double   inv_percentile;     ///< 1/(1-percentile/100), inf at 100%
    };

    /// Iterate over the percentile distribution at halving-distance steps.
    ///
    /// Calls `func(PercentileEntry)` at each percentile step where the value
    /// changes, using the standard HdrHistogram halving iteration:
    ///   0%, 50%, 75%, 87.5%, 93.75%, ..., 99.9%, 99.99%, ..., 100%
    ///
    /// The `ticks_per_half_distance` parameter controls how many entries are
    /// emitted between each halving step (default 5, matching hdrhistogram.org).
    ///
    /// @param func  Callback invoked for each percentile entry
    /// @param ticks_per_half_distance  Number of ticks between halving steps
    template <typename Func>
        requires std::invocable<Func, const PercentileEntry&>
    void for_each_percentile(Func func,
                             int ticks_per_half_distance = 5) const {
        if (total_count_ == 0 || ticks_per_half_distance < 1) return;

        uint64_t accumulated = 0;
        int32_t  bucket_idx = 0;

        // Walk through percentile steps
        double percentile_to_iterate_to = 0.0;
        uint64_t last_value_reported = 0;
        bool first = true;

        // Advance bucket_idx to next non-empty bucket, accumulating counts
        auto advance_to = [&](uint64_t count_needed) -> bool {
            while (accumulated < count_needed && bucket_idx < counts_len_) {
                uint64_t count = counts_[bucket_idx];
                if (count > 0) {
                    accumulated += count;
                }
                if (accumulated < count_needed) {
                    bucket_idx++;
                }
            }
            return bucket_idx < counts_len_;
        };

        // Iterate through halving percentile steps
        for (double half_distance = 100.0; ; ) {
            double step = half_distance / ticks_per_half_distance;

            for (int tick = 0; tick < ticks_per_half_distance; ++tick) {
                double target_percentile = percentile_to_iterate_to + step;
                if (target_percentile > 100.0) target_percentile = 100.0;

                uint64_t count_at_percentile = static_cast<uint64_t>(
                    std::ceil((target_percentile / 100.0) *
                              static_cast<double>(total_count_)));
                if (count_at_percentile == 0) count_at_percentile = 1;

                if (!advance_to(count_at_percentile)) break;

                uint64_t val = value_from_index(bucket_idx);
                uint64_t high = next_non_equivalent_value(val);
                uint64_t midpoint = val + (high - val) / 2;

                // Only emit when the value changes (or first entry)
                if (first || midpoint != last_value_reported) {
                    double pct = std::min(100.0,
                        (100.0 * static_cast<double>(accumulated)) /
                        static_cast<double>(total_count_));
                    double inv = (pct < 100.0)
                        ? 1.0 / (1.0 - pct / 100.0)
                        : std::numeric_limits<double>::infinity();

                    func(PercentileEntry{
                        .value = midpoint,
                        .percentile = pct,
                        .total_count_to_val = accumulated,
                        .inv_percentile = inv,
                    });

                    last_value_reported = midpoint;
                    first = false;
                }

                percentile_to_iterate_to = target_percentile;
            }

            // Next half-distance
            half_distance /= 2.0;

            // Stop when we've reached 100%
            if (percentile_to_iterate_to >= 100.0) break;
            // Safety: stop at extremely fine resolution
            if (half_distance < 1e-10) break;
        }

        // Always emit 100th percentile (max value) if not already emitted
        uint64_t max_mid = max_value_;
        if (first || max_mid != last_value_reported) {
            func(PercentileEntry{
                .value = max_value_,
                .percentile = 100.0,
                .total_count_to_val = total_count_,
                .inv_percentile = std::numeric_limits<double>::infinity(),
            });
        }
    }

    /**
     * @brief 合并另一个直方图
     * @return true 合并成功，false 配置不兼容
     */
    [[nodiscard]] bool merge(const HdrHistogram& other) noexcept {
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

    /// Subtract another histogram's counts from this one (windowed measurement).
    ///
    /// Computes the difference `this -= other` for each bucket. Useful for
    /// windowed measurement: take a snapshot at T1, record more samples,
    /// take a snapshot at T2, then `T2.subtract(T1)` yields only the samples
    /// recorded between T1 and T2.
    ///
    /// @note min/max are recomputed by scanning the resulting counts, since
    ///       subtraction may invalidate the previous min/max values.
    /// @return true on success, false if histograms are incompatible
    [[nodiscard]] bool subtract(const HdrHistogram& other) noexcept {
        if (!is_compatible(other)) return false;
        if (other.total_count_ > total_count_) return false;

        // Validate no bucket underflows before mutating
        for (int32_t i = 0; i < counts_len_; ++i) {
            if (counts_[i] < other.counts_[i]) return false;
        }

        // Subtract + recompute min/max in a single pass
        total_count_ -= other.total_count_;
        min_value_ = std::numeric_limits<uint64_t>::max();
        max_value_ = 0;
        for (int32_t i = 0; i < counts_len_; ++i) {
            counts_[i] -= other.counts_[i];
            if (counts_[i] > 0) {
                uint64_t v = value_from_index(i);
                min_value_ = std::min(min_value_, v);
                max_value_ = std::max(max_value_, next_non_equivalent_value(v) - 1);
            }
        }

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

        std::string result = std::format(
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
        if (dropped_count_ > 0) {
            result += std::format("  dropped: {} (out of range)\n", dropped_count_);
        }
        return result;
    }

    /// JSON-formatted histogram summary for monitoring system integration.
    /// Includes count, min, max, mean, stddev, and standard percentiles.
    /// Consistent with the to_json() pattern used by RttStats, TransportStats, etc.
    [[nodiscard]] std::string to_json() const {
        if (total_count_ == 0 && dropped_count_ == 0) {
            return "{\"count\":0}";
        }
        if (total_count_ == 0) {
            return std::format("{{\"count\":0,\"dropped\":{}}}", dropped_count_);
        }
        auto vals = get_percentiles({50.0, 90.0, 99.0, 99.9, 99.99});
        std::string json = std::format(
            "{{"
            "\"count\":{},\"min\":{},\"max\":{},"
            "\"mean\":{:.1f},\"stddev\":{:.1f},"
            "\"p50\":{},\"p90\":{},\"p99\":{},\"p999\":{},\"p9999\":{}",
            total_count_, get_min_value(), get_max_value(),
            get_mean(), get_std_deviation(),
            vals[0], vals[1], vals[2], vals[3], vals[4]);
        if (dropped_count_ > 0) {
            json += std::format(",\"dropped\":{}", dropped_count_);
        }
        json += "}";
        return json;
    }

    /// Output the percentile distribution in the standard HDR Histogram text format.
    ///
    /// Produces output compatible with hdrhistogram.org and hdr-histogram-plotter.
    /// Each line: "Value  Percentile  TotalCount  1/(1-Percentile)"
    ///
    /// @param output_value_unit_scaling  Divisor for output values (e.g., 1000.0
    ///        to convert nanoseconds to microseconds). Default 1.0 (no scaling).
    /// @return Multi-line string in standard HDR Histogram percentile distribution format
    [[nodiscard]] std::string output_percentile_distribution(
        double output_value_unit_scaling = 1.0) const {
        if (output_value_unit_scaling <= 0.0) output_value_unit_scaling = 1.0;

        std::string result;
        result.reserve(4096);

        result += std::format("{:>12s} {:>14s} {:>10s} {:>14s}\n\n",
                              "Value", "Percentile", "TotalCount", "1/(1-Percentile)");

        if (total_count_ == 0) return result;

        uint64_t accumulated = 0;
        for (int32_t i = 0; i < counts_len_; ++i) {
            uint64_t count = counts_[i];
            if (count == 0) continue;

            accumulated += count;
            uint64_t val = value_from_index(i);
            double percentile = (100.0 * static_cast<double>(accumulated)) /
                                static_cast<double>(total_count_);
            double scaled_value = static_cast<double>(val) / output_value_unit_scaling;

            if (percentile < 100.0) {
                double inv = 1.0 / (1.0 - percentile / 100.0);
                result += std::format("{:12.3f} {:14.6f} {:10} {:14.2f}\n",
                                      scaled_value, percentile / 100.0,
                                      accumulated, inv);
            } else {
                result += std::format("{:12.3f} {:14.6f} {:10} {:>14s}\n",
                                      scaled_value, 1.0, accumulated, "inf");
            }
        }

        // Footer
        result += std::format(
            "#[Mean    = {:12.3f}, StdDeviation   = {:12.3f}]\n"
            "#[Max     = {:12.3f}, Total count    = {:10}]\n"
            "#[Buckets = {:10}, SubBuckets     = {:10}]\n",
            get_mean() / output_value_unit_scaling,
            get_std_deviation() / output_value_unit_scaling,
            static_cast<double>(get_max_value()) / output_value_unit_scaling,
            total_count_,
            bucket_count_, sub_bucket_count_);

        return result;
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
    uint64_t dropped_count_ = 0;  ///< Samples rejected (out of trackable range)

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

    /// Human-readable multi-line summary for logging and diagnostics.
    [[nodiscard]] std::string dump() const {
        if (count == 0) {
            return std::format("{}: (empty, no samples)", name);
        }
        return std::format(
            "{} ({} samples):\n"
            "  avg: {:.1f} ns, stddev: {:.1f} ns\n"
            "  min: {:.1f} ns, max: {:.1f} ns\n"
            "  p50: {:.1f} ns, p90: {:.1f} ns, p99: {:.1f} ns, p99.9: {:.1f} ns",
            name, count,
            avg_ns, stddev_ns,
            min_ns, max_ns,
            p50_ns, p90_ns, p99_ns, p999_ns);
    }

    /// JSON-formatted summary for monitoring system integration.
    /// Consistent with to_json() patterns in TransportStats, RttStats, etc.
    /// @note `name` is not JSON-escaped; callers must ensure it contains no
    ///       quotes or control characters (true for all Recorder-generated names).
    [[nodiscard]] std::string to_json() const {
        if (count == 0) {
            return std::format("{{\"name\":\"{}\",\"count\":0}}", name);
        }
        return std::format(
            "{{\"name\":\"{}\",\"count\":{},\"avg_ns\":{:.2f},"
            "\"min_ns\":{:.2f},\"max_ns\":{:.2f},\"stddev_ns\":{:.2f},"
            "\"p50_ns\":{:.2f},\"p90_ns\":{:.2f},"
            "\"p99_ns\":{:.2f},\"p999_ns\":{:.2f}}}",
            name, count, avg_ns,
            min_ns, max_ns, stddev_ns,
            p50_ns, p90_ns, p99_ns, p999_ns);
    }

    /// Compute delta between two snapshots for interval-based monitoring.
    /// Numeric fields are subtracted; name is taken from the later snapshot.
    [[nodiscard]] friend Stats operator-(const Stats& lhs, const Stats& rhs) noexcept {
        return Stats{
            .name      = lhs.name,
            .count     = lhs.count - rhs.count,
            .avg_ns    = lhs.avg_ns,     // point-in-time, not diffable
            .min_ns    = lhs.min_ns,     // point-in-time
            .max_ns    = lhs.max_ns,     // point-in-time
            .p50_ns    = lhs.p50_ns,     // point-in-time
            .p90_ns    = lhs.p90_ns,     // point-in-time
            .p99_ns    = lhs.p99_ns,     // point-in-time
            .p999_ns   = lhs.p999_ns,    // point-in-time
            .stddev_ns = lhs.stddev_ns,  // point-in-time
        };
    }

    [[nodiscard]] friend bool operator==(const Stats&, const Stats&) = default;
};

}  // namespace eph::utils

/// std::format support for Stats.
/// Example: std::format("{}", stats) → "MyBench (1000 samples): avg=42.3ns p99=128.7ns"
template <>
struct std::formatter<eph::utils::Stats> : std::formatter<std::string> {
    auto format(const eph::utils::Stats& s, auto& ctx) const {
        if (s.count == 0) {
            return std::formatter<std::string>::format(
                std::format("{}: no samples", s.name), ctx);
        }
        return std::formatter<std::string>::format(
            std::format("{} (n={}): avg={:.1f}ns p50={:.1f}ns p99={:.1f}ns max={:.1f}ns",
                s.name, s.count, s.avg_ns, s.p50_ns, s.p99_ns, s.max_ns),
            ctx);
    }
};
