#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

#include "eph/containers/evicting_queue.hpp"

namespace eph::containers {

template <size_t MaxDataSize = 256, size_t Capacity = 256>
class EvictingQueueBytes {
   public:
    struct DataWrap {
        uint64_t id;
        uint32_t len;
        uint64_t ts;
        std::array<uint8_t, MaxDataSize> data;
    };

    EvictingQueueBytes() = default;
    ~EvictingQueueBytes() = default;

    // ===========================================================================
    // Writer (Wait-free)
    // ===========================================================================

    bool try_push_wts(std::span<const uint8_t> payload, uint64_t ts) noexcept {
        if (payload.size() > MaxDataSize) [[unlikely]] {
            return false;
        }

        queue_.produce([&](DataWrap& slot) {
            slot.id = ++push_count_;
            slot.len = static_cast<uint32_t>(payload.size());
            slot.ts = ts;
            std::memcpy(slot.data.data(), payload.data(), payload.size());
        });

        return true;
    }

    bool try_push(std::span<const uint8_t> payload) noexcept {
        if (payload.size() > MaxDataSize) [[unlikely]] {
            return false;
        }

        queue_.produce([&](DataWrap& slot) {
            slot.id = ++push_count_;
            slot.len = static_cast<uint32_t>(payload.size());
            std::memcpy(slot.data.data(), payload.data(), payload.size());
        });

        return true;
    }

    // ===========================================================================
    // 非阻塞 Reader (Lock-free)
    // ===========================================================================

    /**
     * @brief 消费端（非阻塞）：调用者预分配 buffer，返回实际拷贝字节数或 std::nullopt
     */
    std::optional<uint32_t> try_pop_latest(
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
    std::optional<uint32_t> try_pop_latest_wts(std::span<uint8_t> out_buf, 
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
    bool try_consume_latest(F&& visitor) noexcept {
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
    bool try_consume_latest_wts(F&& visitor) noexcept {
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
    // 状态查询
    // ===========================================================================

    [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }

    [[nodiscard]] bool busy() const noexcept { return queue_.busy(); }

   private:
    EvictingQueue<DataWrap, Capacity> queue_;

    // ---------------------------------------------------------------------------
    // Writer 独占区
    // ---------------------------------------------------------------------------
    alignas(64) uint64_t push_count_{0};

    // ---------------------------------------------------------------------------
    // Reader 独占区
    // ---------------------------------------------------------------------------
    alignas(64) uint64_t last_pop_id_{0};
};

}  // namespace eph::containers
