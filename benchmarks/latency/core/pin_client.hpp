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
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

#include "core/bench_conf.hpp"
#include "core/config.hpp"
#include "eph/utils/cpu.hpp"

namespace bench {

/// Pin the calling thread using `cpu_client` from bench.conf globals.
///
/// Lookup order:
///   1. $BENCH_CLIENT_CPU env var                     (CLI override)
///   2. `cpu_client` key in the globals section       (bench.conf)
///   3. `fallback_cpu` argument                       (safe default)
///
/// Policy is relaxed (no isolcpus / sibling / NUMA / IRQ checks) because
/// the bench runs on shared dev hosts; the critical property is *fixed*,
/// not isolated. `thread_name` must be ≤ 15 chars (pthread_setname_np).
inline void
pin_client_from_globals(const ScenarioConfig& globals,
                        std::string_view thread_name,
                        int fallback_cpu = 4) noexcept {
    int cpu = fallback_cpu;
    const char* source = "fallback";

    if (const char* env = std::getenv("BENCH_CLIENT_CPU"); env && *env) {
        cpu = config_detail::parse_int(env, fallback_cpu);
        source = "BENCH_CLIENT_CPU";
    } else if (globals.has("cpu_client")) {
        auto v = globals.get_u32("cpu_client", static_cast<uint32_t>(fallback_cpu));
        if (v) {
            cpu = static_cast<int>(*v);
            source = "bench.conf cpu_client";
        }
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
    SPDLOG_INFO("[{}] pinned to CPU {} (source: {})",
                thread_name, cpu, source);
}

/// TOML-based overload: preferred for newly migrated callers.
/// Lookup order:
///   1. $BENCH_CLIENT_CPU env var                     (CLI override)
///   2. `cpu.cpu_client` in config.toml               (structured BenchConfig)
///   3. `fallback_cpu` argument                       (safe default)
///
/// Legacy overload (above) will be removed in Stage 3.
inline void
pin_client_from_cfg(const BenchConfig& cfg,
                    std::string_view thread_name,
                    int fallback_cpu = 4) noexcept {
    int cpu = fallback_cpu;
    const char* source = "fallback";

    if (const char* env = std::getenv("BENCH_CLIENT_CPU"); env && *env) {
        cpu = config_detail::parse_int(env, fallback_cpu);
        source = "BENCH_CLIENT_CPU";
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
    SPDLOG_INFO("[{}] pinned to CPU {} (source: {})",
                thread_name, cpu, source);
}

} // namespace bench
