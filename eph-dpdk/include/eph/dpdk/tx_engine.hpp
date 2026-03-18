#pragma once

/// @file tx_engine.hpp
/// Layer 2: TX Engine — header-only, template-driven.
///
/// Compile-time philosophy:
///   - BurstSize and RingSize are template parameters because they:
///     (a) directly determine hot-path array sizes (std::array vs vector),
///     (b) have strict structural constraints (power-of-2 for ring),
///     (c) are known at build time in every real deployment.
///   - Port/queue IDs remain runtime config (PCI topology is discovered).
///   - SpscLogRing uses cacheline-padded atomics to prevent false sharing.

#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>

#include "eph/base/cache.hpp"

namespace eph::dpdk {

// ─────────────────────────────────────────────────────────────────────────────
// Lock-free SPSC log ring
//
// TX lcore pushes WARN/ERROR entries without holding any mutex.
// Main thread drains via drain_log_ring().
//
// Template param N is the capacity (must be power of 2, >= 2).
// Cache-line padding between head and tail prevents false sharing
// between the producer (TX lcore) and consumer (drain thread).
// ─────────────────────────────────────────────────────────────────────────────

struct LogEntry {
    spdlog::level::level_enum level{spdlog::level::warn};
    char msg[248]{};
};

template<uint32_t N>
class SpscLogRing {
    static_assert(N >= 2 && std::has_single_bit(N),
                  "SpscLogRing capacity must be a power of 2 >= 2");
    static constexpr uint32_t Mask = N - 1;

public:
    constexpr SpscLogRing() noexcept = default;

