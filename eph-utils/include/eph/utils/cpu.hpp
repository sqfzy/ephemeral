/// @file cpu.hpp
/// @brief CPU topology discovery, thread affinity, real-time scheduling, and spin-wait helpers.
///
/// Provides utilities for pinning threads to specific CPU cores, querying
/// physical topology (socket/core/thread), enabling real-time scheduling
/// policies, and issuing architecture-specific spin-wait hints.
///
/// Platform support:
/// - Linux: full support (via `/proc/cpuinfo` and `pthread` APIs).
/// - macOS: partial (no hard affinity; uses QoS / time-constraint policy).
/// - Windows: basic / stub.

#pragma once

#include <algorithm>
#include <charconv>
#include <expected>
#include <format>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#endif

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

// for _mm_pause
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||             \
    defined(_M_IX86)
#include <immintrin.h>
#endif

namespace eph::utils {

namespace detail {

/// @brief Lazily-initialized logger for the CPU utilities subsystem.
inline const std::shared_ptr<spdlog::logger>& cpu_logger() {
    static auto l = [] {
        auto lg = spdlog::get("utils.cpu");
        if (!lg) lg = spdlog::stdout_color_mt("utils.cpu");
        // Inherit level from spdlog global default
        return lg;
    }();
    return l;
}

} // namespace detail

/// @brief Physical CPU topology information for a single logical processor.
///
/// Each entry maps a logical hardware thread to its physical socket and
/// core, enabling NUMA-aware thread placement.
struct CpuTopologyInfo {
  unsigned socket_id;    ///< Physical CPU socket ID.
  unsigned core_id;      ///< Physical core ID within the socket.
  unsigned hw_thread_id; ///< Logical hardware thread (hyper-thread) ID.
};

/// @brief Detect system CPU topology.
///
/// Returns one `CpuTopologyInfo` per logical CPU, sorted by hardware
/// thread ID. On Linux, parses `/proc/cpuinfo`; on other platforms,
/// returns a simplified single-socket topology.
///
/// @return A vector of topology entries on success, or an error string
///         describing why detection failed.
///
/// @note On ARM Linux (no `physical id` / `core id` in cpuinfo),
///       falls back to a simplified topology using only `processor` IDs.
[[nodiscard]] inline std::expected<std::vector<CpuTopologyInfo>, std::string>
get_cpu_topology() {
  auto log = detail::cpu_logger();
  std::vector<CpuTopologyInfo> cpus;

  SPDLOG_LOGGER_DEBUG(log, "Detecting CPU topology");

#if defined(__linux__)
  // /proc/cpuinfo format: "key\t: value\n" per field, blank lines between CPUs.
  // We match "physical id", "core id", "processor" and extract the integer after ": ".
  static constexpr std::string_view kKeys[] = {
      "physical id", "core id", "processor"
  };

  // Parse the unsigned integer after ": " in a cpuinfo line.
  // Returns (true, value) on success, (false, 0) on parse failure.
  auto parse_cpuinfo_line = [](std::string_view line, std::string_view key)
      -> std::pair<bool, unsigned> {
    auto pos = line.find(key);
    if (pos == std::string_view::npos) return {false, 0};

    // Find the colon separator after the key
    auto colon = line.find(':', pos + key.size());
    if (colon == std::string_view::npos) return {false, 0};

    // Skip whitespace after colon
    auto val_start = colon + 1;
    while (val_start < line.size() && line[val_start] == ' ') ++val_start;
    if (val_start >= line.size()) return {false, 0};

    unsigned value = 0;
    auto [ptr, ec] = std::from_chars(
        line.data() + val_start, line.data() + line.size(), value);
    if (ec != std::errc{}) return {false, 0};

    return {true, value};
  };

  std::ifstream cpuinfo("/proc/cpuinfo");
  if (!cpuinfo) {
    SPDLOG_LOGGER_WARN(log, "Failed to open /proc/cpuinfo, "
                             "returning simplified topology");
    // Fall through to the non-Linux fallback below
  }
  CpuTopologyInfo element{};
  unsigned valid_mask = 0;
  // Track processor IDs separately for ARM fallback (no physical id / core id)
  std::vector<unsigned> processor_ids;

  for (std::string line; getline(cpuinfo, line);) {
    // Empty line separates CPU blocks in /proc/cpuinfo.
    // Reset partial state to avoid leaking fields across blocks
    // (e.g., ARM where physical id / core id may be absent).
    if (line.empty() || line.find_first_not_of(" \t\r") == std::string::npos) {
      if (valid_mask != 0 && valid_mask != 7) {
        SPDLOG_LOGGER_TRACE(log,
            "Discarding partial CPU entry (valid_mask=0x{:x})", valid_mask);
      }
      valid_mask = 0;
      element = {};
      continue;
    }

    for (unsigned i = 0; i < 3; ++i) {
      if (valid_mask & (1 << i)) continue;

      auto [ok, value] = parse_cpuinfo_line(line, kKeys[i]);
      if (!ok) continue;

      if (i == 0)      element.socket_id = value;
      else if (i == 1) element.core_id = value;
      else {
        element.hw_thread_id = value;
        processor_ids.push_back(value);
      }

      valid_mask |= (1 << i);
      if (valid_mask == 7) { // All three fields collected (x86)
        cpus.push_back(element);
        valid_mask = 0;
        element = {};
      }
      break;
    }
  }

  // ARM /proc/cpuinfo lacks "physical id" and "core id" — fall back to
  // simplified topology using only the "processor" field we collected.
  if (cpus.empty() && !processor_ids.empty()) {
    SPDLOG_LOGGER_INFO(log,
        "physical id/core id not found in /proc/cpuinfo, "
        "using simplified topology ({} processors)", processor_ids.size());
    for (unsigned id : processor_ids) {
      cpus.push_back({0, id, id});
    }
  }

  auto hw_threads = std::thread::hardware_concurrency();
  // hardware_concurrency() may return 0 if the value is not computable
  if (hw_threads != 0 && cpus.size() != hw_threads) {
    auto msg = std::format(
        "CPU topology detection failed: parsed {} CPUs, expected {}",
        cpus.size(), hw_threads);
    SPDLOG_LOGGER_ERROR(log, "{}", msg);
    return std::unexpected(std::move(msg));
  }
  if (cpus.empty()) {
    auto msg = std::string("CPU topology detection failed: no CPUs found");
    SPDLOG_LOGGER_ERROR(log, "{}", msg);
    return std::unexpected(std::move(msg));
  }

  // 按 hw_thread_id 排序
  std::ranges::sort(cpus, {}, &CpuTopologyInfo::hw_thread_id);
#else
  // macOS/Windows 回退方案
  SPDLOG_LOGGER_DEBUG(log, "Using simplified topology (non-Linux)");
  for (unsigned i = 0; i < std::thread::hardware_concurrency(); ++i) {
    cpus.push_back({0, i, i});
  }
#endif

  SPDLOG_LOGGER_INFO(log, "Detected {} CPUs", cpus.size());
  return cpus;
}

/// @brief Pin the calling thread to a specific CPU core.
///
/// Sets the thread's CPU affinity so it runs exclusively on the
/// specified core, reducing thread migration and cache invalidation.
///
/// @param cpu_id Logical CPU ID (0 to `hardware_concurrency()-1`).
///               A negative value is a no-op (returns success).
/// @param name   Optional thread/role label for log messages.
/// @return `std::expected<void>` on success, or an error string.
///
/// @note Linux: uses `pthread_setaffinity_np` for hard affinity.
/// @note macOS: hard affinity unavailable; sets QoS class instead.
/// @warning Excessive pinning may cause load imbalance.
[[nodiscard]] inline std::expected<void, std::string>
set_thread_affinity(int cpu_id, const char* name = nullptr) {
  if (cpu_id < 0) return {};
  auto log = detail::cpu_logger();
  const char* tag = name ? name : "thread";
#if defined(__linux__)
  if (static_cast<unsigned>(cpu_id) >= CPU_SETSIZE) {
    auto msg = std::format(
        "cpu_id={} exceeds CPU_SETSIZE={} for {}", cpu_id, CPU_SETSIZE, tag);
    SPDLOG_LOGGER_ERROR(log, "{}", msg);
    return std::unexpected(std::move(msg));
  }
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(cpu_id, &cpuset);
  int ret = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  if (ret != 0) {
    auto msg = std::format("Failed to pin {} to cpu_id={}: {}",
        tag, cpu_id, std::generic_category().message(ret));
    SPDLOG_LOGGER_ERROR(log, "{}", msg);
    return std::unexpected(std::move(msg));
  }
  SPDLOG_LOGGER_INFO(log, "{} pinned to cpu_id={}", tag, cpu_id);
  return {};
#elif defined(__APPLE__)
  // macOS 不支持硬亲和性，只能设置 QoS
  SPDLOG_LOGGER_DEBUG(log,
      "macOS: setting QoS instead of hard affinity for {} (cpu_id={})",
      tag, cpu_id);
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  return {};
#else
  auto msg = std::format(
      "Thread affinity not supported on this platform for {} (cpu_id={})",
      tag, cpu_id);
  SPDLOG_LOGGER_WARN(log, "{}", msg);
  return std::unexpected(std::move(msg));
#endif
}

/// @brief Real-time scheduling policy for set_thread_realtime().
enum class RealtimePolicy {
    Fifo,       ///< SCHED_FIFO -- first-in-first-out, no time slicing.
    RoundRobin, ///< SCHED_RR -- round-robin with time quantum.
};

/// @brief Switch the calling thread to a real-time scheduling policy.
///
/// Promotes the thread to `SCHED_FIFO` or `SCHED_RR` to reduce
/// scheduling jitter. Ideal for DPDK poll threads, low-latency trading
/// paths, and other tail-latency-sensitive workloads.
///
/// @param policy   Scheduling policy: `Fifo` or `RoundRobin`.
/// @param priority Real-time priority (1-99). Defaults to the
///                 system-maximum when negative.
/// @param name     Optional thread/role label for log messages.
/// @return `std::expected<void>` on success, or an error string.
///
/// @note Linux: uses `pthread_setschedparam`. Requires `CAP_SYS_NICE`
///       or root. Grant via `ulimit -r 99` or
///       `setcap cap_sys_nice+ep <binary>`.
/// @note macOS: approximates real-time with `THREAD_TIME_CONSTRAINT_POLICY`.
///
/// @warning A `SCHED_FIFO` priority-99 thread preempts all non-RT threads.
///          An infinite loop at this priority may render the system
///          unresponsive. Ensure the thread has an exit condition.
[[nodiscard]] inline std::expected<void, std::string>
set_thread_realtime(RealtimePolicy policy = RealtimePolicy::Fifo,
                    int priority = -1,
                    const char* name = nullptr) {
  auto log = detail::cpu_logger();
  const char* tag = name ? name : "thread";

#if defined(__linux__)
  int linux_policy = (policy == RealtimePolicy::Fifo) ? SCHED_FIFO : SCHED_RR;
  const char* policy_name = (policy == RealtimePolicy::Fifo) ? "SCHED_FIFO" : "SCHED_RR";

  int max_prio = sched_get_priority_max(linux_policy);
  int min_prio = sched_get_priority_min(linux_policy);
  if (max_prio < 0 || min_prio < 0) {
    auto msg = std::format("Failed to query {} priority range for {}: {}",
        policy_name, tag, std::generic_category().message(errno));
    SPDLOG_LOGGER_ERROR(log, "{}", msg);
    return std::unexpected(std::move(msg));
  }

  // Default to max priority; clamp user-provided value to valid range
  if (priority < 0) {
    priority = max_prio;
  } else {
    priority = std::clamp(priority, min_prio, max_prio);
  }

  sched_param param{};
  param.sched_priority = priority;

  int ret = pthread_setschedparam(pthread_self(), linux_policy, &param);
  if (ret != 0) {
    auto msg = std::format(
        "Failed to set {} priority={} for {}: {}. "
        "Hint: run as root, or: setcap cap_sys_nice+ep <binary>",
        policy_name, priority, tag, std::generic_category().message(ret));
    SPDLOG_LOGGER_ERROR(log, "{}", msg);
    return std::unexpected(std::move(msg));
  }

  SPDLOG_LOGGER_INFO(log, "{} set to {} priority={} (range {}-{})",
      tag, policy_name, priority, min_prio, max_prio);
  return {};

#elif defined(__APPLE__)
  // macOS: use THREAD_TIME_CONSTRAINT_POLICY for near-realtime behavior.
  // This requests the Mach scheduler to treat this thread as time-critical.
  SPDLOG_LOGGER_DEBUG(log,
      "macOS: setting THREAD_TIME_CONSTRAINT_POLICY for {} (priority={})",
      tag, priority);
  // Period/computation/constraint in Mach absolute time units.
  // These values request ~1ms scheduling granularity.
  mach_timebase_info_data_t info;
  mach_timebase_info(&info);
  uint32_t ms_in_abs = static_cast<uint32_t>(1000000ULL * info.denom / info.numer);

  thread_time_constraint_policy_data_t tc_policy{};
  tc_policy.period      = ms_in_abs;      // 1ms period
  tc_policy.computation = ms_in_abs / 2;  // 0.5ms computation
  tc_policy.constraint  = ms_in_abs;      // 1ms deadline
  tc_policy.preemptible = 0;

  auto kr = thread_policy_set(
      mach_thread_self(),
      THREAD_TIME_CONSTRAINT_POLICY,
      reinterpret_cast<thread_policy_t>(&tc_policy),
      THREAD_TIME_CONSTRAINT_POLICY_COUNT);
  if (kr != KERN_SUCCESS) {
    auto msg = std::format(
        "macOS: THREAD_TIME_CONSTRAINT_POLICY failed for {}: kern_return={}",
        tag, kr);
    SPDLOG_LOGGER_WARN(log, "{}", msg);
    return std::unexpected(std::move(msg));
  }
  SPDLOG_LOGGER_INFO(log, "{} set to THREAD_TIME_CONSTRAINT_POLICY", tag);
  return {};

#else
  auto msg = std::format(
      "Realtime scheduling not supported on this platform for {}", tag);
  SPDLOG_LOGGER_WARN(log, "{}", msg);
  return std::unexpected(std::move(msg));
#endif
}

/// @brief Query the CPU's nominal base frequency.
///
/// Reads the base (non-boosted) frequency from system information.
/// Actual runtime frequency may vary due to turbo boost or power saving.
///
/// @return CPU frequency in GHz, or `nullopt` if detection failed.
///
/// @note Linux: parses the "model name" line in `/proc/cpuinfo`.
/// @note Other platforms: returns `nullopt`.
/// @note For precise timing, prefer TSC calibration (`TSC::init()`)
///       over this nominal value.
[[nodiscard]] inline std::optional<double> get_cpu_base_frequency() {
  auto log = detail::cpu_logger();
#if defined(__linux__)
  // Parse "model name" line, looking for "@ X.XX GHz" pattern.
  // Example: "model name\t: Intel(R) Core(TM) i7-10700K CPU @ 3.80GHz"
  std::ifstream cpuinfo("/proc/cpuinfo");
  if (!cpuinfo) {
    SPDLOG_LOGGER_WARN(log, "Failed to open /proc/cpuinfo, "
                             "cannot detect CPU frequency");
    return std::nullopt;
  }
  for (std::string line; getline(cpuinfo, line);) {
    if (line.find("model name") == std::string::npos) continue;

    auto at_pos = line.find('@');
    if (at_pos == std::string::npos) continue;

    // Skip whitespace after '@'
    auto num_start = at_pos + 1;
    while (num_start < line.size() && line[num_start] == ' ') ++num_start;

    // Parse the floating-point frequency value
    double freq = 0.0;
    auto [ptr, ec] = std::from_chars(
        line.data() + num_start, line.data() + line.size(), freq);
    if (ec != std::errc{}) continue;

    // Verify "GHz" follows (skip whitespace)
    std::string_view rest(ptr, line.data() + line.size() - ptr);
    while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    if (rest.starts_with("GHz")) {
      SPDLOG_LOGGER_DEBUG(log, "CPU base frequency: {:.2f} GHz", freq);
      return freq;
    }
  }
  SPDLOG_LOGGER_WARN(log,
      "Could not parse CPU frequency from /proc/cpuinfo");
#else
  SPDLOG_LOGGER_DEBUG(log,
      "CPU frequency detection not available on this platform");
#endif
  return std::nullopt;
}

/// @brief Issue a CPU spin-wait hint instruction.
///
/// Call inside spin-lock or busy-wait loops to:
/// - Reduce power consumption (avoid speculative execution waste).
/// - Yield pipeline resources to the hyper-thread sibling.
/// - Avoid memory-ordering violations on some architectures.
///
/// @code
///   while (!lock.try_lock()) {
///       cpu_relax();  // much better than an empty loop
///   }
/// @endcode
///
/// @note x86: emits `PAUSE`.
/// @note ARM64: emits `YIELD`.
/// @note Other: falls back to `std::this_thread::yield()`.
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||             \
    defined(_M_IX86)
  _mm_pause(); // 提示 CPU 这是一个自旋循环，降低功耗并避免流水线清空
#elif defined(__aarch64__)
  asm volatile("yield"); // ARM64
#else
  std::this_thread::yield(); // Fallback
#endif
}

} // namespace eph::utils

/// @brief `std::format` support for CpuTopologyInfo.
///
/// Example: `std::format("{}", info)` produces `"socket=0 core=2 thread=4"`.
template <>
struct std::formatter<eph::utils::CpuTopologyInfo> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    auto format(const eph::utils::CpuTopologyInfo& info,
                std::format_context& ctx) const {
        return std::format_to(ctx.out(), "socket={} core={} thread={}",
                              info.socket_id, info.core_id, info.hw_thread_id);
    }
};
