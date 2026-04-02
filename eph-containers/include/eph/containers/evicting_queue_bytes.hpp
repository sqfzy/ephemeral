#pragma once

/// @file evicting_queue_bytes.hpp
/// @brief Byte-oriented evicting SPSC queue for variable-length messages.
///
/// Wraps EvictingQueue with a fixed-size `DataWrap` envelope that carries a
/// payload byte array, a length field, a monotonic message ID, and an
/// optional timestamp. The writer is **wait-free** (overwrites old messages
/// when full); the reader is **lock-free** (SeqLock-based optimistic read).
///
/// Typical use: streaming binary market-data frames between a network
/// thread and a strategy thread where dropping stale data is acceptable.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>

#include "eph/containers/evicting_queue.hpp"

namespace eph::containers {

// ---------------------------------------------------------------------------
// Standalone Stats type (enables std::formatter specialization)
// ---------------------------------------------------------------------------

/// @brief Point-in-time statistics snapshot for EvictingQueueBytes monitoring.
///
/// Obtain a snapshot via `EvictingQueueBytes::stats()`. Supports delta
/// computation (`operator-`), throughput calculation, and loss-rate queries.
struct EvictingQueueBytesStats {
    uint64_t total_pushed;     ///< Total messages ever pushed (writer-side)
    uint64_t last_pop_id;      ///< ID of last consumed message (reader-side)
    uint64_t total_popped;     ///< Total messages actually consumed (reader-side)
    size_t   current_size;     ///< Approximate unread entries
    size_t   capacity;         ///< Fixed capacity
    uint64_t total_overwritten; ///< Messages overwritten before being read

    /// @brief Multi-line human-readable dump for logging/debugging.
    /// @return Formatted string with all counter values.
    [[nodiscard]] std::string dump() const {
        return std::format(
            "EvictingQueueBytes::Stats:\n"
            "  capacity: {}\n"
            "  current_size: {}\n"
            "  total_pushed: {}\n"
            "  last_pop_id: {}\n"
            "  total_popped: {}\n"
            "  total_overwritten: {}",
            capacity, current_size,
            total_pushed, last_pop_id, total_popped, total_overwritten);
    }

    /// @brief JSON-formatted stats for monitoring system integration.
    /// @return Compact single-line JSON string.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"capacity\":{},\"current_size\":{},\"total_pushed\":{},"
            "\"last_pop_id\":{},\"total_popped\":{},\"total_overwritten\":{}}}",
            capacity, current_size, total_pushed,
            last_pop_id, total_popped, total_overwritten);
    }

    /// @brief Compute delta between two snapshots for interval-based monitoring.
    /// @param lhs Later snapshot (e.g. at time T2).
    /// @param rhs Earlier snapshot (e.g. at time T1).
    /// @return Per-field difference; `current_size` and `capacity` are taken from @p lhs.
    [[nodiscard]] friend EvictingQueueBytesStats operator-(const EvictingQueueBytesStats& lhs,
                                                           const EvictingQueueBytesStats& rhs) noexcept {
        return EvictingQueueBytesStats{
            .total_pushed      = lhs.total_pushed - rhs.total_pushed,
            .last_pop_id       = lhs.last_pop_id - rhs.last_pop_id,
            .total_popped      = lhs.total_popped - rhs.total_popped,
            .current_size      = lhs.current_size,
            .capacity          = lhs.capacity,
            .total_overwritten = lhs.total_overwritten - rhs.total_overwritten,
        };
    }

    /// @brief Messages consumed per second over a measurement interval.
    ///
    /// Apply to a delta snapshot: `auto delta = t2 - t1; delta.throughput(elapsed_ns);`
    ///
    /// @param duration_ns Measurement interval in nanoseconds.
    /// @return Throughput in messages/second, or 0.0 if @p duration_ns is zero.
    [[nodiscard]] double throughput(uint64_t duration_ns) const noexcept {
        return duration_ns > 0
            ? static_cast<double>(total_popped) * 1e9 / static_cast<double>(duration_ns)
            : 0.0;
    }

    /// @brief Data loss rate as a fraction in [0.0, 1.0].
    /// @return Clamped ratio of overwritten to total_pushed.
    [[nodiscard]] double loss_rate() const noexcept {
        return total_pushed > 0
            ? std::clamp(static_cast<double>(total_overwritten) / static_cast<double>(total_pushed), 0.0, 1.0)
            : 0.0;
    }

    [[nodiscard]] friend bool operator==(const EvictingQueueBytesStats&,
                                          const EvictingQueueBytesStats&) = default;
};

