#pragma once

/// @file tx_worker.hpp
/// Independent TX worker component for eph::net::Transport.
///
/// TxWorker owns the TX thread, TX SPSC queue, TX stats, ping/pong state,
/// and TLS sequence monitoring. It receives a TransportCore& reference for
/// shared connection state (TCP, TLS crypto, config, lifecycle atomics).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <thread>
#include <type_traits>

#include <spdlog/spdlog.h>

#include "eph/core/tcp_concept.hpp"
#include "eph/core/framer_concept.hpp"
#include "eph/containers/bounded_queue.hpp"
#include "eph/transport/transport_core.hpp"
#include "eph/transport/transport_types.hpp"
#include "eph/transport/tls_record.hpp"
#include "eph/transport/websocket.hpp"
#include "eph/transport/ws_framer.hpp"
#include "eph/transport/detail/message_types.hpp"
#include "eph/utils/cpu.hpp"

// kEnableTimestamps constant defined once in transport_types.hpp (included via detail/message_types.hpp).
#include "eph/utils/hdr_histogram.hpp"

namespace eph::net {

/// Aggregate stats snapshot returned by TxWorker::stats().
///
/// Contains all TX-side counters and latency histograms captured at
/// a single point in time. Thread-safe to read (all source atomics
/// use relaxed ordering).
struct TxWorkerStats {
    uint64_t packets        = 0;  ///< Total messages sent on the wire
    uint64_t bytes          = 0;  ///< Total application payload bytes sent
    uint64_t text_packets   = 0;  ///< Text frame count (subset of packets)
    uint64_t text_bytes     = 0;  ///< Text frame bytes (subset of bytes)
    uint64_t dropped        = 0;  ///< Messages dropped due to TCP send failure
    uint64_t crypto_errors  = 0;  ///< TLS encryption failures
    uint64_t queue_full_count = 0; ///< Number of times enqueue failed (queue full)
    size_t   queue_hwm      = 0;  ///< Peak TX queue occupancy since last reset
    RttStats tx_latency{};        ///< Total TX latency: enqueue to wire (+ kernel TX if available)
    RttStats tx_queue_wait{};     ///< Queue transit time: enqueue to drain by TX thread
    RttStats tx_encode{};         ///< Encode + encrypt time: drain to TCP send
};

// ---------------------------------------------------------------------------
// TxWorker
// ---------------------------------------------------------------------------

/// TX worker: owns the TX thread, SPSC queue, stats, and ping/pong state.
///
/// Template parameters mirror Transport's: TcpImpl, Framer, MaxPayload,
/// QueueDepth. The worker operates on shared state via TransportCore&.
template <TcpTransport TcpImpl, MessageFramer Framer,
          size_t MaxPayload, size_t QueueDepth>
class TxWorker {
    static_assert(MaxPayload > 0, "MaxPayload must be > 0");
    static_assert(MaxPayload <= tls_const::kMaxRecordPayload,
                  "MaxPayload exceeds TLS max record size (16384)");
    static_assert(std::has_single_bit(QueueDepth),
                  "QueueDepth must be power of 2");

    static constexpr bool kIsWebSocket = std::is_same_v<Framer, WsFramer>;
    // Uses namespace-level eph::net::kEnableTimestamps from transport_types.hpp
    // (no local shadow — single source of truth for the compile-time switch).

public:
    using TxMsg  = detail::TxMessage<MaxPayload>;
    using TxQueue = eph::containers::BoundedQueue<TxMsg, QueueDepth>;

    // -- Constructor ----------------------------------------------------------

    explicit TxWorker(TransportCore<TcpImpl>& core,
                      std::atomic<uint64_t>& pong_timeouts) noexcept
        : core_(core)
        , pong_timeouts_(pong_timeouts)
    {}

    // Non-copyable, non-movable (owns a thread)
    TxWorker(const TxWorker&) = delete;
    TxWorker& operator=(const TxWorker&) = delete;
    TxWorker(TxWorker&&) = delete;
    TxWorker& operator=(TxWorker&&) = delete;

    ~TxWorker() { stop(); }

    // -- Lifecycle ------------------------------------------------------------

