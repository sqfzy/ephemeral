#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <concepts>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

#include "eph/containers/concepts.hpp"
#include "eph/utils/alignment.hpp"
#include "eph/utils/cpu.hpp"

namespace eph::containers {

using eph::utils::CACHE_LINE_SIZE;
using eph::utils::Align;
using eph::utils::cpu_relax;

// ---------------------------------------------------------------------------
// Standalone Stats type (enables std::formatter specialization)
// ---------------------------------------------------------------------------

/// Queue statistics snapshot for monitoring/debugging.
/// Defined as a standalone type (rather than nested in the template) to enable
/// std::formatter specialization — nested types in class templates cannot be
/// specialized without knowing the template arguments.
struct EvictingQueueStats {
    uint64_t total_pushed;       ///< Total writes since construction
    uint64_t total_popped;       ///< Total successful reads
    uint64_t overwritten;        ///< Approximate overwrites (data loss)
    size_t   current_size;       ///< Approximate unread entries
    size_t   capacity;           ///< Fixed capacity

    /// Multi-line formatted dump for logging/debugging.
    [[nodiscard]] std::string dump() const {
        double loss_rate = total_pushed > 0
            ? static_cast<double>(overwritten) * 100.0 / static_cast<double>(total_pushed)
            : 0.0;
        return std::format(
            "EvictingQueue::Stats:\n"
            "  capacity: {}\n"
            "  current_size: {}\n"
            "  total_pushed: {}\n"
            "  total_popped: {}\n"
            "  overwritten: {} ({:.1f}% loss)",
            capacity, current_size,
            total_pushed, total_popped,
            overwritten, loss_rate);
    }

    /// JSON-formatted stats for monitoring system integration.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"capacity\":{},\"current_size\":{},\"total_pushed\":{},"
            "\"total_popped\":{},\"overwritten\":{}}}",
            capacity, current_size, total_pushed, total_popped, overwritten);
    }

    /// Compute delta between two snapshots for interval-based monitoring.
    [[nodiscard]] friend EvictingQueueStats operator-(const EvictingQueueStats& lhs,
                                                      const EvictingQueueStats& rhs) noexcept {
        return EvictingQueueStats{
            .total_pushed = lhs.total_pushed - rhs.total_pushed,
            .total_popped = lhs.total_popped - rhs.total_popped,
            .overwritten  = lhs.overwritten - rhs.overwritten,
            .current_size = lhs.current_size,  // point-in-time, not diffable
            .capacity     = lhs.capacity,
        };
    }

    [[nodiscard]] friend bool operator==(const EvictingQueueStats&,
                                          const EvictingQueueStats&) = default;
};

/**
 * @brief 多缓冲顺序锁可丢弃 SPSC 队列
 *
 * 特性：
 * - Writer Wait-free: 写入者永远不会阻塞，队列满时直接覆盖旧数据。
 * - Reader Lock-free: 读取者通过乐观读取，获取最新数据。
 *
 * 内存布局：
 * ```
 * ┌─────────────────────────────────────────────────┐
 * │ Global Zone (读写共享 Cache Line)               │
 * │  - global_index_  (全局索引)                    │
 * ├─────────────────────────────────────────────────┤
 * │ Writer Hot Zone (独占 Cache Line)               │
 * │  - writer_.index_  (本地影子索引)               │
 * ├─────────────────────────────────────────────────┤
 * │ Reader Hot Zone (独占 Cache Line)               │
 * │  - reader_.index_  (本地缓存索引)               │
 * ├─────────────────────────────────────────────────┤
 * │ Slots Zone (数据存储区)                         │
 * │  - slots_[0..N-1]                               │
 * └─────────────────────────────────────────────────┘
 * ```
 *
 * PERF 注释测量条件：payload = { uint64_t id; double data[3]; } (32B),
 * buffer size = 8 slots, x86-64 (Intel i7), 两核 pinned.
 *
 * @tparam T 数据类型 (必须满足 TriviallyCopyable 概念)
 * @tparam Capacity 缓冲槽位数量，必须是 2 的幂。
 */
template <typename T, size_t Capacity = 8>
    requires TrivialData<T>
