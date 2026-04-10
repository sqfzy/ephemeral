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
inline void install_shutdown_handlers() noexcept {
    std::signal(SIGINT,  detail::on_shutdown_signal);
    std::signal(SIGTERM, detail::on_shutdown_signal);
}

} // namespace eph::utils
