#pragma once

/// @file bounded_queue_bytes.hpp
/// @brief Byte-oriented bounded SPSC queue for variable-length messages.
///
/// Wraps BoundedQueue with a fixed-size `DataWrap` envelope that carries a
/// payload byte array, a length field, and an optional timestamp. Unlike the
/// evicting variant, writes **fail** (or spin-wait) when the queue is full,
/// guaranteeing no data loss.
///
/// Typical use: reliable streaming of binary frames between threads where
/// back-pressure is preferred over dropping stale data.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>

#include "eph/containers/bounded_queue.hpp"

namespace eph::containers {

// ---------------------------------------------------------------------------
// Standalone Stats type (enables std::formatter specialization)
// ---------------------------------------------------------------------------

/// @brief Point-in-time statistics snapshot for BoundedQueueBytes monitoring.
///
/// Obtain a snapshot via `BoundedQueueBytes::stats()`. Supports delta
/// computation (`operator-`) and throughput calculation.
struct BoundedQueueBytesStats {
    size_t total_pushed;   ///< Total messages ever pushed (monotonic)
    size_t total_popped;   ///< Total messages ever popped (monotonic)
    size_t current_size;   ///< Approximate current occupancy
    size_t capacity;       ///< Fixed capacity

    /// @brief Multi-line human-readable dump for logging/debugging.
    /// @return Formatted string with capacity, utilization, and counters.
    [[nodiscard]] std::string dump() const {
        double utilization = capacity > 0
            ? static_cast<double>(current_size) * 100.0 / static_cast<double>(capacity)
            : 0.0;
        return std::format(
            "BoundedQueueBytes::Stats:\n"
            "  capacity: {}\n"
            "  current_size: {} ({:.1f}% full)\n"
            "  total_pushed: {}\n"
            "  total_popped: {}",
            capacity, current_size, utilization,
            total_pushed, total_popped);
    }

    /// @brief JSON-formatted stats for monitoring system integration.
    /// @return Compact single-line JSON string.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"capacity\":{},\"current_size\":{},\"total_pushed\":{},\"total_popped\":{}}}",
            capacity, current_size, total_pushed, total_popped);
    }

    /// @brief Compute delta between two snapshots for interval-based monitoring.
    /// @param lhs Later snapshot (e.g. at time T2).
    /// @param rhs Earlier snapshot (e.g. at time T1).
    /// @return Per-field difference; `current_size` and `capacity` from @p lhs.
    [[nodiscard]] friend BoundedQueueBytesStats operator-(const BoundedQueueBytesStats& lhs,
                                                          const BoundedQueueBytesStats& rhs) noexcept {
        return BoundedQueueBytesStats{
            .total_pushed = lhs.total_pushed >= rhs.total_pushed ? lhs.total_pushed - rhs.total_pushed : 0,
            .total_popped = lhs.total_popped >= rhs.total_popped ? lhs.total_popped - rhs.total_popped : 0,
            .current_size = lhs.current_size,
            .capacity     = lhs.capacity,
        };
    }

    /// @brief Messages consumed per second over a measurement interval.
    ///
    /// Apply to a delta: `auto delta = t2 - t1; delta.throughput(elapsed_ns);`
    ///
    /// @param duration_ns Measurement interval in nanoseconds.
    /// @return Throughput in messages/second, or 0.0 if @p duration_ns is zero.
    [[nodiscard]] double throughput(uint64_t duration_ns) const noexcept {
        return duration_ns > 0
            ? static_cast<double>(total_popped) * 1e9 / static_cast<double>(duration_ns)
            : 0.0;
    }

    [[nodiscard]] friend bool operator==(const BoundedQueueBytesStats&,
                                          const BoundedQueueBytesStats&) = default;
};

/// @brief Byte-oriented bounded SPSC queue for variable-length messages.
///
/// Each message is stored in a fixed-size `DataWrap` envelope containing an
/// optional timestamp, a length, and a byte payload of up to @p MaxDataSize
/// bytes. The underlying BoundedQueue applies back-pressure: writes fail
/// (or spin-wait) when the queue is full -- no data is silently dropped.
///
/// @tparam MaxDataSize Maximum payload size in bytes per message (default 256).
///                     Must fit in `uint32_t`.
/// @tparam Capacity    Number of message slots (default 256). Must be a power of two.
template <size_t MaxDataSize = 256, size_t Capacity = 256>
class BoundedQueueBytes {
    static_assert(MaxDataSize <= UINT32_MAX,
                  "MaxDataSize must fit in uint32_t (used for len field)");