class alignas(Align<T>) EvictingQueue {
    static_assert(std::has_single_bit(Capacity), "Capacity must be power of 2");
    static_assert(Capacity > 1, "Primary template requires Capacity > 1");
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
                  "EvictingQueue requires lock-free std::atomic<uint64_t>");

   private:
    // ---- seq 编码/解码辅助 ----
    // seq 低 1 位为 lock flag，高 63 位为 global index
    static constexpr uint64_t encode_seq(uint64_t idx, bool locked) noexcept {
        return (idx << 1) | static_cast<uint64_t>(locked);
    }
    static constexpr uint64_t decode_idx(uint64_t seq) noexcept { return seq >> 1; }
    static constexpr bool is_locked(uint64_t seq) noexcept { return seq & 1; }

    // 全局索引/全局序列号。每生产一个数据就+1。
    alignas(Align<T>) std::atomic<uint64_t> global_index_{0};

    // ---------------------------------------------------------------------------
    // Writer 独占区
    // ---------------------------------------------------------------------------
    struct alignas(Align<T>) WriterLine {
        // global_index_ 影子索引，省去一次原子读取。
        uint64_t shadow_global_index_{0};
    } writer_;

    // ---------------------------------------------------------------------------
    // Reader 独占区
    // ---------------------------------------------------------------------------
    struct alignas(Align<T>) ReaderLine {
        // global_index_ 本地缓存，用于检查是否有新数据。
        uint64_t last_global_index_{0};
    } reader_;

    // ---------------------------------------------------------------------------
    // 核心存储区
    // ---------------------------------------------------------------------------
    struct alignas(Align<T>) Slot {
        // 内存布局 (64 bits):
        // [ 63 ............................ 1 |   0   ]
        // [      Global Index (高 63 位)      | flag ]
        //
        // Global Index：最近一次写该槽位时的
        // global_index_，用于验证数据的新旧。 flag = 1 表示 Writer
        // 正在写入（锁定）；0 表示空闲。
        std::atomic<uint64_t> seq_{0};
        T data_{};
    };
    static_assert(sizeof(Slot) <= Align<T> || sizeof(Slot) % CACHE_LINE_SIZE == 0,
                  "Slot size may cause false sharing");
    alignas(Align<T>) std::array<Slot, Capacity> slots_{};

   public:
    EvictingQueue() = default;
    ~EvictingQueue() = default;

    EvictingQueue(const EvictingQueue&) = delete;
    EvictingQueue& operator=(const EvictingQueue&) = delete;
    EvictingQueue(EvictingQueue&&) = delete;
    EvictingQueue& operator=(EvictingQueue&&) = delete;

    // ===========================================================================
    // Writer 操作
    // ===========================================================================

    /**
     * @brief 零拷贝写入 (Visitor 模式)
     *
     * @tparam F 回调类型，签名应为 void(T& data)
     * @param writer_func 用于在 Slot 原位初始化或修改数据的回调函数
     */
    template <typename F>
        requires std::invocable<F, T&>
    void produce(F&& writer_func) noexcept {
        // 1. 获取下一个写入位置 (使用本地 Shadow Index)
        const uint64_t current_idx = writer_.shadow_global_index_;
        const uint64_t next_idx = current_idx + 1;  // PERF: 6.70%

        Slot& s = slots_[next_idx & (Capacity - 1)];

        // 2. 锁定槽位 (seq = odd, 表示写入中)
        // Happens-before 链起点：seq(odd) 告知 reader 此 slot 不可读
        s.seq_.store(encode_seq(next_idx, true), std::memory_order_relaxed);

        // 3. Store-Store Fence: seq(odd) ─hb→ data writes
        // 保证 reader 看到 seq 变奇后，后续读到的 data 要么是旧的要么是新的，
        // 不会读到 data 的中间态而 seq 仍为偶数
        std::atomic_thread_fence(std::memory_order_release);

        // 4. 执行数据写入/修改
        // PERF: ~39.19%
        std::invoke(std::forward<F>(writer_func), s.data_);

        // 5. Store-Store Fence: data writes ─hb→ seq(even)
        // 保证 data 完全写入后才发布 seq(even)，reader 读到偶数 seq 时 data 已一致
        std::atomic_thread_fence(std::memory_order_release);

        // 6. 解锁槽位 (seq = even, 表示空闲)
        // PERF: 17.68%
        s.seq_.store(encode_seq(next_idx, false), std::memory_order_relaxed);

        // 7. 发布全局索引: seq(even) ─hb→ global_index
        // Reader acquire global_index 后，能看到对应 slot 的 seq(even) 和完整 data
        // PERF: 17.68%
        global_index_.store(next_idx, std::memory_order_release);

        // 8. 更新本地影子索引
        // PERF: 15.68%
        writer_.shadow_global_index_ = next_idx;
    }

    /**
     * @brief 写入新数据 (Copy/Move)
     */
    template <typename U>
        requires std::is_assignable_v<T&, U>
    void push(U&& val) noexcept {
        produce([&](T& slot) { slot = std::forward<U>(val); });
    }

    /**
     * @brief 原地构造写入 (Emplace)
     */
    /// @note TrivialData 蕴含 trivially_destructible，无需 destroy_at
    template <typename... Args>
        requires std::is_constructible_v<T, Args...>
    void emplace(Args&&... args) noexcept {
        produce([&](T& slot) {
            std::construct_at(&slot, std::forward<Args>(args)...);
        });
    }

    /// Batch write N items using a visitor pattern.
    ///
    /// Calls visitor(T& slot, size_t index) for each of the N items.
    /// Each write is individually sequenced (wait-free, overwrites oldest).
    /// The reader will see all N items after this call returns.
    ///
    /// @param count    Number of items to write
    /// @param visitor  Callable: void(T& slot, size_t index)
    template <typename F>
        requires std::invocable<F, T&, size_t>
    void produce_n(size_t count, F&& visitor) noexcept {
        for (size_t i = 0; i < count; ++i) {
            produce([&](T& slot) {
                std::invoke(std::forward<F>(visitor), slot, i);
            });
        }
    }

    /// Batch push N items from a span.
    void push_n(std::span<const T> items) noexcept {
        produce_n(items.size(), [&](T& slot, size_t i) {
            slot = items[i];
        });
    }

    // ===========================================================================
    // Reader 操作
    // ===========================================================================

    /**
     * @brief 核心：尝试零拷贝读取 (Visitor Pattern)
     *
     * @return true 读取成功; false 数据脏 (发生竞争)
     */
    template <typename F>
        requires std::invocable<F, const T&>
    [[nodiscard]] bool try_consume_latest(F&& visitor) noexcept {
        // 1. 获取当前最新的全局索引 (Acquire)
        uint64_t idx =
            global_index_.load(std::memory_order_acquire);  // PERF: 48.83%

        if (idx <= reader_.last_global_index_) {
            return false;
        }

        const Slot& s = slots_[idx & (Capacity - 1)];

        // 2. 读取开始前的版本号 (Acquire)
        // PERF: 45.41%
        uint64_t seq1 = s.seq_.load(std::memory_order_acquire);

        // 如果版本号为奇数，说明 Writer 正在写入
        if (is_locked(seq1)) [[unlikely]]
            return false;

        // 解析出槽位当前实际存储的数据 index
        uint64_t actual_idx = decode_idx(seq1);

        // 如果实际数据的 index 小于等于上次读取的 index，说明这是已经读过的数据
        if (actual_idx <= reader_.last_global_index_) {
            return false;
        }

        // 3. 执行读取
        std::invoke(std::forward<F>(visitor), s.data_);

        // 4. Load-Load Barrier
        // 强制 CPU 保证先完成上述数据的读取，再读取下方的 seq2
        std::atomic_thread_fence(std::memory_order_acquire);

        // 5. 再次读取版本号 (Relaxed)
        uint64_t seq2 = s.seq_.load(std::memory_order_relaxed);

        if (seq1 == seq2) {
            reader_.last_global_index_ = actual_idx;
            return true;
        }

        return false;
    }

    /**
     * @brief 尝试读取最新数据 (值拷贝)
     */
    [[nodiscard]] bool try_pop_latest(T& out) noexcept {
        return try_consume_latest([&out](const T& data) { out = data; });
    }

    /**
     * @brief 尝试读取并返回可选值
     */
    [[nodiscard]] std::optional<T> try_pop_latest() noexcept {
        std::optional<T> res;
        if (try_consume_latest([&res](const T& data) { res.emplace(data); })) {
            return res;
        }
        return std::nullopt;
    }

    /**
     * @brief 零拷贝查看最新数据但不标记为已消费 (Visitor 模式, Reader 线程专用)
     *
     * 与 try_consume_latest 相同的乐观读取逻辑，但不推进
     * reader_.last_global_index_，因此后续 try_consume_latest
     * 仍会返回同一条数据。适用于需要预检查消息类型或决定
     * 路由逻辑后再决定是否消费的场景。
     *
     * @tparam F 回调类型，签名应为 void(const T&)
     * @param visitor 访问数据的回调（零拷贝）
     * @return true 成功读取; false 无新数据或读取被并发写入打断
     *
     * @note 仅 Reader 线程可调用。visitor 在 seqlock 保护区内执行，
     *       应尽量短小以减少被并发写入打断的概率。
     */
    template <typename F>
        requires std::invocable<F, const T&>
    [[nodiscard]] bool try_peek_latest(F&& visitor) noexcept {
        uint64_t idx = global_index_.load(std::memory_order_acquire);
        if (idx <= reader_.last_global_index_) return false;

        const Slot& s = slots_[idx & (Capacity - 1)];
        uint64_t seq1 = s.seq_.load(std::memory_order_acquire);
        if (is_locked(seq1)) [[unlikely]] return false;

        uint64_t actual_idx = decode_idx(seq1);
        if (actual_idx <= reader_.last_global_index_) return false;

        std::invoke(std::forward<F>(visitor), s.data_);

        std::atomic_thread_fence(std::memory_order_acquire);
        uint64_t seq2 = s.seq_.load(std::memory_order_relaxed);

        return seq1 == seq2;
        // Note: last_global_index_ is NOT advanced — element stays "unread"
    }

    /// @brief 查看最新数据但不消费 (值拷贝, Reader 线程专用)
    [[nodiscard]] bool try_peek_latest(T& out) noexcept {
        return try_peek_latest([&out](const T& data) { out = data; });
    }

    /// @brief 查看最新数据并返回可选值 (Reader 线程专用)
    [[nodiscard]] std::optional<T> try_peek_latest() noexcept {
        std::optional<T> res;
        if (try_peek_latest([&res](const T& data) { res.emplace(data); })) {
            return res;
        }
        return std::nullopt;
    }

    /**
     * @brief 带超时的零拷贝查看最新数据但不标记为已消费
     *
     * 自旋等待直到有新数据可用或超时。与 try_peek_latest 相同，
     * 不推进 reader 索引，因此后续 consume 仍可读取同一条数据。
     *
     * @param visitor 访问数据的回调 void(const T&)
     * @param timeout 最大等待时间
     * @return true 读取成功; false 超时
     */
    template <typename F, typename Rep, typename Period>
        requires std::invocable<F, const T&>
    [[nodiscard]] bool try_peek_latest_for(
        F&& visitor, std::chrono::duration<Rep, Period> timeout) noexcept {
        if (try_peek_latest(std::forward<F>(visitor))) return true;
        auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            cpu_relax();
            if (try_peek_latest(std::forward<F>(visitor))) return true;
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    /// @brief 带超时的值拷贝查看 (不消费)
    template <typename Rep, typename Period>
    [[nodiscard]] bool try_peek_latest_for(
        T& out, std::chrono::duration<Rep, Period> timeout) noexcept {
        return try_peek_latest_for(
            [&out](const T& data) { out = data; }, timeout);
    }

    /// @brief 带超时的查看并返回可选值 (不消费)
    template <typename Rep, typename Period>
    [[nodiscard]] std::optional<T> try_peek_latest_for(
        std::chrono::duration<Rep, Period> timeout) noexcept {
        std::optional<T> res;
        (void)try_peek_latest_for(
            [&res](const T& data) { res.emplace(data); }, timeout);
        return res;
    }

    /**
     * @brief 阻塞式零拷贝读取 (自旋直到成功)
     */
    template <typename F>
        requires std::invocable<F, const T&>
    void consume_latest(F&& visitor) noexcept {
        while (!try_consume_latest(std::forward<F>(visitor))) {
            cpu_relax();
        }
    }

    /**
     * @brief 阻塞式值拷贝读取 (自旋直到成功)
     */
    void pop_latest(T& out) noexcept {
        consume_latest([&out](const T& data) { out = data; });
    }

    /**
     * @brief 阻塞式读取并返回值 (自旋直到成功)
     */
    [[nodiscard]] T pop_latest() noexcept {
        T out;
        pop_latest(out);
        return out;
    }

    // ===========================================================================
    // Reader 带超时操作
    // ===========================================================================

    /**
     * @brief 带超时的零拷贝读取最新数据
     *
     * 自旋等待直到有新数据可用或超时。适用于非 CPU-pinned 线程。
     *
     * @param visitor 访问数据的回调 void(const T&)
     * @param timeout 最大等待时间
     * @return true 读取成功; false 超时
     */
    template <typename F, typename Rep, typename Period>
        requires std::invocable<F, const T&>
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
     * @brief 带超时的值拷贝读取
     * @return true 读取成功; false 超时
     */
    template <typename Rep, typename Period>
    [[nodiscard]] bool try_pop_latest_for(
        T& out, std::chrono::duration<Rep, Period> timeout) noexcept {
        return try_consume_latest_for(
            [&out](const T& data) { out = data; }, timeout);
    }

    /**
     * @brief 带超时的读取并返回可选值
     * @return std::optional 包含数据（成功时）或空（超时时）
     */
    template <typename Rep, typename Period>
    [[nodiscard]] std::optional<T> try_pop_latest_for(
        std::chrono::duration<Rep, Period> timeout) noexcept {
        std::optional<T> res;
        (void)try_consume_latest_for(
            [&res](const T& data) { res.emplace(data); }, timeout);
        return res;
    }

    // ===========================================================================
    // 状态查询
    // ===========================================================================

    /// 重置队列状态，丢弃所有未读数据。
    ///
    /// @warning 仅在确保无并发读写时调用。
    void clear() noexcept {
        // 将 reader 的 last_global_index_ 追赶到 writer 的当前位置，
        // 使后续 try_consume_latest 认为没有新数据。
        uint64_t idx = global_index_.load(std::memory_order_acquire);
        reader_.last_global_index_ = idx;
    }

    /**
     * @brief 获取缓冲区容量
     */
    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return Capacity;
    }

    /// Approximate number of unread entries (for monitoring/debugging only).
    /// The result may be stale by the time it is read in a concurrent context.
    [[nodiscard]] size_t size_approx() const noexcept {
        uint64_t written = global_index_.load(std::memory_order_relaxed);
        uint64_t read    = reader_.last_global_index_;
        // Clamp: writer may have advanced past Capacity unread entries
        uint64_t pending = (written >= read) ? (written - read) : 0;
        return static_cast<size_t>(std::min(pending, static_cast<uint64_t>(Capacity)));
    }

    /// Check if there are no unread entries (approximate).
    [[nodiscard]] bool empty() const noexcept { return size_approx() == 0; }

    /// Check if the queue is at capacity (writes are overwriting unread data).
    /// @note Approximate — suitable for monitoring, not synchronization.
    [[nodiscard]] bool full() const noexcept { return size_approx() >= Capacity; }

    /// Total number of writes performed since construction.
    /// Useful for monitoring throughput and computing discard rates.
    /// @note Relaxed load — safe to call from any thread for approximate monitoring.
    [[nodiscard]] uint64_t write_count() const noexcept {
        return global_index_.load(std::memory_order_relaxed);
    }

    /// Approximate number of writes that overwrote unread data.
    /// Useful for monitoring data loss due to slow consumers.
    /// Formula: writes - reads_consumed - buffered = lost items.
    /// @note Approximate — derived from relaxed atomic loads.
    [[nodiscard]] uint64_t overwrite_count_approx() const noexcept {
        uint64_t written = global_index_.load(std::memory_order_relaxed);
        uint64_t read = reader_.last_global_index_;
        if (written <= read + Capacity) return 0;
        return written - read - Capacity;
    }

    /// Total number of successful reads performed since construction.
    /// Useful for monitoring consumer throughput and computing discard rates
    /// (write_count - read_count = unconsumed + overwritten).
    /// @note Only accurate when called from the reader thread.
    [[nodiscard]] uint64_t read_count() const noexcept {
        return reader_.last_global_index_;
    }

    /// Alias for the standalone EvictingQueueStats type.
    using Stats = EvictingQueueStats;

    /// Take a point-in-time statistics snapshot.
    [[nodiscard]] Stats stats() const noexcept {
        return Stats{
            .total_pushed = write_count(),
            .total_popped = read_count(),
            .overwritten  = overwrite_count_approx(),
            .current_size = size_approx(),
            .capacity     = Capacity,
        };
    }
};

