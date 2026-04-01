#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#if defined(__GNUC__) || defined(__clang__)
#include <x86intrin.h>
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace eph::utils {

namespace detail {

inline const std::shared_ptr<spdlog::logger>& tsc_logger() {
    static auto l = [] {
        auto lg = spdlog::get("utils.tsc");
        if (!lg) lg = spdlog::stdout_color_mt("utils.tsc");
        // Inherit level from spdlog global default
        return lg;
    }();
    return l;
}

} // namespace detail

/**
 * @brief 高精度时间测量工具
 *
 * 基于 CPU 时间戳计数器（TSC）的纳秒级计时器。
 *
 * 使用要求：
 * - 现代 x86_64 CPU（支持 rdtscp 指令）或 ARM64（支持 cntvct_el0）
 * - 建议在程序启动时调用 TSC::init() 进行校准
 *
 * @example
 * @code
 *   // 初始化并校准
 *   if (!TSC::init()) {
 *     std::println(stderr, "TSC initialization failed");
 *     return 1;
 *   }
 *
 *   // 测量代码段执行时间
 *   uint64_t start = TSC::now();
 *   do_something();
 *   uint64_t end = TSC::now();
 *
 *   if (auto latency_ns = TSC::to_ns(end - start)) {
 *     std::println("Latency: {:.2f} ns", *latency_ns);
 *   }
 * @endcode
 *
 * @warning TSC 在某些系统上可能不稳定（如老旧 CPU 的变频、虚拟机）
 * @warning 线程迁移到不同核心可能导致 TSC 不连续（使用 rdtscp 可缓解）
 */
class TSC {
public:
  /**
   * @brief 获取当前 CPU 时间戳周期数
   *
   * 使用硬件计数器读取当前 CPU 周期数，具有极低开销（~20 cycles）。
   *
   * @return uint64_t CPU 周期计数（自系统启动以来的累计值）
   *
   * @note x86_64: 使用 rdtscp 指令（序列化读取，防止乱序执行）
   * @note ARM64: 使用 cntvct_el0 寄存器（虚拟计数器）
   * @note 其他平台: 回退到 std::chrono::steady_clock
   *
   * @warning 返回值仅在同一 CPU 核心上单调递增
   */
  [[nodiscard]] static inline uint64_t now() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    unsigned int aux;
    uint64_t tsc = __rdtscp(&aux);
    std::atomic_signal_fence(std::memory_order_seq_cst);
    return tsc;
#elif defined(__aarch64__) || defined(_M_ARM64)
    uint64_t val;
    asm volatile("isb; mrs %0, cntvct_el0" : "=r"(val) : : "memory");
    return val;
#else
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
  }

  /**
   * @brief 初始化并校准 TSC 频率
   *
   * 通过与 std::chrono::steady_clock 对比来计算 TSC 的实际频率。
   * 必须在使用 to_ns()/to_cycles() 之前调用。
   *
   * @param duration 校准持续时间，越长越精确（推荐 100-500ms）
   * @return true 校准成功
   * @return false 校准失败（TSC 不可靠或硬件不支持）
   *
   * @note 校准期间会预热 CPU（避免节能状态影响）
   * @note 自动打印 CPU 频率和可靠性警告
   *
   * @example
   * @code
   *   // 使用默认 200ms 校准时间
   *   if (!TSC::init()) {
   *     std::println(stderr, "TSC not available");
   *     std::exit(1);
   *   }
   *
   *   // 使用更长的校准时间提高精度
   *   TSC::init(std::chrono::milliseconds(500));
   * @endcode
   *
   * @note Thread-safe: concurrent calls are serialized via std::call_once.
   *       Only the first call performs calibration; subsequent calls return the
   *       cached result.
   */
  static bool
  init(std::chrono::milliseconds duration = std::chrono::milliseconds(200)) {
    std::call_once(init_flag_, [&] { do_init_(duration); });
    return initialized_.load(std::memory_order_acquire);
  }

  /**
   * @brief 将 CPU 周期数转换为纳秒
   *
   * @param cycles CPU 周期数（通常是两次 now() 调用的差值）
   * @return std::optional<double> 成功时返回纳秒数，未初始化时返回 nullopt
   *
   * @warning 必须先调用 init() 校准
   */
  [[nodiscard]] static std::optional<double> to_ns(uint64_t cycles) noexcept {
    if (!initialized_.load(std::memory_order_acquire)) [[unlikely]] {
      return std::nullopt;
    }
    return static_cast<double>(cycles) * ns_per_cycle_;
  }

  /**
   * @brief 将纳秒转换为 CPU 周期数
   *
   * @tparam Rep 数值类型（默认 double）
   * @param ns 纳秒数
   * @return std::optional<uint64_t> 成功时返回周期数，未初始化时返回 nullopt
   *
   * @warning 必须先调用 init() 校准
   */
  template <typename Rep = double>
  [[nodiscard]] static std::optional<uint64_t> to_cycles(Rep ns) noexcept {
    if (!initialized_.load(std::memory_order_acquire)) [[unlikely]] {
      return std::nullopt;
    }
    if (ns < 0) [[unlikely]] {
      return std::nullopt;
    }
    auto cycles = static_cast<double>(ns) / ns_per_cycle_;
    if (cycles > static_cast<double>(UINT64_MAX)) [[unlikely]] {
      return UINT64_MAX; // Saturate on overflow instead of UB
    }
    return static_cast<uint64_t>(cycles);
  }

  /**
   * @brief 将 std::chrono::duration 转换为 CPU 周期数
   *
   * @tparam Rep std::chrono::duration 的表示类型
   * @tparam Period std::chrono::duration 的周期类型
   * @param d 时间长度
   * @return std::optional<uint64_t> 成功时返回周期数
   *
   * @example
   * @code
   *   if (auto cycles = TSC::to_cycles(std::chrono::microseconds(100))) {
   *     std::println("100us = {} cycles", *cycles);
   *   }
   * @endcode
   */
  template <class Rep, class Period>
  [[nodiscard]] static std::optional<uint64_t>
  to_cycles(std::chrono::duration<Rep, Period> d) noexcept {
    if (!initialized_.load(std::memory_order_acquire)) [[unlikely]] {
      return std::nullopt;
    }
    double ns = std::chrono::duration<double, std::nano>(d).count();
    return to_cycles(ns);
  }

  /**
   * @brief 检查 TSC 是否已初始化
   */
  [[nodiscard]] static bool is_initialized() noexcept {
    return initialized_.load(std::memory_order_acquire);
  }

  /**
   * @brief 获取校准的纳秒/周期比率（用于高级用途）
   *
   * @return std::optional<double> 已初始化时返回比率
   */
  [[nodiscard]] static std::optional<double> get_ns_per_cycle() noexcept {
    if (!initialized_.load(std::memory_order_acquire)) {
      return std::nullopt;
    }
    return ns_per_cycle_;
  }

private:
  // ns_per_cycle_ is written exactly once by init() before initialized_ is
  // set to true.  The release store on initialized_ in init() and the acquire
  // load in to_ns()/to_cycles() establish a happens-before relationship,
  // guaranteeing that any thread observing initialized_==true will also see
  // the fully-written ns_per_cycle_ value.  This makes concurrent reads from
  // worker threads safe without requiring ns_per_cycle_ itself to be atomic.
  static inline double ns_per_cycle_ = 0.0;
  static inline std::atomic<bool> initialized_{false};
  static inline std::once_flag init_flag_;

  /// Actual calibration logic, called exactly once via std::call_once.
  static bool
  do_init_(std::chrono::milliseconds duration) {
    using namespace std::chrono;

    auto log = detail::tsc_logger();
    SPDLOG_LOGGER_INFO(log, "Calibrating TSC...");

    // === 1. 检查 TSC 可靠性 ===
    check_tsc_reliability();

    // === 2. 预热：让 CPU 退出节能状态并填充指令缓存 ===
    auto warm_up = [](auto dur) {
      auto start = steady_clock::now();
      while (steady_clock::now() - start < dur) {
        std::atomic_signal_fence(std::memory_order_relaxed);
      }
    };
    warm_up(milliseconds(20));

    // === 3. 多轮采样取中位数（提高鲁棒性）===
    constexpr int num_samples = 5;
    double samples[num_samples];

    for (int i = 0; i < num_samples; ++i) {
      auto t1 = steady_clock::now();
      uint64_t c1 = now();

      warm_up(duration);

      uint64_t c2 = now();
      auto t2 = steady_clock::now();

      if (c2 <= c1) {
        SPDLOG_LOGGER_ERROR(log, "TSC went backwards (sample {})", i + 1);
        return false;
      }

      double ns_elapsed = duration_cast<nanoseconds>(t2 - t1).count();
      double cycles_elapsed = static_cast<double>(c2 - c1);

      if (cycles_elapsed < 1.0 || ns_elapsed < 1.0) {
        SPDLOG_LOGGER_ERROR(log, "Invalid calibration sample {}", i + 1);
        return false;
      }

      samples[i] = ns_elapsed / cycles_elapsed;
    }

    // 计算中位数（更鲁棒）
    std::ranges::sort(samples);
    ns_per_cycle_ = samples[num_samples / 2];

    // === 4. 合理性检查 ===
    double freq_ghz = 1.0 / ns_per_cycle_;
    if (freq_ghz < 0.5 || freq_ghz > 10.0) {
      SPDLOG_LOGGER_ERROR(log,
          "Suspicious CPU frequency: {:.2f} GHz (expected 0.5-10 GHz)",
          freq_ghz);
      return false;
    }

    // 检查样本离散度（标准差）
    double mean = 0.0;
    for (double s : samples) {
      mean += s;
    }
    mean /= num_samples;

    double variance = 0.0;
    for (double s : samples) {
      double diff = s - mean;
      variance += diff * diff;
    }
    double stddev = std::sqrt(variance / num_samples);
    double cv = stddev / mean; // 变异系数

    if (cv > 0.01) { // 超过 1% 的变异被认为不稳定
      SPDLOG_LOGGER_WARN(log,
          "High calibration variance (CV={:.2f}%), TSC may be unstable",
          cv * 100);
    }

    // === 5. 标记为已初始化 ===
    // Release store: guarantees all preceding writes (especially ns_per_cycle_)
    // are visible to any thread that observes initialized_==true via acquire load.
    initialized_.store(true, std::memory_order_release);

    SPDLOG_LOGGER_INFO(log,
        "TSC calibrated: CPU frequency {:.2f} GHz (CV={:.2f}%)",
        freq_ghz, cv * 100);

    return true;
  }

  /**
   * @brief 检查 TSC 的可靠性（Linux x86_64）
   *
   * 读取 /proc/cpuinfo 检查：
   * - constant_tsc: TSC 频率不随 CPU 频率变化
   * - nonstop_tsc: TSC 在 C-states（节能状态）下继续计数
   * - tsc_reliable: 内核标记 TSC 为不可靠
   */
  static void check_tsc_reliability() noexcept {
    auto log = detail::tsc_logger();
#if defined(__linux__) && (defined(__x86_64__) || defined(_M_X64))
    bool has_constant_tsc = false;
    bool has_nonstop_tsc = false;
    bool has_tsc_reliable = false;

    // 读取 /proc/cpuinfo
    std::ifstream cpuinfo("/proc/cpuinfo");
    if (!cpuinfo) {
      SPDLOG_LOGGER_WARN(log, "Failed to open /proc/cpuinfo, "
                               "cannot verify TSC reliability");
    } else {
      std::string line;
      while (std::getline(cpuinfo, line)) {
        if (line.find("constant_tsc") != std::string::npos)
          has_constant_tsc = true;
        if (line.find("nonstop_tsc") != std::string::npos)
          has_nonstop_tsc = true;
        if (line.find("tsc_reliable") != std::string::npos)
          has_tsc_reliable = true;
      }

      if (!has_constant_tsc) {
        SPDLOG_LOGGER_WARN(log,
            "CPU does not support constant_tsc "
            "(frequency scaling may affect accuracy)");
      }
      if (!has_nonstop_tsc) {
        SPDLOG_LOGGER_WARN(log,
            "CPU does not support nonstop_tsc "
            "(C-states may affect accuracy)");
      }
      if (has_tsc_reliable) {
        SPDLOG_LOGGER_DEBUG(log, "TSC marked as reliable by kernel");
      }
    }
#elif defined(__aarch64__) || defined(_M_ARM64)
    // ARM64 cntvct_el0 is generally reliable
    SPDLOG_LOGGER_DEBUG(log, "Using ARM64 generic timer (cntvct_el0)");
#else
    SPDLOG_LOGGER_WARN(log,
        "Hardware TSC not available, using std::chrono (higher overhead)");
#endif
  }
};

} // namespace eph::utils