    /// Spawn the TX thread.
    void start() {
        SPDLOG_LOGGER_DEBUG(detail::transport_logger(), "TxWorker::start()");
        tx_thread_ = std::thread([this] { tx_loop_(); });
    }

    /// Join the TX thread (blocks until the thread exits).
    void stop() {
        if (tx_thread_.joinable()) {
            SPDLOG_LOGGER_DEBUG(detail::transport_logger(),
                "TxWorker::stop(): joining TX thread");
            tx_thread_.join();
        }
    }

    // -- Enqueue API (application thread) ------------------------------------

    /// Enqueue a single message (non-blocking, returns kQueueFull immediately
    /// if the queue has no capacity).
    [[nodiscard]] SendError enqueue(const void* data, size_t len,
                                    uint8_t opcode) noexcept {
        if (len > 0 && !data) [[unlikely]] return SendError::kNullData;
        if (len > MaxPayload) return SendError::kMessageTooLarge;
        if (!core_.running.load(std::memory_order_acquire))
            return SendError::kNotConnected;

        bool ok = tx_queue_.try_produce([&](TxMsg& msg) {
            std::memcpy(msg.data, data, len);
            msg.len = static_cast<uint16_t>(len);
            msg.opcode = opcode;
            if constexpr (kEnableTimestamps) {
                msg.tsc = eph::utils::TSC::now();
            }
        });

        if (!ok) {
            queue_full_count_.fetch_add(1, std::memory_order_relaxed);
            update_hwm_(QueueDepth);
            SPDLOG_LOGGER_TRACE(detail::transport_logger(),
                "TX enqueue failed: queue full (len={}, opcode={})",
                len, opcode);
            return SendError::kQueueFull;
        }
        SPDLOG_LOGGER_TRACE(detail::transport_logger(),
            "TX enqueue: len={}, opcode={}", len, opcode);
        // Sample HWM every 64 enqueues to avoid cross-core size() read
        // on every send(). size() reads both writer tail and reader head
        // which sit on separate cache lines — expensive on every call.
        if ((++tx_hwm_counter_ & 63) == 0) {
            update_hwm_(tx_queue_.size());
        }
        return SendError::kOk;
    }

    /// Enqueue a single message with timeout (spins for up to `timeout`
    /// waiting for queue capacity).
    template <typename Rep, typename Period>
    [[nodiscard]] SendError enqueue_for(
            const void* data, size_t len,
            std::chrono::duration<Rep, Period> timeout,
            uint8_t opcode) noexcept {
        if (len > 0 && !data) [[unlikely]] return SendError::kNullData;
        if (len > MaxPayload) return SendError::kMessageTooLarge;
        if (!core_.running.load(std::memory_order_acquire))
            return SendError::kNotConnected;

        bool ok = tx_queue_.try_produce_for([&](TxMsg& msg) {
            std::memcpy(msg.data, data, len);
            msg.len = static_cast<uint16_t>(len);
            msg.opcode = opcode;
            if constexpr (kEnableTimestamps) {
                msg.tsc = eph::utils::TSC::now();
            }
        }, timeout);

        if (!ok) {
            queue_full_count_.fetch_add(1, std::memory_order_relaxed);
            update_hwm_(QueueDepth);
            return SendError::kQueueFull;
        }
        if ((++tx_hwm_counter_ & 63) == 0) {
            update_hwm_(tx_queue_.size());
        }
        return SendError::kOk;
    }

    /// Batch-enqueue: writes all messages atomically with a single tail update.
    [[nodiscard]] SendError enqueue_batch(
            const std::span<const uint8_t>* payloads, size_t count,
            uint8_t opcode) noexcept {
        if (!core_.running.load(std::memory_order_acquire))
            return SendError::kNotConnected;
        if (count > 0 && !payloads) [[unlikely]] return SendError::kNullData;

        for (size_t i = 0; i < count; ++i) {
            if (payloads[i].size() > MaxPayload)
                return SendError::kMessageTooLarge;
        }

        bool ok = tx_queue_.try_produce_n(count,
            [&](TxMsg& slot, size_t i) {
                std::memcpy(slot.data, payloads[i].data(), payloads[i].size());
                slot.len = static_cast<uint16_t>(payloads[i].size());
                slot.opcode = opcode;
                if constexpr (kEnableTimestamps) {
                    slot.tsc = eph::utils::TSC::now();
                }
            });

        if (!ok) {
            queue_full_count_.fetch_add(1, std::memory_order_relaxed);
            return SendError::kQueueFull;
        }
        return SendError::kOk;
    }

