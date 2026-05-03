/// @file time.hpp
/// @brief High-precision TSC (Time Stamp Counter) timing utilities.
///
/// Provides nanosecond-resolution timing based on the CPU's hardware
/// timestamp counter. On x86-64, reads `rdtscp`; on ARM64, reads
/// `cntvct_el0`; elsewhere, falls back to `std::chrono::steady_clock`.
///
/// The TSC must be calibrated once at startup via `TSC::init()` before
/// cycle-to-nanosecond conversions (`to_ns`, `to_cycles`) can be used.
///
/// @warning TSC readings may be non-monotonic across cores on older
///          hardware or under virtualization. Pin threads to specific
///          cores for reliable measurements.

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

/// @brief Lazily-initialized logger for the TSC subsystem.
inline const std::shared_ptr<spdlog::logger>& tsc_logger() {
    static auto l = [] {
        auto lg = spdlog::get("utils.tsc");
        if (!lg) {
            try { lg = spdlog::stdout_color_mt("utils.tsc"); }
            catch (const spdlog::spdlog_ex&) { lg = spdlog::get("utils.tsc"); }
        }
        if (!lg) lg = spdlog::default_logger();
        // Inherit level from spdlog global default
        return lg;
    }();
    return l;
}

} // namespace detail

/// @brief High-precision timing based on the CPU Time Stamp Counter (TSC).
///
/// Provides nanosecond-level timing with ~20-cycle overhead per read.
/// Requires a modern x86-64 CPU (`rdtscp`) or ARM64 (`cntvct_el0`).
/// Call `TSC::init()` once at startup to calibrate the counter frequency.
///
/// @code
///   if (!TSC::init()) { /* handle error */ }
///
///   uint64_t start = TSC::now();
///   do_something();
///   uint64_t end = TSC::now();
///
///   if (auto ns = TSC::to_ns(end - start)) {
///       std::println("Latency: {:.2f} ns", *ns);
///   }
/// @endcode
///
/// @warning TSC may be unstable on older CPUs with frequency scaling or
///          under virtualization.
/// @warning Thread migration across cores can cause non-monotonic reads;
///          `rdtscp` mitigates this on x86-64.
class TSC {
public:
  /// @brief Read the current CPU timestamp counter value.
  ///
  /// Reads the hardware cycle counter with minimal overhead (~20 cycles).
  ///
  /// @return Cumulative CPU cycle count since system boot.
  ///
  /// @note x86-64: uses `rdtscp` (serializing read, prevents reordering).
  /// @note ARM64: reads `cntvct_el0` virtual counter register.
  /// @note Other: falls back to `std::chrono::steady_clock`.
  ///
  /// @warning Only monotonically increasing within a single CPU core.
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

  /// @brief Calibrate the TSC frequency against the system clock.
  ///
  /// Computes the TSC-to-nanosecond ratio by comparing TSC deltas with
  /// `std::chrono::steady_clock` over the specified duration. Must be
  /// called before `to_ns()` or `to_cycles()`.
  ///
  /// @param duration Calibration window (longer = more precise; 100-500 ms
  ///                 recommended). Default is 200 ms.
  /// @return `true` on successful calibration, `false` if the TSC is
  ///         unreliable or hardware is unsupported.
  ///
  /// @note Warms up the CPU before sampling to avoid power-saving artifacts.
  /// @note Thread-safe: concurrent calls are serialized via `std::call_once`.
  ///       Only the first call performs calibration; subsequent calls return
  ///       the cached result.
  static bool
  init(std::chrono::milliseconds duration = std::chrono::milliseconds(200)) {
    std::call_once(init_flag_, [&] { do_init_(duration); });
    return initialized_.load(std::memory_order_acquire);
  }

  /// @brief Convert CPU cycles to nanoseconds.
  ///
  /// @param cycles CPU cycle count (typically a delta between two `now()` calls).
  /// @return Nanoseconds as `double`, or `nullopt` if `init()` has not been called.
  ///
  /// @warning Returns `nullopt` until `init()` completes successfully.
  [[nodiscard]] static std::optional<double> to_ns(uint64_t cycles) noexcept {
    if (!initialized_.load(std::memory_order_acquire)) [[unlikely]] {
      return std::nullopt;
    }
    return static_cast<double>(cycles) * ns_per_cycle_;
  }

