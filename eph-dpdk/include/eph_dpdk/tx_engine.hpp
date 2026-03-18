#pragma once

/// @file tx_engine.hpp
/// Layer 2: TX Engine — header-only.
///
/// Responsibilities:
///   - SPSC ring between application thread (producer) and TX lcore (consumer)
///   - mbuf lifecycle: alloc on enqueue → fill → tx_burst → free unsent
///   - Drop accounting with rate-limited WARN logging
///   - mbuf pool utilization monitoring
///   - Lock-free log ring so TX lcore never calls spdlog directly

#include "hal.hpp"

#include <atomic>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <rte_errno.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>

namespace eph_dpdk {

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

struct TxEngineConfig {
    uint16_t port_id    = 0;
    uint16_t queue_id   = 0;

    /// SPSC ring capacity between app thread and TX lcore (must be power of 2).
    uint32_t ring_size  = 1024;

    /// Maximum mbufs dequeued and sent in one tx_burst call.
    uint16_t burst_size = 32;

    /// Log WARN when pool in-use fraction exceeds this threshold (0–1).
    float    pool_high_watermark = 0.80f;

    /// Accumulate this many drops before emitting a WARN to the log ring.
    /// Set to 1 in tests to trigger immediately.
    uint32_t drop_log_interval = 1000;