   public:
    /// @brief Fixed-size envelope wrapping a variable-length byte payload.
    struct DataWrap {
        uint64_t ts;                              ///< User-supplied timestamp (0 if unset).
        uint32_t len;                             ///< Actual payload length in bytes.
        std::array<uint8_t, MaxDataSize> data;    ///< Payload buffer (only [0, len) is valid).
    };

    BoundedQueueBytes() = default;
    ~BoundedQueueBytes() = default;

    // ===========================================================================
    // 非阻塞操作 (Writer)
    // ===========================================================================

    /// @brief Try to push a timestamped byte payload (non-blocking).
    /// @param payload Message bytes (must be <= MaxDataSize).
    /// @param ts      User-supplied timestamp.
    /// @return `true` on success; `false` if the queue is full or payload is oversized.
    [[nodiscard]] bool try_push_wts(std::span<const uint8_t> payload, uint64_t ts) noexcept {
        if (payload.size() > MaxDataSize) [[unlikely]] {
            return false;
        }

        return queue_.try_produce([&](DataWrap& slot) {
            slot.ts = ts;
            slot.len = static_cast<uint32_t>(payload.size());
            std::memcpy(slot.data.data(), payload.data(), payload.size());
        });
    }

    /// @brief Try to push a byte payload without timestamp (non-blocking, ts=0).
    /// @param payload Message bytes (must be <= MaxDataSize).
    /// @return `true` on success; `false` if full or oversized.
    [[nodiscard]] bool try_push(std::span<const uint8_t> payload) noexcept {
        return try_push_wts(payload, 0);
    }

    // ===========================================================================
    // 批量非阻塞操作 (Writer)
    // ===========================================================================

    /// @brief Batch push timestamped payloads (all-or-nothing, non-blocking).
    ///
    /// @param payloads   Array of payload spans (one per message).
    /// @param timestamps Array of timestamps (parallel to @p payloads).
    /// @param count      Number of messages.
    /// @return `true` if all enqueued; `false` if insufficient space or any
    ///         payload exceeds MaxDataSize (nothing written).
    [[nodiscard]] bool try_push_n_wts(const std::span<const uint8_t>* payloads,
                                       const uint64_t* timestamps,
                                       size_t count) noexcept {
        for (size_t i = 0; i < count; ++i) {
            if (payloads[i].size() > MaxDataSize) [[unlikely]] return false;
        }
        return queue_.try_produce_n(count, [&](DataWrap& slot, size_t i) {
            slot.ts = timestamps[i];
            slot.len = static_cast<uint32_t>(payloads[i].size());
            std::memcpy(slot.data.data(), payloads[i].data(), payloads[i].size());
        });
    }

    /// @brief Batch push payloads without timestamps (all-or-nothing, ts=0).
    /// @param payloads Array of payload spans.
    /// @param count    Number of messages.
    /// @return `true` if all enqueued; `false` if insufficient space or oversized.
    [[nodiscard]] bool try_push_n(const std::span<const uint8_t>* payloads,
                                   size_t count) noexcept {
        for (size_t i = 0; i < count; ++i) {
            if (payloads[i].size() > MaxDataSize) [[unlikely]] return false;
        }
        return queue_.try_produce_n(count, [&](DataWrap& slot, size_t i) {
            slot.ts = 0;
            slot.len = static_cast<uint32_t>(payloads[i].size());
            std::memcpy(slot.data.data(), payloads[i].data(), payloads[i].size());
        });
    }

    // ===========================================================================
    // 批量非阻塞操作 (Reader)
    // ===========================================================================

