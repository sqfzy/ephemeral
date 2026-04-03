#pragma once

/// @file rx_worker.hpp
/// Independent RX worker component for eph::net::Transport.
///
/// RxWorker owns the RX thread, RX SPSC queue, RX stats, histograms,
/// and a FrameProcessor instance. It receives a TransportCore& reference
/// for shared connection state (TCP, TLS crypto, config, lifecycle atomics).
///
/// The RX loop (TLS reassembly, decryption, frame processing) is fully
/// self-contained here. Transport-level operations (reconnect, stop) are
/// delegated via callbacks injected at construction time.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/core/tcp_concept.hpp"
#include "eph/core/framer_concept.hpp"
#include "eph/core/transport_errors.hpp"
#include "eph/containers/bounded_queue.hpp"
#include "eph/containers/evicting_queue.hpp"
#include "eph/transport/detail/transport_core.hpp"
#include "eph/transport/transport_types.hpp"
#include "eph/transport/detail/frame_processor.hpp"
#include "eph/transport/detail/tls_record.hpp"
#include "eph/transport/detail/tls_constants.hpp"
#include "eph/transport/detail/websocket.hpp"
#include "eph/transport/ws_framer.hpp"
#include "eph/transport/detail/message_types.hpp"
#include "eph/utils/cpu.hpp"
#include "eph/utils/hdr_histogram.hpp"

namespace eph::net {

// kEnableTimestamps is defined once in transport_types.hpp (included above).

// ---------------------------------------------------------------------------
// RxWorkerStats — aggregate snapshot returned by RxWorker::stats()
// ---------------------------------------------------------------------------

/// Aggregate stats snapshot returned by RxWorker::stats().
///
/// Contains all RX-side counters and latency histograms captured at
/// a single point in time. Thread-safe to read (all source atomics
/// use relaxed ordering).
struct RxWorkerStats {
    uint64_t rx_packets       = 0;  ///< Total messages decoded and delivered
    uint64_t rx_bytes         = 0;  ///< Total application payload bytes received
    uint64_t rx_text_packets  = 0;  ///< Text frame count (subset of rx_packets)
    uint64_t rx_text_bytes    = 0;  ///< Text frame bytes (subset of rx_bytes)
    uint64_t rx_dropped       = 0;  ///< Messages dropped (queue full or oversized)
    uint64_t decrypt_errors   = 0;  ///< TLS decryption failures
    uint64_t ws_pings_received = 0; ///< WebSocket Ping frames received
    uint64_t ws_pongs_sent    = 0;  ///< WebSocket Pong frames sent in response
    size_t   rx_queue_hwm     = 0;  ///< Peak RX queue occupancy since last reset
    RttStats rx_latency{};          ///< Total RX latency: NIC arrival to frame decoded
    RttStats rx_decrypt{};          ///< TLS decrypt latency: arrival to decrypt complete
    RttStats rx_decode{};           ///< WS decode latency: decrypt complete to frame decoded
};

// ---------------------------------------------------------------------------
// RxWorker
// ---------------------------------------------------------------------------

/// RX worker: owns the RX thread, SPSC queue, stats, histograms, and a
/// FrameProcessor instance.
///
/// Template parameters:
///   TcpImpl        -- a type satisfying the TcpTransport concept
///   Framer         -- message framer (WsFramer or generic)
///   MaxPayload     -- maximum application payload size per message
///   QueueDepth     -- SPSC queue depth (must be power of 2)
///   RxQueueTmpl    -- queue template: BoundedQueue or EvictingQueue
///   LastOnlyDeliver -- when true, only deliver the last data frame per batch
template <TcpTransport TcpImpl, MessageFramer Framer,
          size_t MaxPayload, size_t QueueDepth,
          template<typename, size_t> class RxQueueTmpl = eph::containers::BoundedQueue,
          bool LastOnlyDeliver = false>
class RxWorker {
    static_assert(MaxPayload > 0, "MaxPayload must be > 0");
    static_assert(MaxPayload <= tls_const::kMaxRecordPayload,
                  "MaxPayload exceeds TLS max record size (16384)");
    static_assert(std::has_single_bit(QueueDepth),
                  "QueueDepth must be power of 2");

    static constexpr bool kIsWebSocket = std::is_same_v<Framer, WsFramer>;

public:
    using RxMsg  = detail::RxMessage<MaxPayload>;
    using RxQueue = RxQueueTmpl<RxMsg, QueueDepth>;

    /// True when the RX queue uses evicting (latest-value) semantics.
    static constexpr bool kRxEvicting =
        std::same_as<RxQueueTmpl<int, 2>,
                     eph::containers::EvictingQueue<int, 2>>;

    // -- Callbacks from Transport -----------------------------------------------

