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

#include "eph/containers/evicting_queue.hpp"

namespace eph::containers {

// ---------------------------------------------------------------------------
// Standalone Stats type (enables std::formatter specialization)
// ---------------------------------------------------------------------------

/// Queue statistics snapshot for EvictingQueueBytes monitoring/debugging.
struct EvictingQueueBytesStats {
    uint64_t total_pushed;     ///< Total messages ever pushed (writer-side)
    uint64_t last_pop_id;      ///< ID of last consumed message (reader-side)
    size_t   current_size;     ///< Approximate unread entries
    size_t   capacity;         ///< Fixed capacity
    uint64_t total_overwritten; ///< Messages overwritten before being read

    /// Multi-line formatted dump for logging/debugging.
    [[nodiscard]] std::string dump() const {
        return std::format(
            "EvictingQueueBytes::Stats:\n"
            "  capacity: {}\n"
            "  current_size: {}\n"
            "  total_pushed: {}\n"
            "  last_pop_id: {}\n"
            "  total_overwritten: {}",
            capacity, current_size,
            total_pushed, last_pop_id, total_overwritten);
    }

    /// JSON-formatted stats for monitoring system integration.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"capacity\":{},\"current_size\":{},\"total_pushed\":{},"
            "\"last_pop_id\":{},\"total_overwritten\":{}}}",
            capacity, current_size, total_pushed,
            last_pop_id, total_overwritten);
    }

    /// Compute delta between two snapshots for interval-based monitoring.
    [[nodiscard]] friend EvictingQueueBytesStats operator-(const EvictingQueueBytesStats& lhs,
                                                           const EvictingQueueBytesStats& rhs) noexcept {
        return EvictingQueueBytesStats{
            .total_pushed      = lhs.total_pushed - rhs.total_pushed,
            .last_pop_id       = lhs.last_pop_id - rhs.last_pop_id,
            .current_size      = lhs.current_size,
            .capacity          = lhs.capacity,
            .total_overwritten = lhs.total_overwritten - rhs.total_overwritten,
        };
    }

    /// Messages consumed per second over a measurement interval.
    /// Apply to a delta snapshot: `auto delta = t2 - t1; delta.throughput(elapsed_ns)`.
    [[nodiscard]] double throughput(uint64_t duration_ns) const noexcept {
        return duration_ns > 0
            ? static_cast<double>(last_pop_id) * 1e9 / static_cast<double>(duration_ns)
            : 0.0;
    }

    /// Data loss rate as a fraction [0.0, 1.0].
    [[nodiscard]] double loss_rate() const noexcept {
        return total_pushed > 0
            ? static_cast<double>(total_overwritten) / static_cast<double>(total_pushed)
            : 0.0;
    }

    [[nodiscard]] friend bool operator==(const EvictingQueueBytesStats&,
                                          const EvictingQueueBytesStats&) = default;
};

template <size_t MaxDataSize = 256, size_t Capacity = 256>
class EvictingQueueBytes {
    static_assert(MaxDataSize <= UINT32_MAX,
                  "MaxDataSize must fit in uint32_t (used for len field)");

   public:
    struct DataWrap {
        uint64_t id;
        uint64_t ts;
        uint32_t len;
        std::array<uint8_t, MaxDataSize> data;
    };

    EvictingQueueBytes() = default;
    ~EvictingQueueBytes() = default;

    // ===========================================================================
    // Writer (Wait-free)
    // ===========================================================================

    /// @note Unlike BoundedQueueBytes::try_push_wts, this method only returns
    /// false when payload exceeds MaxDataSize. The underlying EvictingQueue is
    /// wait-free and never blocks on queue fullness — old data is overwritten.
    [[nodiscard]] bool try_push_wts(std::span<const uint8_t> payload, uint64_t ts) noexcept {
        if (payload.size() > MaxDataSize) [[unlikely]] {
            return false;
        }

        queue_.produce([&](DataWrap& slot) {
            slot.id = ++push_count_;
            slot.ts = ts;
            slot.len = static_cast<uint32_t>(payload.size());
            std::memcpy(slot.data.data(), payload.data(), payload.size());
        });

        return true;
    }

    [[nodiscard]] bool try_push(std::span<const uint8_t> payload) noexcept {
        return try_push_wts(payload, 0);
    }

