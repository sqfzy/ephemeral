#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <functional>
#include <optional>

#include "eph/base/concepts.hpp"
#include "eph/utils/alignment.hpp"
#include "eph/utils/cpu.hpp"

using eph::base::TrivialData;
using eph::utils::Align;
using eph::utils::cpu_relax;

namespace eph::containers {

/**
 * @brief SPSC 无锁不可丢弃队列
 *
 * 内存布局：
 * ```
 * ┌─────────────────────────────────────────────────┐
 * │ Reader Hot Zone (独占 Cache Line)               │
 * │  - reader_.head_         (全局读取索引, 原子)   │
 * │  - reader_.shadow_tail_  (本地影子写入索引)     │
 * ├─────────────────────────────────────────────────┤
 * │ Writer Hot Zone (独占 Cache Line)               │
 * │  - writer_.tail_         (全局写入索引, 原子)   │
 * │  - writer_.shadow_head_  (本地影子读取索引)     │
 * ├─────────────────────────────────────────────────┤
 * │ Slots Zone (数据存储区)                         │
 * │  - buffer_[0..Capacity-1]                       │
 * └─────────────────────────────────────────────────┘
 * ```
 *
 * @tparam T 数据类型，必须满足 TriviallyCopyable 概念。
 * @tparam Capacity 缓冲区容量，必须是 2 的幂。
 */
template <typename T, size_t Capacity>
    requires TrivialData<T>
class BoundedQueue {
    static_assert(std::has_single_bit(Capacity), "Capacity must be power of 2");
    static constexpr size_t mask_ = Capacity - 1;

   private:
    // ---------------------------------------------------------------------------
    // Writer Hot Data
    // ---------------------------------------------------------------------------
    struct alignas(Align<T>) WriterLine {
        /// 全局写入索引 (Tail Pointer)
        std::atomic<size_t> tail_{0};
        /// 本地影子读取索引：记录上一次看到的 head_，减少对 ConsumerLine
        /// 的跨核访问
        size_t shadow_head_{0};
    } writer_;

    // ---------------------------------------------------------------------------
    // Reader Hot Data
    // ---------------------------------------------------------------------------
    struct alignas(Align<T>) ReaderLine {
        /// 全局读取索引 (Head Pointer)
        std::atomic<size_t> head_{0};
        /// 本地影子写入索引：记录上一次看到的 tail_，减少对 ProducerLine
        /// 的跨核访问
        size_t shadow_tail_{0};
    } reader_;

    /// 核心数据存储区
    alignas(Align<T>) std::array<T, Capacity> buffer_{};

   public:
    BoundedQueue() noexcept = default;
    ~BoundedQueue() noexcept = default;

    // ===========================================================================
    // Writer 操作
    // ===========================================================================

