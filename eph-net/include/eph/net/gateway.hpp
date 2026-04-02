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
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace eph::net {

namespace detail {
inline spdlog::logger* gateway_logger() {
    static auto l = [] {
        try {
            return spdlog::stdout_color_mt("net.gateway");
        } catch (const spdlog::spdlog_ex&) {
            return spdlog::get("net.gateway");
        }
    }();
    return l.get();
}
} // namespace detail

/// Connection health status.
enum class ConnHealth : uint8_t {
    Healthy,      ///< Connected and receiving data
    Degraded,     ///< Connected but no data received recently (not yet implemented — reserved for future use)
    Disconnected, ///< Not connected
    Stopped,      ///< Intentionally stopped
};

/// Human-readable health status name.
[[nodiscard]] inline constexpr std::string_view conn_health_name(ConnHealth h) noexcept {
    switch (h) {
    case ConnHealth::Healthy:      return "HEALTHY";
    case ConnHealth::Degraded:     return "DEGRADED";
    case ConnHealth::Disconnected: return "DISCONNECTED";
    case ConnHealth::Stopped:      return "STOPPED";
    }
    return "UNKNOWN";
}

/// Type-erased connection wrapper for Gateway.
/// Stores a transport with its metadata and health state.
struct GatewayConnection {
    /// Unique tag for this connection (e.g., "binance-btcusdt").
    std::string tag;

    /// Type-erased transport handle.
    void* transport_ptr = nullptr;
    void (*stop_fn)(void*) = nullptr;
    bool (*is_running_fn)(void*) = nullptr;
    void (*start_threads_fn)(void*) = nullptr;
    void (*reconnect_fn)(void*) = nullptr;

    /// Health tracking.
    ConnHealth health = ConnHealth::Stopped;
    uint64_t last_health_check_ns = 0;

    /// Connection priority (lower = more important). Used for log ordering.
    uint8_t priority = 128;
};

/// Multi-connection lifecycle manager.
///
/// Thread-safe: all methods are mutex-protected. The monitor thread
/// runs in background and checks health periodically.
///
/// Not copyable/movable — designed as a singleton-like coordinator.
class Gateway {
public:
    struct Config {
        /// How often to check connection health (0 = no monitoring).
        std::chrono::milliseconds health_check_interval{5000};
        /// Mark connection as degraded if no activity for this duration.
        /// Not yet implemented — requires per-connection last-activity timestamps
        /// tracked via a callback from Transport::on_data(). When implemented,
        /// check_health() will compare now - last_activity against this threshold
        /// and transition Healthy → Degraded accordingly.
        std::chrono::milliseconds degraded_threshold{30000};
        /// Optional callback when health changes.
        std::function<void(std::string_view tag, ConnHealth old_h, ConnHealth new_h)>
            on_health_change{};
    };

    Gateway() = default;

    explicit Gateway(Config config)
        : config_(std::move(config)) {}

    ~Gateway() noexcept {
        stop_monitor();
        stop_all();
    }

    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;
    Gateway(Gateway&&) = delete;
    Gateway& operator=(Gateway&&) = delete;

    // ── Connection management ───────────────────────────────────────────

    /// Add a transport to the gateway. Returns connection index.
    /// The transport must outlive the Gateway (or be removed first).
    template <typename Transport>
    size_t add(std::string tag, Transport* tp, uint8_t priority = 128) {
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

    /// Number of managed connections.
    [[nodiscard]] size_t connection_count() const noexcept {
        std::lock_guard lock(mu_);
        return connections_.size();
    }

    /// Get connection health by index.
    [[nodiscard]] ConnHealth health(size_t id) const noexcept {
        std::lock_guard lock(mu_);
        if (id >= connections_.size()) return ConnHealth::Stopped;
        return connections_[id].health;
    }

    /// Get connection tag by index.
    [[nodiscard]] std::string tag(size_t id) const {
        std::lock_guard lock(mu_);
        if (id >= connections_.size()) return "";
        return connections_[id].tag;
    }

    // ── Lifecycle control ───────────────────────────────────────────────

    /// Start all stopped connections' threads.
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
                c.health = ConnHealth::Healthy;
            }
        }
    }

    /// Stop all running connections.
    void stop_all() noexcept {
        std::lock_guard lock(mu_);
        for (size_t i = 0; i < connections_.size(); ++i) {
            auto& c = connections_[i];
            if (c.health != ConnHealth::Stopped && c.stop_fn) {
                SPDLOG_LOGGER_INFO(detail::gateway_logger(), "Gateway: stopping [{}] '{}'", i, c.tag);
                c.stop_fn(c.transport_ptr);
                c.health = ConnHealth::Stopped;
            }
        }
    }

    /// Force reconnect a specific connection.
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

    /// Start background health monitor thread.
    void start_monitor() noexcept {
        if (monitor_running_.exchange(true, std::memory_order_acq_rel)) return;
        monitor_thread_ = std::thread([this] { monitor_loop(); });
        SPDLOG_LOGGER_INFO(detail::gateway_logger(), "Gateway: health monitor started (interval={}ms)",
                     config_.health_check_interval.count());
    }

    /// Stop the health monitor thread.
    void stop_monitor() noexcept {
        if (!monitor_running_.exchange(false, std::memory_order_acq_rel)) return;
        if (monitor_thread_.joinable()) monitor_thread_.join();
        SPDLOG_LOGGER_INFO(detail::gateway_logger(), "Gateway: health monitor stopped");
    }

    /// Run one health check cycle (called by monitor loop or manually).
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

    /// Formatted status dump of all connections.
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
