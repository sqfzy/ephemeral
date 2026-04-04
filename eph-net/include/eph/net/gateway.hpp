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
#include <concepts>
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

#include "eph/core/detail/json_escape.hpp"
#include "eph/net/kill_switch.hpp"  // for Stoppable concept

namespace eph::net {

/// @brief Concept for types manageable by Gateway.
///
/// Refines Stoppable (stop() + is_running()) with start_threads() and
/// reconnect_now(), matching the full lifecycle API that Gateway type-erases.
/// Expressing this as a refinement ensures that any GatewayManageable type
/// is also usable with KillSwitch (which requires only Stoppable).
template <typename T>
concept GatewayManageable = Stoppable<T> && requires(T t) {
    { t.start_threads() };
    { t.reconnect_now() };
};

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
    Degraded,     ///< Connected but no new RX packets for longer than degraded_threshold
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
/// Transport operations are accessed through std::function callbacks
/// that capture the transport pointer, avoiding a common base class.
struct GatewayConnection {
    /// @brief Unique tag for this connection (e.g., "binance-btcusdt").
    std::string tag;

    /// @brief Stop callback (calls Transport::stop()).
    std::function<void()> stop_fn{};
    /// @brief Running check callback (calls Transport::is_running()).
    std::function<bool()> is_running_fn{};
    /// @brief Thread launcher callback (calls Transport::start_threads()).
    std::function<void()> start_threads_fn{};
    /// @brief Reconnect trigger callback (calls Transport::reconnect_now()).
    std::function<void()> reconnect_fn{};

    /// @brief Current health status of this connection.
    ConnHealth health = ConnHealth::Stopped;
    /// @brief Timestamp (ns) of last health check for this connection.
    uint64_t last_health_check_ns = 0;
    /// @brief RX packet counter callback for degraded detection.
    /// Returns current rx_packets from transport stats, or 0 if unavailable.
    std::function<uint64_t()> rx_packets_fn{};
    /// @brief RX packet count at last health check (for delta detection).
    uint64_t last_rx_packets = 0;
    /// @brief Timestamp (ns) when zero-delta was first observed. 0 = not degraded.
    uint64_t degraded_since_ns = 0;

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
        /// Mark connection as degraded if no new RX packets for this duration.
        /// When a transport provides stats().rx_packets (detected at compile time
        /// via if-constexpr), check_health() compares the current rx_packets count
        /// against the previous check. If the count is unchanged for longer than
        /// this threshold, the connection transitions Healthy -> Degraded.
        /// Transports without stats().rx_packets fall back to is_running()-only checks.
        std::chrono::milliseconds degraded_threshold{30000};
        /// Optional callback invoked when a connection's health changes.
        /// Called outside the Gateway lock to prevent deadlock.
        std::function<void(std::string_view tag, ConnHealth old_h, ConnHealth new_h)>
            on_health_change{};

        /// Validate configuration, returning an error description or empty string on success.
        /// Call before constructing a Gateway for early, actionable error messages.
        [[nodiscard]] constexpr std::string_view validate() const noexcept {
            if (health_check_interval.count() < 0)
                return "health_check_interval must be >= 0 (0 disables monitoring)";
            if (degraded_threshold.count() < 0)
                return "degraded_threshold must be >= 0";
            if (health_check_interval.count() > 0 && degraded_threshold.count() > 0 &&
                degraded_threshold <= health_check_interval)
                return "degraded_threshold must be greater than health_check_interval "
                       "(otherwise degraded detection fires every check cycle)";
            return {};
        }

        /// Multi-line formatted dump for logging/debugging.
        /// Callbacks are shown as set/unset (closures cannot be serialized).
        [[nodiscard]] std::string dump() const {
            return std::format(
                "Gateway::Config:\n"
                "  health_check_interval: {}ms\n"
                "  degraded_threshold: {}ms\n"
                "  on_health_change: {}",
                health_check_interval.count(),
                degraded_threshold.count(),
                static_cast<bool>(on_health_change) ? "set" : "unset");
        }

