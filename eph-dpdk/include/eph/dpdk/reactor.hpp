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
    // try/catch handles the race between concurrent first callers:
    // stdout_color_mt throws spdlog_ex if the name is already registered.
    static auto l = [] {
        try {
            return spdlog::stdout_color_mt("dpdk.reactor");
        } catch (const spdlog::spdlog_ex&) {
            return spdlog::get("dpdk.reactor");
        }
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

/// Per-connection entry in the Reactor.
struct ReactorEntry {
    TcpSession<>* session = nullptr;
    net::ConnectionTuple tuple{};
    ReactorDataCallback on_data{};
    std::atomic<bool> connected{false};

    /// FNV-1a hash of the 4-tuple for fast rejection in linear scan.
    [[nodiscard]] static uint64_t hash_tuple(const net::ConnectionTuple& t) noexcept {
        uint64_t h = 14695981039346656037ULL;
        auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ULL; };
        mix(t.src_ip);
        mix(t.dst_ip);
        mix(static_cast<uint64_t>(t.src_port) << 16 | t.dst_port);
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
    struct Config {
        uint16_t port_id      = 0;
        uint16_t rx_queue_id  = 0;
        int      rx_cpu       = -1;   ///< CPU affinity for RX thread (-1 = no pin)
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
        e.session = session;
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
    /// Safe to call while the reactor is running for the connected flag.
    ///
    /// Synchronization protocol (disable → fence → swap → enable):
    ///  1. Set connected=false with release semantics so the RX loop's
    ///     next acquire-load sees the disconnected state.
    ///  2. Issue a seq_cst fence to ensure the RX loop has observed the
    ///     disabled flag before we mutate session/tuple.  Without this
    ///     fence, the RX loop could have already loaded connected=true
    ///     (from the previous state) and proceed to dereference the
    ///     session pointer while we are swapping it.
    ///  3. Swap session pointer, tuple, and hash while the RX loop is
    ///     guaranteed to skip this entry.
    ///  4. Set connected=true with release semantics to re-enable the entry.
    void mark_reconnected(size_t conn_id, TcpSession<>* new_session) noexcept {
        if (!new_session) return;
        if (conn_id < count_.load(std::memory_order_acquire)) {
            // Step 1: Disable so the RX loop skips this entry.
            entries_[conn_id].connected.store(false, std::memory_order_release);

            // Step 2: Fence ensures any RX loop iteration that loaded
            // connected=true (old value) has fully completed before we
            // touch the session pointer.  The RX loop's acquire-load
            // pairs with this fence to establish happens-before.
            std::atomic_thread_fence(std::memory_order_seq_cst);

            // Step 3: Safe to swap — RX loop will skip this entry.
            entries_[conn_id].session = new_session;
            entries_[conn_id].tuple = new_session->connection_tuple();
            hashes_[conn_id] = ReactorEntry::hash_tuple(entries_[conn_id].tuple);

            // Step 4: Re-enable the entry for the RX loop.
            entries_[conn_id].connected.store(true, std::memory_order_release);
        }
    }

    [[nodiscard]] size_t connection_count() const noexcept {
        return count_.load(std::memory_order_acquire);
    }
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
            (void)eph::utils::set_thread_affinity(config_.rx_cpu, "reactor_rx");
        }

        auto* log = detail::reactor_logger();
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
                    if (hashes_[j] != pkt_hash) continue;  // Fast reject
                    if (!parsed.matches(entries_[j].tuple)) continue;  // Collision check

                    auto& entry = entries_[j];
                    if (!entry.connected.load(std::memory_order_acquire)) {
                        rte_pktmbuf_free(pkts[i]);
                        matched = true;
                        break;
                    }

                    // Propagate arrival TSC for latency measurement
                    entry.session->set_last_rx_burst_tsc(burst_tsc);

                    // Direct dispatch — zero ring overhead.
                    // process_rx handles TCP state (seq/ack/flags) and
                    // delivers payload via the inline callback.
                    // process_rx takes ownership of pkts[i] — it frees the mbuf
                    // internally via free_list or on error via free_remaining.
                    // No additional rte_pktmbuf_free needed on the matched path.
                    auto result = entry.session->process_rx(
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

                    entry.session->flush_pending_ack();
                    matched = true;
                    break;
                }

                if (!matched) {
                    rte_pktmbuf_free(pkts[i]);
                }
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
};

} // namespace eph::dpdk