    /// Batch-enqueue with timeout.
    template <typename Rep, typename Period>
    [[nodiscard]] SendError enqueue_batch_for(
            const std::span<const uint8_t>* payloads, size_t count,
            std::chrono::duration<Rep, Period> timeout,
            uint8_t opcode) noexcept {
        if (!core_.running.load(std::memory_order_acquire))
            return SendError::kNotConnected;
        if (count > 0 && !payloads) [[unlikely]] return SendError::kNullData;

        for (size_t i = 0; i < count; ++i) {
            if (payloads[i].size() > MaxPayload)
                return SendError::kMessageTooLarge;
        }

        bool ok = tx_queue_.try_produce_n_for(count,
            [&](TxMsg& slot, size_t i) {
                std::memcpy(slot.data, payloads[i].data(), payloads[i].size());
                slot.len = static_cast<uint16_t>(payloads[i].size());
                slot.opcode = opcode;
                if constexpr (kEnableTimestamps) {
                    slot.tsc = eph::utils::TSC::now();
                }
            }, timeout);

        if (!ok) {
            queue_full_count_.fetch_add(1, std::memory_order_relaxed);
            return SendError::kQueueFull;
        }
        return SendError::kOk;
    }

    // -- Queue queries --------------------------------------------------------

    [[nodiscard]] size_t queue_size() const noexcept {
        return tx_queue_.size();
    }

    [[nodiscard]] double queue_fill_ratio() const noexcept {
        return static_cast<double>(tx_queue_.size())
             / static_cast<double>(QueueDepth);
    }

    [[nodiscard]] size_t queue_hwm() const noexcept {
        return tx_hwm_.load(std::memory_order_relaxed);
    }

    // -- Stats ----------------------------------------------------------------

    /// Return an aggregate snapshot of TX stats + histogram percentiles.
    [[nodiscard]] TxWorkerStats stats() const noexcept {
        return TxWorkerStats{
            .packets        = tx_stats_.packets.load(std::memory_order_relaxed),
            .bytes          = tx_stats_.bytes.load(std::memory_order_relaxed),
            .text_packets   = tx_stats_.text_packets.load(std::memory_order_relaxed),
            .text_bytes     = tx_stats_.text_bytes.load(std::memory_order_relaxed),
            .dropped        = tx_stats_.dropped.load(std::memory_order_relaxed),
            .crypto_errors  = tx_stats_.crypto_errors.load(std::memory_order_relaxed),
            .queue_full_count = queue_full_count_.load(std::memory_order_relaxed),
            .queue_hwm      = tx_hwm_.load(std::memory_order_relaxed),
            .tx_latency     = histogram_to_stats_(tx_latency_histogram_),
            .tx_queue_wait  = histogram_to_stats_(tx_queue_wait_histogram_),
            .tx_encode      = histogram_to_stats_(tx_encode_histogram_),
        };
    }

    /// Reset all counters and histograms.
    void reset_stats() noexcept {
        tx_stats_.reset();
        queue_full_count_.store(0, std::memory_order_relaxed);
        tx_hwm_.store(0, std::memory_order_relaxed);
        tx_hwm_counter_ = 0;
        tx_latency_histogram_.reset();
        tx_queue_wait_histogram_.reset();
        tx_encode_histogram_.reset();
    }

    // -- Reconnect support ----------------------------------------------------

    /// Called after a reconnect completes. Drains any stale messages from the
    /// queue and resets the TLS sequence warning flag.
    void on_reconnected() noexcept {
        // Drain stale messages that were queued for the old connection.
        size_t drained = 0;
        while (tx_queue_.try_consume([](const TxMsg&) {})) {
            ++drained;
        }
        if (drained > 0) {
            SPDLOG_LOGGER_DEBUG(detail::transport_logger(),
                "TxWorker::on_reconnected(): drained {} stale messages", drained);
        }
        seq_warning_logged_ = false;
        ping_awaiting_pong_ = false;
    }

