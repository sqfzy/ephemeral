#pragma once

/// @file mock_hal.hpp
/// Fake IDpdkHal for unit tests — no real DPDK hardware or hugepages needed.

#include "eph_dpdk/hal.hpp"

#include <cstring>
#include <deque>
#include <unordered_map>

namespace eph_dpdk::test {

/// A minimal fake DPDK HAL.
///
/// Default configuration simulates a healthy 10G port with one queue.
/// Individual fields can be overridden per-test.
class MockDpdkHal final : public IDpdkHal {
public:
    // ── Layer 1: configurable return values ───────────────────────────────
    int      eal_init_ret           = 0;
    uint16_t dev_count              = 1;
    rte_mempool* pool_ptr           = reinterpret_cast<rte_mempool*>(0xBEEF'0000uLL);
    int      eth_dev_info_ret       = 0;
    int      eth_dev_configure_ret  = 0;
    int      eth_rx_queue_setup_ret = 0;
    int      eth_tx_queue_setup_ret = 0;
    int      eth_dev_start_ret      = 0;
    int      eth_dev_stop_ret       = 0;
    bool     link_up                = true;
    int      eth_stats_get_ret      = 0;
    rte_eth_dev_info dev_info{};

    // ── Layer 1: call counters ────────────────────────────────────────────
    int eal_init_calls          = 0;
    int eal_cleanup_calls       = 0;
    int eth_dev_configure_calls = 0;
    int eth_dev_start_calls     = 0;
    int eth_dev_stop_calls      = 0;
    int eth_dev_close_calls     = 0;
    int mempool_free_calls      = 0;

    // ── Layer 2: mbuf controls ────────────────────────────────────────────
    /// How many successful allocs before returning nullptr (-1 = never fail).
    int      mbuf_alloc_fail_after = -1;
    /// Whether mbuf_fill() succeeds (set false to simulate oversized payload).
    bool     mbuf_fill_ret         = true;
    /// Pool utilization knobs for pool-warning tests.
    uint32_t pool_avail            = 1000;
    uint32_t pool_in_use           = 0;

    // ── Layer 2: tx_burst controls ────────────────────────────────────────
    /// When set, tx_burst sends only this many packets (simulates partial send).
    /// -1 means "send all".
    int tx_burst_send_count = -1;

    // ── Layer 2: call counters ────────────────────────────────────────────
    int      mbuf_alloc_calls        = 0;
    int      mbuf_alloc_fail_calls   = 0;
    int      mbuf_free_calls_layer2  = 0;
    int      eth_tx_burst_calls      = 0;
    uint32_t eth_tx_burst_total_sent = 0;
    int      ring_create_calls       = 0;
    int      ring_free_calls         = 0;

    // ── Time ──────────────────────────────────────────────────────────────
    int64_t current_time_ms  = 0;
    int     sleep_advance_ms = 0;

    MockDpdkHal() {
        dev_info.max_rx_queues   = 16;
        dev_info.max_tx_queues   = 16;
        dev_info.rx_desc_lim     = {.nb_max = 4096, .nb_min = 32, .nb_align = 32};
        dev_info.tx_desc_lim     = {.nb_max = 4096, .nb_min = 32, .nb_align = 32};
        dev_info.rx_offload_capa = ~uint64_t{0};
        dev_info.tx_offload_capa = ~uint64_t{0};
    }

    // ═══ IDpdkHal ══════════════════════════════════════════════════════════

    // ── EAL ──────────────────────────────────────────────────────────────
    int eal_init(int, char**) override { ++eal_init_calls; return eal_init_ret; }
    int eal_cleanup() override { ++eal_cleanup_calls; return 0; }

    // ── Port enumeration ─────────────────────────────────────────────────
    uint16_t eth_dev_count_avail() override { return dev_count; }

    // ── Mempool (Layer 1) ─────────────────────────────────────────────────
    rte_mempool* pktmbuf_pool_create(const char*, uint32_t, uint32_t,
                                     uint16_t, uint16_t, int) override {
        return pool_ptr;
    }
    void mempool_free(rte_mempool*) override { ++mempool_free_calls; }

    // ── Port configuration ────────────────────────────────────────────────
    int eth_dev_info_get(uint16_t, rte_eth_dev_info& info) override {
        info = dev_info; return eth_dev_info_ret;
    }
    int eth_dev_configure(uint16_t, uint16_t, uint16_t, const rte_eth_conf&) override {
        ++eth_dev_configure_calls; return eth_dev_configure_ret;
    }
    int eth_rx_queue_setup(uint16_t, uint16_t, uint16_t, unsigned,
                           const rte_eth_rxconf*, rte_mempool*) override {
        return eth_rx_queue_setup_ret;
    }
    int eth_tx_queue_setup(uint16_t, uint16_t, uint16_t, unsigned,
                           const rte_eth_txconf*) override {
        return eth_tx_queue_setup_ret;
    }
    int eth_dev_start(uint16_t)  override { ++eth_dev_start_calls; return eth_dev_start_ret; }
    int eth_dev_stop(uint16_t)   override { ++eth_dev_stop_calls; return eth_dev_stop_ret; }
    int eth_dev_close(uint16_t)  override { ++eth_dev_close_calls; return 0; }
    int eth_link_get_nowait(uint16_t, rte_eth_link& link) override {
        std::memset(&link, 0, sizeof(link));
        link.link_status = link_up ? RTE_ETH_LINK_UP : RTE_ETH_LINK_DOWN;
        link.link_speed  = RTE_ETH_SPEED_NUM_10G;
        link.link_duplex = RTE_ETH_LINK_FULL_DUPLEX;
        return 0;
    }
    int eth_promiscuous_enable(uint16_t) override { return 0; }
    int eth_stats_get(uint16_t, rte_eth_stats& stats) override {
        std::memset(&stats, 0, sizeof(stats)); return eth_stats_get_ret;
    }
    int eth_stats_reset(uint16_t) override { return 0; }

    // ── mbuf operations (Layer 2) ─────────────────────────────────────────
    rte_mbuf* mbuf_alloc(rte_mempool*) override {
        if (mbuf_alloc_fail_after >= 0 && mbuf_alloc_calls >= mbuf_alloc_fail_after) {
            ++mbuf_alloc_fail_calls;
            return nullptr;
        }
        // Return a unique non-null fake pointer per allocation
        return reinterpret_cast<rte_mbuf*>(uintptr_t{0xAA00'0000} + ++mbuf_alloc_calls);
    }
    void mbuf_free(rte_mbuf*) override { ++mbuf_free_calls_layer2; }
    bool mbuf_fill(rte_mbuf*, const void*, uint16_t) override { return mbuf_fill_ret; }

    uint32_t mempool_avail_count(rte_mempool*)    override { return pool_avail; }
    uint32_t mempool_in_use_count(rte_mempool*)   override { return pool_in_use; }

    // ── TX burst (Layer 2) ───────────────────────────────────────────────
    uint16_t eth_tx_burst(uint16_t, uint16_t, rte_mbuf**, uint16_t nb_pkts) override {
        ++eth_tx_burst_calls;
        uint16_t sent = (tx_burst_send_count < 0)
                        ? nb_pkts
                        : static_cast<uint16_t>(
                              std::min<int>(tx_burst_send_count, nb_pkts));
        eth_tx_burst_total_sent += sent;
        return sent;
    }

    // ── SPSC ring (Layer 2) ───────────────────────────────────────────────
    rte_ring* ring_create(const char*, uint32_t cap, int) override {
        ++ring_create_calls;
        auto id = next_ring_id_++;
        rings_[id] = {.items = {}, .capacity = cap};
        return reinterpret_cast<rte_ring*>(id);
    }
    void ring_free(rte_ring* r) override {
        ++ring_free_calls;
        rings_.erase(ring_id(r));
    }
    int ring_sp_enqueue(rte_ring* r, void* obj) override {
        auto& ring = get_ring(r);
        if (ring.items.size() >= ring.capacity) return -ENOBUFS;
        ring.items.push_back(obj);
        return 0;
    }
    uint32_t ring_sc_dequeue_burst(rte_ring* r, void** objs, uint32_t n) override {
        auto& ring = get_ring(r);
        auto count = static_cast<uint32_t>(
            std::min<size_t>(n, ring.items.size()));
        for (uint32_t i = 0; i < count; ++i) {
            objs[i] = ring.items.front();
            ring.items.pop_front();
        }
        return count;
    }
    uint32_t ring_count(rte_ring* r) override {
        return static_cast<uint32_t>(get_ring(r).items.size());
    }
    uint32_t ring_free_count(rte_ring* r) override {
        auto& ring = get_ring(r);
        return static_cast<uint32_t>(ring.capacity - ring.items.size());
    }

    // ── Time ──────────────────────────────────────────────────────────────
    void    sleep_ms(int ms) override { current_time_ms += sleep_advance_ms * ms; }
    int64_t now_ms()         override { return current_time_ms; }

private:
    struct MockRing { std::deque<void*> items; uint32_t capacity; };

    uintptr_t next_ring_id_ = 0x1000'0000;

    std::unordered_map<uintptr_t, MockRing> rings_;

    static uintptr_t ring_id(rte_ring* r) { return reinterpret_cast<uintptr_t>(r); }
    MockRing& get_ring(rte_ring* r) { return rings_.at(ring_id(r)); }
};

} // namespace eph_dpdk::test
