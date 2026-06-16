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

#include "eph/core/log.hpp"

namespace eph::utils {

namespace detail {
inline spdlog::logger* shutdown_signal_logger() {
    static spdlog::logger* l = ::eph::log::get("utils.shutdown_signal");
    return l;
}
} // namespace detail

/// Process-wide running flag.  Hot loops should poll this with
/// `std::memory_order_relaxed` and break when it goes false.
inline std::atomic<bool> g_shutdown_flag{true};

/// Async-signal-safety contract: `on_shutdown_signal` runs from a real
/// signal handler. `std::atomic<bool>::store` is async-signal-safe ONLY
/// when the implementation is lock-free; a hypothetical platform that
/// implements `atomic<bool>` via a hidden mutex would deadlock if the
/// signal interrupted a thread that already held that mutex. Every
/// platform we target (Linux x86-64 / ARM64, glibc + libstdc++) uses a
/// lock-free single-byte atomic, but pin the requirement at compile
/// time so a future toolchain port that introduces a non-lock-free
/// atomic_bool fails here rather than at first SIGTERM under load.
/// (Same defense the audit-sweep signal handler applies in
/// eph-net-dpdk/include/eph/net/dpdk/detail/nicctl.hpp via R29.)
static_assert(std::atomic<bool>::is_always_lock_free,
              "shutdown_signal handler requires lock-free std::atomic<bool> "
              "for async-signal-safe store");

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
        EPH_LOG_WARN(detail::shutdown_signal_logger(), "install_shutdown_handlers: std::signal(SIGINT) failed — "
                    "default disposition (TERMINATE) remains in effect; "
                    "graceful shutdown via g_shutdown_flag is unavailable "
                    "for SIGINT");
    }
    if (std::signal(SIGTERM, detail::on_shutdown_signal) == SIG_ERR) {
        EPH_LOG_WARN(detail::shutdown_signal_logger(), "install_shutdown_handlers: std::signal(SIGTERM) failed "
                    "— default disposition (TERMINATE) remains in effect; "
                    "graceful shutdown via g_shutdown_flag is unavailable "
                    "for SIGTERM");
    }
}

} // namespace eph::utils