    /**
     * @brief 批量推入带时间戳的字节流 (Wait-free)
     *
     * 所有 payload 必须 <= MaxDataSize，否则全部不写入。
     * 底层 EvictingQueue 为 wait-free 写入——旧数据会被覆盖。
     *
     * @param payloads   各消息的 payload span 数组
     * @param timestamps 各消息的时间戳数组（与 payloads 等长）
     * @param count      消息数量
     * @return true 全部写入成功; false 某个 payload 过大
     */
    [[nodiscard]] bool push_n_wts(const std::span<const uint8_t>* payloads,
                                   const uint64_t* timestamps,
                                   size_t count) noexcept {
        for (size_t i = 0; i < count; ++i) {
            if (payloads[i].size() > MaxDataSize) [[unlikely]] return false;
        }
        queue_.produce_n(count, [&](DataWrap& slot, size_t i) {
            slot.id = ++push_count_;
            slot.ts = timestamps[i];
            slot.len = static_cast<uint32_t>(payloads[i].size());
            std::memcpy(slot.data.data(), payloads[i].data(), payloads[i].size());
        });
        return true;
    }

    /**
     * @brief 批量推入字节流 (Wait-free, ts=0)
     */
    [[nodiscard]] bool push_n(const std::span<const uint8_t>* payloads,
                               size_t count) noexcept {
        for (size_t i = 0; i < count; ++i) {
            if (payloads[i].size() > MaxDataSize) [[unlikely]] return false;
        }
        queue_.produce_n(count, [&](DataWrap& slot, size_t i) {
            slot.id = ++push_count_;
            slot.ts = 0;
            slot.len = static_cast<uint32_t>(payloads[i].size());
            std::memcpy(slot.data.data(), payloads[i].data(), payloads[i].size());
        });
        return true;
    }

    // ===========================================================================
    // 非阻塞 Reader (Lock-free)
    // ===========================================================================