    // -- Direct access to queue (for Transport-level control frames) ----------

    /// Raw queue reference — used by Transport for control-frame enqueue
    /// (close, ping) that bypass the normal enqueue path.
    [[nodiscard]] TxQueue& queue() noexcept { return tx_queue_; }
    [[nodiscard]] const TxQueue& queue() const noexcept { return tx_queue_; }

    /// Raw stats reference — for Transport-level aggregation.
    [[nodiscard]] ThreadStats& thread_stats() noexcept { return tx_stats_; }
    [[nodiscard]] const ThreadStats& thread_stats() const noexcept { return tx_stats_; }

    /// Histogram accessors for Transport-level stats aggregation.
    [[nodiscard]] const eph::utils::HdrHistogram& latency_histogram() const noexcept {
        return tx_latency_histogram_;
    }
    [[nodiscard]] const eph::utils::HdrHistogram& queue_wait_histogram() const noexcept {
        return tx_queue_wait_histogram_;
    }
    [[nodiscard]] const eph::utils::HdrHistogram& encode_histogram() const noexcept {
        return tx_encode_histogram_;
    }

private:
    // -- Dependencies (references) --------------------------------------------
    TransportCore<TcpImpl>& core_;
    std::atomic<uint64_t>&  pong_timeouts_;

    // -- Owned state ----------------------------------------------------------
    TxQueue       tx_queue_{};
    std::thread   tx_thread_{};
    ThreadStats   tx_stats_{};
    std::atomic<size_t>   tx_hwm_{0};
    std::atomic<uint64_t> queue_full_count_{0};
    uint64_t      tx_hwm_counter_{0};
    bool          ping_awaiting_pong_{false};
    bool          seq_warning_logged_{false};