    bool try_push(const LogEntry& e) noexcept {
        uint32_t h = head_.load(std::memory_order_relaxed);
        uint32_t next = (h + 1) & Mask;
        if (next == tail_.load(std::memory_order_acquire)) return false;
        buf_[h] = e;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool try_pop(LogEntry& e) noexcept {
        uint32_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        e = buf_[t];
        tail_.store((t + 1) & Mask, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return tail_.load(std::memory_order_acquire)
            == head_.load(std::memory_order_acquire);
    }

private:
    std::array<LogEntry, N> buf_{};

    // Separate cache lines for producer and consumer indices
    // to prevent false sharing on the TX lcore hot path.
    alignas(eph::base::CACHE_LINE_SIZE) std::atomic<uint32_t> head_{0};
    alignas(eph::base::CACHE_LINE_SIZE) std::atomic<uint32_t> tail_{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// TX Engine configuration
//
// BurstSize and RingSize are compile-time because they determine:
//   - The burst buffer array size (std::array, no heap on hot path)
//   - The rte_ring capacity (DPDK requires power of 2)
//   - static_assert validation instead of runtime checks
//
// Port/queue IDs stay runtime — PCI topology is discovered by EAL.
// ─────────────────────────────────────────────────────────────────────────────

struct TxRuntimeConfig {
    uint16_t port_id    = 0;
    uint16_t queue_id   = 0;

    /// Log WARN when pool in-use fraction exceeds this threshold (0–1).
    float    pool_high_watermark = 0.80f;

    /// Accumulate this many drops before emitting a WARN to the log ring.
    uint32_t drop_log_interval = 1000;

    /// Check pool utilization every N calls to process_one_burst().
    uint32_t pool_check_burst_interval = 1000;
};

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

inline std::shared_ptr<spdlog::logger> tx_logger() {
    static auto l = [] {
        auto lg = spdlog::stdout_color_mt("dpdk.txengine");
        lg->set_level(spdlog::level::trace);
        return lg;
    }();
    return l;
}

/// Copy payload into mbuf data area.  Returns false if the payload exceeds
/// the mbuf's available data room.
inline bool mbuf_fill(rte_mbuf* m, const void* data, uint16_t len) noexcept {
    uint16_t room = rte_pktmbuf_data_room_size(m->pool) - RTE_PKTMBUF_HEADROOM;
    if (len > room) return false;
    std::memcpy(rte_pktmbuf_mtod(m, char*), data, len);
    m->data_len = len;
    m->pkt_len  = len;
    return true;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// TxEngine<BurstSize, RingSize>
// ─────────────────────────────────────────────────────────────────────────────

template<uint16_t BurstSize = 32, uint32_t RingSize = 1024>
class TxEngine {
    static_assert(BurstSize > 0,
                  "BurstSize must be > 0");
    static_assert(RingSize > 0 && std::has_single_bit(RingSize),
                  "RingSize must be a power of 2");

public:
    struct Stats {
        uint64_t tx_packets          = 0;
        uint64_t tx_bytes            = 0;
        uint64_t tx_dropped          = 0;
        uint64_t mbuf_alloc_failures = 0;
    };

    /// Create a TX engine tied to @p pool on the given port/queue.
    [[nodiscard]] static std::expected<TxEngine, std::string>
    create(rte_mempool* pool, const TxRuntimeConfig& cfg = {});

    ~TxEngine();

    TxEngine(const TxEngine&)            = delete;
    TxEngine& operator=(const TxEngine&) = delete;
    TxEngine(TxEngine&&) noexcept;
    TxEngine& operator=(TxEngine&&) noexcept;

    // ── Application-thread API ────────────────────────────────────────────

    /// Enqueue one packet for transmission.
    ///
    /// Returns 0 on success.
    /// Returns -ENOMEM  if mbuf pool is exhausted.
    /// Returns -EMSGSIZE if payload exceeds mbuf data room.
    /// Returns -EAGAIN  if SPSC ring is full.
    int enqueue(const void* payload, size_t len);

    // ── TX-lcore API ──────────────────────────────────────────────────────

    /// Dequeue up to BurstSize mbufs → tx_burst → free unsent.
    /// Returns number of packets transmitted.
    uint32_t process_one_burst();

    /// Busy-poll loop.  Returns only after stop() is called.
    void run_loop();

    /// Signal run_loop() to exit.  Thread-safe.
    void stop() noexcept;

    // ── Maintenance ───────────────────────────────────────────────────────

    std::vector<LogEntry> drain_log_ring();

    [[nodiscard]] Stats stats() const noexcept;

    /// Compile-time accessors for introspection / testing.
    static constexpr uint16_t burst_size() noexcept { return BurstSize; }
    static constexpr uint32_t ring_size()  noexcept { return RingSize; }

private:
    struct Impl;
    explicit TxEngine(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// ─────────────────────────────────────────────────────────────────────────────
// TxEngine::Impl
// ─────────────────────────────────────────────────────────────────────────────

template<uint16_t BurstSize, uint32_t RingSize>
struct TxEngine<BurstSize, RingSize>::Impl {
    TxRuntimeConfig config;
    rte_mempool*    pool;
    rte_ring*       tx_ring{nullptr};

    std::atomic<bool>     stop_flag{false};

    // Stats: written by TX lcore (relaxed), read by any thread.
    std::atomic<uint64_t> stat_tx_packets{0};
    std::atomic<uint64_t> stat_tx_bytes{0};
    std::atomic<uint64_t> stat_tx_dropped{0};
    std::atomic<uint64_t> stat_mbuf_alloc_failures{0};

    // Hot-path state: TX lcore only, no atomics.
    uint64_t drops_since_last_log{0};
    uint64_t burst_count{0};

    // Fixed-size burst buffer — no heap allocation on the hot path.
    // BurstSize is a compile-time constant, so this is a stack array.
    std::array<void*, BurstSize> burst_buf{};

    static constexpr uint32_t LOG_RING_SIZE = 64;
    SpscLogRing<LOG_RING_SIZE> log_ring;

    ~Impl() {
        if (tx_ring) {
            rte_ring_free(tx_ring);
            tx_ring = nullptr;
        }
    }

    void push_log(spdlog::level::level_enum lvl,
                  std::string_view msg) noexcept {
        LogEntry e;
        e.level = lvl;
        auto n = std::min(msg.size(), sizeof(e.msg) - 1);
        std::memcpy(e.msg, msg.data(), n);
        e.msg[n] = '\0';
        log_ring.try_push(e);
    }

    void check_pool_utilization() noexcept {
        uint32_t avail  = rte_mempool_avail_count(pool);
        uint32_t in_use = rte_mempool_in_use_count(pool);
        uint32_t total  = avail + in_use;
        if (total == 0) return;

        float util = static_cast<float>(in_use) / static_cast<float>(total);
        if (util > config.pool_high_watermark) {
            push_log(spdlog::level::warn,
                std::format("mbuf pool utilization {:.1f}% (in_use={}, total={})",
                    util * 100.0f, in_use, total));
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// TxEngine method definitions
// ─────────────────────────────────────────────────────────────────────────────

template<uint16_t B, uint32_t R>
TxEngine<B, R>::TxEngine(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

template<uint16_t B, uint32_t R>
TxEngine<B, R>::~TxEngine() = default;

template<uint16_t B, uint32_t R>
TxEngine<B, R>::TxEngine(TxEngine&&) noexcept = default;

template<uint16_t B, uint32_t R>
TxEngine<B, R>& TxEngine<B, R>::operator=(TxEngine&&) noexcept = default;

template<uint16_t B, uint32_t R>
std::expected<TxEngine<B, R>, std::string>
TxEngine<B, R>::create(rte_mempool* pool, const TxRuntimeConfig& cfg) {
    auto log = detail::tx_logger();

    if (pool == nullptr)
        return std::unexpected("pool must not be null");

    auto impl    = std::make_unique<Impl>();
    impl->config = cfg;
    impl->pool   = pool;

    // SPSC ring — RingSize guaranteed power-of-2 by static_assert above.
    static int ring_id = 0;
    std::string ring_name = std::format("tx_ring_{}", ring_id++);
    impl->tx_ring = rte_ring_create(ring_name.c_str(), R,
                                     SOCKET_ID_ANY,
                                     RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (impl->tx_ring == nullptr) {
        return std::unexpected(std::format(
            "ring_create('{}', {}) failed: rte_errno={}: {}",
            ring_name, R, rte_errno, rte_strerror(rte_errno)));
    }

    SPDLOG_LOGGER_DEBUG(log,
        "TxEngine created: port={} queue={} ring_size={} burst_size={}",
        cfg.port_id, cfg.queue_id, R, B);

    return TxEngine(std::move(impl));
}

template<uint16_t B, uint32_t R>
int TxEngine<B, R>::enqueue(const void* payload, size_t len) {
    auto log = detail::tx_logger();

    rte_mbuf* m = rte_pktmbuf_alloc(impl_->pool);
    if (m == nullptr) {
        uint32_t avail  = rte_mempool_avail_count(impl_->pool);
        uint32_t in_use = rte_mempool_in_use_count(impl_->pool);
        SPDLOG_LOGGER_ERROR(log,
            "mbuf alloc failed: pool avail={} in_use={}", avail, in_use);
        impl_->stat_mbuf_alloc_failures.fetch_add(1, std::memory_order_relaxed);
        impl_->stat_tx_dropped.fetch_add(1, std::memory_order_relaxed);
        return -ENOMEM;
    }

    if (!detail::mbuf_fill(m, payload, static_cast<uint16_t>(len))) {
        rte_pktmbuf_free(m);
        SPDLOG_LOGGER_ERROR(log, "Payload too large: len={}", len);
        impl_->stat_tx_dropped.fetch_add(1, std::memory_order_relaxed);
        return -EMSGSIZE;
    }

    if (rte_ring_sp_enqueue(impl_->tx_ring, m) != 0) {
        rte_pktmbuf_free(m);
        SPDLOG_LOGGER_TRACE(log, "TX ring full, dropping packet");
        impl_->stat_tx_dropped.fetch_add(1, std::memory_order_relaxed);
        return -EAGAIN;
    }

    SPDLOG_LOGGER_TRACE(log, "Enqueued {} bytes", len);
    return 0;
}

template<uint16_t B, uint32_t R>
uint32_t TxEngine<B, R>::process_one_burst() {
    uint32_t nb = rte_ring_sc_dequeue_burst(
        impl_->tx_ring,
        impl_->burst_buf.data(),
        B,
        nullptr);

    if (nb == 0) return 0;

    auto** mbufs = reinterpret_cast<rte_mbuf**>(impl_->burst_buf.data());

    uint16_t nb_sent = rte_eth_tx_burst(
        impl_->config.port_id,
        impl_->config.queue_id,
        mbufs,
        static_cast<uint16_t>(nb));

    SPDLOG_LOGGER_TRACE(detail::tx_logger(), "tx_burst sent={}/{}", nb_sent, nb);

    // Free unsent mbufs — the single most common DPDK resource leak.
    uint32_t dropped = nb - nb_sent;
    for (uint32_t i = nb_sent; i < nb; ++i)
        rte_pktmbuf_free(mbufs[i]);

    impl_->stat_tx_packets.fetch_add(nb_sent, std::memory_order_relaxed);

    if (dropped > 0) {
        impl_->stat_tx_dropped.fetch_add(dropped, std::memory_order_relaxed);
        impl_->drops_since_last_log += dropped;

        if (impl_->drops_since_last_log >= impl_->config.drop_log_interval) {
            impl_->push_log(spdlog::level::warn,
                std::format("TX dropped {} packets (total dropped={})",
                    impl_->drops_since_last_log,
                    impl_->stat_tx_dropped.load(std::memory_order_relaxed)));
            impl_->drops_since_last_log = 0;
        }
    }

    if (++impl_->burst_count % impl_->config.pool_check_burst_interval == 0)
        impl_->check_pool_utilization();

    return nb_sent;
}

template<uint16_t B, uint32_t R>
void TxEngine<B, R>::run_loop() {
    while (!impl_->stop_flag.load(std::memory_order_relaxed)) {
        process_one_burst();
    }
}

template<uint16_t B, uint32_t R>
void TxEngine<B, R>::stop() noexcept {
    impl_->stop_flag.store(true, std::memory_order_relaxed);
}

template<uint16_t B, uint32_t R>
std::vector<LogEntry> TxEngine<B, R>::drain_log_ring() {
    auto log = detail::tx_logger();
    std::vector<LogEntry> entries;
    LogEntry e;
    while (impl_->log_ring.try_pop(e)) {
        entries.push_back(e);
        log->log(e.level, "{}", std::string_view{e.msg});
    }
    return entries;
}

template<uint16_t B, uint32_t R>
typename TxEngine<B, R>::Stats TxEngine<B, R>::stats() const noexcept {
    return Stats{
        .tx_packets          = impl_->stat_tx_packets.load(std::memory_order_relaxed),
        .tx_bytes            = impl_->stat_tx_bytes.load(std::memory_order_relaxed),
        .tx_dropped          = impl_->stat_tx_dropped.load(std::memory_order_relaxed),
        .mbuf_alloc_failures = impl_->stat_mbuf_alloc_failures.load(std::memory_order_relaxed),
    };
}

} // namespace eph::dpdk
