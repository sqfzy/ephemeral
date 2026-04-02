#pragma once

/// @file system_stats.hpp
/// RAII system resource profiler (getrusage-based, Linux/macOS only).
///
/// Captures CPU time, page faults, and context switches between
/// construction and snapshot(). Standalone — no coupling with Recorder.

#if !defined(__linux__) && !defined(__APPLE__)
#error "system_stats.hpp requires POSIX (getrusage). Not available on this platform."
#endif

#include <sys/resource.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <memory>
#include <string>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace eph::utils {

namespace detail {
/// @brief Lazily-initialized logger for the system-stats subsystem.
inline spdlog::logger* system_stats_logger() {
    static auto l = [] {
        try {
            return spdlog::stdout_color_mt("utils.system_stats");
        } catch (const spdlog::spdlog_ex&) {
            return spdlog::get("utils.system_stats");
        }
    }();
    return l.get();
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
    long maxrss_kb;      ///< Peak resident set size (KB, from getrusage ru_maxrss)
    long rss_kb;         ///< Current RSS (KB, from /proc/self/statm; 0 if unavailable)
    int  thread_count;   ///< Number of threads (from /proc/self/status; 0 if unavailable)

    /// Multi-line formatted dump for logging/debugging.
    [[nodiscard]] std::string dump() const {
        return std::format(
            "SystemResourceStats:\n"
            "  cpu: user={:.4f}s sys={:.4f}s total={:.4f}s\n"
            "  memory: rss={}KB maxrss={}KB threads={}\n"
            "  faults: major={} minor={}\n"
            "  ctx_switches: voluntary={} involuntary={}",
            user_cpu_s, sys_cpu_s, total_cpu_s,
            rss_kb, maxrss_kb, thread_count,
            majflt, minflt, nvcsw, nivcsw);
    }

    /// JSON-formatted stats for monitoring system integration.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"user_cpu_s\":{:.6f},\"sys_cpu_s\":{:.6f},\"total_cpu_s\":{:.6f},"
            "\"maxrss_kb\":{},\"rss_kb\":{},\"thread_count\":{},"
            "\"majflt\":{},\"minflt\":{},\"nvcsw\":{},\"nivcsw\":{}}}",
            user_cpu_s, sys_cpu_s, total_cpu_s,
            maxrss_kb, rss_kb, thread_count,
            majflt, minflt, nvcsw, nivcsw);
    }

    /// Compute delta between two snapshots for interval-based monitoring.
    /// Memory and thread fields are point-in-time (taken from lhs, not diffed).
    [[nodiscard]] friend SystemResourceStats operator-(
        const SystemResourceStats& lhs, const SystemResourceStats& rhs) noexcept {
        return SystemResourceStats{
            .majflt      = lhs.majflt     - rhs.majflt,
            .minflt      = lhs.minflt     - rhs.minflt,
            .nvcsw       = lhs.nvcsw      - rhs.nvcsw,
            .nivcsw      = lhs.nivcsw     - rhs.nivcsw,
            .user_cpu_s  = lhs.user_cpu_s - rhs.user_cpu_s,
            .sys_cpu_s   = lhs.sys_cpu_s  - rhs.sys_cpu_s,
            .total_cpu_s = lhs.total_cpu_s - rhs.total_cpu_s,
            .maxrss_kb   = lhs.maxrss_kb,
            .rss_kb      = lhs.rss_kb,
            .thread_count = lhs.thread_count,
        };
    }

    /// Equality comparison (floating-point fields use approximate comparison).
    [[nodiscard]] friend bool operator==(
        const SystemResourceStats& lhs, const SystemResourceStats& rhs) noexcept {
        constexpr double kEps = 1e-9;
        return lhs.majflt == rhs.majflt &&
               lhs.minflt == rhs.minflt &&
               lhs.nvcsw  == rhs.nvcsw  &&
               lhs.nivcsw == rhs.nivcsw &&
               lhs.maxrss_kb == rhs.maxrss_kb &&
               lhs.rss_kb == rhs.rss_kb &&
               lhs.thread_count == rhs.thread_count &&
               std::abs(lhs.user_cpu_s  - rhs.user_cpu_s)  < kEps &&
               std::abs(lhs.sys_cpu_s   - rhs.sys_cpu_s)   < kEps &&
               std::abs(lhs.total_cpu_s - rhs.total_cpu_s)  < kEps;
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
        if (getrusage(RUSAGE_SELF, &initial_rusage_) != 0) {
            SPDLOG_LOGGER_WARN(detail::system_stats_logger(),
                "getrusage failed at construction: {}", std::strerror(errno));
            initial_rusage_ = {};
        }
    }

    ~SystemStats() {
        if (auto_log_) {
            try {
                log_report();
            } catch (...) {
                // Swallow exceptions to prevent std::terminate from a destructor
            }
        }
    }

    SystemStats(const SystemStats&) = delete;
    SystemStats& operator=(const SystemStats&) = delete;
    SystemStats(SystemStats&&) = default;
    SystemStats& operator=(SystemStats&&) = default;

    /// Compute resource delta since construction or last reset().
    [[nodiscard]] SystemResourceStats snapshot() const noexcept {
        rusage current{};
        if (getrusage(RUSAGE_SELF, &current) != 0) {
            SPDLOG_LOGGER_WARN(detail::system_stats_logger(),
                "getrusage failed in snapshot: {}", std::strerror(errno));
            return {};
        }
        return compute_delta(current);
    }

    /// Reset the baseline to the current resource state.
    void reset() noexcept {
        if (getrusage(RUSAGE_SELF, &initial_rusage_) != 0) {
            SPDLOG_LOGGER_WARN(detail::system_stats_logger(),
                "getrusage failed in reset: {}", std::strerror(errno));
            initial_rusage_ = {};
        }
    }

    /// Log a formatted resource report via spdlog at INFO level.
    void log_report() const {
        auto s = snapshot();
        auto log = detail::system_stats_logger();
        SPDLOG_LOGGER_INFO(log,
            "System resources: user={:.4f}s sys={:.4f}s "
            "rss={}KB maxrss={}KB threads={} "
            "majflt={} minflt={} vcsw={} ivcsw={}",
            s.user_cpu_s, s.sys_cpu_s,
            s.rss_kb, s.maxrss_kb, s.thread_count,
            s.majflt, s.minflt, s.nvcsw, s.nivcsw);
    }

    /// @deprecated Use log_report() instead. Kept for backward compatibility.
    void print_report() const {
        log_report();
    }

   private:
    rusage initial_rusage_{};
    bool auto_log_;

    /// Read current RSS from /proc/self/statm (Linux). Returns 0 on failure.
    static long read_current_rss_kb() noexcept {
#if defined(__linux__)
        // /proc/self/statm fields: size resident shared text lib data dt (in pages)
        auto closer = [](FILE* f) { if (f) std::fclose(f); };
        std::unique_ptr<FILE, decltype(closer)> fp(std::fopen("/proc/self/statm", "r"), closer);
        if (!fp) return 0;
        long pages = 0;
        // Skip first field (size), read second (resident)
        if (std::fscanf(fp.get(), "%*ld %ld", &pages) != 1) pages = 0;
        // Convert pages to KB
        long ps = sysconf(_SC_PAGESIZE);
        if (ps <= 0) return 0;
        long page_kb = ps / 1024;
        return pages * page_kb;
#else
        return 0;
#endif
    }

    /// Read thread count from /proc/self/status (Linux). Returns 0 on failure.
    static int read_thread_count() noexcept {
#if defined(__linux__)
        auto closer = [](FILE* f) { if (f) std::fclose(f); };
        std::unique_ptr<FILE, decltype(closer)> fp(std::fopen("/proc/self/status", "r"), closer);
        if (!fp) return 0;
        char line[256];
        int threads = 0;
        while (std::fgets(line, sizeof(line), fp.get())) {
            if (std::sscanf(line, "Threads: %d", &threads) == 1) break;
        }
        return threads;
#else
        return 0;
#endif
    }

    [[nodiscard]] SystemResourceStats compute_delta(
        const rusage& current) const noexcept {
        auto time_diff = [](const timeval& t1, const timeval& t2) {
            return static_cast<double>(t2.tv_sec - t1.tv_sec) +
                   static_cast<double>(t2.tv_usec - t1.tv_usec) / 1e6;
        };

        double utime_s =
            time_diff(initial_rusage_.ru_utime, current.ru_utime);
        double stime_s =
            time_diff(initial_rusage_.ru_stime, current.ru_stime);

        return SystemResourceStats{
            .majflt      = current.ru_majflt - initial_rusage_.ru_majflt,
            .minflt      = current.ru_minflt - initial_rusage_.ru_minflt,
            .nvcsw       = current.ru_nvcsw  - initial_rusage_.ru_nvcsw,
            .nivcsw      = current.ru_nivcsw - initial_rusage_.ru_nivcsw,
            .user_cpu_s  = utime_s,
            .sys_cpu_s   = stime_s,
            .total_cpu_s = utime_s + stime_s,
            .maxrss_kb   = current.ru_maxrss,  // Peak RSS from getrusage
            .rss_kb      = read_current_rss_kb(),
            .thread_count = read_thread_count(),
        };
    }
};

}  // namespace eph::utils

/// @brief `std::format` support for SystemResourceStats.
///
/// Example: `std::format("{}", stats)` produces a single-line summary.
template <>
struct std::formatter<eph::utils::SystemResourceStats> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    auto format(const eph::utils::SystemResourceStats& s,
                std::format_context& ctx) const {
        return std::format_to(
            ctx.out(),
            "cpu={:.4f}s/{:.4f}s rss={}KB maxrss={}KB threads={} "
            "majflt={} minflt={} vcsw={} ivcsw={}",
            s.user_cpu_s, s.sys_cpu_s, s.rss_kb, s.maxrss_kb, s.thread_count,
            s.majflt, s.minflt, s.nvcsw, s.nivcsw);
    }
};
