/// @file core/pin_client.hpp
/// Thin wrapper that every lat_* client binary calls right after loading
/// bench.conf globals. Reads `cpu_client` (or a caller-provided default)
/// and hands it to `eph::utils::pin_thread` with the dev-host relaxed
/// policy — mirrors how mockex/src/main.cpp pins to `cpu_mock`.
///
/// Bench hygiene: every thread involved in the measurement must be on a
/// fixed CPU, otherwise CFS migration + cross-core cache misses flood the
/// one-way latency histograms with bogus tail. Every lat client must call
/// this helper before any TSC calibration or network setup.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

#include "core/bench_conf.hpp"
#include "eph/utils/cpu.hpp"

namespace bench {

/// Pin the calling thread using `cpu.cpu_client` from config.toml.
///
/// Lookup order:
///   1. $BENCH_CLIENT_CPU env var                     (CLI override)
///   2. `cpu.cpu_client` in config.toml               (structured BenchConfig)
///   3. `fallback_cpu` argument                       (safe default)
///
/// Policy is relaxed (no isolcpus / sibling / NUMA / IRQ checks) because
/// the bench runs on shared dev hosts; the critical property is *fixed*,
/// not isolated. `thread_name` must be ≤ 15 chars (pthread_setname_np).
inline void
pin_client_from_cfg(const BenchConfig& cfg,
                    std::string_view thread_name,
                    int fallback_cpu = 4) noexcept {
    int cpu = fallback_cpu;
    const char* source = "fallback";

    if (const char* env = std::getenv("BENCH_CLIENT_CPU"); env && *env) {
        // Validating parse: a typo'd BENCH_CLIENT_CPU=foo would
        // previously coerce to atoi("foo")=0 (a valid CPU id) and pin
        // to core 0 silently — exactly the "shared dev host" core that
        // gets the most IRQ noise. Reject non-numeric input and fall
        // through to the config / fallback chain so the misconfiguration
        // surfaces in the log instead of skewing measurements.
        char* end = nullptr;
        long parsed = std::strtol(env, &end, 10);
        if (end != env && *end == '\0' && parsed >= 0 && parsed < 1024) {
            cpu = static_cast<int>(parsed);
            source = "BENCH_CLIENT_CPU";
        } else {
            SPDLOG_WARN("[{}] BENCH_CLIENT_CPU='{}' is not a valid "
                        "non-negative integer; falling back to config/default",
                        thread_name, env);
            // fall through to cfg/fallback below
            if (cfg.cpu.cpu_client >= 0) {
                cpu = cfg.cpu.cpu_client;
                source = "config.toml cpu.cpu_client (env was invalid)";
            }
        }
    } else if (cfg.cpu.cpu_client >= 0) {
        cpu = cfg.cpu.cpu_client;
        source = "config.toml cpu.cpu_client";
    }

    eph::utils::CpuPinPolicy policy{
        .require_isolcpus            = false,
        .require_no_sibling_conflict = false,
        .require_same_numa           = false,
        .warn_irq_overlap            = false,
    };
    auto res = eph::utils::pin_thread(cpu, thread_name, policy);
    if (!res) {
        SPDLOG_WARN("[{}] CPU pin to core {} failed ({}): {} (non-fatal; "
                    "bench numbers will be noisier)",
                    thread_name, cpu, source, res.error());
        return;
    }
    // Permanent pin: bench process lives until measurement ends, then
    // exits — no scope where un-registering would help. Detach the
    // guard so the registry entry sticks around for the program's life.
    res->release();
    SPDLOG_INFO("[{}] pinned to CPU {} (source: {})",
                thread_name, cpu, source);

    // Tail-spike hardening (lat_ws DPDK baseline showed isolated TX
    // latencies of 1–11 ms while the median was 22 µs — a 500× tail
    // dominated by occasional page faults / scheduler preempts). The
    // two-step hardening below is a standard HFT recipe:
    //
    //   1. mlockall — pin the entire address space into RAM so no demand
    //      paging happens during the measurement window. Eliminates the
    //      page-fault class of tail spikes.
    //   2. SCHED_FIFO @ priority 50 — promote the bench thread out of
    //      CFS so the kernel scheduler cannot preempt it for the typical
    //      maintenance / migration / load-balance kicks. Priority 50 is
    //      well above any non-RT thread on a stock host but well below
    //      99 (which would pre-empt watchdog / RCU softirq workers and
    //      can wedge the box).
    //
    // Both steps are best-effort: failure (no CAP_IPC_LOCK / CAP_SYS_NICE
    // / ulimit too small) downgrades to a WARN — the bench still runs,
    // numbers just stay noisy. This matches `pin_thread`'s philosophy
    // of soft-degrading on dev hosts.
    auto lock_r = eph::utils::lock_memory(
        eph::utils::LockMemoryOptions{},
        std::string{thread_name}.c_str());
    if (!lock_r) {
        SPDLOG_WARN("[{}] mlockall failed: {} (bench tail latencies "
                    "may include page-fault spikes)",
                    thread_name, lock_r.error());
    }
    // SCHED_FIFO promotion was tried during the lat_ws hardening
    // bisect and removed: it interacts badly with the kernel's RT
    // throttler and the kernel mock's softirq wake path. Net effect
    // observed: TX tail improved 10× but RX tail regressed 30-90×
    // because the mock's blocking read() wakeup got starved. The
    // current config — mlockall ONLY, on both halves — eliminates
    // the page-fault class of spikes without introducing the RT-
    // starvation class. See the lat_ws hardening notes / trail in
    // benchmarks/latency/tools/irq_steer.sh's docstring for the
    // bisect details.
}

} // namespace bench