  /// @brief Convert nanoseconds to CPU cycles.
  ///
  /// @tparam Rep Numeric type for the nanosecond value (default `double`).
  /// @param ns   Duration in nanoseconds.
  /// @return Cycle count, or `nullopt` if `init()` has not been called or
  ///         `ns` is negative. Saturates to `UINT64_MAX` on overflow.
  ///
  /// @warning Returns `nullopt` until `init()` completes successfully.
  template <typename Rep = double>
  [[nodiscard]] static std::optional<uint64_t> to_cycles(Rep ns) noexcept {
    if (!initialized_.load(std::memory_order_acquire)) [[unlikely]] {
      return std::nullopt;
    }
    // Use !(ns >= 0) instead of (ns < 0) to also reject NaN inputs.
    // NaN comparisons always return false, so (NaN < 0) is false and
    // would let NaN pass through to static_cast<uint64_t>(NaN) = UB.
    if (!(ns >= 0)) [[unlikely]] {
      return std::nullopt;
    }
    auto cycles = static_cast<double>(ns) / ns_per_cycle_;
    // `static_cast<double>(UINT64_MAX)` rounds UP to 2^64 because UINT64_MAX
    // is unrepresentable in IEEE-754 double (53-bit mantissa). Using `<=` would
    // let `cycles == 2^64` through, and the subsequent
    // `static_cast<uint64_t>(2^64)` is undefined behavior per [conv.fpint].
    // Use strict `<` so any double whose mathematical value is at least
    // UINT64_MAX (including the rounded-up boundary alias) is saturated.
    if (!(cycles < static_cast<double>(UINT64_MAX))) [[unlikely]] {
      return UINT64_MAX; // Saturate on overflow/NaN instead of UB
    }
    return static_cast<uint64_t>(cycles);
  }

  /// @brief Convert a `std::chrono::duration` to CPU cycles.
  ///
  /// @tparam Rep    Duration's representation type.
  /// @tparam Period Duration's tick period.
  /// @param d       Duration to convert.
  /// @return Cycle count, or `nullopt` if uncalibrated.
  ///
  /// @code
  ///   if (auto c = TSC::to_cycles(std::chrono::microseconds(100))) {
  ///       std::println("100us = {} cycles", *c);
  ///   }
  /// @endcode
  template <class Rep, class Period>
  [[nodiscard]] static std::optional<uint64_t>
  to_cycles(std::chrono::duration<Rep, Period> d) noexcept {
    if (!initialized_.load(std::memory_order_acquire)) [[unlikely]] {
      return std::nullopt;
    }
    double ns = std::chrono::duration<double, std::nano>(d).count();
    return to_cycles(ns);
  }

  /// @brief Compute the elapsed time in nanoseconds between two TSC readings.
  ///
  /// Convenience wrapper that combines cycle delta with to_ns() conversion.
  /// Useful for simple timing patterns without ScopedTSC/measure_tsc.
  ///
  /// @param start TSC value from a previous now() call.
  /// @param end   TSC value from a later now() call (must be >= start).
  /// @return Elapsed nanoseconds, or `nullopt` if uncalibrated OR if
  ///         `end < start` (caller violated the precondition — usually
  ///         a sign of thread migration across cores with non-coherent
  ///         TSCs, or a swapped argument order). Returning nullopt
  ///         instead of letting the unsigned underflow silently produce
  ///         a multi-second "latency" that pollutes downstream
  ///         percentile aggregation.
  ///
  /// @code
  ///   auto start = TSC::now();
  ///   do_work();
  ///   if (auto ns = TSC::delta_ns(start, TSC::now())) {
  ///       SPDLOG_INFO("work took {:.1f} ns", *ns);
  ///   }
  /// @endcode
  [[nodiscard]] static std::optional<double>
  delta_ns(uint64_t start, uint64_t end) noexcept {
    if (end < start) [[unlikely]] {
      // Underflow guard: end < start means either (a) the caller
      // swapped arguments, or (b) the two reads were taken on
      // different cores whose TSCs are not coherent (modest
      // unsafe-VM / older-CPU hazard despite rdtscp's serialization).
      // Both are bugs in caller's measurement setup; surfacing as
      // nullopt forces the caller to handle it explicitly rather
      // than getting a 1.8e10 ns "fast" sample.
      SPDLOG_LOGGER_WARN(detail::tsc_logger(),
        "TSC::delta_ns: end < start (start={} end={}) — "
        "thread migrated across cores with non-coherent TSCs, "
        "or arguments swapped; returning nullopt",
        start, end);
      return std::nullopt;
    }
    return to_ns(end - start);
  }

  /// @brief Check whether TSC calibration has completed.
  /// @return `true` if `init()` succeeded, `false` otherwise.
  [[nodiscard]] static bool is_initialized() noexcept {
    return initialized_.load(std::memory_order_acquire);
  }

