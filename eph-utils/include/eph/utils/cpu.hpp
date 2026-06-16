/// @file cpu.hpp
/// @brief CPU topology discovery, thread pinning, real-time scheduling, and spin-wait helpers.
///
/// Provides utilities for pinning threads to specific CPU cores (with
/// optional isolcpus / SMT sibling / NUMA / IRQ-overlap validation),
/// querying physical topology (socket/core/thread), enabling real-time
/// scheduling policies, and issuing architecture-specific spin-wait
/// hints.
///
/// Platform support:
/// - Linux: full support (via `/proc/cpuinfo`, `/sys/devices/system/cpu/*`,
///   `/proc/interrupts`, and `pthread` APIs).
/// - macOS: partial (no hard affinity; uses QoS / time-constraint policy).
/// - Windows: basic / stub.

#pragma once

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <expected>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/mman.h>
#endif

#include "eph/core/log.hpp"

// for _mm_pause
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||             \
    defined(_M_IX86)
#include <immintrin.h>
#endif

namespace eph::utils {

namespace detail {

/// @brief Lazily-initialized logger for the CPU utilities subsystem.
inline spdlog::logger* cpu_logger() {
    static spdlog::logger* l = ::eph::log::get("utils.cpu");
    return l;
}

// ─── pin_thread helpers (Linux-only path; stubs compiled away elsewhere) ──
//
// Previously lived under `cpu_pin_detail` in the now-retired cpu_pin.hpp.
// Consolidated here so cpu.hpp owns the entire thread-affinity surface.

/// Process-wide registry of pinned cpus — lets `pin_thread` detect
/// SMT-sibling / NUMA conflicts across threads. Thread-safe.
///
/// `g_pinned_owner_role` carries a short diagnostic label per cpu (the
/// pthread name passed to `pin_thread`, or the role string passed to
/// `register_external_pin`) so conflict messages can name the culprit
/// instead of just printing the cpu id. Always written / read under
/// `g_pin_mutex`; absence in the map is fine — error formatters render
/// "(by ?)" or omit the suffix.
inline std::mutex g_pin_mutex;
inline std::set<int> g_pinned_cpus;
inline std::map<int, std::string> g_pinned_owner_role;

/// Parse a `/sys` cpu list file (e.g. `isolated`, `thread_siblings_list`),
/// expanding comma-separated ranges like `1-3,5` into individual ids.
/// Returns an empty set if the file is missing/empty.
inline std::set<int> read_cpu_list_file(const std::string& path) {
    std::set<int> out;
    std::ifstream f(path);
    if (!f.is_open()) return out;
    std::string line;
    if (!std::getline(f, line)) return out;

    while (!line.empty() &&
           (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
    }
    if (line.empty()) return out;

    // Sanity bound on individual CPU ids and on range expansion. Linux's
    // CONFIG_NR_CPUS theoretical maximum is 8192; real-world hosts top out
    // around 1024. A malformed / hostile sysfs file containing
    // "0-2147483647" would otherwise expand into INT_MAX inserts here,
    // a runaway std::set growth that exhausts memory before the caller
    // even sees the result. The cap also coincidentally rejects negative
    // CPU ids (stoi handles "-5" but a negative id is meaningless to
    // every consumer of this API).
    constexpr int kMaxCpuId = 8192;
    std::stringstream ss(line);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token.empty()) continue;
        auto dash = token.find('-');
        try {
            if (dash == std::string::npos) {
                int id = std::stoi(token);
                if (id >= 0 && id < kMaxCpuId) out.insert(id);
            } else {
                int lo = std::stoi(token.substr(0, dash));
                int hi = std::stoi(token.substr(dash + 1));
                if (lo < 0) lo = 0;
                if (hi >= kMaxCpuId) hi = kMaxCpuId - 1;
                // Cap inserts per token: a 0..8191 range is 8192 entries
                // — already larger than any sane inventory but bounded.
                for (int i = lo; i <= hi; ++i) out.insert(i);
            }
        } catch (...) {
            // Ignore malformed tokens; best effort.
        }
    }
    return out;
}

/// Return the NUMA node id of `cpu` by probing `/sys/.../cpuN/node*`,
/// or -1 on a non-NUMA system / unreadable sysfs.
inline int read_numa_node(int cpu) {
    for (int node = 0; node < 64; ++node) {
        std::string p = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                        "/node" + std::to_string(node);
        if (access(p.c_str(), F_OK) == 0) return node;
    }
    return -1;
}

/// Check whether `cpu`'s column in /proc/interrupts has any non-zero IRQ
/// count. Best effort — unreadable /proc or parse errors return false.
inline bool cpu_has_active_irq(int cpu) {
    std::ifstream f("/proc/interrupts");
    if (!f.is_open()) return false;

    std::string header;
    if (!std::getline(f, header)) return false;

    std::stringstream hss(header);
    std::vector<std::string> cols;
    for (std::string tok; hss >> tok;) cols.push_back(tok);

    int col_idx = -1;
    const std::string want = "CPU" + std::to_string(cpu);
    for (size_t i = 0; i < cols.size(); ++i) {
        if (cols[i] == want) { col_idx = static_cast<int>(i); break; }
    }
    if (col_idx < 0) return false;

    std::string line;
    while (std::getline(f, line)) {
        std::stringstream lss(line);
        std::vector<std::string> toks;
        for (std::string tok; lss >> tok;) toks.push_back(tok);
        // Row shape: `<irq>:  <cnt0> <cnt1> ... <ctl>`. After split, the
        // count column is at position col_idx + 1 (header has no leading
        // label, but the row starts with "<irq>:").
        const size_t want_idx = static_cast<size_t>(col_idx + 1);
        if (toks.size() <= want_idx) continue;
        const auto& cnt = toks[want_idx];
        bool numeric = !cnt.empty();
        for (char c : cnt) if (c < '0' || c > '9') { numeric = false; break; }
        if (!numeric) continue;
        try {
            if (std::stoll(cnt) > 0) return true;
        } catch (...) {}
    }
    return false;
}

/// Set the OS-level thread name (shown in `top -H`, `gdb`). Max 15 chars + NUL.
inline void set_thread_name(std::string_view name) noexcept {
    char buf[16] = {};
    auto n = std::min(name.size(), sizeof(buf) - 1);
    // ISO C requires src/dst to be valid pointers even when count==0
    // (UBSan -fsanitize=undefined catches this). std::string_view::data()
    // may legitimately return nullptr for an empty view (e.g. a default-
    // constructed std::string_view{}). Mirrors the equivalent batch-2 guard
    // in eph-net/include/eph/net/hmac_sha256.hpp.
    if (n > 0) std::memcpy(buf, name.data(), n);
#if defined(__linux__)
    pthread_setname_np(pthread_self(), buf);
#else
    (void)buf;
#endif
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

  EPH_LOG_DEBUG(log, "Detecting CPU topology");

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
    EPH_LOG_WARN(log, "Failed to open /proc/cpuinfo, "
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
        EPH_LOG_TRACE(log,
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
    EPH_LOG_INFO(log,
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
    EPH_LOG_ERROR(log, "{}", msg);
    return std::unexpected(std::move(msg));
  }
  if (cpus.empty()) {
    auto msg = std::string("CPU topology detection failed: no CPUs found");
    EPH_LOG_ERROR(log, "{}", msg);
    return std::unexpected(std::move(msg));
  }

  // Sort by hw_thread_id
  std::ranges::sort(cpus, {}, &CpuTopologyInfo::hw_thread_id);
#else
  // macOS/Windows fallback
  EPH_LOG_DEBUG(log, "Using simplified topology (non-Linux)");
  for (unsigned i = 0; i < std::thread::hardware_concurrency(); ++i) {
    cpus.push_back({0, i, i});
  }
#endif

  EPH_LOG_INFO(log, "Detected {} CPUs", cpus.size());
  return cpus;
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
    EPH_LOG_ERROR(log, "{}", msg);
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
    EPH_LOG_ERROR(log, "{}", msg);
    return std::unexpected(std::move(msg));
  }

  EPH_LOG_INFO(log, "{} set to {} priority={} (range {}-{})",
      tag, policy_name, priority, min_prio, max_prio);
  return {};

#elif defined(__APPLE__)
  // macOS: use THREAD_TIME_CONSTRAINT_POLICY for near-realtime behavior.
  // This requests the Mach scheduler to treat this thread as time-critical.
  EPH_LOG_DEBUG(log,
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
    EPH_LOG_WARN(log, "{}", msg);
    return std::unexpected(std::move(msg));
  }
  EPH_LOG_INFO(log, "{} set to THREAD_TIME_CONSTRAINT_POLICY", tag);
  return {};

#else
  auto msg = std::format(
      "Realtime scheduling not supported on this platform for {}", tag);
  EPH_LOG_WARN(log, "{}", msg);
  return std::unexpected(std::move(msg));
#endif
}

/// @brief Options for lock_memory().
struct LockMemoryOptions {
    /// Lock pages already mapped (MCL_CURRENT). Default true.
    bool current = true;
    /// Lock pages mapped by future allocations (MCL_FUTURE). Default true.
    /// Combined with current, this is the canonical HFT setting: every
    /// page touched by the process — heap, stack, mmap'd region — stays
    /// resident for the process lifetime, eliminating page-fault tail
    /// spikes during measurement / production hot loops.
    bool future = true;
    /// Lock pages lazily on fault (MCL_ONFAULT, Linux ≥ 4.4). Reduces
    /// the up-front RSS cost (don't pre-fault unused regions) at the
    /// price of a one-time fault on first access. Most HFT users want
    /// the eager behaviour (default false). Ignored on platforms
    /// without MCL_ONFAULT.
    bool on_fault = false;
};

/// @brief Lock the calling process's address space into RAM.
///
/// Wraps `mlockall` with structured options + actionable error
/// diagnostics. Intended use: the bench / production hot path calls
/// this **after** thread pinning so subsequent allocations on the
/// pinned core are immune to page-fault preemption.
///
/// @param opts Bitset of mlockall flags. See LockMemoryOptions.
/// @param tag  Optional label for log messages (e.g. "lat_ws_client").
/// @return ok() on success; otherwise an actionable error message
///         identifying the likely cause (`EPERM` → root / CAP_IPC_LOCK
///         missing; `ENOMEM` → ulimit -l too small; `EINVAL` → bad flag
///         combination).
///
/// @note Linux: `mlockall(MCL_CURRENT | MCL_FUTURE [| MCL_ONFAULT])`.
/// @note macOS: `mlockall` with same MCL_CURRENT / MCL_FUTURE semantics
///       (no MCL_ONFAULT — silently dropped).
/// @note Other platforms: returns an error.
///
/// @warning A process with `MCL_FUTURE` that exhausts `RLIMIT_MEMLOCK`
///          will start to hit `ENOMEM` on subsequent `mmap` / heap
///          growth. Set `ulimit -l unlimited` for HFT workloads, or
///          size the limit comfortably above peak RSS.
[[nodiscard]] inline std::expected<void, std::string>
lock_memory(LockMemoryOptions opts = {}, const char* tag = nullptr) {
    [[maybe_unused]] auto log = detail::cpu_logger();
    [[maybe_unused]] const char* label = tag ? tag : "process";

#if defined(__linux__) || defined(__APPLE__)
    int flags = 0;
    if (opts.current) flags |= MCL_CURRENT;
    if (opts.future)  flags |= MCL_FUTURE;
#  if defined(MCL_ONFAULT)
    if (opts.on_fault) flags |= MCL_ONFAULT;
#  else
    if (opts.on_fault) {
        EPH_LOG_DEBUG(log,
            "[{}] lock_memory: MCL_ONFAULT not available on this platform; "
            "ignoring on_fault=true", label);
    }
#  endif

    if (flags == 0) {
        // Caller passed all-false options; treat as a no-op rather than
        // an error — easier to wire `lock_memory({.current=cfg.lock})`
        // style without conditionals.
        EPH_LOG_INFO(log,
            "[{}] lock_memory: no flags requested, no-op", label);
        return {};
    }

    if (::mlockall(flags) == 0) {
        EPH_LOG_INFO(log,
            "[{}] lock_memory: ok (current={} future={} on_fault={})",
            label, opts.current, opts.future, opts.on_fault);
        return {};
    }

    const int err = errno;
    // Pull the current RLIMIT_MEMLOCK so the diagnostic is actionable
    // without the user having to chase ulimit -l themselves.
    [[maybe_unused]] rlim_t mlock_max = 0;
#  if defined(__linux__)
    rlimit rl{};
    if (::getrlimit(RLIMIT_MEMLOCK, &rl) == 0) {
        mlock_max = rl.rlim_cur;
    }
#  endif

    std::string hint;
    switch (err) {
        case EPERM:
            hint = "run as root, or grant CAP_IPC_LOCK: "
                   "setcap cap_ipc_lock+ep <binary>";
            break;
        case ENOMEM:
            hint = std::format(
                "RLIMIT_MEMLOCK ({} bytes) is too small for working set; "
                "set ulimit -l unlimited or raise the limit",
                static_cast<unsigned long long>(mlock_max));
            break;
        case EINVAL:
            hint = "invalid mlockall flag combination "
                   "(check on_fault on older kernels)";
            break;
        case EAGAIN:
            hint = "kernel could not lock all pages "
                   "(RLIMIT_MEMLOCK partial fail); retry or relax options";
            break;
        default:
            hint = std::generic_category().message(err);
            break;
    }
    auto msg = std::format(
        "[{}] lock_memory: mlockall(flags={:#x}) failed: {} ({})",
        label, flags, std::generic_category().message(err), hint);
    EPH_LOG_ERROR(log, "{}", msg);
    return std::unexpected(std::move(msg));

#else
    auto msg = std::format(
        "[{}] lock_memory: not supported on this platform", label);
    EPH_LOG_WARN(log, "{}", msg);
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
    EPH_LOG_WARN(log, "Failed to open /proc/cpuinfo, "
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
      EPH_LOG_DEBUG(log, "CPU base frequency: {:.2f} GHz", freq);
      return freq;
    }
  }
  EPH_LOG_WARN(log,
      "Could not parse CPU frequency from /proc/cpuinfo");
#else
  EPH_LOG_DEBUG(log,
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
  _mm_pause(); // Hint to CPU this is a spin loop, reducing power and avoiding pipeline stalls
#elif defined(__aarch64__)
  asm volatile("yield"); // ARM64
#else
  std::this_thread::yield(); // Fallback
#endif
}

// ──────────────────────────────────────────────────────────────────────
// Thread pinning with optional topology validation (formerly cpu_pin.hpp)
//
// Scope: arbitrary user-spawned threads (mockex / latency client / app
// worker / bench harness). For **DPDK EAL lcore threads** prefer the
// purpose-built `eph::dpdk::pin_lcore` / `pin_lcores` /
// typed `EalGuard::init` API in `eph/dpdk/lcore_pin.hpp` — EAL itself
// performs the `pthread_setaffinity_np` (the worker lcore thread doesn't
// exist yet at the point you'd want to pin it), so the dpdk path is
// "register the cpu pre-EAL, let `rte_eal_init` do the bind". Both paths
// share this file's process-wide pin registry, so a strict `pin_thread`
// on a non-lcore worker still detects SMT / NUMA conflicts against
// running EAL lcores. See `eph-net-dpdk/docs/lcore-pin-integration.md`.
// ──────────────────────────────────────────────────────────────────────

/// @brief Policy controlling which topology checks `pin_thread` enforces.
///
/// Every flag defaults to `false` — the default-constructed policy is
/// "best-effort pin, no validation". Opt into checks explicitly when you
/// need the HFT / production guarantees:
///
/// @code
///   CpuPinPolicy strict{
///       .require_isolcpus            = true,
///       .require_no_sibling_conflict = true,
///       .require_same_numa           = true,
///       .warn_irq_overlap            = true,
///   };
///   auto r = pin_thread(cpu, "poll", strict);
/// @endcode
///
/// Rationale for relaxed default: every in-repo caller passes an
/// all-false policy (mockex, latency/core/pin_client.hpp, bench targets);
/// making relaxed the default eliminates boilerplate and matches
/// the dev-host reality of HFT libraries that ship without isolcpus.
struct CpuPinPolicy {
    bool require_isolcpus            = false;  ///< fail if cpu is not in /sys/.../isolated
    bool require_no_sibling_conflict = false;  ///< fail if an SMT sibling is already pinned
    bool require_same_numa           = false;  ///< fail if NUMA node differs from prior pins
    bool warn_irq_overlap            = false;  ///< warn if /proc/interrupts shows IRQs on cpu
};

namespace detail {

/// Run the four policy checks (isolcpus / SMT sibling / NUMA / IRQ-warn) shared
/// by `pin_thread` and any future external-pin registrar. Reads only sysfs / proc
/// + the process-wide pinned-cpu registry, so it's a pure-ish validator (no
/// pthread side effects). Linux-only; non-Linux returns ok unconditionally.
[[nodiscard]] inline std::expected<void, std::string>
validate_pin_policy(int cpu, CpuPinPolicy const& policy) {
#if defined(__linux__)
    if (policy.require_isolcpus) {
        auto isolated = read_cpu_list_file("/sys/devices/system/cpu/isolated");
        if (!isolated.contains(cpu)) {
            return std::unexpected(
                "cpu " + std::to_string(cpu) +
                " is not in /sys/devices/system/cpu/isolated "
                "(pass policy.require_isolcpus=false to relax)");
        }
    }

    if (policy.require_no_sibling_conflict) {
        std::lock_guard g(g_pin_mutex);
        auto siblings = read_cpu_list_file(
            "/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
            "/topology/thread_siblings_list");
        for (int s : siblings) {
            if (s == cpu) continue;
            if (g_pinned_cpus.contains(s)) {
                return std::unexpected(
                    "cpu " + std::to_string(cpu) +
                    " shares an SMT physical core with already-pinned cpu " +
                    std::to_string(s));
            }
        }
    }

    if (policy.require_same_numa) {
        std::lock_guard g(g_pin_mutex);
        int node = read_numa_node(cpu);
        if (node >= 0) {
            for (int p : g_pinned_cpus) {
                int pnode = read_numa_node(p);
                if (pnode >= 0 && pnode != node) {
                    return std::unexpected(
                        "cpu " + std::to_string(cpu) + " (NUMA " +
                        std::to_string(node) +
                        ") differs from already-pinned cpu " +
                        std::to_string(p) + " (NUMA " +
                        std::to_string(pnode) + ")");
                }
            }
        }
    }

    if (policy.warn_irq_overlap && cpu_has_active_irq(cpu)) {
        EPH_LOG_WARN(detail::cpu_logger(), 
            "pin_thread: cpu {} has active IRQs in /proc/interrupts "
            "(rebind NIC IRQs via /proc/irq/N/smp_affinity)", cpu);
    }
    return {};
#else
    (void)cpu; (void)policy;
    return {};
#endif
}

} // namespace detail

// Forward decls so PinGuard's destructor can call unregister_external_pin.
inline void unregister_external_pin(int cpu) noexcept;

/// @brief Move-only RAII guard for a single cpu in the process-wide pin
/// registry. Destruction calls `unregister_external_pin(cpu)`; `release()`
/// detaches ownership so the registry entry survives.
///
/// The guard does NOT undo `pthread_setaffinity_np` — the OS-level affinity
/// stays in effect on the thread that called `pin_thread`. Only the
/// **registry** entry is reverted. This matches the contract the registry
/// promises: it tracks "who claims this cpu", not "is the kernel scheduler
/// limited to this cpu". A thread that wants to truly un-pin itself must
/// pin to the original mask separately.
///
/// Empty / default-constructed guards (`cpu_ == -1`) are no-ops — useful
/// for "maybe pin" patterns (`PinGuard g; if (...) g = std::move(*r);`).
class PinGuard {
public:
    PinGuard() noexcept = default;
    PinGuard(PinGuard&& other) noexcept : cpu_(other.cpu_) { other.cpu_ = -1; }
    PinGuard& operator=(PinGuard&& other) noexcept {
        if (this != &other) {
            if (cpu_ >= 0) unregister_external_pin(cpu_);
            cpu_       = other.cpu_;
            other.cpu_ = -1;
        }
        return *this;
    }
    PinGuard(const PinGuard&)            = delete;
    PinGuard& operator=(const PinGuard&) = delete;

    ~PinGuard() noexcept {
        if (cpu_ >= 0) unregister_external_pin(cpu_);
    }

    [[nodiscard]] int  cpu()   const noexcept { return cpu_; }
    [[nodiscard]] bool empty() const noexcept { return cpu_ < 0; }

    /// Detach ownership: destructor becomes a no-op, registry entry stays.
    /// Use when the pin should outlive the guard scope (e.g. a worker
    /// thread that runs for the whole program — the OS affinity sticks
    /// regardless, and we want the registry to reflect that).
    void release() noexcept { cpu_ = -1; }

    /// Wrap an already-registered cpu into a guard. Used by `pin_thread`
    /// and `eph::dpdk::pin_lcore` once they've successfully registered.
    /// **Caller must have written to the registry first** — `adopt` does
    /// not check the registry state.
    [[nodiscard]] static PinGuard adopt(int cpu) noexcept {
        PinGuard g;
        g.cpu_ = cpu;
        return g;
    }

private:
    int cpu_ = -1;
};

/// @brief Pin the calling thread to @p cpu with policy-driven validation.
///
/// Steps, in order (Linux path):
///   1. isolcpus check  (unless policy.require_isolcpus == false)
///   2. SMT sibling check against the process-wide pinned-cpu registry
///   3. NUMA check against the registry
///   4. IRQ overlap warn-only scan of /proc/interrupts
///   5. pthread_setaffinity_np + pthread_getaffinity_np verification
///   6. register cpu, set pthread name
///
/// On macOS / other platforms, falls back to QoS-only (no hard affinity);
/// all validation policy bits are ignored.
///
/// @param cpu     logical cpu id (must be >= 0)
/// @param name    short thread label (≤ 15 chars for pthread_setname_np)
/// @param policy  which checks to enforce (default: all relaxed)
/// @return `PinGuard` on success (RAII unregister-on-drop; call
///         `.release()` for permanent-pin semantics); error string on
///         failure. The pthread affinity is left in whatever state the
///         OS reports — see the policy comment in the body for the
///         post-`pthread_setaffinity_np` failure mode.
///
/// @note **Do NOT use this for DPDK EAL lcore threads.** Worker lcore
///       threads are `pthread_create`d *inside* `rte_eal_init`, so you
///       can't call `pin_thread` on them from the control thread, and
///       calling it from inside the lcore body races EAL's own affinity
///       setup. Use `eph::dpdk::pin_lcore` / `pin_lcores` /
///       typed `EalGuard::init` instead — they validate against
///       this same registry pre-EAL and let `rte_eal_init` perform the
///       actual `setaffinity` from the `--lcores=N@cpu` argv. Mixing
///       the two paths still composes correctly: `pin_thread` on a
///       non-lcore worker will see lcore cpus as occupied (SMT / NUMA
///       conflict diagnostics included). Full rationale and escape-
///       hatch rules: `eph-net-dpdk/docs/lcore-pin-integration.md`.
[[nodiscard]] inline std::expected<PinGuard, std::string>
pin_thread(int cpu, std::string_view name, CpuPinPolicy policy = {}) {
    if (cpu < 0) {
        EPH_LOG_ERROR(detail::cpu_logger(), "pin_thread('{}'): cpu={} must be >= 0", name, cpu);
        return std::unexpected("pin_thread: cpu must be >= 0");
    }

#if defined(__linux__)
    if (auto v = detail::validate_pin_policy(cpu, policy); !v) {
        EPH_LOG_ERROR(detail::cpu_logger(), "pin_thread('{}'): validate_pin_policy(cpu={}) "
                      "failed: {}", name, cpu, v.error());
        return std::unexpected(std::move(v.error()));
    }

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
        rc != 0) {
        EPH_LOG_ERROR(detail::cpu_logger(), "pin_thread('{}'): pthread_setaffinity_np(cpu={}) "
                      "failed: {} (rc={})",
                      name, cpu, std::strerror(rc), rc);
        return std::unexpected(
            std::string("pthread_setaffinity_np failed: ") + std::strerror(rc));
    }

    cpu_set_t verify;
    CPU_ZERO(&verify);
    if (pthread_getaffinity_np(pthread_self(), sizeof(verify), &verify) != 0) {
        EPH_LOG_ERROR(detail::cpu_logger(), "pin_thread('{}'): pthread_getaffinity_np(cpu={}) "
                      "failed: {} (errno={})",
                      name, cpu, std::strerror(errno), errno);
        return std::unexpected(
            std::string("pthread_getaffinity_np failed: ") +
            std::strerror(errno));
    }
    if (!CPU_ISSET(cpu, &verify) || CPU_COUNT(&verify) != 1) {
        EPH_LOG_ERROR(detail::cpu_logger(), "pin_thread('{}'): affinity verification mismatch — "
                      "requested cpu={}, verified isset={}, cpu_count={}",
                      name, cpu, CPU_ISSET(cpu, &verify), CPU_COUNT(&verify));
        return std::unexpected(
            "affinity verification mismatch for cpu " + std::to_string(cpu));
    }

    // Final registry update: refuse if the cpu has been claimed concurrently
    // (by another pin_thread or by register_external_pin between the policy
    // check above and now). pthread_setaffinity_np already ran successfully,
    // so the *OS* affinity is in effect even if we reject here — the caller
    // is informed via the unexpected error and is responsible for whatever
    // recovery makes sense (rebind elsewhere, accept the OS state, etc.).
    {
        std::lock_guard g(detail::g_pin_mutex);
        if (detail::g_pinned_cpus.contains(cpu)) {
            auto it = detail::g_pinned_owner_role.find(cpu);
            std::string by = (it != detail::g_pinned_owner_role.end() && !it->second.empty())
                                 ? std::format(" by {}", it->second)
                                 : std::string{};
            // Log AFTER the registry probe so the conflict is visible — the
            // OS affinity is already in effect (pthread_setaffinity_np
            // returned success above), so this is the only chance for an
            // operator to see *who* the colliding owner is.
            EPH_LOG_WARN(detail::cpu_logger(), "pin_thread('{}'): cpu {} already pinned{} "
                         "— OS affinity is in effect but registry rejected",
                         name, cpu, by);
            return std::unexpected(std::format(
                "pin_thread: cpu {} already pinned{}", cpu, by));
        }
        detail::g_pinned_cpus.insert(cpu);
        detail::g_pinned_owner_role[cpu] = std::string{name};
    }
    detail::set_thread_name(name);

    EPH_LOG_INFO(detail::cpu_logger(), "pin_thread: '{}' pinned to cpu {} (verified)", name, cpu);
    return PinGuard::adopt(cpu);
#elif defined(__APPLE__)
    (void)policy;
    detail::set_thread_name(name);
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    EPH_LOG_INFO(detail::cpu_logger(), "pin_thread: '{}' QoS set (macOS has no hard affinity)", name);
    // No registry entry on macOS (no hard affinity to track) — return
    // an empty guard so callers' API shape stays consistent.
    return PinGuard{};
#else
    (void)policy;
    EPH_LOG_ERROR(detail::cpu_logger(), "pin_thread('{}'): hard affinity not supported on this "
                  "platform (cpu={} requested)",
                  name, cpu);
    return std::unexpected(
        "pin_thread: hard affinity not supported on this platform");
#endif
}

/// @brief 1-parameter overload: anonymous best-effort pin.
///
/// Equivalent to `pin_thread(cpu, "", CpuPinPolicy{})`. Convenience for
/// microbenchmarks and quick scripts that don't care about role tags.
[[nodiscard]] inline std::expected<PinGuard, std::string>
pin_thread(int cpu) noexcept {
    return pin_thread(cpu, std::string_view{}, CpuPinPolicy{});
}

// ──────────────────────────────────────────────────────────────────────
// External-pin registration (DPDK lcore / RT framework integration)
// ──────────────────────────────────────────────────────────────────────
//
// Surfaces the process-wide pin registry that `pin_thread` already maintains
// so external mechanisms — DPDK EAL lcores in particular — can declare which
// cpus they are about to bind. With the cpu in the registry, subsequent
// `pin_thread(.., {require_no_sibling_conflict=true})` calls see the
// occupation and fail fast on SMT / NUMA conflicts instead of silently
// fighting the lcore for cycles.
//
// `pin_thread` itself does not call `register_external_pin` — its successful
// path inserts straight into `detail::g_pinned_cpus` (see stage 3 for
// duplicate-pin tightening). External registrars must use these APIs.

/// @brief Mark @p cpu as already-pinned by an external mechanism.
///
/// Use when a non-`pin_thread` mechanism (DPDK EAL lcore, an RT framework,
/// a binding done by a parent process before fork) has bound a thread to
/// @p cpu. The cpu enters the same process-wide registry that `pin_thread`
/// consults, so subsequent strict pins detect SMT / NUMA conflicts.
///
/// @param cpu   logical cpu id (must be >= 0)
/// @param role  short diagnostic label (e.g. "lcore-0(rx-worker)"); shown
///              in conflict error messages from `pin_thread` /
///              `register_external_pin`. Empty string is allowed but
///              produces less useful error output.
/// @return `{}` on success; `unexpected` if @p cpu < 0 or already
///         registered (the existing role is included in the message).
///
/// @note Thread-safe; serializes on `g_pin_mutex` together with
///       `pin_thread`. Idempotent only via explicit
///       `unregister_external_pin` — re-registering the same cpu fails.
[[nodiscard]] inline std::expected<void, std::string>
register_external_pin(int cpu, std::string role) {
    if (cpu < 0) {
        EPH_LOG_ERROR(detail::cpu_logger(), "register_external_pin: cpu={} must be >= 0 (role='{}')",
                      cpu, role);
        return std::unexpected("register_external_pin: cpu must be >= 0");
    }
    std::lock_guard g(detail::g_pin_mutex);
    if (detail::g_pinned_cpus.contains(cpu)) {
        auto it = detail::g_pinned_owner_role.find(cpu);
        std::string by = (it != detail::g_pinned_owner_role.end() && !it->second.empty())
                             ? std::format(" by {}", it->second)
                             : std::string{};
        EPH_LOG_WARN(detail::cpu_logger(), "register_external_pin: cpu {} already occupied{}", cpu, by);
        return std::unexpected(std::format(
            "register_external_pin: cpu {} already occupied{}", cpu, by));
    }
    detail::g_pinned_cpus.insert(cpu);
    detail::g_pinned_owner_role[cpu] = std::move(role);
    EPH_LOG_INFO(detail::cpu_logger(), "register_external_pin: cpu {} registered as '{}'",
                 cpu, detail::g_pinned_owner_role[cpu]);
    return {};
}

/// @brief Release a cpu previously registered via `register_external_pin`.
///
/// Idempotent: calling on a cpu that is not in the registry is a no-op.
/// Will also erase a cpu that was added via `pin_thread`; callers should
/// not unregister cpus they did not register themselves (mixing ownership
/// like that is a bug).
///
/// @param cpu  logical cpu id; negative values are silently ignored.
inline void unregister_external_pin(int cpu) noexcept {
    if (cpu < 0) return;
    std::lock_guard g(detail::g_pin_mutex);
    detail::g_pinned_cpus.erase(cpu);
    detail::g_pinned_owner_role.erase(cpu);
}

/// @brief Query whether @p cpu is in the process-wide pin registry.
///
/// Returns true if any mechanism (`pin_thread` or `register_external_pin`)
/// has marked the cpu as occupied. Negative cpu values return false.
[[nodiscard]] inline bool is_cpu_externally_pinned(int cpu) noexcept {
    if (cpu < 0) return false;
    std::lock_guard g(detail::g_pin_mutex);
    return detail::g_pinned_cpus.contains(cpu);
}

/// @brief Test-only: clear the pinned-cpu registry so independent test
/// cases don't collide. Never call from production code.
inline void reset_pin_registry_for_tests() noexcept {
    std::lock_guard g(detail::g_pin_mutex);
    detail::g_pinned_cpus.clear();
    detail::g_pinned_owner_role.clear();
}

} // namespace eph::utils

// `spin_for_ns` needs TSC::now() which lives in time.hpp. The include
// lives at the bottom so callers that only want cpu_relax don't pay for
// TSC calibration symbols.
#include "eph/utils/time.hpp"

namespace eph::utils {

/// @brief Busy-wait for approximately `ns` nanoseconds using TSC + cpu_relax.
///
/// Intended for simulating CPU-bound work (e.g. spinning a mock server's
/// "business logic" for 200 ns) or micro-delays shorter than what any
/// syscall-based sleep can deliver. Requires `TSC::init()` to have run.
///
/// @note `ns <= 0` is a no-op. Accuracy is bounded by TSC read overhead
///       (~20 cycles) and the cpu_relax granularity — suitable for
///       hundreds of ns and up; for sub-100ns delays a hand-unrolled loop
///       will be more precise.
inline void spin_for_ns(long ns) noexcept {
    if (ns <= 0) return;
    auto target = TSC::to_cycles(static_cast<double>(ns));
    if (!target || *target == 0) return;
    uint64_t deadline = TSC::now() + *target;
    while (TSC::now() < deadline) {
        cpu_relax();
    }
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