    /// Callbacks injected by the owning Transport for operations that
    /// RxWorker cannot perform on its own (reconnect, control-frame send).
    struct Callbacks {
        /// Attempt reconnection. Returns true if reconnected successfully.
        std::function<bool()> do_reconnect;
        /// Send a control-frame response (pong/close) back to the server.
        /// Parameters: (data, len, opcode). Returns SendError.
        std::function<SendError(const void*, size_t, uint8_t)> send_response;
    };

    // -- ReceivedMessage --------------------------------------------------------

    /// Received message with opcode metadata.
    struct ReceivedMessage {
        std::vector<uint8_t> data;
        uint8_t opcode = ws::opcode::kBinary;

        /// Check if this is a text message.
        [[nodiscard]] bool is_text() const noexcept {
            return opcode == ws::opcode::kText;
        }
        /// Check if this is a binary message.
        [[nodiscard]] bool is_binary() const noexcept {
            return opcode == ws::opcode::kBinary;
        }
        /// Check if this is a close frame.
        [[nodiscard]] bool is_close() const noexcept {
            return opcode == ws::opcode::kClose;
        }
        /// Return the payload as a string_view (valid only for text messages).
        [[nodiscard]] std::string_view text() const noexcept {
            return {reinterpret_cast<const char*>(data.data()), data.size()};
        }
        /// Extract the close status code from a close frame payload.
        /// Returns 0 if the payload is too short (< 2 bytes) or not a close frame.
        [[nodiscard]] uint16_t close_code() const noexcept {
            if (opcode != ws::opcode::kClose || data.size() < 2) return 0;
            return static_cast<uint16_t>((data[0] << 8) | data[1]);
        }
        /// Extract the close reason string from a close frame payload.
        [[nodiscard]] std::string_view close_reason() const noexcept {
            if (opcode != ws::opcode::kClose || data.size() <= 2) return {};
            return {reinterpret_cast<const char*>(data.data() + 2),
                    data.size() - 2};
        }
    };

    // -- Constructor -------------------------------------------------------------

    explicit RxWorker(TransportCore<TcpImpl>& core,
                      std::atomic<uint64_t>& reconnect_count,
                      Callbacks callbacks) noexcept
        : core_(core)
        , reconnect_count_(reconnect_count)
        , callbacks_(std::move(callbacks))
    {}

    // Non-copyable, non-movable (owns a thread)
    RxWorker(const RxWorker&) = delete;
    RxWorker& operator=(const RxWorker&) = delete;
    RxWorker(RxWorker&&) = delete;
    RxWorker& operator=(RxWorker&&) = delete;

    ~RxWorker() { stop(); }

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /// Spawn the RX thread.
    void start() {
        SPDLOG_LOGGER_DEBUG(detail::transport_logger(), "RxWorker::start()");
        rx_thread_ = std::thread([this] { rx_loop_(); });
    }

    /// Join the RX thread (blocks until the thread exits).
    void stop() {
        if (rx_thread_.joinable()) {
            SPDLOG_LOGGER_DEBUG(detail::transport_logger(),
                "RxWorker::stop(): joining RX thread");
            rx_thread_.join();
        }
    }

    // -----------------------------------------------------------------------
    // Receive API (application thread)
    // -----------------------------------------------------------------------

    /// Try to receive a message (non-blocking).
    /// @param callback  Called with (data_ptr, len) if a message is available.
    /// @return true if a message was consumed, false if queue empty.
    template <typename F>
        requires std::invocable<F, const uint8_t*, size_t>
    [[nodiscard]] bool recv(F&& callback) {
        bool consumed = rx_consume_([&](const RxMsg& msg) {
            SPDLOG_LOGGER_TRACE(detail::transport_logger(),
                "RX dequeue: len={}, opcode={}", msg.len, msg.opcode);
            std::invoke(std::forward<F>(callback),
                        static_cast<const uint8_t*>(msg.data),
                        static_cast<size_t>(msg.len));
        });
        return consumed;
    }

    /// Try to receive a message with opcode (non-blocking).
    template <typename F>
        requires std::invocable<F, const uint8_t*, size_t, uint8_t>
    [[nodiscard]] bool recv(F&& callback) {
        bool consumed = rx_consume_([&](const RxMsg& msg) {
            SPDLOG_LOGGER_TRACE(detail::transport_logger(),
                "RX dequeue: len={}, opcode={}", msg.len, msg.opcode);
            std::invoke(std::forward<F>(callback),
                        static_cast<const uint8_t*>(msg.data),
                        static_cast<size_t>(msg.len), msg.opcode);
        });
        return consumed;
    }