/**
 * @brief SPSC 顺序锁 (SeqLock) - 单槽位特化 (N=1)
 *
 * 特性：
 * - Writer Wait-free: 写入者永远不会阻塞。
 * - Reader Lock-free: 读取者通过乐观读取，获取最新数据。
 *
 * 内存布局：
 * ```
 * ┌─────────────────────────────────────────────────┐
 * │ SeqLock Zone (全局共享 Cache Line)              │
 * │ - seq_  (版本号/锁)                             │
 * ├─────────────────────────────────────────────────┤
 * │ Reader Local Zone (独占 Cache Line)             │
 * │ - last_seq_ (本地缓存版本号)                    │
 * ├─────────────────────────────────────────────────┤
 * │ │ Data Zone (独占 Cache Line)                   │
 * │ - data_  (实际数据)                             │
 * └─────────────────────────────────────────────────┘
 * ```
 *
 * @tparam T 数据类型 (必须满足 TriviallyCopyable 概念)
 */
template <typename T>
    requires TrivialData<T>
class alignas(Align<T>) EvictingQueue<T, 1> {
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
                  "SeqLock requires lock-free std::atomic<uint64_t>");

   private:
    // 偶数=空闲，奇数=正在写入
    alignas(Align<T>) std::atomic<uint64_t> seq_{0};
    alignas(Align<T>) uint64_t last_seq_{0};
    alignas(Align<T>) T data_{};

   public:
    EvictingQueue() noexcept = default;
    ~EvictingQueue() noexcept = default;

    EvictingQueue(const EvictingQueue&) = delete;
    EvictingQueue& operator=(const EvictingQueue&) = delete;
    EvictingQueue(EvictingQueue&&) = delete;
    EvictingQueue& operator=(EvictingQueue&&) = delete;

    // ===========================================================================
    // Writer
    // ===========================================================================

    /**
     * @brief 零拷贝写入 (Visitor 模式)
     *
     * @tparam F 回调类型，签名应为 void(T& data)
     * @param writer_func 用于初始化或修改数据的回调函数
     */
    template <typename F>
        requires std::invocable<F, T&>
    void produce(F&& writer_func) noexcept {
        // PERF: 39.91%
        uint64_t seq = seq_.load(std::memory_order_relaxed);

        // 1. Lock (Seq=Odd)
        seq_.store(seq + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);

        // 2. Write
        // PERF: 35.19%
        std::invoke(std::forward<F>(writer_func), data_);

        // 3. Unlock (Seq=Even)
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(seq + 2, std::memory_order_relaxed);
    }

    template <typename U>
        requires std::is_assignable_v<T&, U>
    void push(U&& val) noexcept {
        produce([&](T& slot) { slot = std::forward<U>(val); });
    }

    /// @note TrivialData 蕴含 trivially_destructible，无需 destroy_at
    template <typename... Args>
        requires std::is_constructible_v<T, Args...>
    void emplace(Args&&... args) noexcept {
        produce([&](T& slot) {
            std::construct_at(&slot, std::forward<Args>(args)...);
        });
    }

    /// Batch write N items using a visitor pattern.
    ///
    /// Calls visitor(T& slot, size_t index) for each of the N items.
    /// Each write is individually sequenced (wait-free).
    /// For Capacity=1, only the last item will be visible to the reader.
    ///
    /// @param count    Number of items to write
    /// @param visitor  Callable: void(T& slot, size_t index)
    template <typename F>
        requires std::invocable<F, T&, size_t>
    void produce_n(size_t count, F&& visitor) noexcept {
        for (size_t i = 0; i < count; ++i) {
            produce([&](T& slot) {
                std::invoke(std::forward<F>(visitor), slot, i);
            });
        }
    }

    /// Batch push N items from a span.
    /// For Capacity=1, only the last item will be visible to the reader.
    void push_n(std::span<const T> items) noexcept {
        produce_n(items.size(), [&](T& slot, size_t i) {
            slot = items[i];
        });
    }

    // ===========================================================================
    // Reader
    // ===========================================================================

    template <typename F>
        requires std::invocable<F, const T&>
    [[nodiscard]] bool try_consume_latest(F&& visitor) noexcept {
        // 1. 读取开始版本号 (Acquire)
        // PERF: 45.42%
        uint64_t seq0 = seq_.load(std::memory_order_acquire);
        if (seq0 <= last_seq_ || (seq0 & 1)) return false;

        // 2. 读取数据
        std::invoke(std::forward<F>(visitor), data_);

        // 3. Load-Load Barrier
        std::atomic_thread_fence(std::memory_order_acquire);

        // 4. 验证结束版本号
        uint64_t seq1 = seq_.load(std::memory_order_relaxed);
        if (seq0 == seq1) {
            last_seq_ = seq1;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool try_pop_latest(T& out) noexcept {
        return try_consume_latest([&out](const T& slot) { out = slot; });
    }

    [[nodiscard]] std::optional<T> try_pop_latest() noexcept {
        std::optional<T> res;
        if (try_consume_latest([&res](const T& slot) { res.emplace(slot); })) {
            return res;
        }
        return std::nullopt;
    }

    /// Zero-copy non-consuming peek for Capacity=1 specialization (Visitor pattern).
    /// Reads the latest data without advancing last_seq_, so subsequent
    /// try_consume_latest will still return the same value.
    template <typename F>
        requires std::invocable<F, const T&>
    [[nodiscard]] bool try_peek_latest(F&& visitor) noexcept {
        uint64_t seq0 = seq_.load(std::memory_order_acquire);
        if (seq0 <= last_seq_ || (seq0 & 1)) return false;

        std::invoke(std::forward<F>(visitor), data_);

        std::atomic_thread_fence(std::memory_order_acquire);
        uint64_t seq1 = seq_.load(std::memory_order_relaxed);

        return seq0 == seq1;
        // Note: last_seq_ is NOT advanced
    }

    [[nodiscard]] bool try_peek_latest(T& out) noexcept {
        return try_peek_latest([&out](const T& data) { out = data; });
    }

    [[nodiscard]] std::optional<T> try_peek_latest() noexcept {
        std::optional<T> res;
        if (try_peek_latest([&res](const T& data) { res.emplace(data); })) {
            return res;
        }
        return std::nullopt;
    }

    /// 带超时的零拷贝查看最新数据但不标记为已消费 (Capacity=1 特化)
    template <typename F, typename Rep, typename Period>
        requires std::invocable<F, const T&>
    [[nodiscard]] bool try_peek_latest_for(
        F&& visitor, std::chrono::duration<Rep, Period> timeout) noexcept {
        if (try_peek_latest(std::forward<F>(visitor))) return true;
        auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            cpu_relax();
            if (try_peek_latest(std::forward<F>(visitor))) return true;
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    template <typename Rep, typename Period>
    [[nodiscard]] bool try_peek_latest_for(
        T& out, std::chrono::duration<Rep, Period> timeout) noexcept {
        return try_peek_latest_for(
            [&out](const T& data) { out = data; }, timeout);
    }

    template <typename Rep, typename Period>
    [[nodiscard]] std::optional<T> try_peek_latest_for(
        std::chrono::duration<Rep, Period> timeout) noexcept {
        std::optional<T> res;
        (void)try_peek_latest_for(
            [&res](const T& data) { res.emplace(data); }, timeout);
        return res;
    }

    template <typename F>
        requires std::invocable<F, const T&>
    void consume_latest(F&& visitor) noexcept {
        while (!try_consume_latest(std::forward<F>(visitor))) {
            cpu_relax();
        }
    }

    void pop_latest(T& out) noexcept {
        consume_latest([&out](const T& slot) { out = slot; });
    }

    [[nodiscard]] T pop_latest() noexcept {
        T out;
        pop_latest(out);
        return out;
    }

    // ===========================================================================
    // Reader 带超时操作
    // ===========================================================================

    template <typename F, typename Rep, typename Period>
        requires std::invocable<F, const T&>
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

    template <typename Rep, typename Period>
    [[nodiscard]] bool try_pop_latest_for(
        T& out, std::chrono::duration<Rep, Period> timeout) noexcept {
        return try_consume_latest_for(
            [&out](const T& data) { out = data; }, timeout);
    }

    template <typename Rep, typename Period>
    [[nodiscard]] std::optional<T> try_pop_latest_for(
        std::chrono::duration<Rep, Period> timeout) noexcept {
        std::optional<T> res;
        (void)try_consume_latest_for(
            [&res](const T& data) { res.emplace(data); }, timeout);
        return res;
    }

    // ===========================================================================
    // 状态查询
    // ===========================================================================

    /// 重置队列状态，丢弃所有未读数据。
    ///
    /// @warning 仅在确保无并发读写时调用。
    void clear() noexcept {
        uint64_t seq = seq_.load(std::memory_order_acquire);
        last_seq_ = seq;
    }

    [[nodiscard]] static constexpr size_t capacity() noexcept { return 1; }

    /// Approximate number of unread entries (0 or 1 for single-slot).
    /// The result may be stale by the time it is read in a concurrent context.
    [[nodiscard]] size_t size_approx() const noexcept {
        uint64_t seq = seq_.load(std::memory_order_relaxed);
        // If writer is mid-write (odd seq) or no new data since last read, empty
        if ((seq & 1) || seq <= last_seq_) return 0;
        return 1;
    }

    /// Check if there are no unread entries (approximate).
    [[nodiscard]] bool empty() const noexcept { return size_approx() == 0; }

    /// Check if the queue is at capacity (writes are overwriting unread data).
    /// For single-slot, this is equivalent to !empty().
    /// @note Approximate — suitable for monitoring, not synchronization.
    [[nodiscard]] bool full() const noexcept { return size_approx() >= 1; }

    /// Total number of writes performed since construction.
    /// @note Relaxed load — safe to call from any thread for approximate monitoring.
    [[nodiscard]] uint64_t write_count() const noexcept {
        // seq_ increments by 2 per write (odd→even), so total writes = seq / 2
        return seq_.load(std::memory_order_relaxed) / 2;
    }

    /// Approximate number of writes that overwrote unread data.
    /// Uses actual read count for accuracy (consistent with the primary
    /// template's formula: max(0, writes - reads - Capacity)).
    /// @note Approximate — derived from relaxed atomic loads.
    [[nodiscard]] uint64_t overwrite_count_approx() const noexcept {
        uint64_t writes = write_count();
        uint64_t reads  = read_count();
        if (writes <= reads + 1) return 0;
        return writes - reads - 1;
    }

    /// Total number of successful reads performed since construction.
    /// Useful for monitoring consumer throughput and computing discard rates.
    /// @note Only accurate when called from the reader thread.
    [[nodiscard]] uint64_t read_count() const noexcept {
        // seq increments by 2 per write; last_seq_ tracks the seq after last read
        return last_seq_ / 2;
    }

    /// Alias for the standalone EvictingQueueStats type.
    using Stats = EvictingQueueStats;

    /// Take a point-in-time statistics snapshot.
    [[nodiscard]] Stats stats() const noexcept {
        return Stats{
            .total_pushed = write_count(),
            .total_popped = read_count(),
            .overwritten  = overwrite_count_approx(),
            .current_size = size_approx(),
            .capacity     = 1,
        };
    }
};

}  // namespace eph::containers

// std::formatter specialization for EvictingQueueStats
template <>
struct std::formatter<eph::containers::EvictingQueueStats>
    : std::formatter<std::string> {
    auto format(const eph::containers::EvictingQueueStats& s,
                std::format_context& ctx) const {
        return std::formatter<std::string>::format(s.dump(), ctx);
    }
};
