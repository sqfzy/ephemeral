#pragma once

/// @file hal.hpp
/// Hardware Abstraction Layer for DPDK calls.
///
/// IDpdkHal exists solely to decouple DpdkPlatform's business logic from the
/// real DPDK API so that unit tests can inject a mock without needing real
/// hardware, hugepages, or VFIO-bound NICs.

#include <cstdint>
#include <memory>

#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>

namespace eph_dpdk {

class IDpdkHal {
public:
    virtual ~IDpdkHal() = default;

    // ── EAL ──────────────────────────────────────────────────────────────
    /// Wraps rte_eal_init().  Must be called exactly once per process.
    virtual int eal_init(int argc, char** argv) = 0;
    virtual int eal_cleanup() = 0;

    // ── Port enumeration ─────────────────────────────────────────────────
    virtual uint16_t eth_dev_count_avail() = 0;

    // ── Mempool ──────────────────────────────────────────────────────────
    virtual rte_mempool* pktmbuf_pool_create(const char* name,
                                             uint32_t    n,
                                             uint32_t    cache_size,
                                             uint16_t    priv_size,
                                             uint16_t    data_room_size,
                                             int         socket_id) = 0;
    virtual void mempool_free(rte_mempool* mp) = 0;

    // ── Port info & configuration ─────────────────────────────────────────
    virtual int eth_dev_info_get(uint16_t port_id, rte_eth_dev_info& info) = 0;

    virtual int eth_dev_configure(uint16_t          port_id,
                                  uint16_t          nb_rx_q,
                                  uint16_t          nb_tx_q,
                                  const rte_eth_conf& conf) = 0;

    virtual int eth_rx_queue_setup(uint16_t          port_id,
                                   uint16_t          queue_id,
                                   uint16_t          nb_rx_desc,
                                   unsigned          socket_id,
                                   const rte_eth_rxconf* rxconf,
                                   rte_mempool*      mp) = 0;

    virtual int eth_tx_queue_setup(uint16_t          port_id,
                                   uint16_t          queue_id,
                                   uint16_t          nb_tx_desc,
                                   unsigned          socket_id,
                                   const rte_eth_txconf* txconf) = 0;

    // ── Port lifecycle ────────────────────────────────────────────────────
    virtual int eth_dev_start(uint16_t port_id) = 0;
    virtual int eth_dev_stop(uint16_t port_id)  = 0;
    virtual int eth_dev_close(uint16_t port_id) = 0;

    // ── Link & promiscuous ────────────────────────────────────────────────
    virtual int eth_link_get_nowait(uint16_t port_id, rte_eth_link& link) = 0;
    virtual int eth_promiscuous_enable(uint16_t port_id) = 0;

    // ── Statistics ────────────────────────────────────────────────────────
    virtual int eth_stats_get(uint16_t port_id, rte_eth_stats& stats) = 0;
    virtual int eth_stats_reset(uint16_t port_id) = 0;

    // ── mbuf operations (Layer 2) ─────────────────────────────────────────
    virtual rte_mbuf* mbuf_alloc(rte_mempool* pool) = 0;
    virtual void      mbuf_free(rte_mbuf* m) = 0;
    /// Copy @p len bytes from @p data into @p m and set data/pkt length.
    /// Returns false if @p len exceeds the mbuf's available data room.
    virtual bool      mbuf_fill(rte_mbuf* m, const void* data, uint16_t len) = 0;
    virtual uint32_t  mempool_avail_count(rte_mempool* pool) = 0;
    virtual uint32_t  mempool_in_use_count(rte_mempool* pool) = 0;

    // ── TX burst (Layer 2) ───────────────────────────────────────────────
    /// Wraps rte_eth_tx_burst().  Returns number of packets actually sent.
    /// Caller must free any packets NOT sent (indices [nb_sent, nb_pkts)).
    virtual uint16_t eth_tx_burst(uint16_t port_id, uint16_t queue_id,
                                   rte_mbuf** tx_pkts, uint16_t nb_pkts) = 0;

    // ── SPSC ring (Layer 2) ───────────────────────────────────────────────
    /// Create a SPSC ring (RING_F_SP_ENQ | RING_F_SC_DEQ).
    /// @p count must be a power of 2.
    virtual rte_ring* ring_create(const char* name, uint32_t count, int socket_id) = 0;
    virtual void      ring_free(rte_ring* r) = 0;
    /// Returns 0 on success, -ENOBUFS if ring is full.
    virtual int       ring_sp_enqueue(rte_ring* r, void* obj) = 0;
    /// Returns number of items dequeued (≤ n).
    virtual uint32_t  ring_sc_dequeue_burst(rte_ring* r, void** objs, uint32_t n) = 0;
    virtual uint32_t  ring_count(rte_ring* r) = 0;
    virtual uint32_t  ring_free_count(rte_ring* r) = 0;

    // ── Time (injectable for deterministic tests) ─────────────────────────
    /// Sleep for @p ms milliseconds.  Mock can make this a no-op.
    virtual void sleep_ms(int ms) = 0;
    /// Current monotonic time in milliseconds.
    virtual int64_t now_ms() = 0;
};

/// Returns the production DPDK HAL that delegates to real DPDK functions.
/// Defined in hal_real.cpp.
std::shared_ptr<IDpdkHal> make_real_hal();

} // namespace eph_dpdk
