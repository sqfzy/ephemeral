#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <functional>
#include <optional>
#include <span>

#include "eph/base/concepts.hpp"
#include "eph/utils/alignment.hpp"
#include "eph/utils/cpu.hpp"

namespace eph::containers {

using eph::base::TrivialData;
using eph::utils::Align;
using eph::utils::cpu_relax;

/**
 * @brief SPSC 无锁不可丢弃队列
 *
 * 内存布局：
 * ```
 * ┌─────────────────────────────────────────────────┐
 * │ Writer Hot Zone (独占 Cache Line)               │
 * │  - writer_.tail_         (全局写入索引, 原子)   │
 * │  - writer_.shadow_head_  (本地影子读取索引)     │
 * ├─────────────────────────────────────────────────┤
 * │ Reader Hot Zone (独占 Cache Line)               │
 * │  - reader_.head_         (全局读取索引, 原子)   │
 * │  - reader_.shadow_tail_  (本地影子写入索引)     │
 * ├─────────────────────────────────────────────────┤
 * │ Slots Zone (数据存储区)                         │
 * │  - buffer_[0..Capacity-1]                       │
 * └─────────────────────────────────────────────────┘
 * ```
 *
 * @note 阻塞接口 (push/pop/produce/consume) 使用纯 cpu_relax() 自旋，
 *       适用于 CPU-pinned 线程间短暂拥塞场景。若队列可能长期满/空，
 *       应使用 try_ 系列接口配合自定义退避策略。
 *
 * @tparam T 数据类型，必须满足 TriviallyCopyable 概念。
 * @tparam Capacity 缓冲区容量，必须是 2 的幂。
 */
template <typename T, size_t Capacity>
    requires TrivialData<T>
class BoundedQueue {
    static_assert(std::has_single_bit(Capacity), "Capacity must be power of 2");
    static_assert(std::atomic<size_t>::is_always_lock_free,
                  "BoundedQueue requires lock-free std::atomic<size_t>");
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

    static_assert(sizeof(WriterLine) <= Align<T>,
                  "WriterLine exceeds cache line size");
    static_assert(sizeof(ReaderLine) <= Align<T>,
                  "ReaderLine exceeds cache line size");

    /// 核心数据存储区
    alignas(Align<T>) std::array<T, Capacity> buffer_{};

   public:
    BoundedQueue() noexcept = default;
    ~BoundedQueue() noexcept = default;

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;
    BoundedQueue(BoundedQueue&&) = delete;
    BoundedQueue& operator=(BoundedQueue&&) = delete;

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
        const size_t tail = writer_.tail_.load(std::memory_order_relaxed);

        if (tail - writer_.shadow_head_ >= Capacity) {
            const size_t head = reader_.head_.load(std::memory_order_acquire);
            writer_.shadow_head_ = head;

            if (tail - head >= Capacity) {
                return false;
            }
        }

