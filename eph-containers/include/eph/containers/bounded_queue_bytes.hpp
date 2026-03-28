#pragma once

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

/// Queue statistics snapshot for BoundedQueueBytes monitoring/debugging.
struct BoundedQueueBytesStats {
    size_t total_pushed;   ///< Total messages ever pushed (monotonic)
    size_t total_popped;   ///< Total messages ever popped (monotonic)
    size_t current_size;   ///< Approximate current occupancy
    size_t capacity;       ///< Fixed capacity

    /// Multi-line formatted dump for logging/debugging.
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

    /// JSON-formatted stats for monitoring system integration.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"capacity\":{},\"current_size\":{},\"total_pushed\":{},\"total_popped\":{}}}",
            capacity, current_size, total_pushed, total_popped);
    }

    /// Compute delta between two snapshots for interval-based monitoring.
    [[nodiscard]] friend BoundedQueueBytesStats operator-(const BoundedQueueBytesStats& lhs,
                                                          const BoundedQueueBytesStats& rhs) noexcept {
        return BoundedQueueBytesStats{
            .total_pushed = lhs.total_pushed - rhs.total_pushed,
            .total_popped = lhs.total_popped - rhs.total_popped,
            .current_size = lhs.current_size,
            .capacity     = lhs.capacity,
        };
    }

    /// Messages consumed per second over a measurement interval.
    /// Apply to a delta snapshot: `auto delta = t2 - t1; delta.throughput(elapsed_ns)`.
    [[nodiscard]] double throughput(uint64_t duration_ns) const noexcept {
        return duration_ns > 0
            ? static_cast<double>(total_popped) * 1e9 / static_cast<double>(duration_ns)
            : 0.0;
    }

    [[nodiscard]] friend bool operator==(const BoundedQueueBytesStats&,
                                          const BoundedQueueBytesStats&) = default;
};

template <size_t MaxDataSize = 256, size_t Capacity = 256>
class BoundedQueueBytes {
    static_assert(MaxDataSize <= UINT32_MAX,
                  "MaxDataSize must fit in uint32_t (used for len field)");

   public:
    struct DataWrap {
        uint64_t ts;
        uint32_t len;
        std::array<uint8_t, MaxDataSize> data;
    };

    BoundedQueueBytes() = default;
    ~BoundedQueueBytes() = default;

    // ===========================================================================
    // 非阻塞操作 (Writer)
    // ===========================================================================

    /**
     * @brief 尝试推入带时间戳的字节流
     */
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

    /**
     * @brief 尝试推入字节流
     */
    [[nodiscard]] bool try_push(std::span<const uint8_t> payload) noexcept {
        return try_push_wts(payload, 0);
    }

    // ===========================================================================
    // 批量非阻塞操作 (Writer)
    // ===========================================================================

    /**
     * @brief 批量推入带时间戳的字节流 (all-or-nothing 语义)
     *
     * @param payloads 各消息的 payload span 数组
     * @param timestamps 各消息的时间戳数组（与 payloads 等长）
     * @param count 消息数量
     * @return true 全部入队成功; false 空间不足或某个 payload 过大，无任何写入
     */
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

    /**
     * @brief 批量推入字节流 (all-or-nothing 语义, ts=0)
     */
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

    /**
     * @brief 批量零拷贝消费 (尽力而为语义)
     *
     * 消费最多 n 个消息，对每个消息调用 visitor(data_span, index)。
     *
     * @param n 最大消费数量
     * @param visitor 回调 void(std::span<const uint8_t>, size_t index)
     * @return 实际消费的消息数量
     */
    template <typename F>
        requires std::invocable<F, std::span<const uint8_t>, size_t>
    [[nodiscard]] size_t try_consume_n(size_t n, F&& visitor) noexcept {
        return queue_.try_consume_n(n, [&](const DataWrap& msg, size_t idx) {
            uint32_t safe_len = std::min(msg.len, static_cast<uint32_t>(MaxDataSize));
            std::invoke(std::forward<F>(visitor),
                        std::span<const uint8_t>{msg.data.data(), safe_len}, idx);
        });
    }

    /**
     * @brief 批量零拷贝消费带时间戳 (尽力而为语义)
     *
     * @param n 最大消费数量
     * @param visitor 回调 void(std::span<const uint8_t>, uint64_t ts, size_t index)
     * @return 实际消费的消息数量
     */
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

    /**
     * @brief 一次性消费所有排队消息 (尽力而为语义)
     *
     * 等价于 try_consume_n(Capacity, visitor)，语义更清晰。
     * 适用于关闭前 drain 或周期性批量处理。
     *
     * @param visitor 回调 void(std::span<const uint8_t>, size_t index)
     * @return 实际消费的消息数量
     */
    template <typename F>
        requires std::invocable<F, std::span<const uint8_t>, size_t>
    [[nodiscard]] size_t try_consume_all(F&& visitor) noexcept {
        return try_consume_n(Capacity, std::forward<F>(visitor));
    }