        /// JSON-formatted config for monitoring system integration.
        [[nodiscard]] std::string to_json() const {
            return std::format(
                "{{\"health_check_interval_ms\":{},\"degraded_threshold_ms\":{},"
                "\"on_health_change\":{}}}",
                health_check_interval.count(),
                degraded_threshold.count(),
                static_cast<bool>(on_health_change) ? "true" : "false");
        }

        /// Equality comparison. Compares interval and threshold values.
        /// Callbacks are compared by presence (both set or both unset), not identity,
        /// because std::function does not support meaningful equality comparison.
        [[nodiscard]] friend bool operator==(const Config& a, const Config& b) noexcept {
            return a.health_check_interval == b.health_check_interval &&
                   a.degraded_threshold == b.degraded_threshold &&
                   static_cast<bool>(a.on_health_change) == static_cast<bool>(b.on_health_change);
        }

        /// Check for non-fatal contradictions or likely misconfigurations.
        /// Returns a list of warning messages (empty if no issues).
        /// Unlike validate() which blocks construction, these are advisory.
        [[nodiscard]] std::vector<std::string> warnings() const {
            std::vector<std::string> w;
            if (health_check_interval.count() > 0 && health_check_interval.count() < 100)
                w.emplace_back(std::format(
                    "health_check_interval={}ms is very short — frequent "
                    "is_running() polls may add overhead", health_check_interval.count()));
            if (health_check_interval.count() > 60000)
                w.emplace_back(std::format(
                    "health_check_interval={}ms (>60s) — disconnections may "
                    "go undetected for over a minute",
                    health_check_interval.count()));
            if (!on_health_change)
                w.emplace_back("on_health_change callback not set — health "
                               "transitions will only appear in logs");
            return w;
        }
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
    /// @warning The transport must outlive the Gateway (or be removed via
    /// remove()/remove_by_tag() first). Gateway captures a non-owning pointer
    /// in closures; destroying a transport without removing it causes UB.
    ///
    /// @tparam Transport  Any type satisfying the GatewayManageable concept.
    /// @param tag       Human-readable identifier for this connection (e.g., "binance-btcusdt").
    /// @param tp        Pointer to the transport instance. Must not be null.
    /// @param priority  Connection priority (lower = more important). Used for log ordering.
    /// @return Connection index for use with health(), tag(), reconnect(), etc.
    ///         Returns SIZE_MAX if tp is null.
    template <GatewayManageable Transport>
    [[nodiscard]] size_t add(std::string tag, Transport* tp, uint8_t priority = 128) {
        if (!tp) {
            SPDLOG_LOGGER_ERROR(detail::gateway_logger(), "Gateway::add: null transport for tag '{}'", tag);
            return SIZE_MAX;
        }

        GatewayConnection conn;
        conn.tag = std::move(tag);
        conn.stop_fn = [tp] { tp->stop(); };
        conn.is_running_fn = [tp] { return tp->is_running(); };
        conn.start_threads_fn = [tp] { tp->start_threads(); };
        conn.reconnect_fn = [tp] { tp->reconnect_now(); };
        if constexpr (requires { tp->stats().rx_packets; }) {
            conn.rx_packets_fn = [tp]() -> uint64_t {
                return tp->stats().rx_packets;
            };
        }
        conn.priority = priority;
        conn.health = tp->is_running() ? ConnHealth::Healthy : ConnHealth::Stopped;

        std::lock_guard lock(mu_);
        size_t id = connections_.size();
        connections_.push_back(std::move(conn));
        SPDLOG_LOGGER_INFO(detail::gateway_logger(), "Gateway: added connection [{}] '{}' (priority={})",
                     id, connections_[id].tag, priority);
        return id;
    }

