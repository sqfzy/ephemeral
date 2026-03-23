#pragma once

/// @file system_stats.hpp
/// RAII system resource profiler (getrusage-based).
///
/// Captures CPU time, page faults, and context switches between
/// construction and snapshot(). Standalone — no coupling with Recorder.

#include <sys/resource.h>

#include <format>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace eph::utils {

namespace detail {
inline std::shared_ptr<spdlog::logger> system_stats_logger() {
    static auto l = [] {
        auto lg = spdlog::get("utils.system_stats");
        if (!lg) lg = spdlog::stdout_color_mt("utils.system_stats");
        return lg;
    }();
    return l;
}
} // namespace detail

/// System resource consumption snapshot (delta from a baseline).
struct SystemResourceStats {
    long majflt;         ///< Major page faults (required disk I/O)
    long minflt;         ///< Minor page faults (no disk I/O needed)
    long nvcsw;          ///< Voluntary context switches (thread yielded CPU)
    long nivcsw;         ///< Involuntary context switches (preempted)
    double user_cpu_s;   ///< User CPU time (seconds)
    double sys_cpu_s;    ///< System CPU time (seconds)
    double total_cpu_s;  ///< Total CPU time (seconds)

    /// Multi-line formatted dump for logging/debugging.
    [[nodiscard]] std::string dump() const {
        return std::format(
            "SystemResourceStats:\n"
            "  cpu: user={:.4f}s sys={:.4f}s total={:.4f}s\n"
            "  faults: major={} minor={}\n"
            "  ctx_switches: voluntary={} involuntary={}",
            user_cpu_s, sys_cpu_s, total_cpu_s,
            majflt, minflt, nvcsw, nivcsw);
    }
};

/// RAII system resource profiler.
///
/// Snapshots getrusage at construction; snapshot() computes the delta.
/// Decoupled from Recorder — callers decide measurement granularity.
///
/// @example
/// @code
/// SystemStats sys_stats;
/// // ... run code under test ...
/// auto resource = sys_stats.snapshot();
/// SPDLOG_INFO("Resource usage: {}", resource);
/// @endcode
class SystemStats {
   public:
    /// @param auto_log Log resource report at destruction via spdlog.
    explicit SystemStats(bool auto_log = false)
        : auto_log_(auto_log) {
        getrusage(RUSAGE_SELF, &initial_rusage_);
    }

    ~SystemStats() {
        if (auto_log_) {
            log_report();
        }
    }

    SystemStats(const SystemStats&) = delete;
    SystemStats& operator=(const SystemStats&) = delete;
    SystemStats(SystemStats&&) = default;
    SystemStats& operator=(SystemStats&&) = default;

    /// Compute resource delta since construction or last reset().
    [[nodiscard]] SystemResourceStats snapshot() const noexcept {
        rusage current{};
        getrusage(RUSAGE_SELF, &current);
        return compute_delta(current);
    }

    /// Reset the baseline to the current resource state.
    void reset() noexcept {
        getrusage(RUSAGE_SELF, &initial_rusage_);
    }

    /// Log a formatted resource report via spdlog at INFO level.
    void log_report() const {
        auto s = snapshot();
        auto log = detail::system_stats_logger();
        SPDLOG_LOGGER_INFO(log,
            "System resources: user={:.4f}s sys={:.4f}s "
            "majflt={} minflt={} vcsw={} ivcsw={}",
            s.user_cpu_s, s.sys_cpu_s,
            s.majflt, s.minflt, s.nvcsw, s.nivcsw);
    }

    /// @deprecated Use log_report() instead. Kept for backward compatibility.
    void print_report() const {
        log_report();
    }

   private:
    rusage initial_rusage_{};
    bool auto_log_;

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

/// std::format support for SystemResourceStats.
template <>
struct std::formatter<eph::utils::SystemResourceStats> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    auto format(const eph::utils::SystemResourceStats& s,
                std::format_context& ctx) const {
        return std::format_to(
            ctx.out(),
            "cpu={:.4f}s/{:.4f}s majflt={} minflt={} vcsw={} ivcsw={}",
            s.user_cpu_s, s.sys_cpu_s, s.majflt, s.minflt, s.nvcsw, s.nivcsw);
    }
};
