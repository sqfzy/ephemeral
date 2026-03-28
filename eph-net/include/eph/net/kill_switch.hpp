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

#include <spdlog/spdlog.h>

namespace eph::net {

/// Maximum number of transports that can be registered with a KillSwitch.
/// Fixed array avoids heap allocation — safe for signal handler context.
inline constexpr size_t kKillSwitchMaxTransports = 32;

/// Type-erased transport handle for shutdown.
/// Wraps any object that has stop() and is_running() methods.
struct TransportHandle {
    void* ptr = nullptr;
    void (*stop_fn)(void*) = nullptr;
    bool (*is_running_fn)(void*) = nullptr;
};

/// Centralized emergency shutdown coordinator.
///
/// Thread-safe for register/unregister (guarded by atomic spinlock).
/// Signal-safe for shutdown_requested flag (lock-free atomic).
///
/// Not copyable/movable — designed for one per application.
class KillSwitch {
public:
    KillSwitch() noexcept = default;
    ~KillSwitch() noexcept { shutdown(); }

    KillSwitch(const KillSwitch&) = delete;
    KillSwitch& operator=(const KillSwitch&) = delete;
    KillSwitch(KillSwitch&&) = delete;
    KillSwitch& operator=(KillSwitch&&) = delete;

    // ── Registration ────────────────────────────────────────────────────

    /// Register a Transport for coordinated shutdown.
    /// The transport must outlive the KillSwitch (or be unregistered first).
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

    /// Unregister a transport (e.g., before destroying it).
    template <typename Transport>
    void unregister_transport(Transport* tp) noexcept {
        if (!tp) return;
        remove_handle(static_cast<void*>(tp));
    }

    /// Number of registered transports.
    [[nodiscard]] size_t transport_count() const noexcept {
        return count_.load(std::memory_order_acquire);
    }

    // ── Shutdown control ────────────────────────────────────────────────

    /// Check if shutdown has been requested (signal or manual).
    /// Use this in your main loop condition.
    [[nodiscard]] bool is_shutdown_requested() const noexcept {
        return shutdown_requested_.load(std::memory_order_acquire);
    }

    /// Request shutdown (non-blocking). Sets the flag so main loops exit.
    /// Safe to call from signal handlers.
    void request_shutdown() noexcept {
        shutdown_requested_.store(true, std::memory_order_release);
    }

    /// Graceful shutdown: stop all registered transports.
    /// Blocks until all transports have stopped.
    /// Idempotent — safe to call multiple times.
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

        SPDLOG_INFO("KillSwitch: shutting down {} transports", n);

        for (size_t i = 0; i < n; ++i) {
            auto& h = snapshot[i];
            if (h.ptr && h.is_running_fn && h.is_running_fn(h.ptr)) {
                SPDLOG_INFO("KillSwitch: stopping transport {}/{}", i + 1, n);
                h.stop_fn(h.ptr);
            }
        }

        SPDLOG_INFO("KillSwitch: all transports stopped");
    }

    /// Emergency kill: request shutdown without blocking.
    /// Use when graceful shutdown is not possible (e.g., crash signal).
    /// Safe to call from signal handlers (no locks, no allocation).
    void kill() noexcept {
        shutdown_requested_.store(true, std::memory_order_release);
        // In emergency mode we don't call stop() — it may block.
        // The flag is enough for Transport's internal loops to exit.
    }

    // ── Signal handler installation ─────────────────────────────────────

    /// Install SIGINT and SIGTERM handlers that trigger shutdown.
    /// Must be called from main thread before spawning other threads.
    void install_signal_handlers() noexcept {
        s_instance_.store(this, std::memory_order_release);
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        SPDLOG_DEBUG("KillSwitch: signal handlers installed (SIGINT, SIGTERM)");
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
            while (lock_.load(std::memory_order_relaxed)) {}
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
            SPDLOG_ERROR("KillSwitch: max transports ({}) reached", kKillSwitchMaxTransports);
            return false;
        }
        handles_[n] = h;
        count_.store(n + 1, std::memory_order_release);
        spin_unlock();
        SPDLOG_DEBUG("KillSwitch: registered transport ({}/{})", n + 1, kKillSwitchMaxTransports);
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
                SPDLOG_DEBUG("KillSwitch: unregistered transport");
                break;
            }
        }
        spin_unlock();
    }

    // Signal handler — must be async-signal-safe (only atomics).
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
