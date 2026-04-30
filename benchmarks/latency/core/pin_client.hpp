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
}

} // namespace bench
