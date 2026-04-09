/// @file cpu_pin.hpp
/// @brief Strict thread pinning with isolcpus / SMT sibling / NUMA / IRQ checks.
///
/// Low-latency and HFT workloads need more than a bare `set_thread_affinity`:
/// the pinned core must also be *isolated* from the OS scheduler, not shared
/// with another pinned thread via its SMT sibling, on the same NUMA node as
/// the rest of the hot path, and ideally free of NIC IRQ storms.
///
/// `pin_thread_strict()` performs all of these checks against `/sys`
/// (isolcpus, topology, NUMA nodes) and `/proc/interrupts`, then sets the
/// affinity via `pthread_setaffinity_np` and verifies it with
/// `pthread_getaffinity_np`. On success it records the pinned cpu in a
/// process-wide registry so subsequent calls can detect sibling/NUMA
/// conflicts.
///
/// @code
///   eph::utils::CpuPinPolicy policy;
///   if (auto r = eph::utils::pin_thread_strict(2, "poll", policy); !r) {
///       spdlog::error("{}", r.error());
///       return 1;
///   }
/// @endcode
///
/// Relaxing is explicit: set `require_isolcpus=false` on dev machines.
/// SMT sibling, NUMA, and the IRQ warning can be toggled independently.

#pragma once

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <expected>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

namespace eph::utils {

/// Policy controlling which topology checks `pin_thread_strict` enforces.
/// All checks default to ON. Disabling `require_isolcpus` is the usual
/// escape hatch for dev hosts.
struct CpuPinPolicy {
    bool require_isolcpus            = true;  ///< fail if cpu is not in /sys/.../isolated
    bool require_no_sibling_conflict = true;  ///< fail if an SMT sibling is already pinned
    bool require_same_numa           = true;  ///< fail if NUMA node differs from prior pins
    bool warn_irq_overlap            = true;  ///< warn if /proc/interrupts shows IRQs on cpu
};

namespace cpu_pin_detail {

/// Process-wide registry of pinned cpus — lets `pin_thread_strict`
/// detect sibling/NUMA conflicts across threads. Thread-safe.
inline std::mutex g_pin_mutex;
inline std::set<int> g_pinned_cpus;

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

    std::stringstream ss(line);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token.empty()) continue;
        auto dash = token.find('-');
        try {
            if (dash == std::string::npos) {
                out.insert(std::stoi(token));
            } else {
                int lo = std::stoi(token.substr(0, dash));
                int hi = std::stoi(token.substr(dash + 1));
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
    std::memcpy(buf, name.data(), n);
    pthread_setname_np(pthread_self(), buf);
}

} // namespace cpu_pin_detail

/// Pin the calling thread to `cpu` with policy-driven validation.
///
/// Steps, in order:
///   1. isolcpus check  (unless policy.require_isolcpus == false)
///   2. SMT sibling check against the process-wide pinned-cpu registry
///   3. NUMA check against the registry
///   4. IRQ overlap warn-only scan of /proc/interrupts
///   5. pthread_setaffinity_np + pthread_getaffinity_np verification
///   6. register cpu, set pthread name
///
/// @param cpu     logical cpu id (must be >= 0)
/// @param name    short thread label (≤ 15 chars for pthread_setname_np)
/// @param policy  which checks to enforce
/// @return `{}` on success, error string on any failure
[[nodiscard]] inline std::expected<void, std::string>
pin_thread_strict(int cpu, std::string_view name, CpuPinPolicy policy = {}) {
    using namespace cpu_pin_detail;

    if (cpu < 0) {
        return std::unexpected("pin_thread_strict: cpu must be >= 0");
    }

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
        spdlog::warn(
            "pin_thread_strict: cpu {} has active IRQs in /proc/interrupts "
            "(rebind NIC IRQs via /proc/irq/N/smp_affinity)", cpu);
    }

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
        rc != 0) {
        return std::unexpected(
            std::string("pthread_setaffinity_np failed: ") + std::strerror(rc));
    }

    cpu_set_t verify;
    CPU_ZERO(&verify);
    if (pthread_getaffinity_np(pthread_self(), sizeof(verify), &verify) != 0) {
        return std::unexpected(
            std::string("pthread_getaffinity_np failed: ") +
            std::strerror(errno));
    }
    if (!CPU_ISSET(cpu, &verify) || CPU_COUNT(&verify) != 1) {
        return std::unexpected(
            "affinity verification mismatch for cpu " + std::to_string(cpu));
    }

    {
        std::lock_guard g(g_pin_mutex);
        g_pinned_cpus.insert(cpu);
    }
    set_thread_name(name);

    spdlog::info("pin_thread_strict: '{}' pinned to cpu {} (verified)", name, cpu);
    return {};
}

/// Test-only: clear the pinned-cpu registry so independent test cases
/// don't collide. Never call from production code.
inline void reset_pin_registry_for_tests() noexcept {
    std::lock_guard g(cpu_pin_detail::g_pin_mutex);
    cpu_pin_detail::g_pinned_cpus.clear();
}

} // namespace eph::utils
