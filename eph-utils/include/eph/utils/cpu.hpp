#pragma once

#include <algorithm>
#include <charconv>
#include <expected>
#include <format>
#include <fstream>
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

inline std::shared_ptr<spdlog::logger> cpu_logger() {
    static auto l = [] {
        auto lg = spdlog::get("utils.cpu");
        if (!lg) lg = spdlog::stdout_color_mt("utils.cpu");
        // Inherit level from spdlog global default
        return lg;
    }();
    return l;
}

} // namespace detail

/**
 * @brief CPU 拓扑和亲和性管理工具
 * 
 * 提供以下功能：
 * - 获取 CPU 物理拓扑信息（Socket/Core/Thread）
 * - 绑定线程到指定 CPU 核心
 * - 查询 CPU 基准频率
 * - CPU 自旋等待优化
 * 
 * 使用场景：
 * - 高性能计算应用需要控制线程调度
 * - 减少跨 NUMA 节点的内存访问
 * - 实现用户态自旋锁
 * 
 * 平台支持：
 * - Linux: 完整支持（通过 /proc/cpuinfo 和 pthread API）
 * - macOS: 部分支持（不支持硬亲和性）
 * - Windows: 基础支持
 * 
 * @example
 * @code
 *   // 获取 CPU 拓扑
 *   auto topology = cpu::get_cpu_topology();
 *   
 *   // 绑定当前线程到 CPU 0
 *   cpu::set_thread_affinity(0);
 *   
 *   // 自旋等待（低功耗）
 *   while (!ready.load()) {
 *     cpu::cpu_relax();
 *   }
 * @endcode
 */
struct CpuTopologyInfo {
  unsigned socket_id;    // 物理 CPU Socket
  unsigned core_id;      // 物理核心
  unsigned hw_thread_id; // 硬件线程（超线程）
};

/**
 * @brief 获取系统 CPU 拓扑信息
 *
 * 返回每个逻辑 CPU 的物理拓扑信息，按硬件线程 ID 排序。
 *
 * @return std::expected 包含 CPU 拓扑信息列表，或错误描述字符串
 *
 * @note Linux: 解析 /proc/cpuinfo
 * @note 其他平台: 返回简化拓扑（假设单 Socket）
 */
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

/**
 * @brief 绑定当前线程到指定 CPU 核心
 * 
 * 设置线程的 CPU 亲和性，使其只在指定核心上运行。
 * 用于减少线程迁移开销和缓存失效。
 * 
 * @param cpu_id 目标 CPU 核心的逻辑 ID（0 到 hardware_concurrency()-1），
 *               负值表示不绑定（直接返回）
 * @param name 可选的线程/角色名称，用于日志标识
 *
 * @note Linux: 使用 pthread_setaffinity_np 硬绑定
 * @note macOS: 不支持硬亲和性，仅设置 QoS 类
 * @note Windows: 需要额外实现
 *
 * @warning 过度使用可能导致负载不均衡
 */
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

/// Real-time scheduling policy for set_thread_realtime().
enum class RealtimePolicy { Fifo, RoundRobin };

/**
 * @brief 设置当前线程为实时调度策略
 *
 * 将当前线程切换到 SCHED_FIFO 或 SCHED_RR，减少调度延迟。
 * 适用于 DPDK 轮询线程、低延迟交易路径等对尾延迟敏感的场景。
 *
 * @param policy  调度策略：Fifo（先进先出）或 RoundRobin（时间片轮转）
 * @param priority  实时优先级（1–99），默认为系统允许的最大值。
 *                  负值表示不设置（直接返回）
 * @param name  可选的线程/角色名称，用于日志标识
 *
 * @note Linux: 使用 pthread_setschedparam。需要 CAP_SYS_NICE 或 root 权限。
 *   可通过 `ulimit -r 99` 或 `setcap cap_sys_nice+ep <binary>` 授权。
 * @note macOS: 使用 THREAD_TIME_CONSTRAINT_POLICY 近似实时行为。
 *
 * @warning SCHED_FIFO 优先级 99 的线程不会被非实时线程抢占。
 *   如果线程死循环，可能导致系统无响应。确保线程有退出条件。
 */
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

/**
 * @brief 获取 CPU 基准频率
 *
 * 从系统信息中读取 CPU 的标称频率（非实时频率）。
 * 
 * @return double CPU 频率（GHz）
 * 
 * @note Linux: 解析 /proc/cpuinfo 中的 "model name"
 * @note 其他平台: 返回 1.0 GHz（回退值）
 * @note 实际频率可能因睿频/节能而变化，建议使用 TSC 校准
 */
inline double get_cpu_base_frequency() {
  auto log = detail::cpu_logger();
#if defined(__linux__)
  // Parse "model name" line, looking for "@ X.XX GHz" pattern.
  // Example: "model name\t: Intel(R) Core(TM) i7-10700K CPU @ 3.80GHz"
  std::ifstream cpuinfo("/proc/cpuinfo");
  if (!cpuinfo) {
    SPDLOG_LOGGER_WARN(log, "Failed to open /proc/cpuinfo, "
                             "using fallback frequency 1.0 GHz");
    return 1.0;
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
      "Could not parse CPU frequency from /proc/cpuinfo, using fallback");
#else
  SPDLOG_LOGGER_DEBUG(log,
      "CPU frequency detection not available on this platform");
#endif
  return 1.0; // 回退值
}

/**
 * @brief CPU 自旋等待优化指令
 * 
 * 在自旋锁或忙等待循环中调用，提示 CPU 当前处于自旋状态。
 * 效果：
 * - 降低功耗（避免流水线过度投机执行）
 * - 减少超线程兄弟线程的竞争
 * - 在某些架构上避免内存顺序违规
 * 
 * @example
 * @code
 *   while (!lock.try_lock()) {
 *     cpu::cpu_relax();  // 优于空循环
 *   }
 * @endcode
 * 
 * @note x86: 使用 PAUSE 指令
 * @note ARM64: 使用 YIELD 指令
 * @note 其他: 使用 std::this_thread::yield()
 */
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

/// std::format support for CpuTopologyInfo.
/// Example: std::format("{}", info) → "socket=0 core=2 thread=4"
template <>
struct std::formatter<eph::utils::CpuTopologyInfo> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    auto format(const eph::utils::CpuTopologyInfo& info,
                std::format_context& ctx) const {
        return std::format_to(ctx.out(), "socket={} core={} thread={}",
                              info.socket_id, info.core_id, info.hw_thread_id);
    }
};
