#pragma once

/// @file hal_real.hpp
/// Production IDpdkHal implementation — delegates directly to rte_* functions.
/// Include this in translation units that build against real DPDK hardware.

#include "hal.hpp"

#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>

namespace eph_dpdk {

/// Thin wrappers around DPDK C API.  Every method is trivially inline.
class RealDpdkHal final : public IDpdkHal {
public:
    // ── EAL ──────────────────────────────────────────────────────────────
    int eal_init(int argc, char** argv) override { return rte_eal_init(argc, argv); }
    int eal_cleanup()                   override { return rte_eal_cleanup(); }

    // ── Port enumeration ─────────────────────────────────────────────────
    uint16_t eth_dev_count_avail() override { return rte_eth_dev_count_avail(); }

    // ── Mempool ──────────────────────────────────────────────────────────
    rte_mempool* pktmbuf_pool_create(const char* name, uint32_t n,
                                     uint32_t cache_size, uint16_t priv_size,
                                     uint16_t data_room_size, int socket_id) override {
        return rte_pktmbuf_pool_create(name, n, cache_size,
                                       priv_size, data_room_size, socket_id);
    }
    void mempool_free(rte_mempool* mp) override { rte_mempool_free(mp); }

    // ── Port info & configuration ─────────────────────────────────────────
    int eth_dev_info_get(uint16_t port_id, rte_eth_dev_info& info) override {
        return rte_eth_dev_info_get(port_id, &info);
    }
    int eth_dev_configure(uint16_t port_id, uint16_t nb_rx_q, uint16_t nb_tx_q,
                          const rte_eth_conf& conf) override {
        return rte_eth_dev_configure(port_id, nb_rx_q, nb_tx_q, &conf);
    }
    int eth_rx_queue_setup(uint16_t port_id, uint16_t queue_id, uint16_t nb_rx_desc,
                           unsigned socket_id, const rte_eth_rxconf* rxconf,
                           rte_mempool* mp) override {
        return rte_eth_rx_queue_setup(port_id, queue_id, nb_rx_desc, socket_id, rxconf, mp);
    }
    int eth_tx_queue_setup(uint16_t port_id, uint16_t queue_id, uint16_t nb_tx_desc,
                           unsigned socket_id, const rte_eth_txconf* txconf) override {
        return rte_eth_tx_queue_setup(port_id, queue_id, nb_tx_desc, socket_id, txconf);
    }

    // ── Port lifecycle ────────────────────────────────────────────────────
    int eth_dev_start(uint16_t port_id) override { return rte_eth_dev_start(port_id); }
    int eth_dev_stop(uint16_t port_id)  override { return rte_eth_dev_stop(port_id); }
    int eth_dev_close(uint16_t port_id) override { return rte_eth_dev_close(port_id); }

    // ── Link & promiscuous ────────────────────────────────────────────────
    int eth_link_get_nowait(uint16_t port_id, rte_eth_link& link) override {
        return rte_eth_link_get_nowait(port_id, &link);
    }
    int eth_promiscuous_enable(uint16_t port_id) override {
        return rte_eth_promiscuous_enable(port_id);
    }

    // ── Statistics ────────────────────────────────────────────────────────
    int eth_stats_get(uint16_t port_id, rte_eth_stats& stats) override {
        return rte_eth_stats_get(port_id, &stats);
    }
    int eth_stats_reset(uint16_t port_id) override {
        return rte_eth_stats_reset(port_id);
    }

    // ── mbuf operations ───────────────────────────────────────────────────
    rte_mbuf* mbuf_alloc(rte_mempool* pool) override {
        return rte_pktmbuf_alloc(pool);
    }
    void mbuf_free(rte_mbuf* m) override { rte_pktmbuf_free(m); }
    bool mbuf_fill(rte_mbuf* m, const void* data, uint16_t len) override {
        uint16_t room = rte_pktmbuf_data_room_size(m->pool) - RTE_PKTMBUF_HEADROOM;
        if (len > room) return false;
        std::memcpy(rte_pktmbuf_mtod(m, char*), data, len);
        m->data_len = len;
        m->pkt_len  = len;
        return true;
    }
    uint32_t mempool_avail_count(rte_mempool* pool) override {
        return rte_mempool_avail_count(pool);
    }
    uint32_t mempool_in_use_count(rte_mempool* pool) override {
        return rte_mempool_in_use_count(pool);
    }

    // ── TX burst ──────────────────────────────────────────────────────────
    uint16_t eth_tx_burst(uint16_t port_id, uint16_t queue_id,
                          rte_mbuf** tx_pkts, uint16_t nb_pkts) override {
        return rte_eth_tx_burst(port_id, queue_id, tx_pkts, nb_pkts);
    }

    // ── SPSC ring ─────────────────────────────────────────────────────────
    rte_ring* ring_create(const char* name, uint32_t count, int socket_id) override {
        return rte_ring_create(name, count, socket_id, RING_F_SP_ENQ | RING_F_SC_DEQ);
    }
    void ring_free(rte_ring* r) override { rte_ring_free(r); }
    int ring_sp_enqueue(rte_ring* r, void* obj) override {
        return rte_ring_sp_enqueue(r, obj);
    }
    uint32_t ring_sc_dequeue_burst(rte_ring* r, void** objs, uint32_t n) override {
        return rte_ring_sc_dequeue_burst(r, objs, n, nullptr);
    }
    uint32_t ring_count(rte_ring* r) override { return rte_ring_count(r); }
    uint32_t ring_free_count(rte_ring* r) override { return rte_ring_free_count(r); }

    // ── Time ──────────────────────────────────────────────────────────────
    void sleep_ms(int ms) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
    int64_t now_ms() override {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }
};

inline std::shared_ptr<IDpdkHal> make_real_hal() {
    return std::make_shared<RealDpdkHal>();
}

} // namespace eph_dpdk
