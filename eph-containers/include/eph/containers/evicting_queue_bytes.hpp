#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

#include "eph/containers/evicting_queue.hpp"

namespace eph::containers {

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
