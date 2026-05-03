#pragma once

/// @file shutdown_signal.hpp
/// Process-wide cooperative shutdown flag for SIGINT/SIGTERM.
///
/// SIGINT/SIGTERM both flip `g_shutdown_flag` to false.  Hot-path loops
/// poll the flag with relaxed ordering — there is no shared state to
/// synchronize against the flip itself; the only requirement is
/// eventual visibility.
///
/// Originally lived under benchmarks/latency/core/signal.hpp; promoted
/// to eph-utils so that tests and other consumers can use it without
/// reverse-including the bench tree.

#include <atomic>
#include <csignal>

#include <spdlog/spdlog.h>

namespace eph::utils {

/// Process-wide running flag.  Hot loops should poll this with
/// `std::memory_order_relaxed` and break when it goes false.
inline std::atomic<bool> g_shutdown_flag{true};

namespace detail {
inline void on_shutdown_signal(int /*signo*/) noexcept {
    g_shutdown_flag.store(false, std::memory_order_release);
}
} // namespace detail

/// Install SIGINT and SIGTERM handlers that flip `g_shutdown_flag` to
/// false.  Idempotent — safe to call multiple times.  Uses POSIX
/// `signal()` for portability; consumers needing `sigaction` semantics
/// (mask, restart) should install their own handlers.
///
/// Failure mode: `std::signal` returning `SIG_ERR` means the handler
/// failed to install (rare on Linux — typically only if the signal
/// number is invalid or the per-process handler table is somehow
/// corrupted; SIGINT/SIGTERM are always valid). On failure, the
/// process continues to use the default disposition for that signal
/// — TERMINATE — which kills the process abruptly without ever
/// flipping `g_shutdown_flag`. WARN-log if we hit either failure so
/// the operator sees evidence that "graceful shutdown won't actually
/// be graceful" rather than discovering it via process termination
/// during the first SIGTERM. Returns void (consistent with prior
/// signature) — callers that need stronger guarantees should
/// `sigaction` directly.
inline void install_shutdown_handlers() noexcept {
    if (std::signal(SIGINT, detail::on_shutdown_signal) == SIG_ERR) {
        SPDLOG_WARN("install_shutdown_handlers: std::signal(SIGINT) failed — "
                    "default disposition (TERMINATE) remains in effect; "
                    "graceful shutdown via g_shutdown_flag is unavailable "
                    "for SIGINT");
    }
    if (std::signal(SIGTERM, detail::on_shutdown_signal) == SIG_ERR) {
        SPDLOG_WARN("install_shutdown_handlers: std::signal(SIGTERM) failed "
                    "— default disposition (TERMINATE) remains in effect; "
                    "graceful shutdown via g_shutdown_flag is unavailable "
                    "for SIGTERM");
    }
}

} // namespace eph::utils