    /// @brief Remove a connection by index.
    ///
    /// Stops the connection if it is running, then removes it from the
    /// managed list. Indices of connections added after this one shift down.
    ///
    /// @param id  Connection index returned by add().
    /// @return true if the connection was found and removed, false if out of range.
    bool remove(size_t id) noexcept {
        std::function<void()> stop_fn;
        std::function<bool()> is_running_fn;
        std::string removed_tag;

        {
            std::lock_guard lock(mu_);
            if (id >= connections_.size()) return false;
            auto& c = connections_[id];
            removed_tag = c.tag;
            stop_fn = c.stop_fn;
            is_running_fn = c.is_running_fn;
            connections_.erase(connections_.begin() + static_cast<ptrdiff_t>(id));
        }

        // Stop outside the lock to avoid deadlock if stop() calls back into Gateway
        if (stop_fn && is_running_fn && is_running_fn()) {
            SPDLOG_LOGGER_DEBUG(detail::gateway_logger(),
                "Gateway: stopping removed connection [{}] '{}' before removal", id, removed_tag);
            try { stop_fn(); } catch (...) {
                SPDLOG_LOGGER_ERROR(detail::gateway_logger(),
                    "Gateway: stop_fn threw for [{}] '{}' during removal", id, removed_tag);
            }
        }
        SPDLOG_LOGGER_INFO(detail::gateway_logger(),
            "Gateway: removed connection [{}] '{}'", id, removed_tag);
        return true;
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

    /// @brief Remove a connection by tag name.
    ///
    /// Atomic alternative to find_by_tag() + remove() that avoids TOCTOU
    /// issues from index shifts between the two calls.
    ///
    /// @param tag  Tag string of the connection to remove.
    /// @return true if found and removed, false if no connection with that tag exists.
    bool remove_by_tag(std::string_view tag) noexcept {
        std::function<void()> stop_fn;
        std::function<bool()> is_running_fn;
        std::string removed_tag;
        bool found = false;

        {
            std::lock_guard lock(mu_);
            for (size_t i = 0; i < connections_.size(); ++i) {
                if (connections_[i].tag == tag) {
                    auto& c = connections_[i];
                    removed_tag = c.tag;
                    stop_fn = c.stop_fn;
                    is_running_fn = c.is_running_fn;
                    connections_.erase(connections_.begin() + static_cast<ptrdiff_t>(i));
                    found = true;
                    break;
                }
            }
        }

        if (!found) return false;

        // Stop outside the lock to avoid deadlock if stop() calls back into Gateway
        if (stop_fn && is_running_fn && is_running_fn()) {
            SPDLOG_LOGGER_DEBUG(detail::gateway_logger(),
                "Gateway: stopping '{}' before removal", removed_tag);
            try { stop_fn(); } catch (...) {
                SPDLOG_LOGGER_ERROR(detail::gateway_logger(),
                    "Gateway: stop_fn threw for '{}' during removal", removed_tag);
            }
        }
        SPDLOG_LOGGER_INFO(detail::gateway_logger(),
            "Gateway: removed connection '{}'", removed_tag);
        return true;
    }

    /// @brief Look up a connection index by tag.
    ///
    /// Linear scan -- suitable for small connection counts typical of HFT
    /// gateways (2-8 connections). For O(1) lookup, maintain an external map.
    ///
    /// @param tag  Tag string to search for.
    /// @return Connection index, or SIZE_MAX if not found.
    [[nodiscard]] size_t find_by_tag(std::string_view tag) const noexcept {
        std::lock_guard lock(mu_);
        for (size_t i = 0; i < connections_.size(); ++i) {
            if (connections_[i].tag == tag) return i;
        }
        return SIZE_MAX;
    }

    /// @brief Count of connections currently in Healthy state.
    /// @return Number of connections with health == ConnHealth::Healthy.
    [[nodiscard]] size_t healthy_count() const noexcept {
        std::lock_guard lock(mu_);
        size_t n = 0;
        for (const auto& c : connections_) {
            if (c.health == ConnHealth::Healthy) ++n;
        }
        return n;
    }

    /// @brief Check if all managed connections are healthy.
    ///
    /// Convenience predicate for health gates: equivalent to
    /// `healthy_count() == connection_count()` but avoids double iteration.
    /// Returns true when there are no connections (vacuous truth).
    ///
    /// @return true if every connection has ConnHealth::Healthy status.
    [[nodiscard]] bool is_all_healthy() const noexcept {
        std::lock_guard lock(mu_);
        for (const auto& c : connections_) {
            if (c.health != ConnHealth::Healthy) return false;
        }
        return true;
    }

    /// @brief Check if any managed connection is disconnected.
    ///
    /// Useful for alerting: fires when at least one connection has dropped
    /// but hasn't been intentionally stopped.
    ///
    /// @return true if any connection has ConnHealth::Disconnected status.
    [[nodiscard]] bool is_any_disconnected() const noexcept {
        std::lock_guard lock(mu_);
        for (const auto& c : connections_) {
            if (c.health == ConnHealth::Disconnected) return true;
        }
        return false;
    }

    /// @brief Get all connection tags as a vector.
    ///
    /// Convenience method for logging, diagnostics, and test assertions.
    /// Returns a snapshot -- the vector is not updated if connections change.
    ///
    /// @return Vector of tag strings for all managed connections, in index order.
    [[nodiscard]] std::vector<std::string> connection_tags() const {
        std::lock_guard lock(mu_);
        std::vector<std::string> tags;
        tags.reserve(connections_.size());
        for (const auto& c : connections_) {
            tags.push_back(c.tag);
        }
        return tags;
    }

    /// @brief Iterate all connections with a callback.
    ///
    /// The callback receives the connection index, tag, and health for each
    /// managed connection. Useful for building status dashboards, metrics
    /// collection, or custom health reporting without exposing internals.
    ///
    /// The callback is invoked under the Gateway lock -- it must not call
    /// other Gateway methods (deadlock) or block for extended periods.
    ///
    /// @tparam F  Callable as void(size_t index, std::string_view tag, ConnHealth health).
    /// @param fn  The callback to invoke for each connection.
    template <typename F>
        requires std::invocable<F, size_t, std::string_view, ConnHealth>
    void for_each(F&& fn) const {
        std::lock_guard lock(mu_);
        for (size_t i = 0; i < connections_.size(); ++i) {
            fn(i, connections_[i].tag, connections_[i].health);
        }
    }

    // ── Lifecycle control ───────────────────────────────────────────────

    /// @brief Start all stopped connections' threads.
    ///
    /// Only starts connections whose health is ConnHealth::Stopped and whose
    /// transport is not already running. Skips duplicates safely.
    ///
    /// Transport start functions are called outside the lock to prevent
    /// deadlock if start_threads() calls back into Gateway.
    void start_all() noexcept {
        struct StartTarget {
            std::string tag;
            std::function<void()> start_fn;
            std::function<bool()> is_running_fn;
        };
        std::vector<StartTarget> targets;
        {
            std::lock_guard lock(mu_);
            for (size_t i = 0; i < connections_.size(); ++i) {
                auto& c = connections_[i];
                if (c.health == ConnHealth::Stopped && c.start_threads_fn) {
                    targets.push_back({c.tag, c.start_threads_fn, c.is_running_fn});
                }
            }
        }
        for (auto& t : targets) {
            // Guard against spawning duplicate threads if the transport
            // is somehow already running (e.g. started externally).
            bool already_running = false;
            try {
                already_running = t.is_running_fn && t.is_running_fn();
            } catch (...) {
                SPDLOG_LOGGER_ERROR(detail::gateway_logger(),
                    "Gateway: is_running_fn threw for '{}' during start pre-check", t.tag);
            }
            if (already_running) {
                SPDLOG_LOGGER_INFO(detail::gateway_logger(),
                    "Gateway: '{}' already running, skipping start", t.tag);
            } else {
                SPDLOG_LOGGER_INFO(detail::gateway_logger(), "Gateway: starting '{}'", t.tag);
                try {
                    t.start_fn();
                } catch (...) {
                    SPDLOG_LOGGER_ERROR(detail::gateway_logger(),
                        "Gateway: start_threads_fn threw for '{}'", t.tag);
                }
            }
        }
        // Update health after all start calls complete.
        // Match by tag rather than index to handle concurrent
        // remove() calls that may shift indices during unlocked start phase.
        {
            std::lock_guard lock(mu_);
            for (auto& t : targets) {
                for (auto& c : connections_) {
                    if (c.tag == t.tag) {
                        bool running = false;
                        try {
                            running = c.is_running_fn && c.is_running_fn();
                        } catch (...) {
                            SPDLOG_LOGGER_ERROR(detail::gateway_logger(),
                                "Gateway: is_running_fn threw for '{}' during start health update", t.tag);
                        }
                        c.health = running ? ConnHealth::Healthy : ConnHealth::Disconnected;
                        break;
                    }
                }
            }
        }
    }

    /// @brief Stop all running connections.
    ///
    /// Snapshots connection pointers under lock, then calls stop() outside
    /// the lock to avoid deadlock if the transport's stop() calls back into Gateway.
    void stop_all() noexcept {
        struct StopTarget {
            std::string tag;
            std::function<void()> stop_fn;
            std::function<bool()> is_running_fn;
        };
        std::vector<StopTarget> targets;
        {
            std::lock_guard lock(mu_);
            for (size_t i = 0; i < connections_.size(); ++i) {
                auto& c = connections_[i];
                if (c.health != ConnHealth::Stopped && c.stop_fn) {
                    targets.push_back({c.tag, c.stop_fn, c.is_running_fn});
                }
            }
        }
        for (auto& t : targets) {
            SPDLOG_LOGGER_INFO(detail::gateway_logger(), "Gateway: stopping '{}'", t.tag);
            try {
                t.stop_fn();
            } catch (...) {
                SPDLOG_LOGGER_ERROR(detail::gateway_logger(),
                    "Gateway: stop_fn threw for '{}' — continuing", t.tag);
            }
        }
        // Update health after all stop calls complete, reflecting actual state.
        // Match by tag rather than index to handle concurrent
        // remove() calls that may shift indices during unlocked stop phase.
        {
            std::lock_guard lock(mu_);
            for (auto& t : targets) {
                for (auto& c : connections_) {
                    if (c.tag == t.tag) {
                        bool still_running = false;
                        try {
                            still_running = c.is_running_fn &&
                                            c.is_running_fn();
                        } catch (...) {
                            SPDLOG_LOGGER_ERROR(detail::gateway_logger(),
                                "Gateway: is_running_fn threw for '{}' during stop health update", t.tag);
                        }
                        c.health = still_running ? ConnHealth::Disconnected
                                                 : ConnHealth::Stopped;
                        break;
                    }
                }
            }
        }
    }

    /// @brief Force reconnect a specific connection.
    ///
    /// The reconnect function is called OUTSIDE the lock to prevent deadlock
    /// if the transport's reconnect_now() calls back into Gateway methods
    /// (same pattern as stop_all()).
    ///
    /// @param id  Connection index returned by add(). Ignored if out of range.
    void reconnect(size_t id) noexcept {
        std::function<void()> fn;
        std::string tag;
        {
            std::lock_guard lock(mu_);
            if (id >= connections_.size()) return;
            auto& c = connections_[id];
            fn = c.reconnect_fn;
            tag = c.tag;
        }
        if (fn) {
            SPDLOG_LOGGER_INFO(detail::gateway_logger(), "Gateway: reconnecting [{}] '{}'", id, tag);
            try {
                fn();
            } catch (...) {
                SPDLOG_LOGGER_ERROR(detail::gateway_logger(),
                    "Gateway: reconnect_fn threw for [{}] '{}'", id, tag);
            }
        }
    }

    /// @brief Force reconnect a connection identified by tag.
    ///
    /// Atomic alternative to find_by_tag() + reconnect() that avoids TOCTOU
    /// issues from index shifts between the two calls.
    ///
    /// @param tag  Tag string of the connection to reconnect.
    /// @return true if found and reconnect triggered, false if tag not found.
    bool reconnect_by_tag(std::string_view tag) noexcept {
        std::function<void()> fn;
        std::string found_tag;
        {
            std::lock_guard lock(mu_);
            for (auto& c : connections_) {
                if (c.tag == tag) {
                    fn = c.reconnect_fn;
                    found_tag = c.tag;
                    break;
                }
            }
        }
        if (!fn) return false;

        SPDLOG_LOGGER_INFO(detail::gateway_logger(), "Gateway: reconnecting '{}'", found_tag);
        try {
            fn();
        } catch (...) {
            SPDLOG_LOGGER_ERROR(detail::gateway_logger(),
                "Gateway: reconnect_fn threw for '{}'", found_tag);
        }
        return true;
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
                bool running = false;
                try {
                    running = c.is_running_fn && c.is_running_fn();
                } catch (...) {
                    SPDLOG_LOGGER_ERROR(detail::gateway_logger(),
                        "Gateway: is_running_fn threw for '{}' during health check", c.tag);
                }
                if (!running) {
                    c.health = ConnHealth::Disconnected;
                    c.degraded_since_ns = 0;
                } else if (c.rx_packets_fn) {
                    uint64_t current_rx = 0;
                    try {
                        current_rx = c.rx_packets_fn();
                    } catch (...) {
                        SPDLOG_LOGGER_ERROR(detail::gateway_logger(),
                            "Gateway: rx_packets_fn threw for '{}'", c.tag);
                    }
                    if (current_rx > c.last_rx_packets) {
                        // New data received — healthy
                        c.health = ConnHealth::Healthy;
                        c.degraded_since_ns = 0;
                    } else {
                        // No new data since last check
                        auto now_ns = static_cast<uint64_t>(
                            std::chrono::steady_clock::now().time_since_epoch().count());
                        if (c.degraded_since_ns == 0) {
                            c.degraded_since_ns = now_ns;
                        }
                        auto elapsed = std::chrono::nanoseconds(now_ns - c.degraded_since_ns);
                        if (elapsed >= config_.degraded_threshold) {
                            c.health = ConnHealth::Degraded;
                        } else {
                            c.health = ConnHealth::Healthy;
                        }
                    }
                    c.last_rx_packets = current_rx;
                } else {
                    // No rx_packets_fn — fall back to running = healthy
                    c.health = ConnHealth::Healthy;
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
                try {
                    config_.on_health_change(ch.tag, ch.old_h, ch.new_h);
                } catch (...) {
                    SPDLOG_LOGGER_ERROR(detail::gateway_logger(),
                        "Gateway: on_health_change callback threw for [{}] '{}' — continuing",
                        ch.idx, ch.tag);
                }
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
            bool running = c.is_running_fn ? c.is_running_fn() : false;
            result += std::format("  [{}] {} — health={} running={} priority={}\n",
                                  i, c.tag, conn_health_name(c.health),
                                  running ? "yes" : "no", c.priority);
        }
        return result;
    }

    /// @brief JSON-formatted snapshot of gateway state for monitoring.
    ///
    /// Includes connection count, healthy count, monitor status, and
    /// per-connection details (tag, health, priority).
    /// Thread-safe: takes a lock to read consistent state.
    [[nodiscard]] std::string to_json() const {
        std::lock_guard lock(mu_);
        bool monitoring = monitor_running_.load(std::memory_order_relaxed);
        size_t n_healthy = 0;
        for (const auto& c : connections_)
            if (c.health == ConnHealth::Healthy) ++n_healthy;

        std::string result = std::format(
            "{{\"connection_count\":{},\"healthy_count\":{},\"monitor_running\":{},"
            "\"config\":{},\"connections\":[",
            connections_.size(),
            n_healthy,
            monitoring ? "true" : "false",
            config_.to_json());
        for (size_t i = 0; i < connections_.size(); ++i) {
            if (i > 0) result += ',';
            const auto& c = connections_[i];
            result += std::format(
                "{{\"tag\":\"{}\",\"health\":\"{}\",\"priority\":{}}}",
                eph::core::detail::json_escape(c.tag),
                conn_health_name(c.health), c.priority);
        }
        result += "]}";
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

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specialization for ConnHealth
// ─────────────────────────────────────────────────────────────────────────────

/// @brief std::formatter specialization for ConnHealth.
///
/// Formats as "HEALTHY", "DEGRADED", "DISCONNECTED", or "STOPPED".
template <>
struct std::formatter<eph::net::ConnHealth> : std::formatter<std::string_view> {
    auto format(eph::net::ConnHealth h, auto& ctx) const {
        return std::formatter<std::string_view>::format(
            eph::net::conn_health_name(h), ctx);
    }
};

/// @brief std::formatter for Gateway::Config -- compact one-line summary.
template <>
struct std::formatter<eph::net::Gateway::Config> : std::formatter<std::string> {
    auto format(const eph::net::Gateway::Config& c, auto& ctx) const {
        return std::formatter<std::string>::format(
            std::format("Gateway::Config(health={}ms, degraded={}ms)",
                c.health_check_interval.count(),
                c.degraded_threshold.count()),
            ctx);
    }
};