  /// @brief Get the calibrated nanoseconds-per-cycle ratio.
  ///
  /// Intended for advanced use (e.g., manual batch conversions).
  ///
  /// @return The ratio if calibrated, `nullopt` otherwise.
  [[nodiscard]] static std::optional<double> get_ns_per_cycle() noexcept {
    if (!initialized_.load(std::memory_order_acquire)) {
      return std::nullopt;
    }
    return ns_per_cycle_;
  }

  /// @brief Get the coefficient of variation (CV) from calibration.
  ///
  /// A CV > 0.01 (1%) indicates the TSC may be unstable due to
  /// frequency scaling, virtualization, or other factors.
  ///
  /// @return CV value in `[0, 1]` if calibrated, `nullopt` otherwise.
  [[nodiscard]] static std::optional<double> get_calibration_cv() noexcept {
    if (!initialized_.load(std::memory_order_acquire)) {
      return std::nullopt;
    }
    return calibration_cv_;
  }

private:
  // ns_per_cycle_ is written exactly once by init() before initialized_ is
  // set to true.  The release store on initialized_ in init() and the acquire
  // load in to_ns()/to_cycles() establish a happens-before relationship,
  // guaranteeing that any thread observing initialized_==true will also see
  // the fully-written ns_per_cycle_ value.  This makes concurrent reads from
  // worker threads safe without requiring ns_per_cycle_ itself to be atomic.
  static inline double ns_per_cycle_ = 0.0;
  /// Coefficient of variation from calibration sampling (0.0 until calibrated).
  static inline double calibration_cv_ = 0.0;
  static inline std::atomic<bool> initialized_{false};
  static inline std::once_flag init_flag_;

  /// Actual calibration logic, called exactly once via std::call_once.
  static bool
  do_init_(std::chrono::milliseconds duration) {
    using namespace std::chrono;

    auto log = detail::tsc_logger();
    SPDLOG_LOGGER_INFO(log, "Calibrating TSC...");

    // === 1. Check TSC reliability ===
    check_tsc_reliability();

    // === 2. Warm up: bring CPU out of power-saving state and fill instruction cache ===
    auto warm_up = [](auto dur) {
      auto start = steady_clock::now();
      while (steady_clock::now() - start < dur) {
        std::atomic_signal_fence(std::memory_order_relaxed);
      }
    };
    warm_up(milliseconds(20));

    // === 3. Multi-round sampling with median selection (improves robustness) ===
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

    // Select the median sample (more robust than mean against outliers)
    std::ranges::sort(samples);
    ns_per_cycle_ = samples[num_samples / 2];

    // === 4. Sanity check ===
    double freq_ghz = 1.0 / ns_per_cycle_;
    if (freq_ghz < 0.5 || freq_ghz > 10.0) {
      SPDLOG_LOGGER_ERROR(log,
          "Suspicious CPU frequency: {:.2f} GHz (expected 0.5-10 GHz)",
          freq_ghz);
      return false;
    }

    // Check sample dispersion (standard deviation)
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
    double cv = stddev / mean; // coefficient of variation

    // Store calibration CV so callers can query it via get_calibration_cv().
    calibration_cv_ = cv;

    if (cv > 0.01) { // >1% variation is considered unstable
      SPDLOG_LOGGER_ERROR(log,
          "High calibration variance (CV={:.2f}%), TSC may be unstable — "
          "timing measurements are unreliable",
          cv * 100);
    }

    // === 5. Mark as initialized ===
    // Release store: guarantees all preceding writes (especially ns_per_cycle_)
    // are visible to any thread that observes initialized_==true via acquire load.
    initialized_.store(true, std::memory_order_release);

    SPDLOG_LOGGER_INFO(log,
        "TSC calibrated: CPU frequency {:.2f} GHz (CV={:.2f}%)",
        freq_ghz, cv * 100);

    return true;
  }

  /// @brief Check TSC reliability flags (Linux x86-64 only).
  ///
  /// Reads `/proc/cpuinfo` and warns if critical flags are missing:
  /// - `constant_tsc`: frequency invariant across CPU P-states.
  /// - `nonstop_tsc`: counter continues in C-states (sleep).
  /// - `tsc_reliable`: kernel marks TSC as trustworthy.
  static void check_tsc_reliability() noexcept {
    auto log = detail::tsc_logger();
#if defined(__linux__) && (defined(__x86_64__) || defined(_M_X64))
    bool has_constant_tsc = false;
    bool has_nonstop_tsc = false;
    bool has_tsc_reliable = false;

    // Read /proc/cpuinfo
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