    /**
     * @brief：尝试零拷贝写入 (Visitor 模式)
     *
     * @tparam F 回调类型，签名应为 void(T& slot)
     * @param writer_func 用于初始化或修改数据的回调函数
     * @return true 写入成功; false 队列已满
     */
    template <typename F>
        requires std::invocable<F, T&>
    [[nodiscard]] bool try_produce(F&& writer_func) noexcept {
        // 1. 获取本地写入索引 (Relaxed)
        // 只有生产者修改 tail，所以 Relaxed 读取即可
        const size_t tail = writer_.tail_.load(std::memory_order_relaxed);

        // 2. 快速路径：检查影子索引空间
        if (tail - writer_.shadow_head_ >= Capacity) {
            // 3. 慢速路径：影子索引认为已满，必须从内存加载最新的全局 head_
            // (Acquire) Acquire 保证读取到消费者最新的修改
            const size_t head = reader_.head_.load(std::memory_order_acquire);
            writer_.shadow_head_ = head;  // 更新本地缓存

            // 4. 再次检查真实容量状态
            if (tail - head >= Capacity) {
                return false;  // Full
            }
        }

        // 5. 执行数据写入
        // 将 Slot 的引用传递给用户，允许原地构造或赋值
        std::invoke(std::forward<F>(writer_func), buffer_[tail & mask_]);

        // 6. 发布新的写入索引 (Release)
        // Release 保证之前的写入操作对消费者可见
        writer_.tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief 尝试原地构造
     *
     * @param args 构造参数
     * @return true 成功; false 队列已满
     */
    template <typename... Args>
        requires std::is_constructible_v<T, Args...>
    [[nodiscard]] bool try_emplace(Args&&... args) noexcept {
        return try_produce([&](T& slot) {
            std::construct_at(&slot, std::forward<Args>(args)...);
        });
    }

    /**
     * @brief 尝试写入数据
     *
     * @param data 源数据 (左值或右值)
     * @return true 成功; false 队列已满
     */
    template <typename U>
        requires std::is_assignable_v<T&, U>
    [[nodiscard]] bool try_push(U&& data) noexcept {
        return try_produce([&](T& slot) { slot = std::forward<U>(data); });
    }

    /**
     * @brief 阻塞式零拷贝写入 (Visitor 模式)
     * 自旋直到有空间可用，然后执行 writer 回调。
     */
    template <typename F>
        requires std::invocable<F, T&>
    void produce(F&& writer) noexcept {
        while (!try_produce(std::forward<F>(writer))) {
            cpu_relax();
        }
    }

    /**
     * @brief 阻塞式写入 (自旋等待)
     */
    template <typename U>
        requires std::is_assignable_v<T&, U>
    void push(U&& data) noexcept {
        while (!try_push(std::forward<U>(data))) {
            cpu_relax();
        }
    }

    /**
     * @brief 阻塞式原地构造
     */
    template <typename... Args>
    void emplace(Args&&... args) noexcept {
        while (!try_emplace(std::forward<Args>(args)...)) {
            cpu_relax();
        }
    }

    // ===========================================================================
    // Reader 操作
    // ===========================================================================

    /**
     * @brief：尝试零拷贝消费 (Visitor 模式)
     *
     * @tparam F 回调类型，签名应为 void(T& data)
     * @param visitor 访问数据的回调
     * @return true 成功; false 队列为空
     */
    template <typename F>
        requires std::invocable<F, T&>
    [[nodiscard]] bool try_consume(F&& visitor) noexcept {
        // 1. 获取本地读取索引 (Relaxed)
        const size_t head = reader_.head_.load(std::memory_order_relaxed);

        // 2. 快速路径：使用影子索引检查是否有数据
        if (reader_.shadow_tail_ == head) {
            // 3. 慢速路径：影子索引认为已空，重新加载最新的全局 tail_ (Acquire)
            const size_t tail = writer_.tail_.load(std::memory_order_acquire);
            reader_.shadow_tail_ = tail;  // 更新本地缓存

            // 4. 再次检查真实状态
            if (head == tail) {
                return false;
            }
        }

        // 5. 访问缓冲区数据
        std::invoke(std::forward<F>(visitor), buffer_[head & mask_]);

        // 6. 发布新的读取索引 (Release)
        // 告知生产者该 Slot 已被消费，可以重用
        reader_.head_.store(head + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief 尝试读取数据到外部变量
     * @param out [out] 目标对象
     * @return true 成功; false 队列为空
     */
    [[nodiscard]] bool try_pop(T& out) noexcept {
        return try_consume([&out](T& data) { out = std::move(data); });
    }

    /**
     * @brief 尝试读取并返回可选值
     * @return std::optional 包含数据或空
     */
    [[nodiscard]] std::optional<T> try_pop() noexcept {
        std::optional<T> res;
        if (try_consume([&](T& data) { res.emplace(std::move(data)); })) {
            return res;
        }
        return std::nullopt;
    }

    /**
     * @brief 阻塞式消费
     */
    template <typename F>
    void consume(F&& visitor) noexcept {
        while (!try_consume(std::forward<F>(visitor))) {
            cpu_relax();
        }
    }

    /**
     * @brief 阻塞式读取到外部变量
     * 自旋直到有数据可用
     */
    void pop(T& out) noexcept {
        consume([&out](T& data) { out = std::move(data); });
    }

    /**
     * @brief 阻塞式读取
     */
    [[nodiscard]] T pop() noexcept {
        T res;
        consume([&res](T& data) { res = std::move(data); });
        return res;
    }

    // ===========================================================================
    // 状态查询
    // ===========================================================================

    /// 获取当前队列中的元素数量 (估计值)
    [[nodiscard]] size_t size() const noexcept {
        auto tail = writer_.tail_.load(std::memory_order_relaxed);
        auto head = reader_.head_.load(std::memory_order_relaxed);
        return tail - head;
    }

    /// 检查队列是否为空
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    /// 检查队列是否已满
    [[nodiscard]] bool full() const noexcept { return size() >= Capacity; }

    /// 获取队列固定容量
    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return Capacity;
    }
};

}  // namespace eph::containers