    /// Try to receive a message with opcode and arrival TSC (non-blocking).
    /// Requires -DEPH_ENABLE_TIMESTAMPS=1 at compile time.
    template <typename F>
        requires std::invocable<F, const uint8_t*, size_t, uint8_t, uint64_t>
    [[nodiscard]] bool recv(F&& callback) {
        static_assert(kEnableTimestamps,
            "recv() with TSC callback requires -DEPH_ENABLE_TIMESTAMPS=1");
        bool consumed = rx_consume_([&](const RxMsg& msg) {
            SPDLOG_LOGGER_TRACE(detail::transport_logger(),
                "RX dequeue: len={}, opcode={}, tsc={}", msg.len, msg.opcode, msg.tsc);
            std::invoke(std::forward<F>(callback),
                        static_cast<const uint8_t*>(msg.data),
                        static_cast<size_t>(msg.len), msg.opcode, msg.tsc);
        });
        return consumed;
    }

    /// Try to receive a message as a copied byte vector (non-blocking).
    [[nodiscard]] std::optional<std::vector<uint8_t>> try_recv() {
        std::optional<std::vector<uint8_t>> result;
        (void)rx_consume_([&](const RxMsg& msg) {
            result.emplace(msg.data, msg.data + msg.len);
        });
        return result;
    }

    /// Try to receive a message with opcode info (non-blocking).
    [[nodiscard]] std::optional<ReceivedMessage> try_recv_msg() {
        std::optional<ReceivedMessage> result;
        (void)rx_consume_([&](const RxMsg& msg) {
            result.emplace(ReceivedMessage{
                .data = std::vector<uint8_t>(msg.data, msg.data + msg.len),
                .opcode = msg.opcode,
            });
        });
        return result;
    }

    // -----------------------------------------------------------------------
    // Peek API — inspect without consuming
    // -----------------------------------------------------------------------

    /// Peek at the next message without consuming (non-blocking).
    template <typename F>
        requires std::invocable<F, const uint8_t*, size_t>
    [[nodiscard]] bool recv_peek(F&& callback) {
        return rx_peek_([&](const RxMsg& msg) {
            std::invoke(std::forward<F>(callback),
                        static_cast<const uint8_t*>(msg.data),
                        static_cast<size_t>(msg.len));
        });
    }

    /// Peek at the next message with opcode without consuming (non-blocking).
    template <typename F>
        requires std::invocable<F, const uint8_t*, size_t, uint8_t>
    [[nodiscard]] bool recv_peek(F&& callback) {
        return rx_peek_([&](const RxMsg& msg) {
            std::invoke(std::forward<F>(callback),
                        static_cast<const uint8_t*>(msg.data),
                        static_cast<size_t>(msg.len),
                        msg.opcode);
        });
    }

    /// Peek at the next message as a copied ReceivedMessage (non-blocking).
    [[nodiscard]] std::optional<ReceivedMessage> peek_recv_msg() {
        std::optional<ReceivedMessage> result;
        (void)rx_peek_([&](const RxMsg& msg) {
            result.emplace(ReceivedMessage{
                .data = std::vector<uint8_t>(msg.data, msg.data + msg.len),
                .opcode = msg.opcode,
            });
        });
        return result;
    }

    // -----------------------------------------------------------------------
    // Batch receive
    // -----------------------------------------------------------------------

    /// Batch-receive up to max_count messages (non-blocking).
    /// @note Not available when RxQueue is EvictingQueue.
    template <typename F>
        requires (std::invocable<F, const uint8_t*, size_t> && !kRxEvicting)
    [[nodiscard]] size_t recv_n(F&& callback, size_t max_count) {
        return rx_queue_.try_consume_n(max_count,
            [&](const RxMsg& msg, [[maybe_unused]] size_t idx) {
                std::invoke(std::forward<F>(callback), msg.data, msg.len);
            });
    }

    /// Batch-receive up to max_count messages with opcode (non-blocking).
    /// @note Not available when RxQueue is EvictingQueue.
    template <typename F>
        requires (std::invocable<F, const uint8_t*, size_t, uint8_t> && !kRxEvicting)
    [[nodiscard]] size_t recv_n(F&& callback, size_t max_count) {
        return rx_queue_.try_consume_n(max_count,
            [&](const RxMsg& msg, [[maybe_unused]] size_t idx) {
                std::invoke(std::forward<F>(callback),
                            msg.data, msg.len, msg.opcode);
            });
    }

    /// Drain all available messages (non-blocking).
    /// @note Not available when RxQueue is EvictingQueue.
    template <typename F>
        requires (std::invocable<F, const uint8_t*, size_t> && !kRxEvicting)
    [[nodiscard]] size_t drain_recv(F&& callback) {
        return recv_n(std::forward<F>(callback), QueueDepth);
    }

    // -----------------------------------------------------------------------
    // Blocking receive
    // -----------------------------------------------------------------------