    /// @brief Batch zero-copy consume (best-effort, non-blocking).
    ///
    /// Consumes up to N messages, invoking `visitor(data_span, index)` for each.
    ///
    /// @tparam F Callable with signature `void(std::span<const uint8_t>, size_t index)`.
    /// @param n       Maximum number of messages to consume.
    /// @param visitor Callback for each message.
    /// @return Number of messages actually consumed.
    template <typename F>
        requires std::invocable<F, std::span<const uint8_t>, size_t>
    [[nodiscard]] size_t try_consume_n(size_t n, F&& visitor) noexcept {
        return queue_.try_consume_n(n, [&](const DataWrap& msg, size_t idx) {
            uint32_t safe_len = std::min(msg.len, static_cast<uint32_t>(MaxDataSize));
            std::invoke(std::forward<F>(visitor),
                        std::span<const uint8_t>{msg.data.data(), safe_len}, idx);
        });
    }

    /// @brief Batch zero-copy consume with timestamps (best-effort).
    ///
    /// @tparam F Callable with signature `void(std::span<const uint8_t>, uint64_t ts, size_t index)`.
    /// @param n       Maximum number of messages.
    /// @param visitor Callback for each message.
    /// @return Number of messages actually consumed.
    template <typename F>
        requires std::invocable<F, std::span<const uint8_t>, uint64_t, size_t>
    [[nodiscard]] size_t try_consume_n_wts(size_t n, F&& visitor) noexcept {
        return queue_.try_consume_n(n, [&](const DataWrap& msg, size_t idx) {
            uint32_t safe_len = std::min(msg.len, static_cast<uint32_t>(MaxDataSize));
            std::invoke(std::forward<F>(visitor),
                        std::span<const uint8_t>{msg.data.data(), safe_len},
                        msg.ts, idx);
        });
    }

    /// @brief Best-effort drain: consume all queued messages visible at call time.
    ///
    /// Equivalent to `try_consume_n(Capacity, visitor)`. Useful for shutdown
    /// drain or periodic batch processing.
    ///
    /// @tparam F Callable with signature `void(std::span<const uint8_t>, size_t index)`.
    /// @param visitor Callback for each message.
    /// @return Number of messages consumed.
    /// @note Does **not** guarantee the queue is empty afterward if the
    ///       producer is concurrently active.
    template <typename F>
        requires std::invocable<F, std::span<const uint8_t>, size_t>
    [[nodiscard]] size_t try_consume_all(F&& visitor) noexcept {
        return try_consume_n(Capacity, std::forward<F>(visitor));
    }

    /// @brief Best-effort drain with timestamps.
    ///
    /// Equivalent to `try_consume_n_wts(Capacity, visitor)`.
    ///
    /// @tparam F Callable with signature `void(std::span<const uint8_t>, uint64_t ts, size_t index)`.
    /// @param visitor Callback for each message.
    /// @return Number of messages consumed.
    template <typename F>
        requires std::invocable<F, std::span<const uint8_t>, uint64_t, size_t>
    [[nodiscard]] size_t try_consume_all_wts(F&& visitor) noexcept {
        return try_consume_n_wts(Capacity, std::forward<F>(visitor));
    }

    // ===========================================================================
    // 非阻塞 Peek 操作 (Reader)
    // ===========================================================================

    /// @brief Peek at the head message without consuming (reader-side only).
    ///
    /// Copies the head message into @p out_buf but does **not** advance the
    /// head pointer. Useful for pre-inspecting message type before consuming.
    ///
    /// @param out_buf Pre-allocated output buffer.
    /// @return Actual bytes copied on success; `std::nullopt` if empty.
    /// @note Multiple calls return the same message until `try_pop`/`try_consume`
    ///       advances the head.
    [[nodiscard]] std::optional<uint32_t> try_peek(std::span<uint8_t> out_buf) noexcept {
        uint32_t copy_len = 0;
        bool success = try_peek_visit([&](std::span<const uint8_t> data) {
            copy_len = static_cast<uint32_t>(std::min({data.size(), out_buf.size(), size_t(UINT32_MAX)}));
            std::memcpy(out_buf.data(), data.data(), copy_len);
        });
        return success ? std::make_optional(copy_len) : std::nullopt;
    }