    /**
     * @brief 一次性消费所有排队消息（带时间戳）
     *
     * 等价于 try_consume_n_wts(Capacity, visitor)。
     *
     * @param visitor 回调 void(std::span<const uint8_t>, uint64_t ts, size_t index)
     * @return 实际消费的消息数量
     */
    template <typename F>
        requires std::invocable<F, std::span<const uint8_t>, uint64_t, size_t>
    [[nodiscard]] size_t try_consume_all_wts(F&& visitor) noexcept {
        return try_consume_n_wts(Capacity, std::forward<F>(visitor));
    }

    // ===========================================================================
    // 非阻塞 Peek 操作 (Reader)
    // ===========================================================================

    /**
     * @brief 查看队首消息但不消费 (Reader 线程专用)
     *
     * 拷贝队首消息到外部 buffer 但不推进 head 指针。适用于消费前需要
     * 预检查消息类型或决定路由逻辑的场景。
     *
     * @param out_buf 输出缓冲区
     * @return 实际拷贝字节数；std::nullopt 表示队列为空
     *
     * @note 仅 Reader 线程可调用。多次调用返回同一消息，
     *       直到 try_pop/try_consume 推进 head。
     */
    [[nodiscard]] std::optional<uint32_t> try_peek(std::span<uint8_t> out_buf) noexcept {
        uint32_t copy_len = 0;
        bool success = try_peek_visit([&](std::span<const uint8_t> data) {
            copy_len = static_cast<uint32_t>(std::min(data.size(), out_buf.size()));
            std::memcpy(out_buf.data(), data.data(), copy_len);
        });
        return success ? std::make_optional(copy_len) : std::nullopt;
    }

    /**
     * @brief 查看队首消息但不消费（带时间戳）
     *
     * @param out_buf 输出缓冲区
     * @param out_ts [out] 时间戳
     * @return 实际拷贝字节数；std::nullopt 表示队列为空
     */
    [[nodiscard]] std::optional<uint32_t> try_peek_wts(std::span<uint8_t> out_buf,
                                                        uint64_t& out_ts) noexcept {
        uint32_t copy_len = 0;
        bool success = try_peek_visit_wts([&](std::span<const uint8_t> data, uint64_t ts) {
            copy_len = static_cast<uint32_t>(std::min(data.size(), out_buf.size()));
            std::memcpy(out_buf.data(), data.data(), copy_len);
            out_ts = ts;
        });
        return success ? std::make_optional(copy_len) : std::nullopt;
    }

    /**
     * @brief 零拷贝查看队首消息但不消费 (Visitor 模式)
     *
     * @param visitor 回调 void(std::span<const uint8_t>)
     * @return true 队列非空且已回调; false 队列为空
     */
    template <typename F>
        requires std::invocable<F, std::span<const uint8_t>>
    [[nodiscard]] bool try_peek_visit(F&& visitor) noexcept {
        return queue_.try_peek([&](const DataWrap& msg) {
            uint32_t safe_len = std::min(msg.len, static_cast<uint32_t>(MaxDataSize));
            std::invoke(std::forward<F>(visitor),
                        std::span<const uint8_t>{msg.data.data(), safe_len});
        });
    }

    /**
     * @brief 零拷贝查看队首消息但不消费（带时间戳, Visitor 模式）
     *
     * @param visitor 回调 void(std::span<const uint8_t>, uint64_t ts)
     * @return true 队列非空且已回调; false 队列为空
     */
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

    /**
     * @brief 阻塞式推入带时间戳的字节流
     * @return 若 payload 过大无法存入则返回 false，否则自旋直到成功存入并返回 true
     */
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

    /**
     * @brief 阻塞式推入字节流
     */
    [[nodiscard]] bool push(std::span<const uint8_t> payload) noexcept {
        return push_wts(payload, 0);
    }

    // ===========================================================================
    // 非阻塞操作 (Reader)
    // ===========================================================================

    /**
     * @brief 消费端：尝试拷贝数据到外部 buffer
     */
    [[nodiscard]] std::optional<uint32_t> try_pop(std::span<uint8_t> out_buf) noexcept {
        uint32_t copy_len = 0;
        bool success = try_consume([&](std::span<const uint8_t> data) {
            copy_len =
                static_cast<uint32_t>(std::min(data.size(), out_buf.size()));
            std::memcpy(out_buf.data(), data.data(), copy_len);
        });

        return success ? std::make_optional(copy_len) : std::nullopt;
    }

    /**
     * @brief 消费端：尝试拷贝带时间戳的数据到外部 buffer
     */
    [[nodiscard]] std::optional<uint32_t> try_pop_wts(std::span<uint8_t> out_buf,
                                        uint64_t& out_ts) noexcept {
        uint32_t copy_len = 0;
        bool success =
            try_consume_wts([&](std::span<const uint8_t> data, uint64_t ts) {
                copy_len = static_cast<uint32_t>(
                    std::min(data.size(), out_buf.size()));
                std::memcpy(out_buf.data(), data.data(), copy_len);
                out_ts = ts;
            });

        return success ? std::make_optional(copy_len) : std::nullopt;
    }