    /// Check pool utilization every N calls to process_one_burst().
    /// Set to 1 in tests to check every burst.
    uint32_t pool_check_burst_interval = 1000;
};

// ─────────────────────────────────────────────────────────────────────────────
// Lock-free SPSC log ring
//
// TX lcore pushes WARN/ERROR entries without holding any mutex.
// Main thread drains via drain_log_ring().
// ─────────────────────────────────────────────────────────────────────────────

struct LogEntry {
    spdlog::level::level_enum level{spdlog::level::warn};
    char msg[248]{};  // keep total struct size at 252 bytes
};

/// SPSC ring buffer.  N must be a power of 2 ≥ 2.
/// Producer: try_push (TX lcore).  Consumer: try_pop (log drain thread).
template<uint32_t N>
class SpscLogRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "N must be a power of 2");
public:
    bool try_push(const LogEntry& e) noexcept {
        uint32_t h = head_.load(std::memory_order_relaxed);
        uint32_t next = (h + 1) & (N - 1);
        if (next == tail_.load(std::memory_order_acquire)) return false;  // full
        buf_[h] = e;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool try_pop(LogEntry& e) noexcept {
        uint32_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;  // empty
        e = buf_[t];
        tail_.store((t + 1) & (N - 1), std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        return tail_.load(std::memory_order_acquire)
            == head_.load(std::memory_order_acquire);
    }

private:
    std::array<LogEntry, N>    buf_{};
    std::atomic<uint32_t>      head_{0};  // written by producer
    std::atomic<uint32_t>      tail_{0};  // written by consumer
};

// ─────────────────────────────────────────────────────────────────────────────
// TxEngine
// ─────────────────────────────────────────────────────────────────────────────

class TxEngine {
public:
    struct Stats {
        uint64_t tx_packets         = 0;
        uint64_t tx_bytes           = 0;  ///< bytes of payload (not wire bytes)
        uint64_t tx_dropped         = 0;  ///< packets dropped for any reason
        uint64_t mbuf_alloc_failures = 0;
    };

    /// Create a TX engine tied to @p pool on the given port/queue.
    /// @p hal must be the same instance used by DpdkPlatform (shares pool).
    static std::expected<TxEngine, std::string>
    create(rte_mempool* pool, const TxEngineConfig& cfg,
           std::shared_ptr<IDpdkHal> hal);

    ~TxEngine();

    TxEngine(const TxEngine&)            = delete;
    TxEngine& operator=(const TxEngine&) = delete;
    TxEngine(TxEngine&&) noexcept;
    TxEngine& operator=(TxEngine&&) noexcept;

    // ── Application-thread API ────────────────────────────────────────────

    /// Enqueue one packet for transmission.
    /// Allocates an mbuf, copies @p payload, pushes to SPSC ring.
    ///
    /// Returns 0 on success.
    /// Returns -ENOMEM  if mbuf pool is exhausted.
    /// Returns -EMSGSIZE if payload exceeds mbuf data room.
    /// Returns -EAGAIN  if SPSC ring is full (non-blocking).
    int enqueue(const void* payload, size_t len);

    // ── TX-lcore API ──────────────────────────────────────────────────────

    /// Process one burst: dequeue from ring → tx_burst → free unsent.
    /// Exposed for deterministic unit testing (avoids infinite loop).
    /// Returns number of packets transmitted.
    uint32_t process_one_burst();

    /// Busy-poll transmission loop.  Returns only after stop() is called.
    /// Intended to run on a dedicated lcore (no blocking syscalls inside).
    void run_loop();

    /// Signal run_loop() to exit.  Thread-safe.
    void stop() noexcept;

    // ── Maintenance API (call from non-hot-path thread) ───────────────────

    /// Drain the lock-free log ring and return entries for inspection.
    /// Also emits each entry to the "dpdk.txengine" spdlog logger.
    std::vector<LogEntry> drain_log_ring();

    /// Approximate stats (reads atomics with relaxed ordering).
    Stats stats() const noexcept;

private:
    struct Impl;
    explicit TxEngine(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
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

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// TxEngine::Impl
// ─────────────────────────────────────────────────────────────────────────────

struct TxEngine::Impl {
    TxEngineConfig            config;
    std::shared_ptr<IDpdkHal> hal;
    rte_mempool*              pool;
    rte_ring*                 tx_ring{nullptr};

    std::atomic<bool>     stop_flag{false};

    // Stats: written by TX lcore with relaxed ordering; read by any thread.
    std::atomic<uint64_t> stat_tx_packets{0};
    std::atomic<uint64_t> stat_tx_bytes{0};
    std::atomic<uint64_t> stat_tx_dropped{0};
    std::atomic<uint64_t> stat_mbuf_alloc_failures{0};

    // Hot-path state: accessed ONLY from the TX lcore (no atomics needed).
    uint64_t drops_since_last_log{0};
    uint64_t burst_count{0};

    // Pre-allocated burst buffer (avoids VLA / hot-path heap alloc).
    std::vector<void*> burst_buf;

    // Lock-free log ring (SPSC: TX lcore → log drain thread).
    static constexpr uint32_t LOG_RING_SIZE = 64;
    SpscLogRing<LOG_RING_SIZE> log_ring;

    ~Impl() {
        if (tx_ring) {
            hal->ring_free(tx_ring);
            tx_ring = nullptr;
        }
    }

    // ── hot-path helpers (called from TX lcore only) ──────────────────────

    void push_log(spdlog::level::level_enum lvl, std::string_view msg) noexcept {
        LogEntry e;
        e.level = lvl;
        auto n = std::min(msg.size(), sizeof(e.msg) - 1);
        std::memcpy(e.msg, msg.data(), n);
        e.msg[n] = '\0';
        // If log ring is full, drop the entry — never block the hot path.
        log_ring.try_push(e);
    }

    void check_pool_utilization() noexcept {
        uint32_t avail   = hal->mempool_avail_count(pool);
        uint32_t in_use  = hal->mempool_in_use_count(pool);
        uint32_t total   = avail + in_use;
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

inline TxEngine::TxEngine(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

inline TxEngine::~TxEngine() = default;

inline TxEngine::TxEngine(TxEngine&&) noexcept            = default;
inline TxEngine& TxEngine::operator=(TxEngine&&) noexcept = default;

inline std::expected<TxEngine, std::string>
TxEngine::create(rte_mempool* pool, const TxEngineConfig& cfg,
                 std::shared_ptr<IDpdkHal> hal) {
    auto log = detail::tx_logger();

    if (pool == nullptr)
        return std::unexpected("pool must not be null");

    // ring_size must be a power of 2 (rte_ring requirement)
    uint32_t rs = cfg.ring_size;
    if (rs == 0 || (rs & (rs - 1)) != 0)
        return std::unexpected(
            std::format("ring_size {} must be a power of 2", rs));

    if (cfg.burst_size == 0)
        return std::unexpected("burst_size must be > 0");

    auto impl       = std::make_unique<Impl>();
    impl->config    = cfg;
    impl->hal       = std::move(hal);
    impl->pool      = pool;
    impl->burst_buf.resize(cfg.burst_size);  // pre-alloc, no hot-path resize

    // Create the SPSC TX ring
    static int ring_id = 0;
    std::string ring_name = std::format("tx_ring_{}", ring_id++);
    impl->tx_ring = impl->hal->ring_create(ring_name.c_str(), cfg.ring_size,
                                            SOCKET_ID_ANY);
    if (impl->tx_ring == nullptr) {
        return std::unexpected(std::format(
            "ring_create('{}', {}) failed: rte_errno={}: {}",
            ring_name, cfg.ring_size, rte_errno, rte_strerror(rte_errno)));
    }

    SPDLOG_LOGGER_DEBUG(log,
        "TxEngine created: port={} queue={} ring_size={} burst_size={}",
        cfg.port_id, cfg.queue_id, cfg.ring_size, cfg.burst_size);

    return TxEngine(std::move(impl));
}

inline int TxEngine::enqueue(const void* payload, size_t len) {
    auto log = detail::tx_logger();

    // Allocate an mbuf from the shared pool.
    // NOTE: alloc is on the application thread — allowed to call heap.
    rte_mbuf* m = impl_->hal->mbuf_alloc(impl_->pool);
    if (m == nullptr) {
        uint32_t avail  = impl_->hal->mempool_avail_count(impl_->pool);
        uint32_t in_use = impl_->hal->mempool_in_use_count(impl_->pool);
        SPDLOG_LOGGER_ERROR(log,
            "mbuf alloc failed: pool avail={} in_use={}", avail, in_use);
        impl_->stat_mbuf_alloc_failures.fetch_add(1, std::memory_order_relaxed);
        impl_->stat_tx_dropped.fetch_add(1, std::memory_order_relaxed);
        return -ENOMEM;
    }

    // Copy payload into mbuf data area.
    if (!impl_->hal->mbuf_fill(m, payload, static_cast<uint16_t>(len))) {
        impl_->hal->mbuf_free(m);
        SPDLOG_LOGGER_ERROR(log,
            "Payload too large: len={}", len);
        impl_->stat_tx_dropped.fetch_add(1, std::memory_order_relaxed);
        return -EMSGSIZE;
    }

    // Push mbuf pointer to SPSC ring (non-blocking).
    if (impl_->hal->ring_sp_enqueue(impl_->tx_ring, m) != 0) {
        // Ring is full — free the mbuf we just allocated and report back.
        impl_->hal->mbuf_free(m);
        SPDLOG_LOGGER_TRACE(log, "TX ring full, dropping packet");
        impl_->stat_tx_dropped.fetch_add(1, std::memory_order_relaxed);
        return -EAGAIN;
    }

    SPDLOG_LOGGER_TRACE(log, "Enqueued {} bytes", len);
    return 0;
}

inline uint32_t TxEngine::process_one_burst() {
    // Dequeue up to burst_size mbuf pointers from the SPSC ring.
    uint32_t nb = impl_->hal->ring_sc_dequeue_burst(
        impl_->tx_ring,
        impl_->burst_buf.data(),
        impl_->config.burst_size);

    if (nb == 0) return 0;

    auto** mbufs = reinterpret_cast<rte_mbuf**>(impl_->burst_buf.data());

    // Transmit the burst.
    uint16_t nb_sent = impl_->hal->eth_tx_burst(
        impl_->config.port_id,
        impl_->config.queue_id,
        mbufs,
        static_cast<uint16_t>(nb));

    SPDLOG_LOGGER_TRACE(detail::tx_logger(), "tx_burst sent={}/{}", nb_sent, nb);

    // Any mbuf not sent is still ours — must free to avoid pool leak.
    // This is the single most common DPDK bug: forgetting this free.
    uint32_t dropped = nb - nb_sent;
    for (uint32_t i = nb_sent; i < nb; ++i)
        impl_->hal->mbuf_free(mbufs[i]);

    impl_->stat_tx_packets.fetch_add(nb_sent, std::memory_order_relaxed);

    if (dropped > 0) {
        impl_->stat_tx_dropped.fetch_add(dropped, std::memory_order_relaxed);
        impl_->drops_since_last_log += dropped;

        // Rate-limited drop warning — avoid flooding the log ring.
        if (impl_->drops_since_last_log >= impl_->config.drop_log_interval) {
            impl_->push_log(spdlog::level::warn,
                std::format("TX dropped {} packets (total dropped={})",
                    impl_->drops_since_last_log,
                    impl_->stat_tx_dropped.load(std::memory_order_relaxed)));
            impl_->drops_since_last_log = 0;
        }
    }

    // Periodic pool utilization check (rate-limited to avoid measurement overhead).
    if (++impl_->burst_count % impl_->config.pool_check_burst_interval == 0)
        impl_->check_pool_utilization();

    return nb_sent;
}

inline void TxEngine::run_loop() {
    while (!impl_->stop_flag.load(std::memory_order_relaxed)) {
        process_one_burst();
    }
}

inline void TxEngine::stop() noexcept {
    impl_->stop_flag.store(true, std::memory_order_relaxed);
}

inline std::vector<LogEntry> TxEngine::drain_log_ring() {
    auto log = detail::tx_logger();
    std::vector<LogEntry> entries;
    LogEntry e;
    while (impl_->log_ring.try_pop(e)) {
        entries.push_back(e);
        log->log(e.level, "{}", std::string_view{e.msg});
    }
    return entries;
}

inline TxEngine::Stats TxEngine::stats() const noexcept {
    return Stats{
        .tx_packets          = impl_->stat_tx_packets.load(std::memory_order_relaxed),
        .tx_bytes            = impl_->stat_tx_bytes.load(std::memory_order_relaxed),
        .tx_dropped          = impl_->stat_tx_dropped.load(std::memory_order_relaxed),
        .mbuf_alloc_failures = impl_->stat_mbuf_alloc_failures.load(std::memory_order_relaxed),
    };
}

} // namespace eph_dpdk
