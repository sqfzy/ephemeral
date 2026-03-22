#pragma once

#include <algorithm>
#include <charconv>
#include <fstream>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

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
 * @return std::vector<CpuTopologyInfo> CPU 拓扑信息列表
 * @throws std::runtime_error 如果解析 /proc/cpuinfo 失败（Linux）
 * 
 * @note Linux: 解析 /proc/cpuinfo
 * @note 其他平台: 返回简化拓扑（假设单 Socket）
 */
inline std::vector<CpuTopologyInfo> get_cpu_topology() {
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
  CpuTopologyInfo element{};
  unsigned valid_mask = 0;
  // Track processor IDs separately for ARM fallback (no physical id / core id)
  std::vector<unsigned> processor_ids;

  for (std::string line; getline(cpuinfo, line);) {
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
    SPDLOG_LOGGER_ERROR(log,
        "CPU topology mismatch: parsed {} CPUs, expected {}",
        cpus.size(), hw_threads);
    throw std::runtime_error("CPU topology detection failed: parsed " +
        std::to_string(cpus.size()) + " CPUs, expected " +
        std::to_string(hw_threads));
  }
  if (cpus.empty()) {
    SPDLOG_LOGGER_ERROR(log, "No CPUs found in /proc/cpuinfo");
    throw std::runtime_error("CPU topology detection failed: no CPUs found");
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
inline void set_thread_affinity(int cpu_id, const char* name = nullptr) {
  if (cpu_id < 0) return;
  auto log = detail::cpu_logger();
  const char* tag = name ? name : "thread";
#if defined(__linux__)
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(cpu_id, &cpuset);
  int ret = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  if (ret != 0) {
    SPDLOG_LOGGER_ERROR(log,
        "Failed to pin {} to cpu_id={}: {}",
        tag, cpu_id, std::generic_category().message(ret));
  } else {
    SPDLOG_LOGGER_INFO(log, "{} pinned to cpu_id={}", tag, cpu_id);
  }
#elif defined(__APPLE__)
  // macOS 不支持硬亲和性，只能设置 QoS
  SPDLOG_LOGGER_DEBUG(log,
      "macOS: setting QoS instead of hard affinity for {} (cpu_id={})",
      tag, cpu_id);
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#else
  SPDLOG_LOGGER_WARN(log,
      "Thread affinity not supported on this platform for {} (cpu_id={})",
      tag, cpu_id);
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