    /**
     * @brief 消费端：尝试零拷贝访问
     */
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

    /**
     * @brief 消费端：尝试零拷贝访问（带时间戳）
     * 由于去掉了 id，此接口不再提供 discarded 参数
     */
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

    /**
     * @brief 消费端：阻塞式拷贝数据到外部 buffer
     */
    uint32_t pop(std::span<uint8_t> out_buf) noexcept {
        uint32_t copy_len = 0;
        consume([&](std::span<const uint8_t> data) {
            copy_len = static_cast<uint32_t>(std::min(data.size(), out_buf.size()));
            std::memcpy(out_buf.data(), data.data(), copy_len);
        });
        return copy_len;
    }

    /**
     * @brief 消费端：阻塞式拷贝带时间戳的数据到外部 buffer
     */
    uint32_t pop_wts(std::span<uint8_t> out_buf, uint64_t& out_ts) noexcept {
        uint32_t copy_len = 0;
        consume_wts([&](std::span<const uint8_t> data, uint64_t ts) {
            copy_len = static_cast<uint32_t>(std::min(data.size(), out_buf.size()));
            std::memcpy(out_buf.data(), data.data(), copy_len);
            out_ts = ts;
        });
        return copy_len;
    }

    /**
     * @brief 消费端：阻塞式零拷贝访问
     */
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

    /**
     * @brief 消费端：阻塞式零拷贝访问（带时间戳）
     */
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

    /**
     * @brief 带超时推入带时间戳的字节流
     * @return true 成功; false 超时或 payload 过大
     */
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

    /**
     * @brief 带超时推入字节流
     */
    template <typename Rep, typename Period>
    [[nodiscard]] bool try_push_for(std::span<const uint8_t> payload,
                                    std::chrono::duration<Rep, Period> timeout) noexcept {
        return try_push_wts_for(payload, 0, timeout);
    }

    // ===========================================================================
    // 带超时操作 (Reader)
    // ===========================================================================

    /**
     * @brief 带超时的零拷贝消费
     */
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

    /**
     * @brief 带超时的零拷贝消费（带时间戳）
     */
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

    /**
     * @brief 带超时的拷贝读取
     */
    template <typename Rep, typename Period>
    [[nodiscard]] std::optional<uint32_t> try_pop_for(
        std::span<uint8_t> out_buf,
        std::chrono::duration<Rep, Period> timeout) noexcept {
        uint32_t copy_len = 0;
        bool ok = try_consume_for([&](std::span<const uint8_t> data) {
            copy_len = static_cast<uint32_t>(std::min(data.size(), out_buf.size()));
            std::memcpy(out_buf.data(), data.data(), copy_len);
        }, timeout);
        return ok ? std::make_optional(copy_len) : std::nullopt;
    }

    /**
     * @brief 带超时的拷贝读取（带时间戳）
     */
    template <typename Rep, typename Period>
    [[nodiscard]] std::optional<uint32_t> try_pop_wts_for(
        std::span<uint8_t> out_buf, uint64_t& out_ts,
        std::chrono::duration<Rep, Period> timeout) noexcept {
        uint32_t copy_len = 0;
        bool ok = try_consume_wts_for(
            [&](std::span<const uint8_t> data, uint64_t ts) {
                copy_len = static_cast<uint32_t>(std::min(data.size(), out_buf.size()));
                std::memcpy(out_buf.data(), data.data(), copy_len);
                out_ts = ts;
            }, timeout);
        return ok ? std::make_optional(copy_len) : std::nullopt;
    }

    // ===========================================================================
    // 状态查询
    // ===========================================================================

    /// 丢弃所有排队中的数据，将队列重置为空状态。
    /// @warning 仅在确保无并发读写时调用。
    void clear() noexcept { queue_.clear(); }

    [[nodiscard]] size_t size() const noexcept { return queue_.size(); }

    [[nodiscard]] bool empty() const noexcept { return queue_.empty(); }

    [[nodiscard]] bool full() const noexcept { return queue_.full(); }

    [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }

    /// 估计剩余可写入空间
    [[nodiscard]] size_t available_write() const noexcept { return queue_.available_write(); }

    /// 估计可读取的消息数量
    [[nodiscard]] size_t available_read() const noexcept { return queue_.available_read(); }

    // ===========================================================================
    // 可观测性
    // ===========================================================================

    /// Alias for the standalone BoundedQueueBytesStats type.
    using Stats = BoundedQueueBytesStats;

    /// Take a point-in-time statistics snapshot.
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

// std::formatter specialization for BoundedQueueBytesStats
template <>
struct std::formatter<eph::containers::BoundedQueueBytesStats>
    : std::formatter<std::string> {
    auto format(const eph::containers::BoundedQueueBytesStats& s,
                std::format_context& ctx) const {
        return std::formatter<std::string>::format(s.dump(), ctx);
    }
};
