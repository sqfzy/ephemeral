/// @file core/cpu_pin.hpp
/// Strict CPU pinning with isolcpus / sibling / NUMA / IRQ checks.
///
/// `pin_thread_strict` is the only supported entry point for bench/mock
/// processes. The plain `eph::utils::set_thread_affinity` lacks the
/// validation that bench data trustworthiness depends on:
///
///   1. CPU must be in `/sys/devices/system/cpu/isolated`
///      (otherwise the OS scheduler will run other tasks on it).
///   2. CPU must not share an SMT physical core with another already-
///      pinned thread of the same process (HT contention destroys
///      tail latency).
///   3. CPU must be in the same NUMA node as previously pinned threads
///      (cross-NUMA memory access is a hidden jitter source).
///   4. NIC IRQs should not target the CPU (warn-only — the bench
///      script is responsible for IRQ steering).
///
/// All checks except (4) are HARD failures unless explicitly relaxed via
/// `CpuPinPolicy`. The `--allow-non-isolated` CLI flag flips
/// `require_isolcpus` off; the other three remain on.
///
/// Pinning sets the affinity AND verifies it via `sched_getaffinity` to
/// catch silent EBUSY-style failures.
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
#include <sys/syscall.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

namespace bench {

struct CpuPinPolicy {
    bool require_isolcpus            = true;
    bool require_no_sibling_conflict = true;
    bool require_same_numa           = true;
    bool warn_irq_overlap            = true;
};

namespace cpu_pin_detail {

/// Process-wide registry of pinned CPUs. Used to detect SMT sibling and
/// NUMA conflicts across multiple `pin_thread_strict` calls in the same
/// process. Mutated under `g_mutex`. The bench client/mock binaries call
/// this once per thread, so contention is irrelevant.
inline std::mutex g_mutex;
inline std::set<int> g_pinned_cpus;

/// Read a comma-separated CPU list file (e.g.
/// `/sys/devices/system/cpu/isolated` or `topology/thread_siblings_list`)
/// and expand ranges like `1-3` into individual integers.
///
/// Returns empty set if the file is missing or empty (caller decides
/// whether that's an error).
inline std::set<int> read_cpu_list_file(const std::string& path) {
    std::set<int> out;
    std::ifstream f(path);
    if (!f.is_open()) return out;
    std::string line;
    if (!std::getline(f, line)) return out;

    // Trim trailing whitespace/newlines.
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' ||
                             line.back() == ' ')) {
        line.pop_back();
    }
    if (line.empty()) return out;

    // Split by ',' and expand ranges.
    std::stringstream ss(line);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token.empty()) continue;
        auto dash = token.find('-');
        if (dash == std::string::npos) {
            try { out.insert(std::stoi(token)); } catch (...) {}
        } else {
            try {
                int lo = std::stoi(token.substr(0, dash));
                int hi = std::stoi(token.substr(dash + 1));
                for (int i = lo; i <= hi; ++i) out.insert(i);
            } catch (...) {}
        }
    }
    return out;
}

/// Read the NUMA node id of `cpu` by globbing
/// `/sys/devices/system/cpu/cpuN/node*`. Returns -1 if not found
/// (e.g. on a non-NUMA system or under chroot without sysfs).
inline int read_numa_node(int cpu) {
    // Use a small range scan to avoid pulling in std::filesystem.
    // Real systems rarely have more than 16 NUMA nodes.
    for (int node = 0; node < 64; ++node) {
        std::string p = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                        "/node" + std::to_string(node);
        if (access(p.c_str(), F_OK) == 0) return node;
    }
    return -1;
}

/// Check whether `cpu` has a non-zero IRQ count in `/proc/interrupts`.
/// Returns true if any line shows count > 0 in the cpu's column. Best
/// effort — parse failures count as "no overlap" rather than spurious warnings.
inline bool cpu_has_active_irq(int cpu) {
    std::ifstream f("/proc/interrupts");
    if (!f.is_open()) return false;

    // Header line:    "           CPU0       CPU1       CPU2 ..."
    std::string header;
    if (!std::getline(f, header)) return false;

    // Locate the column index for CPU<cpu>. We split header tokens.
    std::stringstream hss(header);
    std::vector<std::string> cols;
    for (std::string tok; hss >> tok;) cols.push_back(tok);
    int col_idx = -1;
    std::string want = "CPU" + std::to_string(cpu);
    for (size_t i = 0; i < cols.size(); ++i) {
        if (cols[i] == want) { col_idx = static_cast<int>(i); break; }
    }
    if (col_idx < 0) return false;

    std::string line;
    while (std::getline(f, line)) {
        std::stringstream lss(line);
        std::vector<std::string> toks;
        for (std::string tok; lss >> tok;) toks.push_back(tok);
        // Each row is: <irq>:  <cnt0> <cnt1> ... <ctl>
        // The first token is the IRQ name with trailing ':'. After
        // splitting, the count column is at position col_idx + 1
        // (since header has no leading label).
        size_t want_idx = static_cast<size_t>(col_idx + 1);
        if (toks.size() <= want_idx) continue;
        const std::string& cnt = toks[want_idx];
        // Skip non-numeric (avoid the trailing IRQ description).
        bool numeric = !cnt.empty();
        for (char c : cnt) if (c < '0' || c > '9') { numeric = false; break; }
        if (!numeric) continue;
        try {
            if (std::stoll(cnt) > 0) return true;
        } catch (...) {}
    }
    return false;
}