/// @brief Byte-oriented evicting SPSC queue for variable-length messages.
///
/// Each message is stored in a fixed-size `DataWrap` envelope containing a
/// monotonic ID, an optional timestamp, a length, and a byte payload of up
/// to @p MaxDataSize bytes.  The underlying EvictingQueue provides wait-free
/// writes (overwrites old data when full) and lock-free reads.
///
/// @tparam MaxDataSize Maximum payload size in bytes per message (default 256).
///                     Must fit in `uint32_t`.
/// @tparam Capacity    Number of message slots (default 256). Must be a power of two.
template <size_t MaxDataSize = 256, size_t Capacity = 256>
class EvictingQueueBytes {
    static_assert(MaxDataSize <= UINT32_MAX,
                  "MaxDataSize must fit in uint32_t (used for len field)");

   public:
    /// @brief Fixed-size envelope wrapping a variable-length byte payload.
    struct DataWrap {
        uint64_t id;                              ///< Monotonic 1-based message ID.
        uint64_t ts;                              ///< User-supplied timestamp (0 if unset).
        uint32_t len;                             ///< Actual payload length in bytes.
        std::array<uint8_t, MaxDataSize> data;    ///< Payload buffer (only [0, len) is valid).
    };

    EvictingQueueBytes() = default;
    ~EvictingQueueBytes() = default;

    // ===========================================================================
    // Writer (Wait-free)
    // ===========================================================================

    /// @brief Try to push a timestamped byte payload (wait-free).
    ///
    /// @param payload Message bytes (must be <= MaxDataSize).
    /// @param ts      User-supplied timestamp (e.g. exchange timestamp).
    /// @return `true` on success; `false` only if `payload.size() > MaxDataSize`.
    /// @note Unlike BoundedQueueBytes::try_push_wts, this method only returns
    ///       false when payload exceeds MaxDataSize. The underlying EvictingQueue
    ///       is wait-free and never blocks on queue fullness -- old data is overwritten.
    [[nodiscard]] bool try_push_wts(std::span<const uint8_t> payload, uint64_t ts) noexcept {
        if (payload.size() > MaxDataSize) [[unlikely]] {
            return false;
        }

        queue_.produce([&](DataWrap& slot) {
            slot.id = push_count_.fetch_add(1, std::memory_order_relaxed) + 1;
            slot.ts = ts;
            slot.len = static_cast<uint32_t>(payload.size());
            std::memcpy(slot.data.data(), payload.data(), payload.size());
        });

        return true;
    }

    /// @brief Try to push a byte payload without timestamp (wait-free, ts=0).
    /// @param payload Message bytes (must be <= MaxDataSize).
    /// @return `true` on success; `false` if payload exceeds MaxDataSize.
    [[nodiscard]] bool try_push(std::span<const uint8_t> payload) noexcept {
        return try_push_wts(payload, 0);
    }

    /// @brief Batch push timestamped byte payloads (wait-free).
    ///
    /// All payloads must be <= MaxDataSize; if any exceeds the limit, none
    /// are written. The underlying EvictingQueue is wait-free -- old data
    /// may be overwritten.
    ///
    /// @param payloads   Array of payload spans (one per message).
    /// @param timestamps Array of timestamps (parallel to @p payloads).
    /// @param count      Number of messages.
    /// @return `true` if all were written; `false` if any payload was oversized.
    [[nodiscard]] bool push_n_wts(const std::span<const uint8_t>* payloads,
                                   const uint64_t* timestamps,
                                   size_t count) noexcept {
        for (size_t i = 0; i < count; ++i) {
            if (payloads[i].size() > MaxDataSize) [[unlikely]] return false;
        }
        queue_.produce_n(count, [&](DataWrap& slot, size_t i) {
            slot.id = push_count_.fetch_add(1, std::memory_order_release) + 1;
            slot.ts = timestamps[i];
            slot.len = static_cast<uint32_t>(payloads[i].size());
            std::memcpy(slot.data.data(), payloads[i].data(), payloads[i].size());
        });
        return true;
    }