    /// @brief Peek at the head message with timestamp (non-consuming).
    /// @param out_buf Pre-allocated output buffer.
    /// @param out_ts  [out] Timestamp of the message.
    /// @return Actual bytes copied on success; `std::nullopt` if empty.
    [[nodiscard]] std::optional<uint32_t> try_peek_wts(std::span<uint8_t> out_buf,
                                                        uint64_t& out_ts) noexcept {
        uint32_t copy_len = 0;
        bool success = try_peek_visit_wts([&](std::span<const uint8_t> data, uint64_t ts) {
            copy_len = static_cast<uint32_t>(std::min({data.size(), out_buf.size(), size_t(UINT32_MAX)}));
            std::memcpy(out_buf.data(), data.data(), copy_len);
            out_ts = ts;
        });
        return success ? std::make_optional(copy_len) : std::nullopt;
    }

    /// @brief Zero-copy peek via visitor (non-consuming, reader-side only).
    /// @tparam F Callable with signature `void(std::span<const uint8_t>)`.
    /// @param visitor Callback receiving a span over the payload bytes.
    /// @return `true` if the queue is non-empty; `false` if empty.
    template <typename F>
        requires std::invocable<F, std::span<const uint8_t>>
    [[nodiscard]] bool try_peek_visit(F&& visitor) noexcept {
        return queue_.try_peek([&](const DataWrap& msg) {
            uint32_t safe_len = std::min(msg.len, static_cast<uint32_t>(MaxDataSize));
            std::invoke(std::forward<F>(visitor),
                        std::span<const uint8_t>{msg.data.data(), safe_len});
        });
    }

    /// @brief Zero-copy peek with timestamp via visitor (non-consuming).
    /// @tparam F Callable with signature `void(std::span<const uint8_t>, uint64_t ts)`.
    /// @param visitor Callback receiving payload span and timestamp.
    /// @return `true` if non-empty; `false` if empty.
    template <typename F>
        requires std::invocable<F, std::span<const uint8_t>, uint64_t>
    [[nodiscard]] bool try_peek_visit_wts(F&& visitor) noexcept {
        return queue_.try_peek([&](const DataWrap& msg) {
            uint32_t safe_len = std::min(msg.len, static_cast<uint32_t>(MaxDataSize));
            std::invoke(std::forward<F>(visitor),
                        std::span<const uint8_t>{msg.data.data(), safe_len},
                        msg.ts);
        });
    }

    // ===========================================================================
    // 阻塞操作 (Writer)
    // ===========================================================================

    /// @brief Blocking push with timestamp (spins until space is available).
    /// @param payload Message bytes (must be <= MaxDataSize).
    /// @param ts      User-supplied timestamp.
    /// @return `true` after successful enqueue; `false` only if payload exceeds MaxDataSize.
    [[nodiscard]] bool push_wts(std::span<const uint8_t> payload, uint64_t ts) noexcept {
        if (payload.size() > MaxDataSize) [[unlikely]] {
            return false;
        }

        queue_.produce([&](DataWrap& slot) {
            slot.ts = ts;
            slot.len = static_cast<uint32_t>(payload.size());
            std::memcpy(slot.data.data(), payload.data(), payload.size());
        });
        
        return true;
    }

    /// @brief Blocking push without timestamp (spins, ts=0).
    /// @param payload Message bytes (must be <= MaxDataSize).
    /// @return `true` on success; `false` if payload exceeds MaxDataSize.
    [[nodiscard]] bool push(std::span<const uint8_t> payload) noexcept {
        return push_wts(payload, 0);
    }

    // ===========================================================================
    // 非阻塞操作 (Reader)
    // ===========================================================================

    /// @brief Try to pop the head message into a caller-supplied buffer (non-blocking).
    /// @param out_buf Pre-allocated output buffer.
    /// @return Actual bytes copied on success; `std::nullopt` if empty.
    [[nodiscard]] std::optional<uint32_t> try_pop(std::span<uint8_t> out_buf) noexcept {
        uint32_t copy_len = 0;
        bool success = try_consume([&](std::span<const uint8_t> data) {
            copy_len =
                static_cast<uint32_t>(std::min({data.size(), out_buf.size(), size_t(UINT32_MAX)}));
            std::memcpy(out_buf.data(), data.data(), copy_len);
        });

        return success ? std::make_optional(copy_len) : std::nullopt;
    }

