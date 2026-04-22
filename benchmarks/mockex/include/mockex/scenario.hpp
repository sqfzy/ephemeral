/// @file scenario.hpp
/// Scenario concept + runtime context shared by every mockex handler.
///
/// A scenario is a free function with signature
/// `int run(const ScenarioContext&)`. The function is expected to bind
/// a listening socket, accept one client at a time, serve the echo /
/// push protocol, and return 0 on clean shutdown (peer close or SIGTERM)
/// or non-zero on a fatal error that should terminate the process.
///
/// The concept is purely compile-time — runtime dispatch goes through
/// the function pointer table in `dispatch.hpp`. This keeps the binary
/// footprint minimal: no vtables, no std::function.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include "core/bench_conf.hpp" // bench::BenchConfig / bench::Scenario (new API)
#include "core/config.hpp"     // bench::ScenarioConfig (legacy — deprecated, see below)

namespace mockex {

/// Runtime context handed to every scenario handler.
///
/// Populated by mockex/src/main.cpp after parsing config.toml. Handlers
/// read whichever fields they need.
///
/// `running` is a pointer into `eph::utils::g_shutdown_flag`; scenarios
/// poll it in their accept / recv loops so SIGTERM causes a clean exit.
///
/// ## Field migration (bench.conf → config.toml reshape)
///
/// The `globals` / `section` fields (legacy ScenarioConfig pointers) are
/// deprecated; new code should read from `cfg` / `scenario`
/// (BenchConfig / Scenario). Both pairs are populated in parallel during
/// Stage 2 of the reshape. The legacy pair is removed in Stage 3.
struct ScenarioContext {
    std::string_view         scenario_name;   ///< e.g. "lat_tcp"
    std::string_view         config_path;     ///< absolute path to config.toml

    // New TOML-based API (preferred).
    const bench::BenchConfig*    cfg       = nullptr;   ///< whole config
    const bench::Scenario*       scenario  = nullptr;   ///< [scenarios.<name>] subtable

    // Legacy API (deprecated, removed in S3).
    const bench::ScenarioConfig* globals   = nullptr;   ///< lowercase pre-section keys
    const bench::ScenarioConfig* section   = nullptr;   ///< [lat_<name>] section

    std::atomic<bool>*           running;               ///< true while we should keep serving
};

/// Handler function pointer. Return 0 = clean exit; non-zero propagates
/// to the process exit code.
using ScenarioFn = int (*)(const ScenarioContext&) noexcept;

} // namespace mockex
