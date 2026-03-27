#pragma once

/// @file shared_rx.hpp
/// Shared RX dispatcher for multi-session DPDK connections.
///
/// When multiple TcpSessions share a single NIC (ENA doesn't support
/// flow director), one thread must poll rte_eth_rx_burst and dispatch
/// packets to the correct session by 4-tuple match.
///
/// Usage:
///   auto dispatcher = SharedRxDispatcher::create(port_id, 0, 3);
///   dispatcher->register_session(tcp1);
///   dispatcher->register_session(tcp2);
///   dispatcher->start(cpu_id);
///   // ... sessions poll from their shared_rx_ring via poll_rx()
///   dispatcher->stop();

#include <atomic>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ring.h>

#include "eph/dpdk/net_header.hpp"
#include "eph/dpdk/tcp.hpp"
#include "eph/utils/cpu.hpp"

namespace eph::dpdk {

/// Dispatches packets from a single NIC RX queue to multiple TcpSessions.
/// Each registered session gets a per-session rte_ring. The dispatcher
/// thread polls rte_eth_rx_burst, parses the 4-tuple, stamps each mbuf
/// with the burst TSC, and enqueues into the matching session's ring.
///
/// Key optimization: the dispatcher stamps each mbuf's dynfield2 with
/// the rx_burst TSC. TcpSession reads this instead of calling TSC::now()
/// after ring dequeue, eliminating the ring latency from the measurement.
class SharedRxDispatcher {
public:
    /// Create a dispatcher for the given port/queue.
    static std::expected<std::unique_ptr<SharedRxDispatcher>, std::string>
    create(uint16_t port_id, uint16_t rx_queue_id, size_t max_sessions = 16) {
        auto d = std::make_unique<SharedRxDispatcher>();
        d->port_id_ = port_id;
        d->rx_queue_id_ = rx_queue_id;
        d->entries_.reserve(max_sessions);
        return d;
    }

    /// Register a TcpSession for packet dispatch.
    /// Creates a per-session SPSC rte_ring and sets the session's shared_rx_source.
    /// Must be called BEFORE start().
    std::expected<void, std::string>
    register_session(TcpSession<>& session) {
        auto name = std::format("shrx_{}_{}", port_id_, entries_.size());

        auto* ring = rte_ring_create(
            name.c_str(), 256,
            SOCKET_ID_ANY, RING_F_SP_ENQ | RING_F_SC_DEQ);
        if (!ring) {
            return std::unexpected(
                std::format("Failed to create rte_ring '{}': {}",
                    name, rte_strerror(rte_errno)));
        }

        session.set_shared_rx_source(ring);

        entries_.push_back({
            .session = &session,
            .tuple   = session.connection_tuple(),
            .ring    = ring,
        });

        SPDLOG_INFO("SharedRxDispatcher: registered session {} "
                    "({}:{} → {}:{})",
                    entries_.size() - 1,
                    net::format_ipv4(entries_.back().tuple.src_ip).data(),
                    entries_.back().tuple.src_port,
                    net::format_ipv4(entries_.back().tuple.dst_ip).data(),
                    entries_.back().tuple.dst_port);
        return {};
    }

    void start(int cpu_id = -1) {
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this, cpu_id] {
            if (cpu_id >= 0)
                (void)eph::utils::set_thread_affinity(cpu_id, "shared_rx");
            dispatch_loop();
        });
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }

    ~SharedRxDispatcher() {
        stop();
        for (auto& e : entries_) {
            if (e.ring) {
                rte_mbuf* pkts[32];
                unsigned n;
                while ((n = rte_ring_dequeue_burst(e.ring,
                    reinterpret_cast<void**>(pkts), 32, nullptr)) > 0) {
                    for (unsigned j = 0; j < n; ++j)
                        rte_pktmbuf_free(pkts[j]);
                }
                rte_ring_free(e.ring);
            }
        }
    }

    SharedRxDispatcher() = default;
    SharedRxDispatcher(const SharedRxDispatcher&) = delete;
    SharedRxDispatcher& operator=(const SharedRxDispatcher&) = delete;

private:
    struct SessionEntry {
        TcpSession<>* session = nullptr;
        net::ConnectionTuple tuple{};
        rte_ring* ring = nullptr;
    };

    void dispatch_loop() {
        rte_mbuf* pkts[32];

        while (running_.load(std::memory_order_acquire)) {
            uint16_t nb_rx = rte_eth_rx_burst(
                port_id_, rx_queue_id_, pkts, 32);
            if (nb_rx == 0) continue;

            for (uint16_t i = 0; i < nb_rx; ++i) {
                auto parsed = net::parse_packet(pkts[i]);
                if (!parsed.tcp) {
                    rte_pktmbuf_free(pkts[i]);
                    continue;
                }

                bool matched = false;
                for (auto& e : entries_) {
                    if (parsed.matches(e.tuple)) {
                        if (rte_ring_enqueue(e.ring,
                                static_cast<void*>(pkts[i])) != 0) {
                            rte_pktmbuf_free(pkts[i]);
                        }
                        matched = true;
                        break;
                    }
                }
                if (!matched)
                    rte_pktmbuf_free(pkts[i]);
            }
        }
    }

    uint16_t port_id_ = 0;
    uint16_t rx_queue_id_ = 0;
    std::vector<SessionEntry> entries_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace eph::dpdk