    /// @brief Batch push byte payloads without timestamps (wait-free, ts=0).
    /// @param payloads Array of payload spans.
    /// @param count    Number of messages.
    /// @return `true` if all were written; `false` if any payload was oversized.
    [[nodiscard]] bool push_n(const std::span<const uint8_t>* payloads,
                               size_t count) noexcept {
        for (size_t i = 0; i < count; ++i) {
            if (payloads[i].size() > MaxDataSize) [[unlikely]] return false;
        }
        queue_.produce_n(count, [&](DataWrap& slot, size_t i) {
            slot.id = push_count_.fetch_add(1, std::memory_order_release) + 1;
            slot.ts = 0;
            slot.len = static_cast<uint32_t>(payloads[i].size());
            std::memcpy(slot.data.data(), payloads[i].data(), payloads[i].size());
        });
        return true;
    }

    // ===========================================================================
    // 非阻塞 Reader (Lock-free)
    // ===========================================================================

    /// @brief Try to pop the latest message into a caller-supplied buffer (non-blocking).
    /// @param out_buf Pre-allocated output buffer.
    /// @return Actual bytes copied on success; `std::nullopt` if no new data.
    [[nodiscard]] std::optional<uint32_t> try_pop_latest(
        std::span<uint8_t> out_buf) noexcept {
        uint64_t read_id = 0;
        uint32_t copy_len = 0;

        bool success = queue_.try_consume_latest([&](const auto& msg) {
            read_id = msg.id;

            // 防越界：取 msg.len, MaxDataSize, out_buf.size() 的最小值
            // 防止 SeqLock 脏读期间 msg.len 乱码导致踩内存
            copy_len = std::min({msg.len, static_cast<uint32_t>(MaxDataSize),
                                 static_cast<uint32_t>(out_buf.size())});

            std::memcpy(out_buf.data(), msg.data.data(), copy_len);
        });

        if (success) {
            last_pop_id_.store(read_id, std::memory_order_relaxed);
            return copy_len;
        }

        return std::nullopt;
    }

    /// @brief Try to pop the latest message with timestamp and discard count (non-blocking).
    /// @param out_buf       Pre-allocated output buffer.
    /// @param out_ts        [out] Timestamp of the message.
    /// @param out_discarded [out] Number of messages skipped since the last read.
    /// @return Actual bytes copied on success; `std::nullopt` if no new data.
    [[nodiscard]] std::optional<uint32_t> try_pop_latest_wts(std::span<uint8_t> out_buf,
                                               uint64_t& out_ts, 
                                               uint32_t& out_discarded) noexcept {
        uint32_t copy_len = 0;

        bool success = try_consume_latest_wts([&](std::span<const uint8_t> data, 
                                                  uint64_t ts, 
                                                  uint32_t discarded) {
            copy_len = static_cast<uint32_t>(std::min(data.size(), out_buf.size()));
            std::memcpy(out_buf.data(), data.data(), copy_len);
            out_ts = ts;
            out_discarded = discarded;
        });

        return success ? std::make_optional(copy_len) : std::nullopt;
    }

    /// @brief Try to consume the latest message via zero-copy visitor (non-blocking).
    ///
    /// @tparam F Callable with signature `void(std::span<const uint8_t>)`.
    /// @param visitor Callback receiving a span over the payload bytes.
    /// @return `true` if the visitor was called with consistent data; `false` otherwise.
    template <typename F>
        requires std::invocable<F, std::span<const uint8_t>>
    [[nodiscard]] bool try_consume_latest(F&& visitor) noexcept {
        uint64_t read_id = 0;

        bool success = queue_.try_consume_latest([&](const auto& msg) {
            read_id = msg.id;

            // 防越界：SeqLock 脏读期间 msg.len 可能变为乱码
            // 必须强制截断到 MaxDataSize，确保传给 visitor 的 span 是内存安全的
            uint32_t safe_len =
                std::min(msg.len, static_cast<uint32_t>(MaxDataSize));

            std::invoke(std::forward<F>(visitor),
                        std::span<const uint8_t>{msg.data.data(), safe_len});
        });

        if (success) {
            last_pop_id_.store(read_id, std::memory_order_relaxed);
            return true;
        }

        return false;
    }