    /**
     * @brief 消费端（非阻塞）：调用者预分配 buffer，返回实际拷贝字节数或 std::nullopt
     */
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
            last_pop_id_ = read_id;
            return copy_len;
        }

        return std::nullopt;
    }

    /**
     * @brief 消费端（非阻塞）：调用者预分配 buffer，返回实际拷贝字节数、时间戳和丢包数
     */
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

    /**
     * @brief 消费端（非阻塞零拷贝）
     */
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
            last_pop_id_ = read_id;
            return true;
        }

        return false;
    }

    /**
     * @brief 消费端（非阻塞零拷贝带时间戳和丢包信息）
     */
    template <typename F>
        requires std::invocable<F, std::span<const uint8_t>, uint64_t, uint32_t>
    [[nodiscard]] bool try_consume_latest_wts(F&& visitor) noexcept {
        uint64_t read_id = 0;
        uint32_t discarded = 0;

        bool success = queue_.try_consume_latest([&](const auto& msg) {
            read_id = msg.id;

            // 计算被 SeqLock 覆盖丢弃的包数量
            discarded = (last_pop_id_ == 0)
                            ? 0
                            : static_cast<uint32_t>(read_id - last_pop_id_ - 1);

            uint32_t safe_len =
                std::min(msg.len, static_cast<uint32_t>(MaxDataSize));
            // 将 span, ts, discarded 传给 visitor
            std::invoke(std::forward<F>(visitor),
                        std::span<const uint8_t>{msg.data.data(), safe_len},
                        msg.ts, discarded);
        });

        if (success) {
            last_pop_id_ = read_id;
            return true;
        }

        return false;
    }

    // ===========================================================================
    // 非阻塞 Peek (Lock-free, 不更新 discard 计数)
    // ===========================================================================

    /**
     * @brief 查看最新消息但不更新读取状态 (Reader 线程专用)
     *
     * 与 try_consume_latest 不同，不更新 last_pop_id_，因此后续
     * try_consume_latest_wts 的丢包计数不受影响。适用于消费前
     * 预检查消息内容或类型的场景。
     *
     * @param out_buf 输出缓冲区
     * @return 实际拷贝字节数；std::nullopt 表示无数据
     */
    [[nodiscard]] std::optional<uint32_t> try_peek_latest(
        std::span<uint8_t> out_buf) noexcept {
        uint32_t copy_len = 0;
        bool success = try_peek_latest_visit([&](std::span<const uint8_t> data) {
            copy_len = static_cast<uint32_t>(std::min(data.size(), out_buf.size()));
            std::memcpy(out_buf.data(), data.data(), copy_len);
        });
        return success ? std::make_optional(copy_len) : std::nullopt;
    }

    /**
     * @brief 查看最新消息但不更新读取状态（带时间戳）
     *
     * @param out_buf 输出缓冲区
     * @param out_ts [out] 时间戳
     * @return 实际拷贝字节数；std::nullopt 表示无数据
     */
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

    /**
     * @brief 零拷贝查看最新消息但不更新读取状态 (Visitor 模式)
     *
     * @param visitor 回调 void(std::span<const uint8_t>)
     * @return true 有新数据且已回调; false 无数据
     */
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

    /**
     * @brief 零拷贝查看最新消息但不更新读取状态（带时间戳, Visitor 模式）
     *
     * @param visitor 回调 void(std::span<const uint8_t>, uint64_t ts)
     * @return true 有新数据且已回调; false 无数据
     */
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

    /**
     * @brief 带超时的零拷贝查看最新消息但不更新读取状态 (Visitor 模式)
     *
     * @param visitor 回调 void(std::span<const uint8_t>)
     * @param timeout 最大等待时间
     * @return true 有新数据且已回调; false 超时
     */
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

    /**
     * @brief 带超时的零拷贝查看最新消息但不更新读取状态（带时间戳, Visitor 模式）
     *
     * @param visitor 回调 void(std::span<const uint8_t>, uint64_t ts)
     * @param timeout 最大等待时间
     * @return true 有新数据且已回调; false 超时
     */
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

    /**
     * @brief 消费端：阻塞式读取到外部 buffer，返回拷贝字节数
     */
    uint32_t pop_latest(std::span<uint8_t> out_buf) noexcept {
        uint32_t copy_len = 0;
        consume_latest([&](std::span<const uint8_t> data) {
            copy_len = static_cast<uint32_t>(std::min(data.size(), out_buf.size()));
            std::memcpy(out_buf.data(), data.data(), copy_len);
        });
        return copy_len;
    }

    /**
     * @brief 消费端：阻塞式读取带时间戳和丢包信息到外部 buffer，返回拷贝字节数
     */
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

    /**
     * @brief 消费端：阻塞式零拷贝访问（自旋直到成功读取到新数据）
     */
    template <typename F>
        requires std::invocable<F, std::span<const uint8_t>>
    void consume_latest(F&& visitor) noexcept {
        while (!try_consume_latest(std::forward<F>(visitor))) {
            cpu_relax();
        }
    }

    /**
     * @brief 消费端：阻塞式零拷贝访问（带时间戳和丢包信息，自旋直到成功）
     */
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

    /**
     * @brief 带超时的零拷贝读取最新数据
     */
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

    /**
     * @brief 带超时的拷贝读取
     */
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

    /**
     * @brief 带超时的零拷贝读取最新数据（带时间戳和丢包信息）
     */
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

    /**
     * @brief 带超时的拷贝读取（带时间戳和丢包信息）
     */
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

    /// 丢弃所有未读数据，将队列重置为空状态。
    /// @warning 仅在确保无并发读写时调用。
    void clear() noexcept {
        queue_.clear();
        // 同步 reader 的 last_pop_id_ 到 writer 的 push_count_，
        // 使后续丢包计数从 0 开始。
        last_pop_id_ = push_count_;
    }

    [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }

    /// Total number of messages successfully pushed (writer-side counter).
    /// Useful for monitoring throughput and computing discard rates.
    /// @note Only accurate when called from the writer thread.
    [[nodiscard]] uint64_t total_pushed() const noexcept { return push_count_; }

    /// Approximate number of unread entries (for monitoring/debugging only).
    [[nodiscard]] size_t size_approx() const noexcept { return queue_.size_approx(); }

    /// Check if there are no unread entries (approximate).
    [[nodiscard]] bool empty() const noexcept { return queue_.empty(); }

    // ===========================================================================
    // 可观测性
    // ===========================================================================

    /// Alias for the standalone EvictingQueueBytesStats type.
    using Stats = EvictingQueueBytesStats;

    /// Take a point-in-time statistics snapshot.
    [[nodiscard]] Stats stats() const noexcept {
        auto q_stats = queue_.stats();
        return Stats{
            .total_pushed      = push_count_,
            .last_pop_id       = last_pop_id_,
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
    alignas(CACHE_LINE_SIZE) uint64_t push_count_{0};

    // ---------------------------------------------------------------------------
    // Reader 独占区
    // ---------------------------------------------------------------------------
    alignas(CACHE_LINE_SIZE) uint64_t last_pop_id_{0};
};

}  // namespace eph::containers

// std::formatter specialization for EvictingQueueBytesStats
template <>
struct std::formatter<eph::containers::EvictingQueueBytesStats>
    : std::formatter<std::string> {
    auto format(const eph::containers::EvictingQueueBytesStats& s,
                std::format_context& ctx) const {
        return std::formatter<std::string>::format(s.dump(), ctx);
    }
};
