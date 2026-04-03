#pragma once

/// @file gateway.hpp
/// Multi-connection gateway for coordinated Transport lifecycle management.
///
/// Gateway orchestrates multiple Transport instances across exchanges/venues:
///   - Centralized start/stop/reconnect control
///   - Health monitoring with configurable check interval
///   - Per-connection tagging (exchange, symbol, priority)
///   - Integration with KillSwitch for emergency shutdown
///   - Aggregate statistics across all connections
///
/// Usage:
///   eph::net::Gateway gw;
///   auto id1 = gw.add("binance-btc", std::move(tp1));
///   auto id2 = gw.add("binance-eth", std::move(tp2));
///   gw.start_all();
///   // ... run ...
///   gw.stop_all();

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace eph::net {

namespace detail {
/// @brief Lazily-initialized logger for the Gateway subsystem.
/// @return Pointer to the "net.gateway" spdlog logger.
inline spdlog::logger* gateway_logger() {
    static auto l = [] {
        auto lg = spdlog::get("net.gateway");
        if (!lg) lg = spdlog::stdout_color_mt("net.gateway");
        return lg;
    }();
    return l.get();
}
} // namespace detail

/// @brief Connection health status.
///
/// Tracks the perceived health of a managed transport connection.
enum class ConnHealth : uint8_t {
    Healthy,      ///< Connected and receiving data
    Degraded,     ///< Connected but no data received recently (not yet implemented — reserved for future use)
    Disconnected, ///< Not connected
    Stopped,      ///< Intentionally stopped
};

/// @brief Convert a ConnHealth value to a human-readable status name.
/// @param h  The health status to convert.
/// @return Null-terminated string view such as "HEALTHY", "DISCONNECTED", etc.
[[nodiscard]] inline constexpr std::string_view conn_health_name(ConnHealth h) noexcept {
    switch (h) {
    case ConnHealth::Healthy:      return "HEALTHY";
    case ConnHealth::Degraded:     return "DEGRADED";
    case ConnHealth::Disconnected: return "DISCONNECTED";
    case ConnHealth::Stopped:      return "STOPPED";
    }
    return "UNKNOWN";
}

/// @brief Type-erased connection wrapper for Gateway.
///
/// Stores a transport with its metadata and health state.
/// Transport operations are accessed through type-erased function pointers
/// to avoid requiring a common base class.
struct GatewayConnection {
    /// @brief Unique tag for this connection (e.g., "binance-btcusdt").
    std::string tag;

    /// @brief Type-erased transport handle (opaque pointer).
    void* transport_ptr = nullptr;
    /// @brief Type-erased stop function (calls Transport::stop()).
    void (*stop_fn)(void*) = nullptr;
    /// @brief Type-erased running check (calls Transport::is_running()).
    bool (*is_running_fn)(void*) = nullptr;
    /// @brief Type-erased thread launcher (calls Transport::start_threads()).
    void (*start_threads_fn)(void*) = nullptr;
    /// @brief Type-erased reconnect trigger (calls Transport::reconnect_now()).
    void (*reconnect_fn)(void*) = nullptr;

    /// @brief Current health status of this connection.
    ConnHealth health = ConnHealth::Stopped;
    /// @brief Timestamp (ns) of last health check for this connection.
    uint64_t last_health_check_ns = 0;

    /// @brief Connection priority (lower = more important). Used for log ordering.
    uint8_t priority = 128;
};

/// @brief Multi-connection lifecycle manager.
///
/// Thread-safe: all methods are mutex-protected. The monitor thread
/// runs in background and checks health periodically.
///
/// Not copyable/movable — designed as a singleton-like coordinator.
class Gateway {
public:
    /// @brief Configuration for the Gateway health monitor and callbacks.
    struct Config {
        /// How often to check connection health (0 = no monitoring).
        std::chrono::milliseconds health_check_interval{5000};
        /// Mark connection as degraded if no activity for this duration.
        /// @note Not yet implemented -- requires per-connection last-activity timestamps
        ///       tracked via a callback from Transport::on_data(). When implemented,
        ///       check_health() will compare now - last_activity against this threshold
        ///       and transition Healthy -> Degraded accordingly.
        std::chrono::milliseconds degraded_threshold{30000};
        /// Optional callback invoked when a connection's health changes.
        /// Called outside the Gateway lock to prevent deadlock.
        std::function<void(std::string_view tag, ConnHealth old_h, ConnHealth new_h)>
            on_health_change{};
    };

    /// @brief Construct a Gateway with default configuration.
    Gateway() = default;

    /// @brief Construct a Gateway with explicit configuration.
    /// @param config  Health monitoring and callback configuration.
    explicit Gateway(Config config)
        : config_(std::move(config)) {}

    /// @brief Destructor -- stops the health monitor and all connections.
    ~Gateway() noexcept {
        stop_monitor();
        stop_all();
    }

    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;
    Gateway(Gateway&&) = delete;
    Gateway& operator=(Gateway&&) = delete;

    // ── Connection management ───────────────────────────────────────────

    /// @brief Add a transport to the gateway.
    ///
    /// The transport must outlive the Gateway (or be removed first).
    ///
    /// @tparam Transport  Any type with stop(), is_running(), start_threads(),
    ///                    and reconnect_now() methods.
    /// @param tag       Human-readable identifier for this connection (e.g., "binance-btcusdt").
    /// @param tp        Pointer to the transport instance. Must not be null.
    /// @param priority  Connection priority (lower = more important). Used for log ordering.
    /// @return Connection index for use with health(), tag(), reconnect(), etc.
    ///         Returns SIZE_MAX if tp is null.
    template <typename Transport>
    [[nodiscard]] size_t add(std::string tag, Transport* tp, uint8_t priority = 128) {
        if (!tp) {
            SPDLOG_LOGGER_ERROR(detail::gateway_logger(), "Gateway::add: null transport for tag '{}'", tag);
            return SIZE_MAX;
        }

        GatewayConnection conn;
        conn.tag = std::move(tag);
        conn.transport_ptr = static_cast<void*>(tp);
        conn.stop_fn = [](void* p) { static_cast<Transport*>(p)->stop(); };
        conn.is_running_fn = [](void* p) { return static_cast<Transport*>(p)->is_running(); };
        conn.start_threads_fn = [](void* p) { static_cast<Transport*>(p)->start_threads(); };
        conn.reconnect_fn = [](void* p) { static_cast<Transport*>(p)->reconnect_now(); };
        conn.priority = priority;
        conn.health = tp->is_running() ? ConnHealth::Healthy : ConnHealth::Stopped;

        std::lock_guard lock(mu_);
        size_t id = connections_.size();
        connections_.push_back(std::move(conn));
        SPDLOG_LOGGER_INFO(detail::gateway_logger(), "Gateway: added connection [{}] '{}' (priority={})",
                     id, connections_[id].tag, priority);
        return id;
    }

    /// @brief Number of managed connections.
    /// @return Total number of connections added via add().
    [[nodiscard]] size_t connection_count() const noexcept {
        std::lock_guard lock(mu_);
        return connections_.size();
    }

    /// @brief Get connection health by index.
    /// @param id  Connection index returned by add().
    /// @return Current health status, or ConnHealth::Stopped if id is out of range.
    [[nodiscard]] ConnHealth health(size_t id) const noexcept {
        std::lock_guard lock(mu_);
        if (id >= connections_.size()) return ConnHealth::Stopped;
        return connections_[id].health;
    }

    /// @brief Get connection tag by index.
    /// @param id  Connection index returned by add().
    /// @return The tag string, or empty string if id is out of range.
    [[nodiscard]] std::string tag(size_t id) const {
        std::lock_guard lock(mu_);
        if (id >= connections_.size()) return "";
        return connections_[id].tag;
    }

    // ── Lifecycle control ───────────────────────────────────────────────

    /// @brief Start all stopped connections' threads.
    ///
    /// Only starts connections whose health is ConnHealth::Stopped and whose
    /// transport is not already running. Skips duplicates safely.
    void start_all() noexcept {
        std::lock_guard lock(mu_);
        for (size_t i = 0; i < connections_.size(); ++i) {
            auto& c = connections_[i];
            if (c.health == ConnHealth::Stopped && c.start_threads_fn) {
                // Guard against spawning duplicate threads if the transport
                // is somehow already running (e.g. started externally).
                if (!c.is_running_fn(c.transport_ptr)) {
                    SPDLOG_LOGGER_INFO(detail::gateway_logger(), "Gateway: starting [{}] '{}'", i, c.tag);
                    c.start_threads_fn(c.transport_ptr);
                } else {
                    SPDLOG_LOGGER_INFO(detail::gateway_logger(), "Gateway: [{}] '{}' already running, skipping start", i, c.tag);
                }
                // Verify the transport actually started before marking healthy
                c.health = c.is_running_fn(c.transport_ptr)
                    ? ConnHealth::Healthy : ConnHealth::Disconnected;
            }
        }
    }

    /// @brief Stop all running connections.
    ///
    /// Snapshots connection pointers under lock, then calls stop() outside
    /// the lock to avoid deadlock if the transport's stop() calls back into Gateway.
    void stop_all() noexcept {
        struct StopTarget {
            size_t idx; std::string tag; void* ptr;
            void (*fn)(void*); bool (*is_running_fn)(void*);
        };
        std::vector<StopTarget> targets;
        {
            std::lock_guard lock(mu_);
            for (size_t i = 0; i < connections_.size(); ++i) {
                auto& c = connections_[i];
                if (c.health != ConnHealth::Stopped && c.stop_fn) {
                    targets.push_back({i, c.tag, c.transport_ptr,
                                       c.stop_fn, c.is_running_fn});
                }
            }
        }
        for (auto& t : targets) {
            SPDLOG_LOGGER_INFO(detail::gateway_logger(), "Gateway: stopping [{}] '{}'", t.idx, t.tag);
            try {
                t.fn(t.ptr);
            } catch (...) {
                SPDLOG_LOGGER_ERROR(detail::gateway_logger(),
                    "Gateway: stop_fn threw for [{}] '{}' — continuing", t.idx, t.tag);
            }
        }
        // Update health after all stop calls complete, reflecting actual state.
        // This avoids marking a transport as Stopped if its stop_fn failed.
        {
            std::lock_guard lock(mu_);
            for (auto& t : targets) {
                if (t.idx < connections_.size()) {
                    auto& c = connections_[t.idx];
                    bool still_running = c.is_running_fn &&
                                         c.is_running_fn(c.transport_ptr);
                    c.health = still_running ? ConnHealth::Disconnected
                                             : ConnHealth::Stopped;
                }
            }
        }
    }

    /// @brief Force reconnect a specific connection.
    /// @param id  Connection index returned by add(). Ignored if out of range.
    void reconnect(size_t id) noexcept {
        std::lock_guard lock(mu_);
        if (id >= connections_.size()) return;
        auto& c = connections_[id];
        if (c.reconnect_fn) {
            SPDLOG_LOGGER_INFO(detail::gateway_logger(), "Gateway: reconnecting [{}] '{}'", id, c.tag);
            c.reconnect_fn(c.transport_ptr);
        }
    }

    // ── Health monitoring ───────────────────────────────────────────────

    /// @brief Start background health monitor thread.
    ///
    /// The monitor checks connection health at the configured interval.
    /// Idempotent -- calling when already running is a no-op.
    void start_monitor() noexcept {
        if (monitor_running_.exchange(true, std::memory_order_acq_rel)) return;
        monitor_thread_ = std::thread([this] { monitor_loop(); });
        SPDLOG_LOGGER_INFO(detail::gateway_logger(), "Gateway: health monitor started (interval={}ms)",
                     config_.health_check_interval.count());
    }

    /// @brief Stop the health monitor thread.
    ///
    /// Blocks until the monitor thread exits. Idempotent.
    void stop_monitor() noexcept {
        if (!monitor_running_.exchange(false, std::memory_order_acq_rel)) return;
        if (monitor_thread_.joinable()) monitor_thread_.join();
        SPDLOG_LOGGER_INFO(detail::gateway_logger(), "Gateway: health monitor stopped");
    }

    /// @brief Run one health check cycle (called by monitor loop or manually).
    ///
    /// Health-change callbacks are invoked OUTSIDE the lock to prevent
    /// deadlock if callbacks call back into Gateway methods.
    void check_health() noexcept {
        // Collect health changes under lock, then notify outside.
        struct HealthChange {
            size_t idx;
            std::string tag;
            ConnHealth old_h;
            ConnHealth new_h;
        };
        std::vector<HealthChange> changes;

        {
            std::lock_guard lock(mu_);
            for (size_t i = 0; i < connections_.size(); ++i) {
                auto& c = connections_[i];
                if (c.health == ConnHealth::Stopped) continue;

                ConnHealth old_h = c.health;
                if (c.is_running_fn && c.is_running_fn(c.transport_ptr)) {
                    c.health = ConnHealth::Healthy;
                } else {
                    c.health = ConnHealth::Disconnected;
                }

                if (old_h != c.health) {
                    changes.push_back({i, c.tag, old_h, c.health});
                }
            }
        }

        for (auto& ch : changes) {
            SPDLOG_LOGGER_WARN(detail::gateway_logger(), "Gateway: [{}] '{}' health {} → {}",
                        ch.idx, ch.tag, conn_health_name(ch.old_h),
                        conn_health_name(ch.new_h));
            if (config_.on_health_change) {
                config_.on_health_change(ch.tag, ch.old_h, ch.new_h);
            }
        }
    }

    /// @brief Formatted status dump of all connections.
    /// @return Multi-line string listing each connection's tag, health, running
    ///         state, and priority.
    [[nodiscard]] std::string dump() const {
        std::lock_guard lock(mu_);
        bool monitoring = monitor_running_.load(std::memory_order_relaxed);
        std::string result = std::format("Gateway: {} connections, monitor={}\n",
            connections_.size(), monitoring ? "running" : "stopped");
        for (size_t i = 0; i < connections_.size(); ++i) {
            auto& c = connections_[i];
            bool running = c.is_running_fn ? c.is_running_fn(c.transport_ptr) : false;
            result += std::format("  [{}] {} — health={} running={} priority={}\n",
                                  i, c.tag, conn_health_name(c.health),
                                  running ? "yes" : "no", c.priority);
        }
        return result;
    }

private:
    void monitor_loop() noexcept {
        while (monitor_running_.load(std::memory_order_acquire)) {
            check_health();
            // Sleep in small increments to allow quick shutdown
            auto end = std::chrono::steady_clock::now() + config_.health_check_interval;
            while (std::chrono::steady_clock::now() < end) {
                if (!monitor_running_.load(std::memory_order_relaxed)) return;
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
            }
        }
    }

    Config config_;
    mutable std::mutex mu_;
    std::vector<GatewayConnection> connections_;
    std::atomic<bool> monitor_running_{false};
    std::thread monitor_thread_;
};

} // namespace eph::net