    /// @brief Try to consume the latest message with timestamp and discard count (zero-copy).
    ///
    /// @tparam F Callable with signature `void(std::span<const uint8_t>, uint64_t ts, uint32_t discarded)`.
    /// @param visitor Callback receiving payload span, timestamp, and discard count.
    /// @return `true` on consistent read; `false` otherwise.
    template <typename F>
        requires std::invocable<F, std::span<const uint8_t>, uint64_t, uint32_t>
    [[nodiscard]] bool try_consume_latest_wts(F&& visitor) noexcept {
        uint64_t read_id = 0;
        uint32_t discarded = 0;

        bool success = queue_.try_consume_latest([&](const auto& msg) {
            read_id = msg.id;

            // 计算被 SeqLock 覆盖丢弃的包数量
            // IDs are 1-based (first pushed message has id=1).
            // On first read (prev_pop==0), messages 1..read_id-1 were skipped.
            auto prev_pop = last_pop_id_.load(std::memory_order_relaxed);
            discarded = (read_id > prev_pop + 1)
                ? static_cast<uint32_t>(std::min(read_id - prev_pop - 1, uint64_t(UINT32_MAX)))
                : 0;

            uint32_t safe_len =
                std::min(msg.len, static_cast<uint32_t>(MaxDataSize));
            // 将 span, ts, discarded 传给 visitor
            std::invoke(std::forward<F>(visitor),
                        std::span<const uint8_t>{msg.data.data(), safe_len},
                        msg.ts, discarded);
        });

        if (success) {
            last_pop_id_.store(read_id, std::memory_order_relaxed);
            return true;
        }

        return false;
    }

    // ===========================================================================
    // 非阻塞 Peek (Lock-free, 不更新 discard 计数)
    // ===========================================================================

    /// @brief Peek at the latest message without updating read state (reader-side only).
    ///
    /// Unlike `try_consume_latest`, does **not** advance `last_pop_id_`, so
    /// subsequent `try_consume_latest_wts` discard counts are unaffected.
    /// Useful for pre-inspecting message content before deciding to consume.
    ///
    /// @param out_buf Pre-allocated output buffer.
    /// @return Actual bytes copied on success; `std::nullopt` if no data.
    [[nodiscard]] std::optional<uint32_t> try_peek_latest(
        std::span<uint8_t> out_buf) noexcept {
        uint32_t copy_len = 0;
        bool success = try_peek_latest_visit([&](std::span<const uint8_t> data) {
            copy_len = static_cast<uint32_t>(std::min(data.size(), out_buf.size()));
            std::memcpy(out_buf.data(), data.data(), copy_len);
        });
        return success ? std::make_optional(copy_len) : std::nullopt;
    }

    /// @brief Peek at the latest message with timestamp (reader-side only, non-consuming).
    /// @param out_buf Pre-allocated output buffer.
    /// @param out_ts  [out] Timestamp of the message.
    /// @return Actual bytes copied on success; `std::nullopt` if no data.
    [[nodiscard]] std::optional<uint32_t> try_peek_latest_wts(
        std::span<uint8_t> out_buf, uint64_t& out_ts) noexcept {
        uint32_t copy_len = 0;
        bool success = try_peek_latest_visit_wts(
            [&](std::span<const uint8_t> data, uint64_t ts) {
                copy_len = static_cast<uint32_t>(std::min(data.size(), out_buf.size()));
                std::memcpy(out_buf.data(), data.data(), copy_len);
                out_ts = ts;
            });
        return success ? std::make_optional(copy_len) : std::nullopt;
    }

