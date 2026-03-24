#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <string>

#include "eph/containers/concepts.hpp"
#include "eph/utils/alignment.hpp"
#include "eph/utils/cpu.hpp"

namespace eph::containers {

using eph::utils::Align;
using eph::utils::cpu_relax;

// ---------------------------------------------------------------------------
// Standalone Stats type (enables std::formatter specialization)
// ---------------------------------------------------------------------------

/// Queue statistics snapshot for monitoring/debugging.
/// All values are approximate (relaxed memory order reads).
/// Defined as a standalone type (rather than nested in the template) to enable
/// std::formatter specialization — nested types in class templates cannot be
/// specialized without knowing the template arguments.
struct BoundedQueueStats {
    size_t total_pushed;   ///< Total items ever pushed (monotonic)
    size_t total_popped;   ///< Total items ever popped (monotonic)
    size_t current_size;   ///< Approximate current occupancy
    size_t capacity;       ///< Fixed capacity

    /// Multi-line formatted dump for logging/debugging.
    [[nodiscard]] std::string dump() const {
        double utilization = capacity > 0
            ? static_cast<double>(current_size) * 100.0 / static_cast<double>(capacity)
            : 0.0;
        return std::format(
            "BoundedQueue::Stats:\n"
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
    /// Usage: auto delta = stats_t2 - stats_t1;
    [[nodiscard]] friend BoundedQueueStats operator-(const BoundedQueueStats& lhs,
                                                     const BoundedQueueStats& rhs) noexcept {
        return BoundedQueueStats{
            .total_pushed = lhs.total_pushed - rhs.total_pushed,
            .total_popped = lhs.total_popped - rhs.total_popped,
            .current_size = lhs.current_size,  // point-in-time, not diffable
            .capacity     = lhs.capacity,
        };
    }

    [[nodiscard]] friend bool operator==(const BoundedQueueStats&,
                                          const BoundedQueueStats&) = default;
};

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
    static_assert(Capacity >= 1 && std::has_single_bit(Capacity),
                  "Capacity must be a power of 2 (1, 2, 4, 8, ...)");
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
     * @brief 批量零拷贝写入 (Visitor 模式)
     *
     * 预留 n 个连续槽位，依次对每个槽位调用 writer(slot, index)，
     * 最后以单次 release store 原子发布所有元素。
     * 避免构造临时数组再 try_push_n，消除热路径堆分配。
     *
     * @param n      要写入的元素个数
     * @param writer 回调 void(T& slot, size_t index)，index 为 0..n-1
     * @return true 全部写入成功; false 剩余空间不足，无任何写入
     */
    template <typename F>
        requires std::invocable<F, T&, size_t>
    [[nodiscard]] bool try_produce_n(size_t n, F&& writer) noexcept {
        if (n == 0) return true;

        const size_t tail = writer_.tail_.load(std::memory_order_relaxed);

        size_t available = Capacity - (tail - writer_.shadow_head_);
        if (available < n) {
            const size_t head = reader_.head_.load(std::memory_order_acquire);
            writer_.shadow_head_ = head;
            available = Capacity - (tail - head);
            if (available < n) {
                return false;
            }
        }

        for (size_t i = 0; i < n; ++i) {
            std::invoke(std::forward<F>(writer),
                        buffer_[(tail + i) & mask_], i);
        }

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

    /**
     * @brief 阻塞式批量写入 (全部或全不语义)
     *
     * 自旋等待直到有足够连续空间容纳所有元素，然后以单次
     * atomic store 原子发布。适用于 CPU-pinned 线程间短暂拥塞。
     *
     * @param data 待写入的元素序列
     */
    void push_n(std::span<const T> data) noexcept {
        while (!try_push_n(data)) {
            cpu_relax();
        }
    }

    /**
     * @brief 阻塞式批量零拷贝写入 (Visitor 模式)
     *
     * 自旋等待直到有足够连续空间，然后依次对每个槽位调用
     * writer(slot, index)，最后以单次 release store 发布。
     *
     * @param n      要写入的元素个数
     * @param writer 回调 void(T& slot, size_t index)，index 为 0..n-1
     */
    template <typename F>
        requires std::invocable<F, T&, size_t>
    void produce_n(size_t n, F&& writer) noexcept {
        while (!try_produce_n(n, std::forward<F>(writer))) {
            cpu_relax();
        }
    }

    // ===========================================================================
    // Writer 带超时操作
    // ===========================================================================

    /**
     * @brief 带超时的零拷贝写入 (Visitor 模式)
     *
     * 自旋等待直到有空间可用或超时。适用于非 CPU-pinned 线程，
     * 需要在"立即返回"和"无限阻塞"之间取得平衡的场景。
     *
     * @param writer_func 用于初始化数据的回调
     * @param timeout     最大等待时间
     * @return true 写入成功; false 超时
     */
    template <typename F, typename Rep, typename Period>
        requires std::invocable<F, T&>
    [[nodiscard]] bool try_produce_for(F&& writer_func,
                                       std::chrono::duration<Rep, Period> timeout) noexcept {
        if (try_produce(std::forward<F>(writer_func))) return true;
        auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            cpu_relax();
            if (try_produce(std::forward<F>(writer_func))) return true;
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    /**
     * @brief 带超时的写入
     * @return true 写入成功; false 超时
     */
    template <typename U, typename Rep, typename Period>
        requires std::is_assignable_v<T&, U>
    [[nodiscard]] bool try_push_for(U&& data,
                                     std::chrono::duration<Rep, Period> timeout) noexcept {
        return try_produce_for([&](T& slot) { slot = std::forward<U>(data); }, timeout);
    }

    /**
     * @brief 带超时的原地构造
     * @return true 成功; false 超时
     */
    template <typename Rep, typename Period, typename... Args>
        requires std::is_constructible_v<T, Args...>
    [[nodiscard]] bool try_emplace_for(std::chrono::duration<Rep, Period> timeout,
                                       Args&&... args) noexcept {
        return try_produce_for(
            [&](T& slot) { std::construct_at(&slot, std::forward<Args>(args)...); },
            timeout);
    }

    /**
     * @brief 带超时的批量写入 (全部或全不语义)
     *
     * 自旋等待直到有足够连续空间容纳所有元素或超时。
     * 成功时以单次 atomic store 原子发布所有元素。
     *
     * @param data    待写入的元素序列
     * @param timeout 最大等待时间
     * @return true 全部写入成功; false 超时，无任何写入
     */
    template <typename Rep, typename Period>
    [[nodiscard]] bool try_push_n_for(std::span<const T> data,
                                       std::chrono::duration<Rep, Period> timeout) noexcept {
        if (try_push_n(data)) return true;
        auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            cpu_relax();
            if (try_push_n(data)) return true;
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    /**
     * @brief 带超时的批量零拷贝写入 (Visitor 模式)
     *
     * 自旋等待直到有足够连续空间或超时。成功时预留 n 个槽位，
     * 依次调用 writer(slot, index)，最后以单次 release store 发布。
     *
     * @param n       要写入的元素个数
     * @param writer  回调 void(T& slot, size_t index)，index 为 0..n-1
     * @param timeout 最大等待时间
     * @return true 全部写入成功; false 超时，无任何写入
     */
    template <typename F, typename Rep, typename Period>
        requires std::invocable<F, T&, size_t>
    [[nodiscard]] bool try_produce_n_for(size_t n, F&& writer,
                                          std::chrono::duration<Rep, Period> timeout) noexcept {
        if (try_produce_n(n, std::forward<F>(writer))) return true;
        auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            cpu_relax();
            if (try_produce_n(n, std::forward<F>(writer))) return true;
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
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
     * @brief 零拷贝查看队首元素但不消费 (Visitor 模式, Reader 线程专用)
     *
     * 对队首元素原地调用 visitor，但不推进 head 指针。适用于消费前需要
     * 预检查消息类型或决定路由逻辑的场景，避免不必要的拷贝开销。
     *
     * @tparam F 回调类型，签名应为 void(const T& data)
     * @param visitor 访问数据的回调（接收 const 引用，不可修改）
     * @return true 队列非空且已回调; false 队列为空
     *
     * @note 仅 Reader 线程可调用。多次调用访问同一元素，
     *       直到 try_pop/consume 推进 head。
     */
    template <typename F>
        requires std::invocable<F, const T&>
    [[nodiscard]] bool try_peek(F&& visitor) noexcept {
        const size_t head = reader_.head_.load(std::memory_order_relaxed);

        if (reader_.shadow_tail_ == head) {
            const size_t tail = writer_.tail_.load(std::memory_order_acquire);
            reader_.shadow_tail_ = tail;

            if (head == tail) {
                return false;
            }
        }

        std::invoke(std::forward<F>(visitor),
                    std::as_const(buffer_[head & mask_]));
        // Do NOT advance head — element stays in the queue
        return true;
    }

    /**
     * @brief 查看队首元素但不消费 (Reader 线程专用)
     *
     * 读取队首元素的拷贝但不推进 head 指针。适用于消费前需要
     * 预检查消息类型或决定路由逻辑的场景。
     *
     * @param out [out] 目标对象（队列非空时写入）
     * @return true 队列非空且已复制; false 队列为空
     *
     * @note 仅 Reader 线程可调用。多次调用返回同一元素，
     *       直到 try_pop/consume 推进 head。
     */
    [[nodiscard]] bool try_peek(T& out) noexcept {
        return try_peek([&out](const T& data) { out = data; });
    }

    /**
     * @brief 查看队首元素并返回可选值 (Reader 线程专用)
     * @return std::optional 包含数据（非空时）或空
     */
    [[nodiscard]] std::optional<T> try_peek() noexcept {
        T val;
        if (try_peek(val)) return val;
        return std::nullopt;
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
     * @brief 批量零拷贝消费 (Visitor 模式, 尽力而为语义)
     *
     * 消费最多 min(n, available) 个元素，对每个元素原地调用
     * visitor(slot, index)，最后以单次 release store 发布所有消费。
     * 与 try_produce_n 镜像对称——写端批量生产，读端批量消费，均为零拷贝。
     *
     * @param n       最大消费数量
     * @param visitor 回调 void(T& slot, size_t index)，index 为 0..consumed-1
     * @return 实际消费的元素数量（0 表示队列为空）
     */
    template <typename F>
        requires std::invocable<F, T&, size_t>
    [[nodiscard]] size_t try_consume_n(size_t n, F&& visitor) noexcept {
        if (n == 0) return 0;

        const size_t head = reader_.head_.load(std::memory_order_relaxed);

        size_t available = reader_.shadow_tail_ - head;
        if (available == 0) {
            const size_t tail = writer_.tail_.load(std::memory_order_acquire);
            reader_.shadow_tail_ = tail;
            available = tail - head;
            if (available == 0) {
                return 0;
            }
        }

        const size_t count = (available < n) ? available : n;

        for (size_t i = 0; i < count; ++i) {
            std::invoke(std::forward<F>(visitor),
                        buffer_[(head + i) & mask_], i);
        }

        reader_.head_.store(head + count, std::memory_order_release);
        return count;
    }

    /**
     * @brief 阻塞式批量零拷贝消费 (Visitor 模式)
     *
     * 自旋等待直到至少有一个元素可消费，然后批量消费最多 n 个元素。
     *
     * @param n       最大消费数量
     * @param visitor 回调 void(T& slot, size_t index)
     * @return 实际消费的元素数量（>= 1）
     */
    template <typename F>
        requires std::invocable<F, T&, size_t>
    size_t consume_n(size_t n, F&& visitor) noexcept {
        size_t count;
        while ((count = try_consume_n(n, std::forward<F>(visitor))) == 0) {
            cpu_relax();
        }
        return count;
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
    // Reader 带超时操作
    // ===========================================================================

    /**
     * @brief 带超时的零拷贝消费 (Visitor 模式)
     *
     * 自旋等待直到有数据可用或超时。
     *
     * @param visitor 访问数据的回调
     * @param timeout 最大等待时间
     * @return true 消费成功; false 超时
     */
    template <typename F, typename Rep, typename Period>
        requires std::invocable<F, T&>
    [[nodiscard]] bool try_consume_for(F&& visitor,
                                       std::chrono::duration<Rep, Period> timeout) noexcept {
        if (try_consume(std::forward<F>(visitor))) return true;
        auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            cpu_relax();
            if (try_consume(std::forward<F>(visitor))) return true;
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    /**
     * @brief 带超时的读取到外部变量
     * @return true 读取成功; false 超时
     */
    template <typename Rep, typename Period>
    [[nodiscard]] bool try_pop_for(T& out,
                                    std::chrono::duration<Rep, Period> timeout) noexcept {
        return try_consume_for([&out](T& data) { out = data; }, timeout);
    }

    /**
     * @brief 带超时的读取并返回可选值
     * @return std::optional 包含数据（成功时）或空（超时时）
     */
    template <typename Rep, typename Period>
    [[nodiscard]] std::optional<T> try_pop_for(
        std::chrono::duration<Rep, Period> timeout) noexcept {
        std::optional<T> res;
        (void)try_consume_for([&](T& data) { res.emplace(data); }, timeout);
        return res;
    }

    /**
     * @brief 带超时的批量读取 (尽力而为语义)
     *
     * 等待至少一个元素可用（或超时），然后一次消费最多 out.size() 个元素。
     * 与 try_pop_n 不同，此方法在队列暂时为空时会自旋等待而非立即返回 0。
     *
     * @param out     输出缓冲区
     * @param timeout 最大等待时间
     * @return 实际读取的元素数量（0 表示超时且队列为空）
     */
    template <typename Rep, typename Period>
    [[nodiscard]] size_t try_pop_n_for(std::span<T> out,
                                        std::chrono::duration<Rep, Period> timeout) noexcept {
        size_t n = try_pop_n(out);
        if (n > 0) return n;
        auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            cpu_relax();
            n = try_pop_n(out);
            if (n > 0) return n;
        } while (std::chrono::steady_clock::now() < deadline);
        return 0;
    }

    /**
     * @brief 带超时的批量零拷贝消费 (Visitor 模式, 尽力而为语义)
     *
     * 等待至少一个元素可用（或超时），然后批量消费最多 n 个元素，
     * 对每个元素原地调用 visitor(slot, index)。
     *
     * @param n       最大消费数量
     * @param visitor 回调 void(T& slot, size_t index)
     * @param timeout 最大等待时间
     * @return 实际消费的元素数量（0 表示超时且队列为空）
     */
    template <typename F, typename Rep, typename Period>
        requires std::invocable<F, T&, size_t>
    [[nodiscard]] size_t try_consume_n_for(size_t n, F&& visitor,
                                            std::chrono::duration<Rep, Period> timeout) noexcept {
        size_t count = try_consume_n(n, std::forward<F>(visitor));
        if (count > 0) return count;
        auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            cpu_relax();
            count = try_consume_n(n, std::forward<F>(visitor));
            if (count > 0) return count;
        } while (std::chrono::steady_clock::now() < deadline);
        return 0;
    }

    /**
     * @brief 消费当前队列中所有可用元素
     *
     * 等效于 try_consume_n(Capacity, visitor)，但语义更清晰。
     * 适用于关机清空、批量处理等场景。
     *
     * @tparam F 回调类型，签名应为 void(T& slot, size_t index)
     * @param visitor 回调函数，index 为 0..consumed-1
     * @return 实际消费的元素数量（0 表示队列为空）
     */
    template <typename F>
        requires std::invocable<F, T&, size_t>
    [[nodiscard]] size_t try_consume_all(F&& visitor) noexcept {
        return try_consume_n(Capacity, std::forward<F>(visitor));
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

    /// 估计剩余可写入空间（Capacity - size()）。
    /// 适用于批量写入前预检查：if (q.available_write() >= n) q.try_push_n(...)
    /// @note 估计值，不保证跨线程一致性。
    [[nodiscard]] size_t available_write() const noexcept {
        size_t used = size();
        return (used < Capacity) ? (Capacity - used) : 0;
    }

    /// 估计可读取的元素数量（等价于 size()，语义别名）。
    /// @note 估计值，不保证跨线程一致性。
    [[nodiscard]] size_t available_read() const noexcept {
        return size();
    }

    // ===========================================================================
    // 可观测性
    // ===========================================================================

    /// Alias for the standalone BoundedQueueStats type.
    using Stats = BoundedQueueStats;

    /// Take a point-in-time statistics snapshot.
    /// Zero overhead — derived from existing atomic indices, no extra counters.
    [[nodiscard]] Stats stats() const noexcept {
        auto tail = writer_.tail_.load(std::memory_order_relaxed);
        auto head = reader_.head_.load(std::memory_order_relaxed);
        return Stats{
            .total_pushed = tail,
            .total_popped = head,
            .current_size = tail - head,
            .capacity     = Capacity,
        };
    }
};

}  // namespace eph::containers

// std::formatter specialization for BoundedQueueStats
template <>
struct std::formatter<eph::containers::BoundedQueueStats>
    : std::formatter<std::string> {
    auto format(const eph::containers::BoundedQueueStats& s,
                std::format_context& ctx) const {
        return std::formatter<std::string>::format(s.dump(), ctx);
    }
};