/// Set the OS-level thread name (visible in `top -H`, `gdb`).
inline void set_thread_name(std::string_view name) noexcept {
    char buf[16] = {};
    auto n = std::min(name.size(), sizeof(buf) - 1);
    std::memcpy(buf, name.data(), n);
    pthread_setname_np(pthread_self(), buf);
}

} // namespace cpu_pin_detail

/// Pin the calling thread to `cpu` and validate per `policy`. Returns an
/// error string on failure; on success the cpu is added to a process-wide
/// registry so subsequent calls can detect sibling/NUMA conflicts.
[[nodiscard]] inline std::expected<void, std::string>
pin_thread_strict(int cpu, std::string_view name, CpuPinPolicy policy = {}) {
    using namespace cpu_pin_detail;

    if (cpu < 0) {
        return std::unexpected("pin_thread_strict: cpu must be >= 0");
    }

    // ── 1. isolcpus check ────────────────────────────────────────────
    if (policy.require_isolcpus) {
        auto isolated = read_cpu_list_file("/sys/devices/system/cpu/isolated");
        if (isolated.find(cpu) == isolated.end()) {
            return std::unexpected(
                "cpu " + std::to_string(cpu) +
                " is not in /sys/devices/system/cpu/isolated. "
                "Pass --allow-non-isolated to bypass (dev only).");
        }
    }

    // ── 2. SMT sibling conflict ──────────────────────────────────────
    if (policy.require_no_sibling_conflict) {
        std::lock_guard<std::mutex> g(g_mutex);
        auto siblings = read_cpu_list_file(
            "/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
            "/topology/thread_siblings_list");
        for (int s : siblings) {
            if (s == cpu) continue;
            if (g_pinned_cpus.count(s)) {
                return std::unexpected(
                    "cpu " + std::to_string(cpu) +
                    " shares an SMT physical core with already-pinned cpu " +
                    std::to_string(s));
            }
        }
    }

    // ── 3. NUMA node conflict ────────────────────────────────────────
    if (policy.require_same_numa) {
        std::lock_guard<std::mutex> g(g_mutex);
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

    // ── 4. IRQ overlap (warn only) ───────────────────────────────────
    if (policy.warn_irq_overlap) {
        if (cpu_has_active_irq(cpu)) {
            spdlog::warn(
                "pin_thread_strict: cpu {} has active IRQs in /proc/interrupts "
                "(rebind NIC IRQs to a different cpu via /proc/irq/N/smp_affinity)",
                cpu);
        }
    }

    // ── 5. Set affinity ──────────────────────────────────────────────
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set); rc != 0) {
        return std::unexpected(
            std::string("pthread_setaffinity_np failed: ") + std::strerror(rc));
    }

    // ── 6. Verify with sched_getaffinity ─────────────────────────────
    cpu_set_t verify;
    CPU_ZERO(&verify);
    if (pthread_getaffinity_np(pthread_self(), sizeof(verify), &verify) != 0) {
        return std::unexpected(
            std::string("pthread_getaffinity_np failed: ") + std::strerror(errno));
    }
    if (!CPU_ISSET(cpu, &verify) || CPU_COUNT(&verify) != 1) {
        return std::unexpected(
            "verification mismatch after setting affinity to cpu " +
            std::to_string(cpu));
    }

    // ── 7. Register & set name ───────────────────────────────────────
    {
        std::lock_guard<std::mutex> g(g_mutex);
        g_pinned_cpus.insert(cpu);
    }
    set_thread_name(name);

    spdlog::info("pin_thread_strict: '{}' pinned to cpu {} (verified)", name, cpu);
    return {};
}

/// Test-only helper: clear the process-wide registry. Production code MUST
/// NOT call this — pinning is logically permanent within a bench run.
inline void reset_pin_registry_for_tests() noexcept {
    std::lock_guard<std::mutex> g(cpu_pin_detail::g_mutex);
    cpu_pin_detail::g_pinned_cpus.clear();
}

} // namespace bench
