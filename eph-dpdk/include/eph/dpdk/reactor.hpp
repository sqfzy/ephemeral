#pragma once

/// @file reactor.hpp
/// Epoll-style multiplexed RX for multiple DPDK connections.
///
/// Replaces SharedRxDispatcher — zero ring overhead. A single RX thread
/// polls the NIC, identifies each packet's connection via 4-tuple match,
/// and directly invokes TcpSession::process_rx in-line. No intermediate
/// rte_ring, no per-connection RX thread.
///
/// Designed for NICs without RSS/Flow Director (e.g., AWS ENA). With
/// RSS-capable NICs, per-connection queues are preferred (no dispatcher
/// needed at all).
///
/// Usage:
///   Reactor reactor({.port_id = 0, .rx_queue_id = 0}, pool);
///   reactor.add_connection(&session1, on_data1);
///   reactor.add_connection(&session2, on_data2);
///   reactor.start();
///   // ... connections receive data via callbacks ...
///   reactor.stop();

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <string>
#include <thread>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <rte_ethdev.h>
#include <rte_mbuf.h>

#include "eph/dpdk/net_header.hpp"
#include "eph/dpdk/tcp.hpp"
#include "eph/utils/cpu.hpp"
#include "eph/utils/time.hpp"

namespace eph::dpdk {

namespace detail {
inline spdlog::logger* reactor_logger() {
    static auto l = [] {
        auto lg = spdlog::get("dpdk.reactor");
        if (!lg) lg = spdlog::stdout_color_mt("dpdk.reactor");
        return lg;
    }();
    return l.get();
}
} // namespace detail

/// Maximum connections a single Reactor can service.
/// Fixed array avoids heap allocation on the dispatch hot path.
inline constexpr size_t kReactorMaxConnections = 16;

/// Callback invoked for each received TCP payload segment.
/// @param data    Pointer to TCP payload (valid only during callback)
/// @param len     Payload length in bytes
/// @param conn_id Connection index in the reactor (0-based)
using ReactorDataCallback =
    std::function<void(const uint8_t* data, uint16_t len, size_t conn_id)>;

/// Callback invoked once per burst after all packets have been dispatched.
/// Use this to trigger batch processing (e.g., Transport::process_pending).
/// Only called when the NIC returned at least one packet (nb_rx > 0);
/// note that not all packets may have matched a registered connection.
using BurstCompleteCallback = std::function<void()>;

/// @brief Per-connection entry in the Reactor's fixed-size connection table.
///
/// Contains the TcpSession pointer (atomic for safe reconnection), the
/// connection 4-tuple for packet matching, a data callback, and a connected
/// flag that gates processing. The session pointer and connected flag use
/// atomic operations to support mark_reconnected() while the RX loop is running.
struct ReactorEntry {
    /// Atomic pointer — mark_reconnected() may swap this while the RX loop
    /// is running. The RX loop loads it once into a local before calling
    /// process_rx, so a concurrent store is safe (both old and new sessions
    /// must remain alive until the next burst cycle).
    std::atomic<TcpSession<>*> session{nullptr};
    net::ConnectionTuple tuple{};
    ReactorDataCallback on_data{};
    std::atomic<bool> connected{false};

    /// Direction-symmetric hash of the 4-tuple for fast rejection in linear scan.
    /// Uses XOR + sum (both commutative) so hash(A→B) == hash(B→A).
    /// This is essential because incoming packets have swapped src/dst
    /// relative to the registered connection tuple.
    [[nodiscard]] static uint64_t hash_tuple(const net::ConnectionTuple& t) noexcept {
        uint64_t h = 14695981039346656037ULL;
        auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ULL; };
        mix(t.src_ip ^ t.dst_ip);
        mix(static_cast<uint64_t>(t.src_ip) + t.dst_ip);
        mix(t.src_port ^ t.dst_port);
        mix(static_cast<uint64_t>(t.src_port) + t.dst_port);
        return h;
    }
};

/// Epoll-style single-thread multiplexed RX for up to kReactorMaxConnections
/// DPDK connections. Zero ring overhead — packets dispatch directly to
/// TcpSession::process_rx via inline callback.
///
/// Threading model:
///   - One RX thread (owned by Reactor) polls NIC and processes all connections
///   - TX threads remain per-connection (owned by Transport or user)
///   - Data delivery via callback (on_data) in the RX thread context
class Reactor {
public:
    /// @brief Reactor configuration: which port/queue to poll and optional CPU pinning.
    struct Config {
        uint16_t port_id      = 0;     ///< DPDK port ID to poll
        uint16_t rx_queue_id  = 0;     ///< RX queue index on the port
        int      rx_cpu       = -1;    ///< CPU affinity for RX thread (-1 = no pin)

        /// Validate configuration, returning an error description or empty string on success.
        [[nodiscard]] constexpr std::string_view validate() const noexcept {
            if (rx_cpu < -1)
                return "rx_cpu must be >= -1 (-1 = no pinning)";
            return {};
        }

