#pragma once

/// @file kill_switch.hpp
/// Centralized emergency shutdown for HFT applications.
///
/// KillSwitch provides coordinated teardown of multiple Transport and
/// FixSession instances in a single atomic operation. Two shutdown paths:
///
///   1. **Graceful** (SIGINT/SIGTERM): logout FIX sessions, close WebSocket
///      connections, join threads, flush stats.
///   2. **Emergency** (SIGSEGV/SIGABRT or manual kill): immediate Transport
///      stop without waiting for server responses.
///
/// Usage:
///   eph::net::KillSwitch ks;
///   ks.register_transport(&tp1);
///   ks.register_transport(&tp2);
///   ks.install_signal_handlers();  // SIGINT, SIGTERM
///
///   // Main loop checks ks.is_shutdown_requested()
///   while (!ks.is_shutdown_requested()) { ... }
///
///   // Or manual trigger:
///   ks.shutdown();  // graceful
///   ks.kill();      // emergency (no waiting)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace eph::net {

namespace detail {
/// @brief Lazily-initialized logger for the KillSwitch subsystem.
/// @return Pointer to the "net.kill_switch" spdlog logger.
inline spdlog::logger* kill_switch_logger() {
    static auto l = [] {
        try {
            return spdlog::stdout_color_mt("net.kill_switch");
        } catch (const spdlog::spdlog_ex&) {
            return spdlog::get("net.kill_switch");
        }
    }();
    return l.get();
}
} // namespace detail

/// @brief Maximum number of transports that can be registered with a KillSwitch.
///
/// Fixed array avoids heap allocation -- safe for signal handler context.
inline constexpr size_t kKillSwitchMaxTransports = 32;

/// @brief Type-erased transport handle for shutdown.
///
/// Wraps any object that has stop() and is_running() methods via
/// function pointers, avoiding the need for a polymorphic base class.
struct TransportHandle {
    void* ptr = nullptr;             ///< Opaque pointer to the transport instance.
    void (*stop_fn)(void*) = nullptr;      ///< Type-erased stop function.
    bool (*is_running_fn)(void*) = nullptr; ///< Type-erased running status check.
};

/// @brief Centralized emergency shutdown coordinator.
///
/// Thread-safe for register/unregister (guarded by atomic spinlock).
/// Signal-safe for shutdown_requested flag (lock-free atomic).
///
/// Not copyable/movable — designed for one per application.
class KillSwitch {
public:
    /// @brief Default constructor -- no transports registered, no signal handlers installed.
    KillSwitch() noexcept = default;
    /// @brief Destructor -- shuts down all registered transports, restores default signal handlers.
    ~KillSwitch() noexcept {
        shutdown();
        // Deregister signal handlers and null out the global instance pointer
        // to prevent use-after-free if a signal arrives after destruction.
        s_instance_.store(nullptr, std::memory_order_release);
        std::signal(SIGINT, SIG_DFL);
        std::signal(SIGTERM, SIG_DFL);
    }

    KillSwitch(const KillSwitch&) = delete;
    KillSwitch& operator=(const KillSwitch&) = delete;
    KillSwitch(KillSwitch&&) = delete;
    KillSwitch& operator=(KillSwitch&&) = delete;

    // ── Registration ────────────────────────────────────────────────────

    /// @brief Register a Transport for coordinated shutdown.
    ///
    /// The transport must outlive the KillSwitch (or be unregistered first).
    ///
    /// @tparam Transport  Any type with stop() and is_running() methods.
    /// @param tp  Pointer to the transport. Must not be null.
    /// @return true if registered successfully, false if null or capacity reached.
    template <typename Transport>
    bool register_transport(Transport* tp) noexcept {
        if (!tp) return false;
        TransportHandle h{
            .ptr = static_cast<void*>(tp),
            .stop_fn = [](void* p) { static_cast<Transport*>(p)->stop(); },
            .is_running_fn = [](void* p) { return static_cast<Transport*>(p)->is_running(); },
        };
        return add_handle(h);
    }

    /// @brief Unregister a transport (e.g., before destroying it).
    /// @tparam Transport  Same type used in register_transport().
    /// @param tp  Pointer to the transport to remove. Null is a no-op.
    template <typename Transport>
    void unregister_transport(Transport* tp) noexcept {
        if (!tp) return;
        remove_handle(static_cast<void*>(tp));
    }

    /// @brief Number of registered transports.
    /// @return Count of currently registered transport handles.
    [[nodiscard]] size_t transport_count() const noexcept {
        return count_.load(std::memory_order_acquire);
    }

    // ── Shutdown control ────────────────────────────────────────────────

    /// @brief Check if shutdown has been requested (signal or manual).
    ///
    /// Use this in your main loop condition:
    /// @code
    ///   while (!ks.is_shutdown_requested()) { ... }
    /// @endcode
    ///
    /// @return true if shutdown() / kill() / request_shutdown() / signal has fired.
    [[nodiscard]] bool is_shutdown_requested() const noexcept {
        return shutdown_requested_.load(std::memory_order_acquire);
    }

    /// @brief Request shutdown (non-blocking). Sets the flag so main loops exit.
    ///
    /// Safe to call from signal handlers (lock-free atomic store).
    void request_shutdown() noexcept {
        shutdown_requested_.store(true, std::memory_order_release);
    }