    /// @brief Try to pop with timestamp into a caller-supplied buffer (non-blocking).
    /// @param out_buf Pre-allocated output buffer.
    /// @param out_ts  [out] Timestamp of the message.
    /// @return Actual bytes copied on success; `std::nullopt` if empty.
    [[nodiscard]] std::optional<uint32_t> try_pop_wts(std::span<uint8_t> out_buf,
                                        uint64_t& out_ts) noexcept {
        uint32_t copy_len = 0;
        bool success =
            try_consume_wts([&](std::span<const uint8_t> data, uint64_t ts) {
                copy_len = static_cast<uint32_t>(
                    std::min({data.size(), out_buf.size(), size_t(UINT32_MAX)}));
                std::memcpy(out_buf.data(), data.data(), copy_len);
                out_ts = ts;
            });

        return success ? std::make_optional(copy_len) : std::nullopt;
    }

    /// @brief Try to consume the head message via zero-copy visitor (non-blocking).
    /// @tparam F Callable with signature `void(std::span<const uint8_t>)`.
    /// @param visitor Callback receiving payload span.
    /// @return `true` on success; `false` if empty.
    template <typename F>
        requires std::invocable<F, std::span<const uint8_t>>
    [[nodiscard]] bool try_consume(F&& visitor) noexcept {
        return queue_.try_consume([&](const DataWrap& msg) {
            uint32_t safe_len =
                std::min(msg.len, static_cast<uint32_t>(MaxDataSize));
            std::invoke(std::forward<F>(visitor),
                        std::span<const uint8_t>{msg.data.data(), safe_len});
        });
    }

    /// @brief Try to consume the head message with timestamp (zero-copy, non-blocking).
    /// @tparam F Callable with signature `void(std::span<const uint8_t>, uint64_t ts)`.
    /// @param visitor Callback receiving payload span and timestamp.
    /// @return `true` on success; `false` if empty.
    /// @note No discard count is provided (bounded queue does not drop data).
    template <typename F>
        requires std::invocable<F, std::span<const uint8_t>, uint64_t>
    [[nodiscard]] bool try_consume_wts(F&& visitor) noexcept {
        return queue_.try_consume([&](const DataWrap& msg) {
            uint32_t safe_len =
                std::min(msg.len, static_cast<uint32_t>(MaxDataSize));
            std::invoke(std::forward<F>(visitor),
                        std::span<const uint8_t>{msg.data.data(), safe_len},
                        msg.ts);
        });
    }

    // ===========================================================================
    // 阻塞操作 (Reader)
    // ===========================================================================

    /// @brief Blocking pop into a caller-supplied buffer (spins until data).
    /// @param out_buf Pre-allocated output buffer.
    /// @return Actual bytes copied.
    uint32_t pop(std::span<uint8_t> out_buf) noexcept {
        uint32_t copy_len = 0;
        consume([&](std::span<const uint8_t> data) {
            copy_len = static_cast<uint32_t>(std::min({data.size(), out_buf.size(), size_t(UINT32_MAX)}));
            std::memcpy(out_buf.data(), data.data(), copy_len);
        });
        return copy_len;
    }

    /// @brief Blocking pop with timestamp (spins until data).
    /// @param out_buf Pre-allocated output buffer.
    /// @param out_ts  [out] Timestamp of the message.
    /// @return Actual bytes copied.
    uint32_t pop_wts(std::span<uint8_t> out_buf, uint64_t& out_ts) noexcept {
        uint32_t copy_len = 0;
        consume_wts([&](std::span<const uint8_t> data, uint64_t ts) {
            copy_len = static_cast<uint32_t>(std::min({data.size(), out_buf.size(), size_t(UINT32_MAX)}));
            std::memcpy(out_buf.data(), data.data(), copy_len);
            out_ts = ts;
        });
        return copy_len;
    }

    /// @brief Blocking zero-copy consume (spins until data).
    /// @tparam F Callable with signature `void(std::span<const uint8_t>)`.
    /// @param visitor Callback receiving payload span.
    template <typename F>
        requires std::invocable<F, std::span<const uint8_t>>
    void consume(F&& visitor) noexcept {
        queue_.consume([&](const DataWrap& msg) {
            uint32_t safe_len =
                std::min(msg.len, static_cast<uint32_t>(MaxDataSize));
            std::invoke(std::forward<F>(visitor),
                        std::span<const uint8_t>{msg.data.data(), safe_len});
        });
    }