        /// Multi-line formatted dump for logging/debugging.
        [[nodiscard]] std::string dump() const {
            return std::format("Reactor::Config(port={}, queue={}, cpu={})",
                               port_id, rx_queue_id, rx_cpu);
        }
    };

    explicit Reactor(Config config) noexcept
        : config_(config) {}

    ~Reactor() { stop(); }

    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;
    Reactor(Reactor&&) = delete;
    Reactor& operator=(Reactor&&) = delete;

    /// Add a connection. Session must be already connected.
    /// Must be called BEFORE start() — modifying entries_ while the RX loop
    /// is running is not safe (no lock on the hot path by design).
    /// Returns connection index (0-based) or error string.
    [[nodiscard]] std::expected<size_t, std::string>
    add_connection(TcpSession<>* session, ReactorDataCallback on_data) {
        if (running_.load(std::memory_order_acquire)) {
            return std::unexpected("add_connection() must be called before start()");
        }
        if (!session) return std::unexpected("session is null");
        size_t idx = count_.load(std::memory_order_relaxed);
        if (idx >= kReactorMaxConnections) {
            return std::unexpected(std::format(
                "reactor full ({}/{} connections)",
                idx, kReactorMaxConnections));
        }
        auto& e = entries_[idx];
        e.session.store(session, std::memory_order_relaxed);
        e.tuple = session->connection_tuple();
        e.on_data = std::move(on_data);
        e.connected.store(session->is_established(), std::memory_order_relaxed);
        hashes_[idx] = ReactorEntry::hash_tuple(e.tuple);

        SPDLOG_LOGGER_DEBUG(detail::reactor_logger(),
            "Added connection {}: {}:{} -> {}:{}",
            idx,
            net::format_ipv4(e.tuple.src_ip).data(), e.tuple.src_port,
            net::format_ipv4(e.tuple.dst_ip).data(), e.tuple.dst_port);

        count_.store(idx + 1, std::memory_order_release);
        return idx;
    }

    /// Start the reactor RX thread.
    void start() {
        if (running_.load(std::memory_order_relaxed)) return;
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { rx_loop(); });
    }

    /// Stop the reactor RX thread (blocks until joined).
    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        if (thread_.joinable()) thread_.join();
        SPDLOG_LOGGER_INFO(detail::reactor_logger(), "Reactor stopped");
    }

    /// Mark a connection as disconnected (skip processing until reconnected).
    /// Safe to call while the reactor is running.
    void mark_disconnected(size_t conn_id) noexcept {
        if (conn_id < count_.load(std::memory_order_acquire))
            entries_[conn_id].connected.store(false, std::memory_order_release);
    }

    /// Mark a connection as reconnected (resume processing).
    /// Safe to call while the reactor is running.
    ///
    /// Synchronization protocol:
    ///  1. Set connected=false (release) so the RX loop's next
    ///     acquire-load sees the disabled state and skips this entry.
    ///  2. Atomically store the new session pointer (release).  The RX
    ///     loop loads session with acquire AFTER its connected check,
    ///     so it either sees the old session (still valid — caller must
    ///     keep old session alive until the next burst cycle) or the
    ///     new one.  Either way, the pointer is valid.
    ///  3. Update tuple and hash (non-atomic writes, but safe because
    ///     the RX loop checks connected (acquire) BEFORE reading these
    ///     fields — the release/acquire pair on connected guarantees
    ///     visibility of the new tuple/hash values).
    ///  4. Set connected=true (release) to re-enable the entry.
    void mark_reconnected(size_t conn_id, TcpSession<>* new_session) noexcept {
        if (!new_session) return;
        if (conn_id < count_.load(std::memory_order_acquire)) {
            // Step 1: Disable so the RX loop skips this entry.
            entries_[conn_id].connected.store(false, std::memory_order_release);

            // Step 2: Atomically swap session pointer.
            entries_[conn_id].session.store(new_session, std::memory_order_release);

            // Step 3: Update tuple and hash for the new connection.
            entries_[conn_id].tuple = new_session->connection_tuple();
            hashes_[conn_id] = ReactorEntry::hash_tuple(entries_[conn_id].tuple);

            // Step 4: Re-enable the entry for the RX loop.
            entries_[conn_id].connected.store(true, std::memory_order_release);
        }
    }

    /// Register a callback invoked after each NIC burst + dispatch cycle.
    /// Must be called before start().
    void set_on_burst_complete(BurstCompleteCallback cb) {
        on_burst_complete_ = std::move(cb);
    }

    /// @brief Number of registered connections (may include disconnected entries).
    [[nodiscard]] size_t connection_count() const noexcept {
        return count_.load(std::memory_order_acquire);
    }

    /// @brief Check if the RX polling thread is currently running.
    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    /// Access entry by index (for stats, diagnostics).
    /// Returns a static empty entry if index is out of bounds.
    [[nodiscard]] const ReactorEntry& entry(size_t i) const noexcept {
        if (i >= count_.load(std::memory_order_acquire)) [[unlikely]] {
            static const ReactorEntry empty{};
            return empty;
        }
        return entries_[i];
    }