    eph::utils::HdrHistogram tx_latency_histogram_{10, 1'000'000'000ULL, 3};
    eph::utils::HdrHistogram tx_queue_wait_histogram_{10, 1'000'000'000ULL, 3};
    eph::utils::HdrHistogram tx_encode_histogram_{10, 1'000'000'000ULL, 3};

    // -- Helpers --------------------------------------------------------------

    /// Update high-watermark atomically (relaxed CAS loop).
    void update_hwm_(size_t current) noexcept {
        size_t prev = tx_hwm_.load(std::memory_order_relaxed);
        while (current > prev &&
               !tx_hwm_.compare_exchange_weak(prev, current,
                   std::memory_order_relaxed, std::memory_order_relaxed)) {
            // prev updated by CAS failure — retry
        }
    }

    /// Convert an HdrHistogram to RttStats.
    static RttStats histogram_to_stats_(
            const eph::utils::HdrHistogram& h) noexcept {
        if (h.get_total_count() == 0) return {};
        return RttStats{
            .count   = h.get_total_count(),
            .min_ns  = h.get_min_value(),
            .max_ns  = h.get_max_value(),
            .mean_ns = h.get_mean(),
            .p50_ns  = h.get_value_at_percentile(50.0),
            .p99_ns  = h.get_value_at_percentile(99.0),
            .p999_ns = h.get_value_at_percentile(99.9),
        };
    }

    // -- TX loop (runs on dedicated thread) -----------------------------------

    void tx_loop_() {
        [[maybe_unused]] auto affinity_ok = eph::utils::set_thread_affinity(core_.config.tx_cpu, "TX");
        [[maybe_unused]] auto log = detail::transport_logger();
        SPDLOG_LOGGER_DEBUG(log, "TX loop started");

        // Frame encode buffer: header overhead + payload + 1 byte for TLS
        // content type append
        constexpr size_t kFrameOverhead = kIsWebSocket
            ? ws::kMaxFrameHeaderLen : Framer::max_overhead();
        constexpr size_t kWsBufSize = kFrameOverhead + MaxPayload + 1;
        // +1 byte: TLS encrypt() temporarily writes the TLS 1.3 inner content
        // type byte at plaintext[plaintext_len]. This extra byte is mandatory.
        static_assert(kWsBufSize > kFrameOverhead + MaxPayload,
                      "ws_buf must include +1 byte for TLS encrypt content type");
        // TLS output buffer: sized for the actual max frame (without the +1 temp byte)
        constexpr size_t kMaxWsFrame = kFrameOverhead + MaxPayload;
        constexpr size_t kTlsBufSize =
            TlsRecordCrypto::encrypted_size(
                static_cast<uint16_t>(kMaxWsFrame));

        // Batch buffers for drain loop (sized from config)
        const int kMaxBatch = core_.config.tx_burst_size;
        auto batch = std::make_unique<TxMsg[]>(kMaxBatch);
        auto tls_bufs_storage = std::make_unique<uint8_t[]>(
            static_cast<size_t>(kMaxBatch) * kTlsBufSize);

        // Single frame encode buffer reused per message
        uint8_t ws_buf[kWsBufSize];

        // WS-specific: precomputed frame template for binary opcode fast path
        [[maybe_unused]] auto ws_tmpl = []() {
            if constexpr (kIsWebSocket) return ws::FrameTemplate::for_binary();
            else return 0; // unused placeholder
        }();

        // Generic framer instance (stateless for most framers)
        [[maybe_unused]] Framer framer_instance{};

        auto last_ping = std::chrono::steady_clock::now();

        while (core_.running.load(std::memory_order_acquire)) {
            // Spin-wait while RX thread is reconnecting to avoid
            // touching crypto/tcp which are being replaced.
            if (core_.reconnecting.load(std::memory_order_acquire)) [[unlikely]] {
                if (!core_.running.load(std::memory_order_acquire)) break;
                eph::utils::cpu_relax();
                continue;
            }

            // Proactive TLS key refresh: warn at 90%, trigger reconnect at 95%.
            if (core_.config.use_tls) [[unlikely]] {
                // Guard against null crypto during reconnect window
                if (!core_.crypto) continue;
                if (core_.reconnecting.load(std::memory_order_acquire)) continue;
                if (!core_.crypto) continue;
                uint64_t seq = core_.crypto->write_seq();

                if (!seq_warning_logged_ &&
                    seq >= tls_record::kSequenceWarnThreshold) {
                    SPDLOG_LOGGER_WARN(log,
                        "TLS write sequence at {}/{} (90%%), "
                        "preemptive reconnect approaching",
                        seq, tls_record::kMaxSequenceNumber);
                    seq_warning_logged_ = true;
                }

                if (seq >= tls_record::kSequenceReconnectThreshold) {
                    SPDLOG_LOGGER_WARN(log,
                        "TLS write sequence at {}/{} (95%%), "
                        "triggering preemptive reconnect for key refresh",
                        seq, tls_record::kMaxSequenceNumber);
                    core_.reconnecting.store(true, std::memory_order_release);
                    core_.tcp->reset();
                    continue;
                }
            }

            // -- WebSocket ping / pong timeout (periodic keepalive) --
            if constexpr (kIsWebSocket) {
                if (core_.config.ping_interval.count() > 0) {
                    auto now = std::chrono::steady_clock::now();

                    // Check pong timeout before sending next ping.
                    if (core_.config.pong_timeout.count() > 0 &&
                        ping_awaiting_pong_) {
                        using SteadyTimePoint =
                            std::chrono::steady_clock::time_point;
                        auto last_pong_tp = SteadyTimePoint{
                            std::chrono::nanoseconds{
                                core_.last_pong_ns.load(
                                    std::memory_order_relaxed)}};
                        if (now - last_pong_tp > core_.config.pong_timeout) {
                            pong_timeouts_.fetch_add(1,
                                std::memory_order_relaxed);
                            SPDLOG_LOGGER_WARN(log,
                                "Pong timeout: no pong received within {}s, "
                                "triggering reconnect",
                                core_.config.pong_timeout.count());
                            core_.reconnecting.store(true,
                                std::memory_order_release);
                            core_.tcp->reset();
                            ping_awaiting_pong_ = false;
                            continue;
                        }
                    }

                    if (now - last_ping >= core_.config.ping_interval) {
                        if (send_ws_ping_(ws_buf, tls_bufs_storage.get())) {
                            ping_awaiting_pong_ = true;
                        }
                        last_ping = now;
                    }
                }
            }

            // Drain: consume as many messages as available, up to kMaxBatch.
            int n = static_cast<int>(tx_queue_.try_consume_n(
                static_cast<size_t>(kMaxBatch),
                [&](const TxMsg& msg, [[maybe_unused]] size_t idx) {
                    batch[idx] = msg;
                }));

            if (n == 0) {
                // If RX signaled a graceful close and the queue is now
                // empty, the Close response has been sent — exit.
                if (core_.closing.load(std::memory_order_acquire)) [[unlikely]] {
                    SPDLOG_LOGGER_DEBUG(log,
                        "TX: closing_ set and queue drained, exiting");
                    core_.running.store(false, std::memory_order_release);
                    break;
                }
                eph::utils::cpu_relax();
                continue;
            }

            // Capture drain TSC once per batch
            [[maybe_unused]] uint64_t drain_tsc = 0;
            if constexpr (kEnableTimestamps) {
                drain_tsc = eph::utils::TSC::now();
            }

            // WS encode -> [TLS encrypt] -> TCP send for each message.
            // TLS mode: pack encrypted records contiguously for a single
            // TCP send, reducing syscall count from N to 1 per batch.
            size_t coalesced_len = 0;
            uint64_t batch_packets = 0;
            uint64_t batch_bytes = 0;
            uint64_t batch_text_packets = 0;
            uint64_t batch_text_bytes = 0;
            uint64_t batch_dropped = 0;

            for (int i = 0; i < n; ++i) {
                size_t ws_len;

                if constexpr (kIsWebSocket) {
                    // Precomputed template for common binary case;
                    // fall back to encode_frame for other opcodes.
                    if (batch[i].opcode == ws::opcode::kBinary) {
                        ws_len = ws_tmpl.encode(
                            ws_buf, batch[i].data, batch[i].len);
                    } else {
                        ws_len = ws::encode_frame(
                            ws_buf, batch[i].opcode,
                            batch[i].data, batch[i].len);
                    }
                } else {
                    ws_len = framer_instance.encode(
                        ws_buf, batch[i].data, batch[i].len, batch[i].opcode);
                }

                // Record TSC for RTT measurement when a ping frame is
                // about to hit the wire.
                if constexpr (kIsWebSocket) {
                    if (batch[i].opcode == ws::opcode::kPing) {
                        core_.last_ping_tsc.store(eph::utils::TSC::now(),
                            std::memory_order_relaxed);
                    }
                }

                // Per-message TX latency breakdown:
                //   total:      enqueue -> flush (+ kernel TX if available)
                //   queue_wait: enqueue -> drain (SPSC queue transit time)
                //   encode:     drain -> flush (WS encode + TLS encrypt)
                if constexpr (kEnableTimestamps) {
                    uint64_t flush_tsc = eph::utils::TSC::now();
                    if (batch[i].tsc > 0 && flush_tsc > batch[i].tsc) {
                        // Total: enqueue -> flush + kernel TX
                        auto total_ns = eph::utils::TSC::to_ns(
                            flush_tsc - batch[i].tsc);
                        if (total_ns) {
                            uint64_t total = static_cast<uint64_t>(*total_ns);
                            if constexpr (requires {
                                core_.tcp->last_kernel_tx_delay_ns(); }) {
                                total +=
                                    core_.tcp->last_kernel_tx_delay_ns();
                            }
                            tx_latency_histogram_.record(total);
                        }
                        // Queue wait: enqueue -> drain
                        if (drain_tsc > batch[i].tsc) {
                            auto qw_ns = eph::utils::TSC::to_ns(
                                drain_tsc - batch[i].tsc);
                            if (qw_ns) tx_queue_wait_histogram_.record(
                                static_cast<uint64_t>(*qw_ns));
                        }
                        // Encode+encrypt: drain -> flush
                        if (flush_tsc > drain_tsc) {
                            auto enc_ns = eph::utils::TSC::to_ns(
                                flush_tsc - drain_tsc);
                            if (enc_ns) tx_encode_histogram_.record(
                                static_cast<uint64_t>(*enc_ns));
                        }
                    }
                }

                if (core_.config.use_tls) {
                    // Guard against null crypto during reconnect (M8).
                    if (!core_.crypto) break;
                    // Pack encrypted records contiguously
                    uint8_t* tls_buf_i =
                        tls_bufs_storage.get() + coalesced_len;
                    uint16_t enc_len = core_.crypto->encrypt(
                        ws_buf, static_cast<uint16_t>(ws_len), tls_buf_i);

                    if (enc_len == 0) {
                        tx_stats_.crypto_errors.fetch_add(1,
                            std::memory_order_relaxed);
                        // Sequence exhaustion: reconnect for fresh keys
                        if (!core_.crypto) break;
                        uint64_t wseq = core_.crypto->write_seq();
                        if (wseq >= tls_record::kMaxSequenceNumber) {
                            SPDLOG_LOGGER_ERROR(log,
                                "TLS write sequence exhausted ({}), "
                                "triggering reconnect for fresh keys", wseq);
                            core_.reconnecting.store(true,
                                std::memory_order_release);
                            core_.tcp->reset();
                            break;
                        }
                    } else {
                        coalesced_len += enc_len;
                        batch_packets++;
                        batch_bytes += batch[i].len;
                        if (batch[i].opcode == ws::opcode::kText) {
                            batch_text_packets++;
                            batch_text_bytes += batch[i].len;
                        }
                    }
                } else {
                    // Plain: send frame directly over TCP.
                    auto result = core_.tcp->send(ws_buf, ws_len);
                    if (!result) {
                        batch_dropped++;
                        SPDLOG_LOGGER_WARN(log,
                            "TCP send failed (dropped): {}", result.error());
                    } else {
                        batch_packets++;
                        batch_bytes += batch[i].len;
                        if (batch[i].opcode == ws::opcode::kText) {
                            batch_text_packets++;
                            batch_text_bytes += batch[i].len;
                        }
                    }
                }
            }

            // Commit batch stats — single coalesced TCP send for TLS mode
            if (core_.config.use_tls && coalesced_len > 0) {
                auto result = core_.tcp->send(
                    tls_bufs_storage.get(), coalesced_len);
                if (!result) {
                    batch_dropped += batch_packets;
                    batch_packets = 0;
                    batch_bytes = 0;
                    batch_text_packets = 0;
                    batch_text_bytes = 0;
                    SPDLOG_LOGGER_WARN(log,
                        "Coalesced TCP send failed ({}B, {} records): {}",
                        coalesced_len, batch_dropped, result.error());
                }
            }
            if (batch_packets > 0) {
                tx_stats_.packets.fetch_add(batch_packets,
                    std::memory_order_relaxed);
                tx_stats_.bytes.fetch_add(batch_bytes,
                    std::memory_order_relaxed);
                if (batch_text_packets > 0) {
                    tx_stats_.text_packets.fetch_add(batch_text_packets,
                        std::memory_order_relaxed);
                    tx_stats_.text_bytes.fetch_add(batch_text_bytes,
                        std::memory_order_relaxed);
                }
            }
            if (batch_dropped > 0) {
                tx_stats_.dropped.fetch_add(batch_dropped,
                    std::memory_order_relaxed);
            }
        }

        // Final drain: send any messages queued before stop() was called.
        // Skip if reconnecting (crypto/tcp may be invalid).
        if (!core_.reconnecting.load(std::memory_order_acquire) &&
            core_.tcp && core_.tcp->is_established()) {
            final_drain_(batch.get(), kMaxBatch, ws_buf,
                         tls_bufs_storage.get(), kTlsBufSize,
                         ws_tmpl, framer_instance, log);
        }

        SPDLOG_LOGGER_DEBUG(log, "TX loop exited");
    }

    /// Final drain helper — sends remaining queued messages before thread exit.
    template <typename WsTmpl, typename FramerInst>
    void final_drain_(TxMsg* batch, int max_batch,
                      uint8_t* ws_buf, uint8_t* tls_bufs_storage,
                      [[maybe_unused]] size_t tls_buf_size,
                      [[maybe_unused]] WsTmpl& ws_tmpl,
                      [[maybe_unused]] FramerInst& framer_instance,
                      [[maybe_unused]] spdlog::logger* log) noexcept {
        int remaining = static_cast<int>(tx_queue_.try_consume_n(
            static_cast<size_t>(max_batch),
            [&](const TxMsg& msg, [[maybe_unused]] size_t idx) {
                batch[idx] = msg;
            }));

        if (remaining <= 0) return;

        SPDLOG_LOGGER_DEBUG(log,
            "TX: draining {} remaining messages before exit", remaining);

        size_t drain_coalesced = 0;
        for (int i = 0; i < remaining; ++i) {
            size_t ws_len;
            if constexpr (kIsWebSocket) {
                if (batch[i].opcode == ws::opcode::kBinary) {
                    ws_len = ws_tmpl.encode(
                        ws_buf, batch[i].data, batch[i].len);
                } else {
                    ws_len = ws::encode_frame(
                        ws_buf, batch[i].opcode,
                        batch[i].data, batch[i].len);
                }
            } else {
                ws_len = framer_instance.encode(
                    ws_buf, batch[i].data, batch[i].len, batch[i].opcode);
            }

            auto account_drain_msg = [&](uint16_t len, uint8_t opcode) {
                tx_stats_.packets.fetch_add(1, std::memory_order_relaxed);
                tx_stats_.bytes.fetch_add(len, std::memory_order_relaxed);
                if (opcode == ws::opcode::kText) {
                    tx_stats_.text_packets.fetch_add(1,
                        std::memory_order_relaxed);
                    tx_stats_.text_bytes.fetch_add(len,
                        std::memory_order_relaxed);
                }
            };

            if (core_.config.use_tls) {
                if (!core_.crypto) break;
                uint8_t* tls_buf_i = tls_bufs_storage + drain_coalesced;
                uint16_t enc_len = core_.crypto->encrypt(
                    ws_buf, static_cast<uint16_t>(ws_len), tls_buf_i);
                if (enc_len > 0) {
                    drain_coalesced += enc_len;
                    account_drain_msg(batch[i].len, batch[i].opcode);
                }
            } else {
                auto result = core_.tcp->send(ws_buf, ws_len);
                if (result) {
                    account_drain_msg(batch[i].len, batch[i].opcode);
                } else {
                    tx_stats_.dropped.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        if (core_.config.use_tls && drain_coalesced > 0) {
            (void)core_.tcp->send(tls_bufs_storage, drain_coalesced);
        }
    }

    /// Send a WebSocket ping frame (called from TX thread only).
    /// Uses caller-provided buffers to avoid extra stack allocations.
    /// Records TSC timestamp for RTT measurement by the RX thread.
    bool send_ws_ping_(uint8_t* ws_buf, uint8_t* tls_buf) noexcept {
        size_t ping_len = ws::build_ping_frame(ws_buf);

        // Record TSC just before TCP send for tightest RTT measurement
        core_.last_ping_tsc.store(eph::utils::TSC::now(),
            std::memory_order_relaxed);

        if (core_.config.use_tls) {
            if (!core_.crypto) return false;
            uint16_t tls_len = core_.crypto->encrypt(
                ws_buf, static_cast<uint16_t>(ping_len), tls_buf);
            if (tls_len == 0) return false;

            auto result = core_.tcp->send(tls_buf, tls_len);
            if (!result) {
                SPDLOG_LOGGER_DEBUG(detail::transport_logger(),
                    "WS ping send failed: {}", result.error());
                return false;
            }
        } else {
            auto result = core_.tcp->send(ws_buf, ping_len);
            if (!result) {
                SPDLOG_LOGGER_DEBUG(detail::transport_logger(),
                    "WS ping send failed: {}", result.error());
                return false;
            }
        }
        return true;
    }
};

} // namespace eph::net
