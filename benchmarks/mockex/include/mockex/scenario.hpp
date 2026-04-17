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

#include "core/config.hpp"  // bench::ScenarioConfig — shared with the C++ bench clients.

namespace mockex {

/// Runtime context handed to every scenario handler.
///
/// `globals` and `section` are both already-parsed bench.conf snapshots.
/// The scenario reads whichever keys it cares about (`port`,
/// `payload_size`, `push_rate_hz`, ...) via the `get_*` accessors.
///
/// `running` is a pointer into `eph::utils::g_shutdown_flag`; scenarios
/// poll it in their accept / recv loops so SIGTERM causes a clean exit.
struct ScenarioContext {
    std::string_view         scenario_name;   ///< e.g. "lat_tcp"
    std::string_view         config_path;     ///< absolute path to bench.conf
    const bench::ScenarioConfig* globals;     ///< lowercase pre-section keys
    const bench::ScenarioConfig* section;     ///< [lat_<name>] section
    std::atomic<bool>*           running;     ///< true while we should keep serving
};

/// Handler function pointer. Return 0 = clean exit; non-zero propagates
/// to the process exit code.
using ScenarioFn = int (*)(const ScenarioContext&) noexcept;

} // namespace mockex