private:
    void rx_loop() {
        if (config_.rx_cpu >= 0) {
            [[maybe_unused]] auto affinity_ok = eph::utils::set_thread_affinity(config_.rx_cpu, "reactor_rx");
        }

        [[maybe_unused]] auto* log = detail::reactor_logger();
        SPDLOG_LOGGER_INFO(log,
            "Reactor RX loop started: {} connections, port={}, queue={}, cpu={}",
            count_.load(std::memory_order_acquire), config_.port_id, config_.rx_queue_id, config_.rx_cpu);

        rte_mbuf* pkts[32];

        while (running_.load(std::memory_order_acquire)) {
            uint16_t nb_rx = rte_eth_rx_burst(
                config_.port_id, config_.rx_queue_id, pkts, 32);

            if (nb_rx == 0) continue;

            const uint64_t burst_tsc = eph::utils::TSC::now();

            for (uint16_t i = 0; i < nb_rx; ++i) {
                auto parsed = net::parse_packet(pkts[i]);
                if (!parsed.tcp) {
                    rte_pktmbuf_free(pkts[i]);
                    continue;
                }

                // Build packet's 4-tuple hash for fast rejection
                net::ConnectionTuple pkt_tuple;
                pkt_tuple.src_ip = parsed.src_ip();
                pkt_tuple.dst_ip = parsed.dst_ip();
                pkt_tuple.src_port = parsed.src_port();
                pkt_tuple.dst_port = parsed.dst_port();
                const uint64_t pkt_hash = ReactorEntry::hash_tuple(pkt_tuple);

                // PERF: Connection dispatch uses linear scan over registered connections.
                // For typical HFT deployments (2-4 connections), this is optimal (no indirection overhead).
                // For >8 connections, consider a hash map lookup by {src_ip, src_port, dst_ip, dst_port}
                // tuple. Benchmark before optimizing — cache-friendly linear scan often beats hash lookup
                // for small N.
                // Linear scan with hash pre-filter (≤16 entries, cache-friendly)
                bool matched = false;
                const size_t n = count_.load(std::memory_order_acquire);
                for (size_t j = 0; j < n; ++j) {
                    auto& entry = entries_[j];
                    // Check connected FIRST (acquire) — provides happens-before
                    // guarantee for subsequent reads of hashes_[j] and entry.tuple,
                    // which are written non-atomically in mark_reconnected() before
                    // the release-store to connected=true.
                    if (!entry.connected.load(std::memory_order_acquire)) continue;
                    if (hashes_[j] != pkt_hash) continue;  // Fast reject
                    if (!parsed.matches(entry.tuple)) continue;  // Collision check

                    // Load session pointer once into a local — safe even if
                    // mark_reconnected() atomically swaps it concurrently.
                    auto* sess = entry.session.load(std::memory_order_acquire);
                    if (!sess) {
                        rte_pktmbuf_free(pkts[i]);
                        matched = true;
                        break;
                    }

                    // Propagate arrival TSC for latency measurement
                    sess->set_last_rx_burst_tsc(burst_tsc);

                    // Direct dispatch — zero ring overhead.
                    // process_rx handles TCP state (seq/ack/flags) and
                    // delivers payload via the inline callback.
                    // process_rx takes ownership of pkts[i] — it frees the mbuf
                    // internally via free_list or on error via free_remaining.
                    // No additional rte_pktmbuf_free needed on the matched path.
                    auto result = sess->process_rx(
                        &pkts[i], 1,
                        [&](const uint8_t* data, uint16_t len) {
                            entry.on_data(data, len, j);
                        });

                    if (!result) {
                        SPDLOG_LOGGER_WARN(log,
                            "Connection {} process_rx error: {}",
                            j, result.error());
                        entry.connected.store(false, std::memory_order_release);
                    }

                    sess->flush_pending_ack();
                    matched = true;
                    break;
                }

                if (!matched) {
                    rte_pktmbuf_free(pkts[i]);
                }
            }

            // Per-burst completion callback — ideal for batch-processing
            // accumulated data (e.g., calling Transport::process_pending()
            // for each connection that received data in this burst).
            if (on_burst_complete_) {
                on_burst_complete_();
            }
        }

        SPDLOG_LOGGER_DEBUG(log, "Reactor RX loop exited");
    }

    Config config_;
    std::array<ReactorEntry, kReactorMaxConnections> entries_{};
    std::array<uint64_t, kReactorMaxConnections> hashes_{};
    std::atomic<size_t> count_{0};
    std::atomic<bool> running_{false};
    std::thread thread_;
    BurstCompleteCallback on_burst_complete_{};
};

} // namespace eph::dpdk