        std::invoke(std::forward<F>(writer_func), buffer_[tail & mask_]);

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
     * @brief 批量尝试写入 (全部或全不语义)
     *
     * 一次提交 N 个元素，仅需单次 atomic store 发布索引，
     * 摊销了逐个 try_push 的原子操作开销。
     *
     * @param data 待写入的元素序列
     * @return true 全部写入成功; false 剩余空间不足，无任何写入
     */
    [[nodiscard]] bool try_push_n(std::span<const T> data) noexcept {
        const size_t n = data.size();
        if (n == 0) return true;

        const size_t tail = writer_.tail_.load(std::memory_order_relaxed);

        // 检查是否有足够的连续空间
        size_t available = Capacity - (tail - writer_.shadow_head_);
        if (available < n) {
            const size_t head = reader_.head_.load(std::memory_order_acquire);
            writer_.shadow_head_ = head;
            available = Capacity - (tail - head);
            if (available < n) {
                return false;
            }
        }

        // 批量写入数据
        for (size_t i = 0; i < n; ++i) {
            buffer_[(tail + i) & mask_] = data[i];
        }

        // 单次 release store 发布所有元素
        writer_.tail_.store(tail + n, std::memory_order_release);
        return true;
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
        requires std::is_constructible_v<T, Args...>
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
        const size_t head = reader_.head_.load(std::memory_order_relaxed);

        if (reader_.shadow_tail_ == head) {
            const size_t tail = writer_.tail_.load(std::memory_order_acquire);
            reader_.shadow_tail_ = tail;

            if (head == tail) {
                return false;
            }
        }

        std::invoke(std::forward<F>(visitor), buffer_[head & mask_]);

        reader_.head_.store(head + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief 尝试读取数据到外部变量
     * @param out [out] 目标对象
     * @return true 成功; false 队列为空
     */
    [[nodiscard]] bool try_pop(T& out) noexcept {
        return try_consume([&out](T& data) { out = data; });
    }

    /**
     * @brief 尝试读取并返回可选值
     * @return std::optional 包含数据或空
     */
    [[nodiscard]] std::optional<T> try_pop() noexcept {
        std::optional<T> res;
        if (try_consume([&](T& data) { res.emplace(data); })) {
            return res;
        }
        return std::nullopt;
    }

    /**
     * @brief 批量尝试读取 (尽力而为语义)
     *
     * 一次消费最多 min(available, out.size()) 个元素，仅需单次 atomic store 发布索引。
     *
     * @param out 输出缓冲区
     * @return 实际读取的元素数量（0 表示队列为空）
     */
    [[nodiscard]] size_t try_pop_n(std::span<T> out) noexcept {
        const size_t max_n = out.size();
        if (max_n == 0) return 0;

        const size_t head = reader_.head_.load(std::memory_order_relaxed);

        // 计算可用元素数量
        size_t available = reader_.shadow_tail_ - head;
        if (available == 0) {
            const size_t tail = writer_.tail_.load(std::memory_order_acquire);
            reader_.shadow_tail_ = tail;
            available = tail - head;
            if (available == 0) {
                return 0;
            }
        }

        const size_t n = (available < max_n) ? available : max_n;

        // 批量读取数据
        for (size_t i = 0; i < n; ++i) {
            out[i] = buffer_[(head + i) & mask_];
        }

        // 单次 release store 发布所有消费
        reader_.head_.store(head + n, std::memory_order_release);
        return n;
    }

    /**
     * @brief 阻塞式消费
     */
    template <typename F>
        requires std::invocable<F, T&>
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
        consume([&out](T& data) { out = data; });
    }

    /**
     * @brief 阻塞式读取
     */
    [[nodiscard]] T pop() noexcept {
        T res;
        consume([&res](T& data) { res = data; });
        return res;
    }

    // ===========================================================================
    // 状态查询
    // ===========================================================================

    /// 丢弃所有排队中的元素，将队列重置为空状态。
    ///
    /// @warning 仅在确保无并发读写时调用（例如重连阶段、初始化前后）。
    ///          在 SPSC 热路径中调用此方法是未定义行为。
    void clear() noexcept {
        // 将 head 追赶到 tail，等效于消费所有元素但不执行任何回调。
        const size_t tail = writer_.tail_.load(std::memory_order_acquire);
        reader_.head_.store(tail, std::memory_order_release);
        // 刷新影子索引，使后续 try_produce/try_consume 看到一致状态
        writer_.shadow_head_ = tail;
        reader_.shadow_tail_ = tail;
    }

    /// 获取当前队列中的元素数量（估计值，仅供监控/调试，不保证跨线程一致性）
    [[nodiscard]] size_t size() const noexcept {
        auto tail = writer_.tail_.load(std::memory_order_relaxed);
        auto head = reader_.head_.load(std::memory_order_relaxed);
        return tail - head;
    }

    /// 检查队列是否为空（估计值）
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    /// 检查队列是否已满（估计值）
    [[nodiscard]] bool full() const noexcept { return size() >= Capacity; }

    /// 获取队列固定容量
    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return Capacity;
    }
};

}  // namespace eph::containers
