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
#include <memory>
#include <string>
#include <string_view>

#include "core/bench_conf.hpp"  // bench::BenchConfig / bench::Scenario

// Forward declaration — full header (mockex/tls_server.hpp) only included
// by handlers and main.cpp that actually wrap fds with TLS. Keeps the
// UDP scenario handlers free of an aws-lc dep they don't use.
namespace mockex::tls { class TlsServer; }

namespace mockex {

/// Runtime context handed to every scenario handler.
///
/// Populated by mockex/src/main.cpp after parsing config.toml. Handlers
/// read whichever fields they need from `cfg` / `scenario`.
///
/// `running` is a pointer into `eph::utils::g_shutdown_flag`; scenarios
/// poll it in their accept / recv loops so SIGTERM causes a clean exit.
///
/// `tls` is non-null when the scenario's `[scenarios.<name>].use_tls =
/// true` and `TlsServer::create(...)` succeeded at startup. TCP-based
/// scenario handlers wrap each accepted fd via `tls->accept_tls(cfd)`
/// when the field is set. UDP handlers ignore it.
struct ScenarioContext {
    std::string_view             scenario_name;   ///< e.g. "lat_tcp"
    std::string_view             config_path;     ///< absolute path to config.toml
    const bench::BenchConfig*    cfg       = nullptr;   ///< whole config
    const bench::Scenario*       scenario  = nullptr;   ///< [scenarios.<name>] subtable
    std::atomic<bool>*           running;               ///< true while we should keep serving
    std::shared_ptr<tls::TlsServer> tls;                ///< nullable; non-null = wrap accepts in TLS
};

/// Handler function pointer. Return 0 = clean exit; non-zero propagates
/// to the process exit code.
using ScenarioFn = int (*)(const ScenarioContext&) noexcept;

} // namespace mockex