    /// @brief Graceful shutdown: stop all registered transports.
    ///
    /// Blocks until all transports have stopped.
    /// Idempotent -- safe to call multiple times.
    /// @note If a transport's stop_fn throws, it is caught and logged;
    ///       remaining transports are still stopped.
    void shutdown() noexcept {
        if (shutdown_done_.exchange(true, std::memory_order_acq_rel)) return;
        shutdown_requested_.store(true, std::memory_order_release);

        // Take a snapshot of handles under lock to prevent concurrent
        // unregister from invalidating pointers during iteration.
        spin_lock();
        size_t n = count_.load(std::memory_order_relaxed);
        std::array<TransportHandle, kKillSwitchMaxTransports> snapshot{};
        for (size_t i = 0; i < n; ++i) snapshot[i] = handles_[i];
        spin_unlock();

        SPDLOG_LOGGER_INFO(detail::kill_switch_logger(), "KillSwitch: shutting down {} transports", n);

        size_t stopped = 0;
        for (size_t i = 0; i < n; ++i) {
            auto& h = snapshot[i];
            if (h.ptr && h.is_running_fn && h.is_running_fn(h.ptr)) {
                SPDLOG_LOGGER_INFO(detail::kill_switch_logger(), "KillSwitch: stopping transport {}/{}", i + 1, n);
                // Continue to next transport even if stop_fn fails —
                // a partial failure must not prevent remaining transports
                // from being stopped.
                try {
                    h.stop_fn(h.ptr);
                    ++stopped;
                } catch (...) {
                    SPDLOG_LOGGER_ERROR(detail::kill_switch_logger(),
                        "KillSwitch: transport {}/{} stop_fn threw — continuing", i + 1, n);
                }
            }
        }

        SPDLOG_LOGGER_INFO(detail::kill_switch_logger(), "KillSwitch: {}/{} transports stopped", stopped, n);
    }

    /// @brief Emergency kill: request shutdown without blocking.
    ///
    /// Use when graceful shutdown is not possible (e.g., crash signal).
    /// Safe to call from signal handlers (no locks, no allocation).
    /// @warning Does NOT call stop() on transports -- relies on Transport
    ///          internal loops checking the shutdown flag to exit.
    void kill() noexcept {
        shutdown_requested_.store(true, std::memory_order_release);
        // In emergency mode we don't call stop() — it may block.
        // The flag is enough for Transport's internal loops to exit.
    }

    // ── Signal handler installation ─────────────────────────────────────

    /// @brief Install SIGINT and SIGTERM handlers that trigger shutdown.
    ///
    /// Must be called from main thread before spawning other threads.
    /// @warning Only one KillSwitch per process can install signal handlers.
    ///          A second call overwrites the global instance pointer.
    void install_signal_handlers() noexcept {
        s_instance_.store(this, std::memory_order_release);
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        SPDLOG_LOGGER_DEBUG(detail::kill_switch_logger(), "KillSwitch: signal handlers installed (SIGINT, SIGTERM)");
    }

private:
    std::array<TransportHandle, kKillSwitchMaxTransports> handles_{};
    std::atomic<size_t> count_{0};
    std::atomic<bool> shutdown_requested_{false};
    std::atomic<bool> shutdown_done_{false};

    // Spinlock for register/unregister (not used in signal path).
    std::atomic<bool> lock_{false};

    void spin_lock() noexcept {
        while (lock_.exchange(true, std::memory_order_acquire)) {
            while (lock_.load(std::memory_order_relaxed)) {
#if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
#elif defined(__aarch64__)
                asm volatile("yield");
#endif
            }
        }
    }
    void spin_unlock() noexcept {
        lock_.store(false, std::memory_order_release);
    }

    bool add_handle(const TransportHandle& h) noexcept {
        spin_lock();
        size_t n = count_.load(std::memory_order_relaxed);
        if (n >= kKillSwitchMaxTransports) {
            spin_unlock();
            SPDLOG_LOGGER_ERROR(detail::kill_switch_logger(), "KillSwitch: max transports ({}) reached", kKillSwitchMaxTransports);
            return false;
        }
        handles_[n] = h;
        count_.store(n + 1, std::memory_order_release);
        spin_unlock();
        SPDLOG_LOGGER_DEBUG(detail::kill_switch_logger(), "KillSwitch: registered transport ({}/{})", n + 1, kKillSwitchMaxTransports);
        return true;
    }

    void remove_handle(void* ptr) noexcept {
        spin_lock();
        size_t n = count_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) {
            if (handles_[i].ptr == ptr) {
                // Swap with last and shrink
                handles_[i] = handles_[n - 1];
                handles_[n - 1] = {};
                count_.store(n - 1, std::memory_order_release);
                SPDLOG_LOGGER_DEBUG(detail::kill_switch_logger(), "KillSwitch: unregistered transport");
                break;
            }
        }
        spin_unlock();
    }

    /// Signal handler — must be async-signal-safe (only atomics, no allocations).
    /// Re-registers SIGINT to SIG_DFL so a second ctrl-C triggers immediate hard kill.
    static void signal_handler(int sig) noexcept {
        auto* ks = s_instance_.load(std::memory_order_acquire);
        if (ks) {
            ks->request_shutdown();
        }
        // Re-register for SIGINT to allow double-ctrl-C for hard kill
        if (sig == SIGINT) {
            std::signal(SIGINT, SIG_DFL);
        }
    }

    // Global instance pointer for signal handler (only one KillSwitch per process).
    // Atomic for safe access from signal handler context.
    static inline std::atomic<KillSwitch*> s_instance_{nullptr};
};

} // namespace eph::net