    /// @brief Blocking zero-copy consume with timestamp (spins until data).
    /// @tparam F Callable with signature `void(std::span<const uint8_t>, uint64_t ts)`.
    /// @param visitor Callback receiving payload span and timestamp.
    template <typename F>
        requires std::invocable<F, std::span<const uint8_t>, uint64_t>
    void consume_wts(F&& visitor) noexcept {
        queue_.consume([&](const DataWrap& msg) {
            uint32_t safe_len =
                std::min(msg.len, static_cast<uint32_t>(MaxDataSize));
            std::invoke(std::forward<F>(visitor),
                        std::span<const uint8_t>{msg.data.data(), safe_len},
                        msg.ts);
        });
    }

    // ===========================================================================
    // 带超时操作 (Writer)
    // ===========================================================================

    /// @brief Push with timestamp and timeout.
    /// @param payload Message bytes (must be <= MaxDataSize).
    /// @param ts      User-supplied timestamp.
    /// @param timeout Maximum wall-clock wait.
    /// @return `true` on success; `false` on timeout or oversized payload.
    template <typename Rep, typename Period>
    [[nodiscard]] bool try_push_wts_for(std::span<const uint8_t> payload, uint64_t ts,
                                        std::chrono::duration<Rep, Period> timeout) noexcept {
        if (payload.size() > MaxDataSize) [[unlikely]] return false;

        return queue_.try_produce_for([&](DataWrap& slot) {
            slot.ts = ts;
            slot.len = static_cast<uint32_t>(payload.size());
            std::memcpy(slot.data.data(), payload.data(), payload.size());
        }, timeout);
    }

    /// @brief Push with timeout (ts=0).
    /// @param payload Message bytes.
    /// @param timeout Maximum wall-clock wait.
    /// @return `true` on success; `false` on timeout or oversized payload.
    template <typename Rep, typename Period>
    [[nodiscard]] bool try_push_for(std::span<const uint8_t> payload,
                                    std::chrono::duration<Rep, Period> timeout) noexcept {
        return try_push_wts_for(payload, 0, timeout);
    }

    // ===========================================================================
    // 带超时操作 (Reader)
    // ===========================================================================

    /// @brief Zero-copy consume with timeout.
    /// @tparam F Callable with signature `void(std::span<const uint8_t>)`.
    /// @tparam Rep, Period `std::chrono::duration` parameters.
    /// @param visitor Callback on success.
    /// @param timeout Maximum wall-clock wait.
    /// @return `true` on success; `false` on timeout.
    template <typename F, typename Rep, typename Period>
        requires std::invocable<F, std::span<const uint8_t>>
    [[nodiscard]] bool try_consume_for(F&& visitor,
                                       std::chrono::duration<Rep, Period> timeout) noexcept {
        return queue_.try_consume_for([&](const DataWrap& msg) {
            uint32_t safe_len =
                std::min(msg.len, static_cast<uint32_t>(MaxDataSize));
            std::invoke(std::forward<F>(visitor),
                        std::span<const uint8_t>{msg.data.data(), safe_len});
        }, timeout);
    }

    /// @brief Zero-copy consume with timestamp and timeout.
    /// @tparam F Callable with signature `void(std::span<const uint8_t>, uint64_t ts)`.
    /// @tparam Rep, Period `std::chrono::duration` parameters.
    /// @param visitor Callback on success.
    /// @param timeout Maximum wall-clock wait.
    /// @return `true` on success; `false` on timeout.
    template <typename F, typename Rep, typename Period>
        requires std::invocable<F, std::span<const uint8_t>, uint64_t>
    [[nodiscard]] bool try_consume_wts_for(F&& visitor,
                                           std::chrono::duration<Rep, Period> timeout) noexcept {
        return queue_.try_consume_for([&](const DataWrap& msg) {
            uint32_t safe_len =
                std::min(msg.len, static_cast<uint32_t>(MaxDataSize));
            std::invoke(std::forward<F>(visitor),
                        std::span<const uint8_t>{msg.data.data(), safe_len},
                        msg.ts);
        }, timeout);
    }

