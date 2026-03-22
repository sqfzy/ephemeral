#pragma once

#include <sys/resource.h>

#include <print>

namespace eph::utils {

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

}  // namespace eph::utils