    /// @brief Zero-copy peek via visitor (non-consuming, reader-side only).
    /// @tparam F Callable with signature `void(std::span<const uint8_t>)`.
    /// @param visitor Callback receiving a span over the payload bytes.
    /// @return `true` if the visitor was called; `false` if no data.
    template <typename F>
        requires std::invocable<F, std::span<const uint8_t>>
    [[nodiscard]] bool try_peek_latest_visit(F&& visitor) noexcept {
        return queue_.try_peek_latest([&](const auto& msg) {
            uint32_t safe_len =
                std::min(msg.len, static_cast<uint32_t>(MaxDataSize));
            std::invoke(std::forward<F>(visitor),
                        std::span<const uint8_t>{msg.data.data(), safe_len});
        });
    }

    /// @brief Zero-copy peek with timestamp via visitor (non-consuming).
    /// @tparam F Callable with signature `void(std::span<const uint8_t>, uint64_t ts)`.
    /// @param visitor Callback receiving payload span and timestamp.
    /// @return `true` if the visitor was called; `false` if no data.
    template <typename F>
        requires std::invocable<F, std::span<const uint8_t>, uint64_t>
    [[nodiscard]] bool try_peek_latest_visit_wts(F&& visitor) noexcept {
        return queue_.try_peek_latest([&](const auto& msg) {
            uint32_t safe_len =
                std::min(msg.len, static_cast<uint32_t>(MaxDataSize));
            std::invoke(std::forward<F>(visitor),
                        std::span<const uint8_t>{msg.data.data(), safe_len},
                        msg.ts);
        });
    }