    /// Blocking receive with timeout.
    template <typename F>
        requires std::invocable<F, const uint8_t*, size_t>
    [[nodiscard]] bool wait_recv(F&& callback, std::chrono::milliseconds timeout) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (core_.running.load(std::memory_order_acquire)) {
            bool got = rx_consume_([&](const RxMsg& msg) {
                std::invoke(std::forward<F>(callback),
                            static_cast<const uint8_t*>(msg.data),
                            static_cast<size_t>(msg.len));
            });
            if (got) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::yield();
        }
        return false;
    }

    /// Blocking receive with opcode and timeout.
    template <typename F>
        requires std::invocable<F, const uint8_t*, size_t, uint8_t>
    [[nodiscard]] bool wait_recv(F&& callback, std::chrono::milliseconds timeout) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (core_.running.load(std::memory_order_acquire)) {
            bool got = rx_consume_([&](const RxMsg& msg) {
                std::invoke(std::forward<F>(callback),
                            static_cast<const uint8_t*>(msg.data),
                            static_cast<size_t>(msg.len), msg.opcode);
            });
            if (got) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::yield();
        }
        return false;
    }

    /// Blocking receive returning a ReceivedMessage with timeout.
    [[nodiscard]] std::optional<ReceivedMessage> wait_recv_msg(
            std::chrono::milliseconds timeout) {
        std::optional<ReceivedMessage> result;
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (core_.running.load(std::memory_order_acquire)) {
            bool got = rx_consume_([&](const RxMsg& msg) {
                result.emplace(ReceivedMessage{
                    .data = std::vector<uint8_t>(msg.data, msg.data + msg.len),
                    .opcode = msg.opcode,
                });
            });
            if (got) return result;
            if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
            std::this_thread::yield();
        }
        return std::nullopt;
    }

    // -----------------------------------------------------------------------
    // Queue queries
    // -----------------------------------------------------------------------

    /// Approximate number of messages available in the RX queue.
    [[nodiscard]] size_t queue_size() const noexcept {
        return rx_size_();
    }

    /// RX queue occupancy as a fraction [0.0, 1.0].
    [[nodiscard]] double queue_fill_ratio() const noexcept {
        return static_cast<double>(rx_size_()) /
               static_cast<double>(QueueDepth);
    }

    /// Peak RX queue occupancy since creation or last reset_stats().
    [[nodiscard]] size_t queue_hwm() const noexcept {
        return rx_hwm_.load(std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------------
    // Stats
    // -----------------------------------------------------------------------

    /// Take a snapshot of all RX stats.
    [[nodiscard]] RxWorkerStats stats() const noexcept {
        return RxWorkerStats{
            .rx_packets        = rx_stats_.packets.load(std::memory_order_relaxed),
            .rx_bytes          = rx_stats_.bytes.load(std::memory_order_relaxed),
            .rx_text_packets   = rx_stats_.text_packets.load(std::memory_order_relaxed),
            .rx_text_bytes     = rx_stats_.text_bytes.load(std::memory_order_relaxed),
            .rx_dropped        = rx_stats_.dropped.load(std::memory_order_relaxed),
            .decrypt_errors    = rx_stats_.crypto_errors.load(std::memory_order_relaxed),
            .ws_pings_received = ws_pings_received_.load(std::memory_order_relaxed),
            .ws_pongs_sent     = ws_pongs_sent_.load(std::memory_order_relaxed),
            .rx_queue_hwm      = rx_hwm_.load(std::memory_order_relaxed),
            .rx_latency        = histogram_to_stats_(rx_latency_histogram_),
            .rx_decrypt        = histogram_to_stats_(rx_decrypt_histogram_),
            .rx_decode         = histogram_to_stats_(rx_decode_histogram_),
        };
    }

    /// Reset all statistics counters to zero.
    void reset_stats() noexcept {
        rx_stats_.reset();
        ws_pings_received_.store(0, std::memory_order_relaxed);
        ws_pongs_sent_.store(0, std::memory_order_relaxed);
        rx_hwm_.store(0, std::memory_order_relaxed);
        rx_hwm_counter_ = 0;
        rx_latency_histogram_.reset();
        rx_decrypt_histogram_.reset();
        rx_decode_histogram_.reset();
    }

    // -----------------------------------------------------------------------
    // Reconnect support
    // -----------------------------------------------------------------------

    /// Called by Transport after a successful reconnect.
    /// Clears internal state that is stale after reconnection.
    void on_reconnected() noexcept {
        rx_seq_warning_logged_ = false;
        // FrameProcessor fragmentation buffer is cleared internally
        // when the frame_processor_ is reset.
        if (frame_processor_) {
            frame_processor_->reset();
        }
    }

    // -- Direct access to owned state (for Transport composition) ----

    /// Direct access to RX stats (for Transport-level stats aggregation).
    [[nodiscard]] const ThreadStats& thread_stats() const noexcept { return rx_stats_; }
    [[nodiscard]] ThreadStats& thread_stats() noexcept { return rx_stats_; }

    /// Direct access to histograms (for Transport-level RTT reporting).
    [[nodiscard]] const auto& latency_histogram() const noexcept { return rx_latency_histogram_; }
    [[nodiscard]] const auto& decrypt_histogram() const noexcept { return rx_decrypt_histogram_; }
    [[nodiscard]] const auto& decode_histogram() const noexcept { return rx_decode_histogram_; }

    /// Direct access to ping/pong counters.
    [[nodiscard]] std::atomic<uint64_t>& pings_received() noexcept { return ws_pings_received_; }
    [[nodiscard]] std::atomic<uint64_t>& pongs_sent() noexcept { return ws_pongs_sent_; }

    /// Direct access to HWM atomic (for Transport-level stats).
    [[nodiscard]] const std::atomic<size_t>& hwm_atomic() const noexcept { return rx_hwm_; }

    /// Direct access to the RX queue (for Transport-level size queries).
    [[nodiscard]] const RxQueue& queue() const noexcept { return rx_queue_; }

private:
    // -----------------------------------------------------------------------
    // RX queue dispatch helpers — abstract BoundedQueue vs EvictingQueue
    // -----------------------------------------------------------------------

    /// Enqueue a decoded message into the RX queue.
    /// BoundedQueue: try_produce (backpressure -- may fail).
    /// EvictingQueue: push (evicts oldest -- never fails).
    bool rx_enqueue_(const uint8_t* data, uint16_t len,
                     uint8_t opcode) noexcept {
        if constexpr (kRxEvicting) {
            rx_queue_.produce([&](RxMsg& slot) {
                std::memcpy(slot.data, data, len);
                slot.len = len;
                slot.opcode = opcode;
                if constexpr (kEnableTimestamps) {
                    slot.tsc = core_.current_arrival_tsc;
                }
            });
            return true;
        } else {
            return rx_queue_.try_produce([&](RxMsg& msg) {
                std::memcpy(msg.data, data, len);
                msg.len = len;
                msg.opcode = opcode;
                if constexpr (kEnableTimestamps) {
                    msg.tsc = core_.current_arrival_tsc;
                }
            });
        }
    }

    /// Consume one message from the RX queue.
    template <typename F>
    bool rx_consume_(F&& visitor) {
        if constexpr (kRxEvicting) {
            return rx_queue_.try_consume_latest(std::forward<F>(visitor));
        } else {
            return rx_queue_.try_consume(std::forward<F>(visitor));
        }
    }

    /// Peek one message from the RX queue without consuming.
    template <typename F>
    bool rx_peek_(F&& visitor) {
        if constexpr (kRxEvicting) {
            return rx_queue_.try_peek_latest(std::forward<F>(visitor));
        } else {
            return rx_queue_.try_peek(std::forward<F>(visitor));
        }
    }

    /// Approximate RX queue size.
    [[nodiscard]] size_t rx_size_() const noexcept {
        if constexpr (kRxEvicting) {
            return rx_queue_.size_approx();
        } else {
            return rx_queue_.size();
        }
    }

    // -----------------------------------------------------------------------
    // Utility helpers
    // -----------------------------------------------------------------------

    /// Convert an HdrHistogram to RttStats.
    static RttStats histogram_to_stats_(const eph::utils::HdrHistogram& h) noexcept {
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

    /// Update a high-watermark atomically (relaxed CAS loop).
    static void update_hwm_(std::atomic<size_t>& hwm, size_t current) noexcept {
        size_t prev = hwm.load(std::memory_order_relaxed);
        while (current > prev &&
               !hwm.compare_exchange_weak(prev, current,
                   std::memory_order_relaxed, std::memory_order_relaxed)) {
            // prev updated by CAS failure -- retry
        }
    }

    // -----------------------------------------------------------------------
    // FrameProcessor types and factory
    // -----------------------------------------------------------------------

    /// DeliverPolicy: enqueues decoded frames into the RX queue.
    struct DeliverPolicy {
        RxWorker* self;
        void operator()(const uint8_t* data, uint16_t len, uint8_t opcode) const noexcept {
            bool ok = self->rx_enqueue_(data, len, opcode);
            if (!ok) {
                self->rx_stats_.dropped.fetch_add(1, std::memory_order_relaxed);
                SPDLOG_LOGGER_TRACE(detail::transport_logger(),
                    "RX enqueue failed: queue full (len={}, opcode={})",
                    len, opcode);
            }
            // Sample HWM every 64 enqueues
            if ((++self->rx_hwm_counter_ & 63) == 0) {
                update_hwm_(self->rx_hwm_, self->rx_size_());
            }
        }
    };

    /// SendFn wrapper: delegates to the callbacks_.send_response.
    struct SendFn {
        RxWorker* self;
        SendError operator()(const void* data, size_t len, uint8_t opcode) const noexcept {
            return self->callbacks_.send_response(data, len, opcode);
        }
    };

    using FrameProc = FrameProcessor<TcpImpl, Framer,
                                      DeliverPolicy, SendFn,
                                      MaxPayload, LastOnlyDeliver>;

    /// Create the FrameProcessor with all dependencies wired in.
    std::unique_ptr<FrameProc> make_frame_processor_() {
        return std::make_unique<FrameProc>(
            typename FrameProc::Deps{
                .core               = core_,
                .deliver            = DeliverPolicy{this},
                .send_response      = SendFn{this},
                .rx_stats           = rx_stats_,
                .ws_pings_received  = ws_pings_received_,
                .ws_pongs_sent      = ws_pongs_sent_,
                .rtt_histogram      = rtt_histogram_,
                .rx_latency_histogram = rx_latency_histogram_,
                .rx_decrypt_histogram = rx_decrypt_histogram_,
                .rx_decode_histogram  = rx_decode_histogram_,
                .rx_hwm             = rx_hwm_,
                .rx_hwm_counter     = rx_hwm_counter_,
            });
    }

    // -----------------------------------------------------------------------
    // RX loop (runs on dedicated thread)
    // -----------------------------------------------------------------------

    void rx_loop_() {
        (void)eph::utils::set_thread_affinity(core_.config.rx_cpu, "RX");
        [[maybe_unused]] auto log = detail::transport_logger();
        SPDLOG_LOGGER_DEBUG(log, "RX loop started");

        // Lazily construct the FrameProcessor on first start.
        if (!frame_processor_) {
            frame_processor_ = make_frame_processor_();
        }

        // Cache TSC conversion factor once -- avoids per-frame acquire
        // load on TSC::initialized_ inside the hot loop.
        if constexpr (kEnableTimestamps) {
            auto npc = eph::utils::TSC::get_ns_per_cycle();
            core_.ns_per_cycle = npc.value_or(0.0);
        }

        // Fixed-size RX buffers -- no heap allocation on hot path.
        // TLS reassembly: accumulates raw TCP bytes until complete TLS records form.
        // Sized for 4x max TLS record to handle burst TCP delivery under high load.
        static constexpr size_t kReassemblyBufSize =
            4 * (tls_const::kMaxRecordPayload + tls_record::kRecordHeaderLen +
                 tls_record::kAuthTagLen + 1);
        auto decrypt_buf = std::make_unique<uint8_t[]>(
            tls_const::kMaxRecordPayload + 256);
        auto reassembly_storage = std::make_unique<uint8_t[]>(kReassemblyBufSize);
        size_t reassembly_len = 0;

        // Frame reassembly: accumulates decrypted bytes when a framed message
        // spans multiple TLS records. Without this, partial frames at
        // TLS record boundaries would be silently discarded.
        static constexpr size_t kFrameReassemblyOverhead = kIsWebSocket
            ? ws::kMaxFrameHeaderLen : Framer::max_overhead();
        static constexpr size_t kWsReassemblyBufSize =
            kFrameReassemblyOverhead + MaxPayload + 256;
        auto ws_reassembly_storage = std::make_unique<uint8_t[]>(kWsReassemblyBufSize);
        size_t ws_reassembly_len = 0;

        while (core_.running.load(std::memory_order_acquire)) {
            // After a server Close frame, stop receiving -- TX will
            // drain the Close response and set running=false.
            if (core_.closing.load(std::memory_order_acquire)) [[unlikely]] {
                eph::utils::cpu_relax();
                continue;
            }

            // Application requested forced reconnect via reconnect_now()
            if (core_.force_reconnect.exchange(false, std::memory_order_acq_rel)) [[unlikely]] {
                SPDLOG_LOGGER_INFO(log, "Processing forced reconnect request");
                reassembly_len = 0;
                ws_reassembly_len = 0;
                frame_processor_->reset();
                if (!callbacks_.do_reconnect()) {
                    core_.running.store(false, std::memory_order_release);
                    break;
                }
                continue;
            }

            // -- Receive data via poll_rx --
            bool reconnect_needed = false;
            auto rx_result = core_.tcp->poll_rx(
                [&](const uint8_t* data, uint16_t len) {
                    if (core_.config.use_tls) {
                        // TLS mode: accumulate into TLS reassembly buffer
                        if (reassembly_len + len <= kReassemblyBufSize) {
                            std::memcpy(reassembly_storage.get() + reassembly_len,
                                        data, len);
                            reassembly_len += len;
                        } else {
                            SPDLOG_LOGGER_ERROR(log,
                                "RX reassembly buffer overflow ({} + {} > {}), "
                                "triggering reconnect",
                                reassembly_len, len, kReassemblyBufSize);
                            reassembly_len = 0;
                            ws_reassembly_len = 0;
                            reconnect_needed = true;
                        }
                    } else {
                        // Plain WS mode: accumulate into WS reassembly buffer
                        if (ws_reassembly_len + len <= kWsReassemblyBufSize) {
                            std::memcpy(ws_reassembly_storage.get() + ws_reassembly_len,
                                        data, len);
                            ws_reassembly_len += len;
                        } else {
                            SPDLOG_LOGGER_ERROR(log,
                                "WS reassembly buffer overflow ({} + {} > {}), "
                                "triggering reconnect",
                                ws_reassembly_len, len, kWsReassemblyBufSize);
                            ws_reassembly_len = 0;
                            reconnect_needed = true;
                        }
                    }
                });

            // Reassembly buffer overflow -> reconnect
            if (reconnect_needed) {
                frame_processor_->reset();
                if (!callbacks_.do_reconnect()) {
                    core_.running.store(false, std::memory_order_release);
                    break;
                }
                continue;
            }

            if (!rx_result) {
                SPDLOG_LOGGER_WARN(log, "TCP rx error: {}",
                                   rx_result.error());

                // Auto-reconnect (fixed interval, discard old messages)
                reassembly_len = 0;
                ws_reassembly_len = 0;
                frame_processor_->reset();
                if (callbacks_.do_reconnect()) {
                    continue; // Resume RX loop with new connection
                }

                // Reconnect exhausted -- stop transport
                core_.running.store(false, std::memory_order_release);
                break;
            }

            // No data received this poll iteration
            if (*rx_result == 0) continue;

            // Set arrival TSC to NIC-arrival time. Start from the
            // rx_burst TSC (captured right after recvmsg / rte_eth_rx_burst),
            // then back-date by kernel stack delay (SO_TIMESTAMPING) so
            // that all downstream consumers measure from the same baseline.
            if constexpr (kEnableTimestamps) {
                core_.current_arrival_tsc = core_.tcp->last_rx_burst_tsc();
                if constexpr (requires { core_.tcp->last_kernel_rx_delay_ns(); }) {
                    uint64_t delay_ns = core_.tcp->last_kernel_rx_delay_ns();
                    if (delay_ns > 0 && core_.ns_per_cycle > 0) {
                        uint64_t delay_cycles = static_cast<uint64_t>(
                            delay_ns / core_.ns_per_cycle);
                        if (delay_cycles >= core_.current_arrival_tsc) {
                            // Kernel delay larger than burst TSC -- stale/invalid.
                            core_.current_arrival_tsc = 0;
                        } else {
                            core_.current_arrival_tsc -= delay_cycles;
                        }
                    }
                }
            }

            // Plain mode: process framed data directly from TCP
            if (!core_.config.use_tls) {
                size_t ws_consumed = frame_processor_->process(
                    ws_reassembly_storage.get(), ws_reassembly_len);

                // Save unconsumed WS bytes for next TCP chunk
                ws_consumed = std::min(ws_consumed, ws_reassembly_len);
                size_t ws_remaining = ws_reassembly_len - ws_consumed;
                if (ws_remaining > 0 && ws_consumed > 0) {
                    std::memmove(ws_reassembly_storage.get(),
                                 ws_reassembly_storage.get() + ws_consumed,
                                 ws_remaining);
                }
                ws_reassembly_len = ws_remaining;
                if constexpr (requires { core_.tcp->flush_pending_ack(); }) {
                    core_.tcp->flush_pending_ack();
                }
                continue;
            }

            // Proactive warning at 90% of TLS read sequence limit,
            // symmetric with the TX thread's write sequence check.
            if (!rx_seq_warning_logged_) [[likely]] {
                uint64_t rseq = core_.crypto->read_seq();
                if (rseq >= tls_record::kSequenceWarnThreshold) [[unlikely]] {
                    SPDLOG_LOGGER_WARN(log,
                        "TLS read sequence at {}/{} (90%%), "
                        "reconnect imminent for key refresh",
                        rseq, tls_record::kMaxSequenceNumber);
                    rx_seq_warning_logged_ = true;
                }
            }

            // Decrypt complete TLS records from reassembly buffer
            size_t consumed = 0;
            while (reassembly_len - consumed >=
                   tls_record::kRecordHeaderLen + tls_record::kAuthTagLen) {
                const uint8_t* rec_ptr = reassembly_storage.get() + consumed;

                uint8_t content_type;
                uint16_t payload_len;
                if (!tls_record::parse_record_header(
                        rec_ptr, content_type, payload_len)) {
                    break;
                }

                size_t record_total = tls_record::kRecordHeaderLen + payload_len;
                if (reassembly_len - consumed < record_total) break;

                uint16_t decrypted_len;
                bool ok = core_.crypto->decrypt(
                    rec_ptr,
                    static_cast<uint16_t>(record_total),
                    decrypt_buf.get(), decrypted_len);

                if (!ok) {
                    rx_stats_.crypto_errors.fetch_add(1, std::memory_order_relaxed);
                    SPDLOG_LOGGER_WARN(log,
                        "TLS decrypt failed -- triggering reconnect");
                    // Corrupted record -> link unreliable, reconnect.
                    reassembly_len = 0;
                    ws_reassembly_len = 0;
                    consumed = 0;
                    frame_processor_->reset();
                    if (!callbacks_.do_reconnect()) {
                        core_.running.store(false, std::memory_order_release);
                    }
                    break; // Resume with fresh connection or exit outer loop
                }

                // Prepend any leftover WS bytes from the previous TLS record.
                const uint8_t* ws_data;
                size_t ws_data_len;
                if (ws_reassembly_len > 0) {
                    if (ws_reassembly_len + decrypted_len <= kWsReassemblyBufSize) {
                        std::memcpy(ws_reassembly_storage.get() + ws_reassembly_len,
                                    decrypt_buf.get(), decrypted_len);
                        ws_reassembly_len += decrypted_len;
                    } else {
                        SPDLOG_LOGGER_WARN(log,
                            "WS reassembly buffer overflow ({} + {} > {}), "
                            "discarding partial frame",
                            ws_reassembly_len, decrypted_len,
                            kWsReassemblyBufSize);
                        ws_reassembly_len = 0;
                        consumed += record_total;
                        continue;
                    }
                    ws_data = ws_reassembly_storage.get();
                    ws_data_len = ws_reassembly_len;
                } else {
                    ws_data = decrypt_buf.get();
                    ws_data_len = decrypted_len;
                }

                // Capture TSC after TLS decrypt, before WS decode.
                if constexpr (kEnableTimestamps) {
                    core_.current_decrypt_done_tsc = eph::utils::TSC::now();
                }

                size_t ws_consumed = frame_processor_->process(ws_data, ws_data_len);

                // Save unconsumed WS bytes for next TLS record
                ws_consumed = std::min(ws_consumed, ws_data_len);
                size_t ws_remaining = ws_data_len - ws_consumed;
                if (ws_remaining > 0) {
                    if (ws_data == decrypt_buf.get()) {
                        // First time: copy leftovers into WS reassembly buffer
                        std::memcpy(ws_reassembly_storage.get(),
                                    decrypt_buf.get() + ws_consumed, ws_remaining);
                    } else {
                        // Already in WS reassembly buffer: compact to front
                        std::memmove(ws_reassembly_storage.get(),
                                     ws_reassembly_storage.get() + ws_consumed,
                                     ws_remaining);
                    }
                    ws_reassembly_len = ws_remaining;
                } else {
                    ws_reassembly_len = 0;
                }

                consumed += record_total;
            }

            // After decrypt failure + failed reconnect, running is false.
            if (!core_.running.load(std::memory_order_acquire)) break;

            // Flush deferred ACK after TLS decrypt completes
            if constexpr (requires { core_.tcp->flush_pending_ack(); }) {
                core_.tcp->flush_pending_ack();
            }

            // Compact: move unconsumed data to front
            if (consumed > 0) {
                reassembly_len -= consumed;
                if (reassembly_len > 0) {
                    std::memmove(reassembly_storage.get(),
                                 reassembly_storage.get() + consumed,
                                 reassembly_len);
                }
            }
        }

        SPDLOG_LOGGER_DEBUG(log, "RX loop exited");
    }

    // -----------------------------------------------------------------------
    // Member data
    // -----------------------------------------------------------------------

    // Dependencies (injected)
    TransportCore<TcpImpl>& core_;
    std::atomic<uint64_t>& reconnect_count_;
    Callbacks callbacks_;

    // Owned state: queue
    RxQueue rx_queue_{};
    std::thread rx_thread_{};

    // Owned state: stats
    ThreadStats rx_stats_{};
    std::atomic<size_t> rx_hwm_{0};
    uint64_t rx_hwm_counter_{0};
    bool rx_seq_warning_logged_{false};

    // Owned state: histograms
    eph::utils::HdrHistogram rx_latency_histogram_{10, 1'000'000'000ULL, 3};
    eph::utils::HdrHistogram rx_decrypt_histogram_{10, 1'000'000'000ULL, 3};
    eph::utils::HdrHistogram rx_decode_histogram_{10, 1'000'000'000ULL, 3};
    eph::utils::HdrHistogram rtt_histogram_{10, 1'000'000'000ULL, 3};

    // Owned state: WS control frame counters
    std::atomic<uint64_t> ws_pings_received_{0};
    std::atomic<uint64_t> ws_pongs_sent_{0};

    // Owned state: FrameProcessor (lazily constructed in start/rx_loop)
    std::unique_ptr<FrameProc> frame_processor_;
};

} // namespace eph::net
