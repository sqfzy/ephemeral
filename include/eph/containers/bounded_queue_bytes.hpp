#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

#include "eph/containers/bounded_queue.hpp"

namespace eph::containers {

template <size_t MaxDataSize = 256, size_t Capacity = 256>
class BoundedQueueBytes {
   public:
    struct DataWrap {
        uint32_t len;
        uint64_t ts;
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
    bool try_push_wts(std::span<const uint8_t> payload, uint64_t ts) noexcept {
        if (payload.size() > MaxDataSize) [[unlikely]] {
            return false;
        }

        return queue_.try_produce([&](DataWrap& slot) {
            slot.len = static_cast<uint32_t>(payload.size());
            slot.ts = ts;
            std::memcpy(slot.data.data(), payload.data(), payload.size());
        });
    }

    /**
     * @brief 尝试推入字节流
     */
    bool try_push(std::span<const uint8_t> payload) noexcept {
        return try_push_wts(payload, 0);
    }

    // ===========================================================================
    // 阻塞操作 (Writer)
    // ===========================================================================

    /**
     * @brief 阻塞式推入带时间戳的字节流
     * @return 若 payload 过大无法存入则返回 false，否则自旋直到成功存入并返回 true
     */
    bool push_wts(std::span<const uint8_t> payload, uint64_t ts) noexcept {
        if (payload.size() > MaxDataSize) [[unlikely]] {
            return false;
        }

        queue_.produce([&](DataWrap& slot) {
            slot.len = static_cast<uint32_t>(payload.size());
            slot.ts = ts;
            std::memcpy(slot.data.data(), payload.data(), payload.size());
        });
        
        return true;
    }

    /**
     * @brief 阻塞式推入字节流
     */
    bool push(std::span<const uint8_t> payload) noexcept {
        return push_wts(payload, 0);
    }

    // ===========================================================================
    // 非阻塞操作 (Reader)
    // ===========================================================================

    /**
     * @brief 消费端：尝试拷贝数据到外部 buffer
     */
    std::optional<uint32_t> try_pop(std::span<uint8_t> out_buf) noexcept {
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
    std::optional<uint32_t> try_pop_wts(std::span<uint8_t> out_buf,
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
    bool try_consume(F&& visitor) noexcept {
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
    bool try_consume_wts(F&& visitor) noexcept {
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
    // 状态查询
    // ===========================================================================

    [[nodiscard]] size_t size() const noexcept { return queue_.size(); }
    
    [[nodiscard]] bool empty() const noexcept { return queue_.empty(); }
    
    [[nodiscard]] bool full() const noexcept { return queue_.full(); }
    
    [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }

   private:
    BoundedQueue<DataWrap, Capacity> queue_;
};

}  // namespace eph::containers