    /// @brief Zero-copy peek with timeout (non-consuming, visitor pattern).
    /// @tparam F Callable with signature `void(std::span<const uint8_t>)`.
    /// @tparam Rep, Period `std::chrono::duration` parameters.
    /// @param visitor Callback receiving a span over the payload bytes.
    /// @param timeout Maximum wall-clock wait.
    /// @return `true` on success; `false` on timeout.
    template <typename F, typename Rep, typename Period>
        requires std::invocable<F, std::span<const uint8_t>>
    [[nodiscard]] bool try_peek_latest_visit_for(
        F&& visitor, std::chrono::duration<Rep, Period> timeout) noexcept {
        if (try_peek_latest_visit(std::forward<F>(visitor))) return true;
        auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            cpu_relax();
            if (try_peek_latest_visit(std::forward<F>(visitor))) return true;
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    /// @brief Zero-copy peek with timestamp and timeout (non-consuming, visitor pattern).
    /// @tparam F Callable with signature `void(std::span<const uint8_t>, uint64_t ts)`.
    /// @tparam Rep, Period `std::chrono::duration` parameters.
    /// @param visitor Callback receiving payload span and timestamp.
    /// @param timeout Maximum wall-clock wait.
    /// @return `true` on success; `false` on timeout.
    template <typename F, typename Rep, typename Period>
        requires std::invocable<F, std::span<const uint8_t>, uint64_t>
    [[nodiscard]] bool try_peek_latest_visit_wts_for(
        F&& visitor, std::chrono::duration<Rep, Period> timeout) noexcept {
        if (try_peek_latest_visit_wts(std::forward<F>(visitor))) return true;
        auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            cpu_relax();
            if (try_peek_latest_visit_wts(std::forward<F>(visitor))) return true;
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    // ===========================================================================
    // 阻塞 Reader (自旋等待)
    // ===========================================================================

    /// @brief Blocking pop into a caller-supplied buffer (spins until new data).
    /// @param out_buf Pre-allocated output buffer.
    /// @return Actual bytes copied.
    uint32_t pop_latest(std::span<uint8_t> out_buf) noexcept {
        uint32_t copy_len = 0;
        consume_latest([&](std::span<const uint8_t> data) {
            copy_len = static_cast<uint32_t>(std::min(data.size(), out_buf.size()));
            std::memcpy(out_buf.data(), data.data(), copy_len);
        });
        return copy_len;
    }

    /// @brief Blocking pop with timestamp and discard count (spins until new data).
    /// @param out_buf       Pre-allocated output buffer.
    /// @param out_ts        [out] Timestamp of the message.
    /// @param out_discarded [out] Messages skipped since the last read.
    /// @return Actual bytes copied.
    uint32_t pop_latest_wts(std::span<uint8_t> out_buf, 
                            uint64_t& out_ts, 
                            uint32_t& out_discarded) noexcept {
        uint32_t copy_len = 0;
        consume_latest_wts([&](std::span<const uint8_t> data, uint64_t ts, uint32_t discarded) {
            copy_len = static_cast<uint32_t>(std::min(data.size(), out_buf.size()));
            std::memcpy(out_buf.data(), data.data(), copy_len);
            out_ts = ts;
            out_discarded = discarded;
        });
        return copy_len;
    }

    /// @brief Blocking zero-copy consume (spins until new data).
    /// @tparam F Callable with signature `void(std::span<const uint8_t>)`.
    /// @param visitor Callback receiving a span over the payload bytes.
    /// @warning Spins indefinitely -- use only on pinned threads.
    template <typename F>
        requires std::invocable<F, std::span<const uint8_t>>
    void consume_latest(F&& visitor) noexcept {
        while (!try_consume_latest(std::forward<F>(visitor))) {
            cpu_relax();
        }
    }

    /// @brief Blocking zero-copy consume with timestamp and discard count (spins).
    /// @tparam F Callable with signature `void(std::span<const uint8_t>, uint64_t ts, uint32_t discarded)`.
    /// @param visitor Callback receiving payload span, timestamp, and discard count.
    template <typename F>
        requires std::invocable<F, std::span<const uint8_t>, uint64_t, uint32_t>
    void consume_latest_wts(F&& visitor) noexcept {
        while (!try_consume_latest_wts(std::forward<F>(visitor))) {
            cpu_relax();
        }
    }

    // ===========================================================================
    // 带超时 Reader
    // ===========================================================================

    /// @brief Zero-copy consume with timeout.
    /// @tparam F Callable with signature `void(std::span<const uint8_t>)`.
    /// @tparam Rep, Period `std::chrono::duration` parameters.
    /// @param visitor Callback on success.
    /// @param timeout Maximum wall-clock wait.
    /// @return `true` on success; `false` on timeout.
    template <typename F, typename Rep, typename Period>
        requires std::invocable<F, std::span<const uint8_t>>
    [[nodiscard]] bool try_consume_latest_for(
        F&& visitor, std::chrono::duration<Rep, Period> timeout) noexcept {
        if (try_consume_latest(std::forward<F>(visitor))) return true;
        auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            cpu_relax();
            if (try_consume_latest(std::forward<F>(visitor))) return true;
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    /// @brief Pop with timeout into a caller-supplied buffer.
    /// @param out_buf Pre-allocated output buffer.
    /// @param timeout Maximum wall-clock wait.
    /// @return Actual bytes copied on success; `std::nullopt` on timeout.
    template <typename Rep, typename Period>
    [[nodiscard]] std::optional<uint32_t> try_pop_latest_for(
        std::span<uint8_t> out_buf,
        std::chrono::duration<Rep, Period> timeout) noexcept {
        uint32_t copy_len = 0;
        bool ok = try_consume_latest_for([&](std::span<const uint8_t> data) {
            copy_len = static_cast<uint32_t>(std::min(data.size(), out_buf.size()));
            std::memcpy(out_buf.data(), data.data(), copy_len);
        }, timeout);
        return ok ? std::make_optional(copy_len) : std::nullopt;
    }

    /// @brief Zero-copy consume with timestamp and discard count, with timeout.
    /// @tparam F Callable with signature `void(std::span<const uint8_t>, uint64_t ts, uint32_t discarded)`.
    /// @tparam Rep, Period `std::chrono::duration` parameters.
    /// @param visitor Callback on success.
    /// @param timeout Maximum wall-clock wait.
    /// @return `true` on success; `false` on timeout.
    template <typename F, typename Rep, typename Period>
        requires std::invocable<F, std::span<const uint8_t>, uint64_t, uint32_t>
    [[nodiscard]] bool try_consume_latest_wts_for(
        F&& visitor, std::chrono::duration<Rep, Period> timeout) noexcept {
        if (try_consume_latest_wts(std::forward<F>(visitor))) return true;
        auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            cpu_relax();
            if (try_consume_latest_wts(std::forward<F>(visitor))) return true;
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    /// @brief Pop with timeout, timestamp, and discard count into a buffer.
    /// @param out_buf       Pre-allocated output buffer.
    /// @param out_ts        [out] Timestamp.
    /// @param out_discarded [out] Messages skipped.
    /// @param timeout       Maximum wall-clock wait.
    /// @return Actual bytes copied on success; `std::nullopt` on timeout.
    template <typename Rep, typename Period>
    [[nodiscard]] std::optional<uint32_t> try_pop_latest_wts_for(
        std::span<uint8_t> out_buf, uint64_t& out_ts, uint32_t& out_discarded,
        std::chrono::duration<Rep, Period> timeout) noexcept {
        uint32_t copy_len = 0;
        bool ok = try_consume_latest_wts_for(
            [&](std::span<const uint8_t> data, uint64_t ts, uint32_t discarded) {
                copy_len = static_cast<uint32_t>(std::min(data.size(), out_buf.size()));
                std::memcpy(out_buf.data(), data.data(), copy_len);
                out_ts = ts;
                out_discarded = discarded;
            }, timeout);
        return ok ? std::make_optional(copy_len) : std::nullopt;
    }

    // ===========================================================================
    // 状态查询
    // ===========================================================================

    /// @brief Discard all unread data and reset the queue to empty.
    /// @warning Not thread-safe. Call only when no concurrent readers/writers.
    void clear() noexcept {
        queue_.clear();
        // 同步 reader 的 last_pop_id_ 到 writer 的 push_count_，
        // 使后续丢包计数从 0 开始。
        last_pop_id_.store(push_count_.load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
    }

    /// @brief Return the fixed queue capacity.
    [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }

    /// @brief Total number of messages successfully pushed (writer-side counter).
    ///
    /// Useful for monitoring throughput and computing discard rates.
    ///
    /// @return Monotonic push count.
    /// @note Only accurate when called from the writer thread.
    [[nodiscard]] uint64_t total_pushed() const noexcept {
        return push_count_.load(std::memory_order_relaxed);
    }

    /// @brief Approximate number of unread entries (for monitoring/debugging only).
    [[nodiscard]] size_t size_approx() const noexcept { return queue_.size_approx(); }

    /// @brief Check if there are no unread entries (approximate).
    [[nodiscard]] bool empty() const noexcept { return queue_.empty(); }

    // ===========================================================================
    // 可观测性
    // ===========================================================================

    /// @brief Alias for the standalone EvictingQueueBytesStats type.
    using Stats = EvictingQueueBytesStats;

    /// @brief Take a point-in-time statistics snapshot.
    /// @return EvictingQueueBytesStats with current counter values.
    [[nodiscard]] Stats stats() const noexcept {
        auto q_stats = queue_.stats();
        return Stats{
            .total_pushed      = push_count_.load(std::memory_order_relaxed),
            .last_pop_id       = last_pop_id_.load(std::memory_order_relaxed),
            .total_popped      = q_stats.total_popped,
            .current_size      = q_stats.current_size,
            .capacity          = Capacity,
            .total_overwritten = q_stats.overwritten,
        };
    }

   private:
    EvictingQueue<DataWrap, Capacity> queue_;

    // ---------------------------------------------------------------------------
    // Writer 独占区
    // ---------------------------------------------------------------------------
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> push_count_{0};

    // ---------------------------------------------------------------------------
    // Reader 独占区
    // ---------------------------------------------------------------------------
    alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> last_pop_id_{0};
};

}  // namespace eph::containers

/// @brief std::formatter specialization for EvictingQueueBytesStats.
///
/// Delegates to EvictingQueueBytesStats::dump() for use with `std::format`/`std::print`.
template <>
struct std::formatter<eph::containers::EvictingQueueBytesStats>
    : std::formatter<std::string> {
    auto format(const eph::containers::EvictingQueueBytesStats& s,
                std::format_context& ctx) const {
        return std::formatter<std::string>::format(s.dump(), ctx);
    }
};
