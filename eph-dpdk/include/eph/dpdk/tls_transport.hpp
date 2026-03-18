#pragma once

/// @file tls_transport.hpp
/// Layer 2: TLS Transport — merged TX Engine + TLS Session.
///
/// This is the core hot-path abstraction.  It owns:
///   - A BoundedQueueBytes SPSC ring (from eph-containers)
///   - TLS session state (key material, AEAD context)
///   - A TX lcore thread that consumes payloads, encrypts, and bursts
///   - A local mbuf preallocation stack (linear, no modulo on hot path)
///
/// Design principles:
///   - MaxPayload and QueueDepth are compile-time (they size the SPSC ring
///     and mbuf cache — no heap allocation on the hot path).
///   - Port/queue IDs, TLS endpoint, lcore affinity are runtime config.
///   - The application thread only touches send() — one memcpy into the
///     SPSC ring slot, then returns.  All mbuf/encrypt/burst work happens
///     on the dedicated TX lcore.
///   - Cache locality: TlsHotState fits in one cache line (key + IV + seq).
///
/// Architecture:
///
///   App thread                          TX lcore (busy poll)
///   ──────────                          ─────────────────────
///   send(payload, len)                  queue_.try_consume(visitor)
///     │                                   │
///     ▼                                   ▼
///   BoundedQueueBytes::push()           mbuf = prealloc stack pop
///     (single memcpy into SPSC slot)    fill TLS record header (AAD)
///     return 0 / -EAGAIN                encrypt in-place (AES-GCM)
///                                       rte_eth_tx_burst()
///                                       free unsent mbufs
///                                       refill mbuf stack if low
///
/// Logging strategy (hot path):
///   TRACE/DEBUG — compile-time eliminated via SPDLOG_ACTIVE_LEVEL
///   WARN/ERROR  — written to lock-free SpscLogRing, drained by main thread
///   Hot-path log messages use snprintf (no heap alloc, no std::format)

#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#include "eph/base/cache.hpp"
#include "eph/containers/bounded_queue_bytes.hpp"
#include "eph/utils/cpu.hpp"

// Forward declaration for BoringSSL AEAD context.
// The actual TLS backend is linked separately; this header only needs
// the opaque pointer.
struct evp_aead_ctx_st;
using EVP_AEAD_CTX = evp_aead_ctx_st;

namespace eph::dpdk {

// ─────────────────────────────────────────────────────────────────────────────
// TLS hot-path state
//
// All fields the TX lcore reads per-packet are packed into one cache line.
// The AEAD context pointer is read-only on the hot path (set during
// handshake, never mutated during encrypt), avoiding write sharing.
//
// Layout on 64-bit (verified by static_assert):
//   key[32] + iv[12] + pad[4] + seq_no[8] + key_len[1] + pad[7] = 64
//
// The AEAD context pointer lives in Impl, NOT here.  Adding an 8-byte
// pointer would push the struct past 64 bytes, rounding to 128 with
// alignas(64).  The pointer is read-only and stays in L1 from Impl.
//
// Record framing constants (content_type, tls_version) are NOT stored here
// because they never change per-connection.  They live in Impl as a
// pre-built 5-byte header template.  Only the length field is patched
// per-packet.  This frees bytes for the AEAD pointer.
// ─────────────────────────────────────────────────────────────────────────────

struct alignas(eph::base::CACHE_LINE_SIZE) TlsHotState {
    std::array<uint8_t, 32> key{};         ///< AES key (16 or 32 bytes used)
    std::array<uint8_t, 12> implicit_iv{}; ///< TLS 1.3 per-connection IV
    uint64_t seq_no{0};                    ///< Nonce = IV XOR seq_no (big-endian)
    uint8_t key_len{16};                   ///< 16 (AES-128) or 32 (AES-256)
    // Layout: key[32] + iv[12] + 4(pad) + seq_no[8] + key_len[1] + 7(pad) = 64
    //
    // The AEAD context pointer (EVP_AEAD_CTX*) is intentionally NOT here.
    // Adding an 8-byte pointer would push the struct to 68 bytes before
    // alignment, rounding up to 128 bytes (two cache lines).  Instead,
    // aead_ctx lives in Impl and is passed to build_tls_record() —
    // it's read-only and likely already in L1 from the previous call.
};

static_assert(sizeof(TlsHotState) <= eph::base::CACHE_LINE_SIZE,
    "TlsHotState must fit in one cache line; "
    "a second line load per encrypt would add ~4ns latency");

// ─────────────────────────────────────────────────────────────────────────────
// TLS record constants
// ─────────────────────────────────────────────────────────────────────────────

namespace tls_const {
    inline constexpr uint16_t kRecordHeaderLen  = 5;     // type(1) + version(2) + length(2)
    inline constexpr uint16_t kAuthTagLen       = 16;    // AES-GCM authentication tag
    inline constexpr uint16_t kMaxRecordPayload = 16384; // TLS max plaintext fragment
} // namespace tls_const

// ─────────────────────────────────────────────────────────────────────────────
// Transport configuration
// ─────────────────────────────────────────────────────────────────────────────

struct TlsTransportConfig {
    // ── Network ──────────────────────────────────────────────────────────
    std::string remote_host;
    uint16_t    remote_port{443};