    /// @brief Pop with timeout into a caller-supplied buffer.
    /// @param out_buf Pre-allocated output buffer.
    /// @param timeout Maximum wall-clock wait.
    /// @return Actual bytes copied on success; `std::nullopt` on timeout.
    template <typename Rep, typename Period>
    [[nodiscard]] std::optional<uint32_t> try_pop_for(
        std::span<uint8_t> out_buf,
        std::chrono::duration<Rep, Period> timeout) noexcept {
        uint32_t copy_len = 0;
        bool ok = try_consume_for([&](std::span<const uint8_t> data) {
            copy_len = static_cast<uint32_t>(std::min({data.size(), out_buf.size(), size_t(UINT32_MAX)}));
            std::memcpy(out_buf.data(), data.data(), copy_len);
        }, timeout);
        return ok ? std::make_optional(copy_len) : std::nullopt;
    }

    /// @brief Pop with timestamp and timeout into a buffer.
    /// @param out_buf Pre-allocated output buffer.
    /// @param out_ts  [out] Timestamp.
    /// @param timeout Maximum wall-clock wait.
    /// @return Actual bytes copied on success; `std::nullopt` on timeout.
    template <typename Rep, typename Period>
    [[nodiscard]] std::optional<uint32_t> try_pop_wts_for(
        std::span<uint8_t> out_buf, uint64_t& out_ts,
        std::chrono::duration<Rep, Period> timeout) noexcept {
        uint32_t copy_len = 0;
        bool ok = try_consume_wts_for(
            [&](std::span<const uint8_t> data, uint64_t ts) {
                copy_len = static_cast<uint32_t>(std::min({data.size(), out_buf.size(), size_t(UINT32_MAX)}));
                std::memcpy(out_buf.data(), data.data(), copy_len);
                out_ts = ts;
            }, timeout);
        return ok ? std::make_optional(copy_len) : std::nullopt;
    }

    // ===========================================================================
    // 状态查询
    // ===========================================================================

    /// @brief Discard all queued data, resetting the queue to empty.
    /// @warning Not thread-safe. Call only when no concurrent readers/writers.
    void clear() noexcept { queue_.clear(); }

    /// @brief Approximate number of queued messages.
    [[nodiscard]] size_t size() const noexcept { return queue_.size(); }

    /// @brief Check if the queue is empty (approximate).
    [[nodiscard]] bool empty() const noexcept { return queue_.empty(); }

    /// @brief Check if the queue is full (approximate).
    [[nodiscard]] bool full() const noexcept { return queue_.full(); }

    /// @brief Return the fixed queue capacity.
    [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }

    /// @brief Estimated remaining write capacity.
    [[nodiscard]] size_t available_write() const noexcept { return queue_.available_write(); }

    /// @brief Estimated number of messages available to read.
    [[nodiscard]] size_t available_read() const noexcept { return queue_.available_read(); }

    // ===========================================================================
    // 可观测性
    // ===========================================================================

    /// @brief Alias for the standalone BoundedQueueBytesStats type.
    using Stats = BoundedQueueBytesStats;

    /// @brief Take a point-in-time statistics snapshot.
    /// @return BoundedQueueBytesStats with current counter values.
    [[nodiscard]] Stats stats() const noexcept {
        auto s = queue_.stats();
        return Stats{
            .total_pushed = s.total_pushed,
            .total_popped = s.total_popped,
            .current_size = s.current_size,
            .capacity     = s.capacity,
        };
    }

   private:
    BoundedQueue<DataWrap, Capacity> queue_;
};

}  // namespace eph::containers

/// @brief std::formatter specialization for BoundedQueueBytesStats.
///
/// Delegates to BoundedQueueBytesStats::dump() for use with `std::format`/`std::print`.
template <>
struct std::formatter<eph::containers::BoundedQueueBytesStats>
    : std::formatter<std::string> {
    auto format(const eph::containers::BoundedQueueBytesStats& s,
                std::format_context& ctx) const {
        return std::formatter<std::string>::format(s.dump(), ctx);
    }
};
