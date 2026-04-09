/// @file core/signal.hpp
/// Process-wide shutdown flag for bench client and mock processes.
///
/// SIGINT/SIGTERM both flip `g_running` to false. Hot-path loops poll this
/// flag with `relaxed` ordering — there is no shared state to synchronize
/// against the flip itself; the only requirement is eventual visibility.
#pragma once

#include <atomic>
#include <csignal>

namespace bench {

inline std::atomic<bool> g_running{true};

namespace detail {
inline void on_signal(int /*signo*/) noexcept {
    g_running.store(false, std::memory_order_release);
}
} // namespace detail

/// Install SIGINT and SIGTERM handlers that flip `g_running` to false.
/// Idempotent — safe to call multiple times. Uses POSIX `signal()` for
/// portability; bench processes are simple enough not to need `sigaction`.
inline void install_signal_handlers() noexcept {
    std::signal(SIGINT,  detail::on_signal);
    std::signal(SIGTERM, detail::on_signal);
}

} // namespace bench