    // ── DPDK ─────────────────────────────────────────────────────────────
    uint16_t port_id{0};
    uint16_t tx_queue_id{0};

    // ── TX lcore ─────────────────────────────────────────────────────────
    /// CPU core ID for the TX lcore.  Should be isolated via isolcpus.
    unsigned tx_lcore_cpu_id{2};

    // ── Mbuf preallocation ───────────────────────────────────────────────
    /// Target number of mbufs in the TX lcore's local stack.
    /// Refilled in bulk when count drops below low_watermark fraction.
    uint32_t mbuf_cache_size{256};

    // ── Monitoring ───────────────────────────────────────────────────────
    /// Refill mbufs when the stack drops below this fraction of cache_size.
    float    mbuf_cache_low_watermark{0.25f};

    /// Accumulate this many TX drops before emitting a single WARN.
    uint32_t drop_log_interval{1000};

    // ── TLS record framing (constant per connection) ─────────────────────
    uint16_t tls_version{0x0303};    // TLS record layer version field
    uint8_t  content_type{0x17};     // application_data
};

// ─────────────────────────────────────────────────────────────────────────────
// Lock-free SPSC log ring for hot-path diagnostics
//
// TX lcore pushes WARN/ERROR entries without any mutex.
// Main thread drains via drain_logs().
// ─────────────────────────────────────────────────────────────────────────────

struct LogEntry {
    spdlog::level::level_enum level{spdlog::level::warn};
    char msg[248]{};
};

namespace detail {

template <uint32_t N>
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
    alignas(eph::base::CACHE_LINE_SIZE) std::atomic<uint32_t> head_{0};
    alignas(eph::base::CACHE_LINE_SIZE) std::atomic<uint32_t> tail_{0};
};

inline std::shared_ptr<spdlog::logger> transport_logger() {
    static auto l = [] {
        auto lg = spdlog::stdout_color_mt("dpdk.transport");
        lg->set_level(spdlog::level::trace);
        return lg;
    }();
    return l;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// MbufStack — linear stack for mbuf preallocation
//
// Much simpler and faster than a circular buffer:
//   - pop() is a single decrement + array read (no modulo)
//   - refill() writes to a contiguous region (rte_pktmbuf_alloc_bulk friendly)
//   - no wraparound edge cases
//
// TX lcore exclusive — no synchronization needed.
// ─────────────────────────────────────────────────────────────────────────────

class MbufStack {
    static constexpr uint32_t kMaxCapacity = 512;

public:
    explicit MbufStack(rte_mempool* pool) noexcept : pool_(pool) {}

    ~MbufStack() {
        // Free all cached mbufs back to the mempool.
        for (uint32_t i = 0; i < count_; ++i) {
            rte_pktmbuf_free(buf_[i]);
        }
        count_ = 0;
    }

    MbufStack(const MbufStack&) = delete;
    MbufStack& operator=(const MbufStack&) = delete;

    /// Pop one mbuf from the top of the stack.
    /// Returns nullptr if the stack is empty.
    [[nodiscard]] rte_mbuf* pop() noexcept {
        if (count_ == 0) [[unlikely]] return nullptr;
        return buf_[--count_];
    }

    /// Refill the stack up to target_count from the mempool in one bulk call.
    /// The bulk region is contiguous: buf_[count_] .. buf_[count_ + need - 1].
    /// Returns false only if the mempool is exhausted.
    bool refill(uint32_t target_count) noexcept {
        if (count_ >= target_count) return true;

        uint32_t need = std::min(target_count - count_, kMaxCapacity - count_);
        if (need == 0) return true;

        int ret = rte_pktmbuf_alloc_bulk(pool_, &buf_[count_], need);
        if (ret != 0) return false;

        count_ += need;
        return true;
    }

    [[nodiscard]] uint32_t count() const noexcept { return count_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] rte_mempool* pool() const noexcept { return pool_; }

private:
    rte_mempool* pool_;
    uint32_t count_{0};
    std::array<rte_mbuf*, kMaxCapacity> buf_{};
};

// ─────────────────────────────────────────────────────────────────────────────
// TlsTransport<MaxPayload, QueueDepth>
//
// Template parameters (compile-time):
//   MaxPayload — maximum application payload per send() call (bytes).
//                Determines SPSC slot size and mbuf data room validation.
//   QueueDepth — SPSC ring capacity (must be power of 2).
//                Determines backpressure behavior: send() returns -EAGAIN
//                when the ring is full.
// ─────────────────────────────────────────────────────────────────────────────

template <size_t MaxPayload = 512, size_t QueueDepth = 1024>
class TlsTransport {
    static_assert(MaxPayload > 0 && MaxPayload <= tls_const::kMaxRecordPayload,
        "MaxPayload must be in (0, 16384]");
    static_assert(QueueDepth > 0 && std::has_single_bit(QueueDepth),
        "QueueDepth must be a power of 2");

    /// Total bytes written into each mbuf:
    ///   TLS record header (5) + ciphertext (MaxPayload) + auth tag (16)
    static constexpr size_t kMbufDataNeeded =
        tls_const::kRecordHeaderLen + MaxPayload + tls_const::kAuthTagLen;

public:
    struct Stats {
        uint64_t tx_packets{0};
        uint64_t tx_bytes{0};
        uint64_t tx_dropped{0};
        uint64_t encrypt_errors{0};
        uint64_t mbuf_alloc_failures{0};
        uint64_t queue_full_count{0};
    };

    // ── Factory ──────────────────────────────────────────────────────────

    /// Create and fully initialize the transport:
    ///   1. Validate mbuf data room against MaxPayload
    ///   2. TLS handshake (blocking, on calling thread)
    ///   3. Extract session keys → TlsHotState
    ///   4. Launch TX lcore thread (preallocates mbufs on entry)
    ///
    /// @param pool  Mempool from Platform::mempool().
    /// @param cfg   Transport configuration.
    /// @return Ready-to-send transport, or error string.
    [[nodiscard]] static std::expected<TlsTransport, std::string>
    create(rte_mempool* pool, const TlsTransportConfig& cfg);

    ~TlsTransport();

    TlsTransport(const TlsTransport&) = delete;
    TlsTransport& operator=(const TlsTransport&) = delete;
    TlsTransport(TlsTransport&&) noexcept;
    TlsTransport& operator=(TlsTransport&&) noexcept;

    // ── Application thread API ───────────────────────────────────────────

    /// Send a payload over the TLS connection.
    ///
    /// Non-blocking.  Copies payload into the SPSC ring slot and returns.
    /// The TX lcore will encrypt and transmit asynchronously.
    ///
    /// @return  0        Success.
    /// @return -EAGAIN   Queue full (backpressure).
    /// @return -EMSGSIZE Payload exceeds MaxPayload.
    /// @return -ENOTCONN Transport not ready or already stopped.
    int send(const void* payload, size_t len) noexcept;

    /// Convenience overload for span.
    int send(std::span<const uint8_t> payload) noexcept {
        return send(payload.data(), payload.size());
    }

    // ── Maintenance (call from main thread) ──────────────────────────────

    /// Drain log entries from the TX lcore's lock-free ring.
    /// Each entry is also forwarded to the dpdk.transport spdlog logger.
    std::vector<LogEntry> drain_logs();

    /// Snapshot of TX statistics.  All counters are monotonically increasing.
    [[nodiscard]] Stats stats() const noexcept;

    /// Signal the TX lcore to exit, then block until it joins.
    /// Called automatically by the destructor.
    void stop() noexcept;

    /// Check if the transport is operational (session ready, not stopped).
    [[nodiscard]] bool is_running() const noexcept;

    // ── Compile-time introspection ───────────────────────────────────────

    static constexpr size_t max_payload()  noexcept { return MaxPayload; }
    static constexpr size_t queue_depth()  noexcept { return QueueDepth; }

private:
    struct Impl;
    explicit TlsTransport(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// ─────────────────────────────────────────────────────────────────────────────
// TlsTransport::Impl
//
// Memory layout ordered to minimize false sharing:
//   Zone 1: App-thread written fields (queue, stat_queue_full)
//   Zone 2: Shared control flags (session_ready, stop_flag) — rare writes
//   Zone 3: TX-lcore hot data (hot_state, stats, mbuf_stack)
//   Zone 4: Cold data (config, thread handle, log ring)
//
// Destruction order:
//   stop_flag must be set and tx_thread joined BEFORE mbuf_stack is
//   destroyed.  The explicit stop() in ~TlsTransport() guarantees this
//   because Impl::~Impl() runs after the tx_thread has been joined.
//   The jthread destructor would also request_stop + join, but we do it
//   explicitly for clarity.
// ─────────────────────────────────────────────────────────────────────────────

template <size_t MaxPayload, size_t QueueDepth>
struct TlsTransport<MaxPayload, QueueDepth>::Impl {

    // ══════════════════════════════════════════════════════════════════════
    // Zone 1: App thread writes
    // ══════════════════════════════════════════════════════════════════════

    /// SPSC queue: app thread produces, TX lcore consumes.
    eph::containers::BoundedQueueBytes<MaxPayload, QueueDepth> tx_queue;

    /// Incremented by app thread when queue is full.  Own cache line to
    /// avoid false sharing with TX lcore's stat counters.
    alignas(eph::base::CACHE_LINE_SIZE)
    std::atomic<uint64_t> stat_queue_full{0};

    // ══════════════════════════════════════════════════════════════════════
    // Zone 2: Shared control (written once or very infrequently)
    // ══════════════════════════════════════════════════════════════════════

    /// Set by main thread (release) after handshake populates hot_state.
    /// Read by TX lcore (acquire) — guarantees visibility of hot_state.
    alignas(eph::base::CACHE_LINE_SIZE)
    std::atomic<bool> session_ready{false};

    /// Set by stop(), read by TX lcore every loop iteration.
    std::atomic<bool> stop_flag{false};

    // ══════════════════════════════════════════════════════════════════════
    // Zone 3: TX lcore hot data (read/written every packet)
    // ══════════════════════════════════════════════════════════════════════

    /// Crypto material — one cache line.  seq_no is the only field
    /// written per-packet; everything else is read-only after handshake.
    alignas(eph::base::CACHE_LINE_SIZE)
    TlsHotState hot_state;

    /// AEAD context — read-only on hot path (initialized during handshake).
    /// Lives outside TlsHotState to keep that struct ≤ 64 bytes.
    /// The pointer itself stays in L1 because it's accessed every packet
    /// from the same TX lcore.
    EVP_AEAD_CTX* aead_ctx{nullptr};

    /// TX statistics.  Written by TX lcore (relaxed), read by stats().
    /// Grouped on one cache line — stats() reads them all at once anyway,
    /// and they're all written by the same thread (TX lcore) so no
    /// false sharing concern among themselves.
    alignas(eph::base::CACHE_LINE_SIZE)
    std::atomic<uint64_t> stat_tx_packets{0};
    std::atomic<uint64_t> stat_tx_bytes{0};
    std::atomic<uint64_t> stat_tx_dropped{0};
    std::atomic<uint64_t> stat_encrypt_errors{0};
    std::atomic<uint64_t> stat_mbuf_alloc_failures{0};

    /// Drop accumulator — TX lcore only, no atomic needed.
    uint64_t drops_since_last_log{0};

    // ══════════════════════════════════════════════════════════════════════
    // Zone 4: Cold data (accessed at startup or infrequently)
    // ══════════════════════════════════════════════════════════════════════

    alignas(eph::base::CACHE_LINE_SIZE)
    TlsTransportConfig config;

    rte_mempool* pool{nullptr};

    /// Pre-built 5-byte TLS record header template.
    /// Bytes 0–2 (type + version) are constant for this connection.
    /// Bytes 3–4 (length) are patched per-packet in build_tls_record().
    std::array<uint8_t, tls_const::kRecordHeaderLen> record_hdr_tpl{};

    /// Mbuf preallocation stack — owned, destroyed after tx_thread joins.
    std::unique_ptr<MbufStack> mbuf_stack;

    /// TX lcore thread — must be declared AFTER mbuf_stack so that on
    /// destruction, jthread joins first (if stop() wasn't called
    /// explicitly), then mbuf_stack is destroyed.
    std::jthread tx_thread;

    static constexpr uint32_t kLogRingSize = 64;
    detail::SpscLogRing<kLogRingSize> log_ring;

    // ── Destructor ───────────────────────────────────────────────────────
    // Default is correct: jthread destructor request_stop + join ensures
    // the TX lcore exits before mbuf_stack is destroyed.
    ~Impl() = default;

    // ── Hot-path log helper ──────────────────────────────────────────────
    //
    // Uses C snprintf — no heap allocation, no exceptions.
    // std::format is forbidden on the TX lcore because it may allocate
    // via operator new, triggering ptmalloc lock contention.

    void push_log(spdlog::level::level_enum lvl,
                  const char* msg) noexcept {
        LogEntry e;
        e.level = lvl;
        std::strncpy(e.msg, msg, sizeof(e.msg) - 1);
        e.msg[sizeof(e.msg) - 1] = '\0';
        log_ring.try_push(e);  // silently dropped if ring is full
    }

    /// Formatted variant — still snprintf-based, no heap allocation.
    /// Use only for WARN/ERROR (which are rare on the hot path).
    template <typename... Args>
    void push_log_fmt(spdlog::level::level_enum lvl,
                      const char* fmt, Args... args) noexcept {
        LogEntry e;
        e.level = lvl;
        std::snprintf(e.msg, sizeof(e.msg), fmt, args...);
        log_ring.try_push(e);
    }

    // ── TLS record construction ──────────────────────────────────────────

    /// Build a complete TLS record in the mbuf:
    ///   [record header 5B][ciphertext len B][auth tag 16B]
    ///
    /// AEAD ordering (critical for correctness):
    ///   1. Fill record header FIRST — it serves as the AEAD additional data
    ///   2. Copy plaintext into mbuf at the ciphertext offset
    ///   3. Encrypt in-place: header as AAD, plaintext → ciphertext + tag
    ///   4. Set mbuf length metadata
    ///   5. Advance sequence number
    ///
    /// @return true on success, false if encrypt fails.
    bool build_tls_record(rte_mbuf* m,
                          const uint8_t* payload,
                          uint16_t len) noexcept {
        using namespace tls_const;

        auto* base = rte_pktmbuf_mtod(m, uint8_t*);

        // ── 1. Fill TLS record header (= AEAD additional data) ──────────
        //
        // Copy the pre-built template (type + version already set),
        // then patch the 2-byte length field for this specific record.
        // Record payload length = ciphertext + auth tag = len + 16.
        uint16_t record_payload_len = len + kAuthTagLen;

        std::memcpy(base, record_hdr_tpl.data(), kRecordHeaderLen);
        base[3] = static_cast<uint8_t>(record_payload_len >> 8);
        base[4] = static_cast<uint8_t>(record_payload_len & 0xFF);

        auto* ct_dst = base + kRecordHeaderLen;

        // ── 2. Copy plaintext into the ciphertext position ──────────────
        std::memcpy(ct_dst, payload, len);

        // ── 3. Construct nonce: IV XOR seq_no (RFC 8446 §5.3) ──────────
        //
        // 12-byte nonce = implicit_iv XOR (seq_no zero-padded to 12 bytes).
        // First 4 bytes of IV pass through untouched; last 8 bytes are
        // XORed with the big-endian 64-bit sequence number.
        alignas(16) uint8_t nonce[12];
        std::memcpy(nonce, hot_state.implicit_iv.data(), 12);
        uint64_t seq_be = __builtin_bswap64(hot_state.seq_no);
        uint64_t* nonce_low = reinterpret_cast<uint64_t*>(nonce + 4);
        *nonce_low ^= seq_be;

        // ── 4. AEAD encrypt in-place ────────────────────────────────────
        //
        // BoringSSL EVP_AEAD_CTX_seal():
        //   - Encrypts ct_dst[0..len-1] (plaintext → ciphertext in-place)
        //   - Appends 16-byte auth tag at ct_dst[len..len+15]
        //   - Uses base[0..4] (record header) as additional authenticated data
        //
        // TODO(tls): Uncomment when BoringSSL is linked:
        //
        //   size_t out_len = 0;
        //   if (!EVP_AEAD_CTX_seal(
        //           aead_ctx,
        //           ct_dst, &out_len,
        //           static_cast<size_t>(len) + kAuthTagLen,
        //           nonce, sizeof(nonce),
        //           ct_dst, len,              // plaintext (in-place)
        //           base, kRecordHeaderLen    // AAD = record header
        //       )) {
        //       return false;
        //   }
        //
        // Placeholder: plaintext goes through unencrypted (+ zeroed tag).
        // Only acceptable for skeleton testing with net_null.
        std::memset(ct_dst + len, 0, kAuthTagLen);
        (void)nonce;

        // ── 5. Set mbuf length metadata ─────────────────────────────────
        uint16_t total_len = kRecordHeaderLen + record_payload_len;
        m->data_len = total_len;
        m->pkt_len  = total_len;

        // ── 6. Advance sequence number ──────────────────────────────────
        hot_state.seq_no++;

        return true;
    }

    // ── TX lcore entry point ────────────────────────────────────────────

    void tx_loop() noexcept {
        auto log = detail::transport_logger();
        SPDLOG_LOGGER_DEBUG(log, "TX lcore started on CPU {}",
                            config.tx_lcore_cpu_id);

        eph::utils::set_thread_affinity(config.tx_lcore_cpu_id);

        // ── Initial mbuf fill ───────────────────────────────────────────
        if (!mbuf_stack->refill(config.mbuf_cache_size)) {
            push_log_fmt(spdlog::level::err,
                "initial mbuf refill failed, pool_avail=%u",
                rte_mempool_avail_count(pool));
        }

        // ── Wait for TLS session ────────────────────────────────────────
        // Acquire load ensures all hot_state writes are visible.
        while (!session_ready.load(std::memory_order_acquire)) {
            if (stop_flag.load(std::memory_order_relaxed)) return;
            eph::utils::cpu_relax();
        }

        SPDLOG_LOGGER_DEBUG(log, "TLS session ready, entering busy poll");

        // Pre-compute watermark to avoid float math per iteration.
        uint32_t refill_target = config.mbuf_cache_size;
        uint32_t low_watermark = static_cast<uint32_t>(
            static_cast<float>(refill_target)
            * config.mbuf_cache_low_watermark);

        // ── Busy poll loop ──────────────────────────────────────────────
        while (!stop_flag.load(std::memory_order_relaxed)) {

            tx_queue.try_consume([&](std::span<const uint8_t> payload) {

                // 1. Acquire mbuf from local stack (no mempool contention).
                rte_mbuf* m = mbuf_stack->pop();
                if (!m) [[unlikely]] {
                    mbuf_stack->refill(refill_target);
                    m = mbuf_stack->pop();
                }
                if (!m) [[unlikely]] {
                    stat_mbuf_alloc_failures.fetch_add(1, std::memory_order_relaxed);
                    stat_tx_dropped.fetch_add(1, std::memory_order_relaxed);
                    accumulate_drop();
                    return;
                }

                // 2. Build TLS record: header (AAD) → copy → encrypt → tag.
                if (!build_tls_record(m, payload.data(),
                        static_cast<uint16_t>(payload.size()))) [[unlikely]] {
                    rte_pktmbuf_free(m);
                    stat_encrypt_errors.fetch_add(1, std::memory_order_relaxed);
                    stat_tx_dropped.fetch_add(1, std::memory_order_relaxed);
                    accumulate_drop();
                    return;
                }

                // 3. Capture pkt_len BEFORE tx_burst.
                //    After tx_burst succeeds, the mbuf is owned by the NIC
                //    driver — accessing any mbuf field is UB.
                uint16_t pkt_len = m->pkt_len;

                // 4. Transmit.
                uint16_t sent = rte_eth_tx_burst(
                    config.port_id, config.tx_queue_id, &m, 1);

                if (sent == 1) {
                    stat_tx_packets.fetch_add(1, std::memory_order_relaxed);
                    stat_tx_bytes.fetch_add(pkt_len, std::memory_order_relaxed);
                } else {
                    // NIC did not consume the mbuf — MUST free it.
                    // This is the #1 DPDK resource leak.
                    rte_pktmbuf_free(m);
                    stat_tx_dropped.fetch_add(1, std::memory_order_relaxed);
                    accumulate_drop();
                }
            });

            // ── Lazy mbuf refill ────────────────────────────────────────
            if (mbuf_stack->count() < low_watermark) {
                if (!mbuf_stack->refill(refill_target)) {
                    push_log_fmt(spdlog::level::warn,
                        "mbuf refill failed, pool_avail=%u, cached=%u",
                        rte_mempool_avail_count(pool),
                        mbuf_stack->count());
                }
            }
        }

        SPDLOG_LOGGER_DEBUG(log, "TX lcore exiting");
    }

    /// Batch TX drop counter.  Emits a WARN every drop_log_interval drops
    /// to prevent per-drop log spam that would overflow the 64-entry ring.
    void accumulate_drop() noexcept {
        drops_since_last_log++;
        if (drops_since_last_log >= config.drop_log_interval) {
            push_log_fmt(spdlog::level::warn,
                "TX dropped %lu packets (total=%lu)",
                static_cast<unsigned long>(drops_since_last_log),
                static_cast<unsigned long>(
                    stat_tx_dropped.load(std::memory_order_relaxed)));
            drops_since_last_log = 0;
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// TlsTransport method definitions
// ─────────────────────────────────────────────────────────────────────────────

template <size_t MP, size_t QD>
TlsTransport<MP, QD>::TlsTransport(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

template <size_t MP, size_t QD>
TlsTransport<MP, QD>::~TlsTransport() {
    if (impl_) stop();
}

template <size_t MP, size_t QD>
TlsTransport<MP, QD>::TlsTransport(TlsTransport&&) noexcept = default;

template <size_t MP, size_t QD>
TlsTransport<MP, QD>&
TlsTransport<MP, QD>::operator=(TlsTransport&&) noexcept = default;

template <size_t MP, size_t QD>
std::expected<TlsTransport<MP, QD>, std::string>
TlsTransport<MP, QD>::create(rte_mempool* pool, const TlsTransportConfig& cfg) {
    auto log = detail::transport_logger();

    // ── Precondition checks ─────────────────────────────────────────────

    if (!pool) {
        SPDLOG_LOGGER_ERROR(log, "create() called with null mempool");
        return std::unexpected("mempool must not be null");
    }

    if (cfg.remote_host.empty()) {
        SPDLOG_LOGGER_ERROR(log, "create() called with empty remote_host");
        return std::unexpected("remote_host must not be empty");
    }

    // Validate mbuf data room at create time.
    // Catches MaxPayload vs pool sizing mismatches early.
    uint16_t data_room = rte_pktmbuf_data_room_size(pool);
    uint16_t avail_room = data_room - RTE_PKTMBUF_HEADROOM;
    if (avail_room < kMbufDataNeeded) {
        SPDLOG_LOGGER_ERROR(log,
            "mbuf data room too small: available={}B, needed={}B "
            "(MaxPayload={}, hdr={}, tag={})",
            avail_room, kMbufDataNeeded,
            MP, tls_const::kRecordHeaderLen, tls_const::kAuthTagLen);
        return std::unexpected(std::format(
            "mbuf data room {}B < needed {}B for MaxPayload={}",
            avail_room, kMbufDataNeeded, MP));
    }

    SPDLOG_LOGGER_DEBUG(log,
        "mbuf data room check passed: available={}B, needed={}B",
        avail_room, kMbufDataNeeded);

    // ── Build Impl ──────────────────────────────────────────────────────

    auto impl = std::make_unique<Impl>();
    impl->config = cfg;
    impl->pool = pool;
    impl->mbuf_stack = std::make_unique<MbufStack>(pool);

    // Pre-build TLS record header template.
    impl->record_hdr_tpl[0] = cfg.content_type;
    impl->record_hdr_tpl[1] = static_cast<uint8_t>(cfg.tls_version >> 8);
    impl->record_hdr_tpl[2] = static_cast<uint8_t>(cfg.tls_version & 0xFF);
    impl->record_hdr_tpl[3] = 0;
    impl->record_hdr_tpl[4] = 0;

    // ── TLS Handshake (blocking, main thread) ───────────────────────────
    //
    // Runs on the calling thread because the handshake involves heap
    // allocations, system calls, and blocking I/O — all forbidden on
    // the TX lcore.
    //
    // TODO(tls): Implement handshake via custom DPDK BIO:
    //
    //   1. SSL_CTX_new(TLS_client_method())
    //   2. SSL_set_bio() with custom DPDK BIO pair:
    //        BIO write → TCP segment → rte_eth_tx_burst()
    //        BIO read  → busy-poll rte_eth_rx_burst() with timeout
    //   3. SSL_do_handshake()
    //   4. On success:
    //        - SSL_get_current_cipher() → log cipher suite [INFO]
    //        - Extract traffic secrets:
    //            BoringSSL: SSL_get_traffic_secrets()
    //            wolfSSL:   wolfSSL_get_keys()
    //        - Derive key + IV via HKDF-Expand-Label
    //        - EVP_AEAD_CTX_init()
    //        - Populate impl->hot_state.{key, implicit_iv, key_len}
    //        - Populate impl->aead_ctx
    //   5. On failure:
    //        - Return std::unexpected with SSL error string

    SPDLOG_LOGGER_INFO(log,
        "TLS handshake with {}:{} (placeholder — no actual crypto)",
        cfg.remote_host, cfg.remote_port);

    // ── Publish session to TX lcore ─────────────────────────────────────
    //
    // Release ensures all writes to hot_state are visible when the TX
    // lcore reads session_ready with acquire.  This prevents the classic
    // "ready=true but key not visible" race (plan §3.2).
    impl->session_ready.store(true, std::memory_order_release);

    // ── Launch TX lcore ─────────────────────────────────────────────────

    SPDLOG_LOGGER_DEBUG(log, "Launching TX lcore on CPU {}",
                        cfg.tx_lcore_cpu_id);

    auto* raw = impl.get();
    impl->tx_thread = std::jthread([raw](std::stop_token) {
        raw->tx_loop();
    });

    SPDLOG_LOGGER_INFO(log,
        "TlsTransport ready: {}:{} [MaxPayload={}, QueueDepth={}]",
        cfg.remote_host, cfg.remote_port, MP, QD);

    return TlsTransport(std::move(impl));
}

template <size_t MP, size_t QD>
int TlsTransport<MP, QD>::send(const void* payload, size_t len) noexcept {
    if (!impl_ || !impl_->session_ready.load(std::memory_order_relaxed))
        [[unlikely]] {
        return -ENOTCONN;
    }

    if (len > MP) [[unlikely]] {
        return -EMSGSIZE;
    }

    auto data = std::span<const uint8_t>{
        static_cast<const uint8_t*>(payload), len};

    if (!impl_->tx_queue.try_push(data)) {
        impl_->stat_queue_full.fetch_add(1, std::memory_order_relaxed);
        return -EAGAIN;
    }

    return 0;
}

template <size_t MP, size_t QD>
std::vector<LogEntry> TlsTransport<MP, QD>::drain_logs() {
    auto log = detail::transport_logger();
    std::vector<LogEntry> entries;
    LogEntry e;
    while (impl_ && impl_->log_ring.try_pop(e)) {
        entries.push_back(e);
        log->log(e.level, "[tx_lcore] {}", std::string_view{e.msg});
    }
    return entries;
}

template <size_t MP, size_t QD>
typename TlsTransport<MP, QD>::Stats
TlsTransport<MP, QD>::stats() const noexcept {
    if (!impl_) return {};
    return Stats{
        .tx_packets          = impl_->stat_tx_packets.load(std::memory_order_relaxed),
        .tx_bytes            = impl_->stat_tx_bytes.load(std::memory_order_relaxed),
        .tx_dropped          = impl_->stat_tx_dropped.load(std::memory_order_relaxed),
        .encrypt_errors      = impl_->stat_encrypt_errors.load(std::memory_order_relaxed),
        .mbuf_alloc_failures = impl_->stat_mbuf_alloc_failures.load(std::memory_order_relaxed),
        .queue_full_count    = impl_->stat_queue_full.load(std::memory_order_relaxed),
    };
}

template <size_t MP, size_t QD>
void TlsTransport<MP, QD>::stop() noexcept {
    if (!impl_) return;

    auto log = detail::transport_logger();
    SPDLOG_LOGGER_DEBUG(log, "stop() called, signalling TX lcore");

    impl_->stop_flag.store(true, std::memory_order_relaxed);
    if (impl_->tx_thread.joinable()) {
        impl_->tx_thread.join();
    }

    SPDLOG_LOGGER_DEBUG(log, "TX lcore joined");
}

template <size_t MP, size_t QD>
bool TlsTransport<MP, QD>::is_running() const noexcept {
    return impl_
        && impl_->session_ready.load(std::memory_order_relaxed)
        && !impl_->stop_flag.load(std::memory_order_relaxed);
}

} // namespace eph::dpdk
