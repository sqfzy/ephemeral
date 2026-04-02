#pragma once

/// @file evicting_queue.hpp
/// @brief Wait-free SPSC evicting queue with SeqLock-based consistency.
///
/// Provides a fixed-capacity, single-producer single-consumer queue where
/// the writer is **wait-free** (never blocks -- overwrites the oldest entry
/// when full) and the reader is **lock-free** (optimistic SeqLock reads).
///
/// Two variants are provided:
///   - Primary template (`Capacity > 1`): multi-slot ring with per-slot
///     sequence locks and a global monotonic index.
///   - Explicit specialization (`Capacity == 1`): classic SeqLock on a
///     single data slot, yielding minimal footprint and the lowest possible
///     latency.
///
/// Capacity must be a power of two so index masking replaces modulo on the
/// hot path.

#include <algorithm>
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

/// @brief Point-in-time statistics snapshot for EvictingQueue monitoring.
///
/// Defined as a standalone (non-nested) type so that a `std::formatter`
/// specialization can be provided without knowing the queue's template
/// arguments. Obtain a snapshot via `EvictingQueue::stats()`.
///
/// All counters are derived from relaxed or acquire atomic loads and are
/// therefore approximate in a concurrent context.
struct EvictingQueueStats {
    uint64_t total_pushed;       ///< Total writes since construction
    uint64_t total_popped;       ///< Total successful reads
    uint64_t overwritten;        ///< Approximate overwrites (data loss)
    size_t   current_size;       ///< Approximate unread entries
    size_t   capacity;           ///< Fixed capacity

    /// @brief Multi-line human-readable dump for logging/debugging.
    /// @return Formatted string including capacity, sizes, and loss rate.
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

    /// @brief JSON-formatted stats for monitoring system integration.
    /// @return Compact single-line JSON string.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"capacity\":{},\"current_size\":{},\"total_pushed\":{},"
            "\"total_popped\":{},\"overwritten\":{}}}",
            capacity, current_size, total_pushed, total_popped, overwritten);
    }

    /// @brief Compute delta between two snapshots for interval-based monitoring.
    /// @param lhs Later snapshot (e.g. at time T2).
    /// @param rhs Earlier snapshot (e.g. at time T1).
    /// @return Per-field difference; `current_size` and `capacity` are taken from @p lhs.
    [[nodiscard]] friend EvictingQueueStats operator-(const EvictingQueueStats& lhs,
                                                      const EvictingQueueStats& rhs) noexcept {
        return EvictingQueueStats{
            .total_pushed = lhs.total_pushed >= rhs.total_pushed ? lhs.total_pushed - rhs.total_pushed : 0,
            .total_popped = lhs.total_popped >= rhs.total_popped ? lhs.total_popped - rhs.total_popped : 0,
            .overwritten  = lhs.overwritten >= rhs.overwritten ? lhs.overwritten - rhs.overwritten : 0,
            .current_size = lhs.current_size,  // point-in-time, not diffable
            .capacity     = lhs.capacity,
        };
    }

    /// @brief Items consumed per second over a measurement interval.
    ///
    /// Apply to a delta snapshot:
    /// @code
    ///   auto delta = t2 - t1;
    ///   double ops = delta.throughput(elapsed_ns);
    /// @endcode
    ///
    /// @param duration_ns Measurement interval in nanoseconds.
    /// @return Throughput in items/second, or 0.0 if @p duration_ns is zero.
    [[nodiscard]] double throughput(uint64_t duration_ns) const noexcept {
        return duration_ns > 0
            ? static_cast<double>(total_popped) * 1e9 / static_cast<double>(duration_ns)
            : 0.0;
    }

    /// @brief Data loss rate as a fraction in [0.0, 1.0].
    ///
    /// 0.0 means no loss (consumer keeping up), 1.0 means total loss.
    /// Apply to a delta snapshot for interval-based monitoring.
    ///
    /// @return Clamped ratio of overwritten to total_pushed.
    [[nodiscard]] double loss_rate() const noexcept {
        return total_pushed > 0
            ? std::clamp(static_cast<double>(overwritten) / static_cast<double>(total_pushed), 0.0, 1.0)
            : 0.0;
    }

    [[nodiscard]] friend bool operator==(const EvictingQueueStats&,
                                          const EvictingQueueStats&) = default;
};

/// @brief Multi-slot SeqLock-based evicting SPSC queue (primary template).
///
/// The writer is **wait-free**: it never blocks and silently overwrites
/// the oldest unread entry when the queue is full.  The reader is
/// **lock-free**: it performs an optimistic SeqLock read and retries on
/// contention rather than blocking.
///
/// Memory layout (each zone occupies its own cache line):
/// @code
///   Global Zone   -- global_index_ (shared atomic, monotonic write counter)
///   Writer Zone   -- shadow_global_index_ (writer-local cached index)
///   Reader Zone   -- last_global_index_   (reader-local consumed index)
///   Slots Zone    -- slots_[0..Capacity-1], each with its own seq lock
/// @endcode
///
/// @note PERF comments in the source reference profiling on an x86-64
///       Intel i7, payload 32 B, 8 slots, two pinned cores.
///
/// @tparam T        Element type -- must satisfy TrivialData (trivially
///                  copyable, trivially destructible, default-initializable).
/// @tparam Capacity Number of ring slots. Must be a power of two and > 1.
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
        // Atomic: written by reader thread, read by monitoring functions from any thread.
        std::atomic<uint64_t> last_global_index_{0};
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

    /// @brief Zero-copy write via visitor pattern (wait-free).
    ///
    /// Acquires the next slot, locks it (odd seq), invokes @p writer_func
    /// to populate the data in-place, then unlocks (even seq) and publishes
    /// the global index. Overwrites the oldest entry when full.
    ///
    /// @tparam F Callable with signature `void(T& slot)`.
    /// @param writer_func Callback that initialises or modifies the slot data.
    /// @note Writer-side only. Exactly one thread may call produce/push/emplace.
    // global_index_ starts at 0 (sentinel: "no data written").
    // First write uses index 1. Reader checks idx > last_read.
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

        // 5+6. Unlock slot: store-release merges the data→seq(even) fence
        // with the seq store.  On ARM64 this emits a single `stlr` (~5ns)
        // instead of `dmb ishst + str` (~15ns).  Semantics are identical:
        // all prior stores (data writes) are visible before seq(even).
        s.seq_.store(encode_seq(next_idx, false), std::memory_order_release);

        // 7. 发布全局索引: seq(even) ─hb→ global_index
        // Reader acquire global_index 后，能看到对应 slot 的 seq(even) 和完整 data
        // PERF: 17.68%
        global_index_.store(next_idx, std::memory_order_release);

        // 8. 更新本地影子索引
        // PERF: 15.68%
        writer_.shadow_global_index_ = next_idx;
    }

    /// @brief Write a new element by copy or move (wait-free).
    /// @tparam U Type assignable to `T&`.
    /// @param val Value to store in the next slot.
    template <typename U>
        requires std::is_assignable_v<T&, U>
    void push(U&& val) noexcept {
        produce([&](T& slot) { slot = std::forward<U>(val); });
    }

    /// @brief Emplace-construct a new element in-place (wait-free).
    /// @tparam Args Constructor argument types for `T`.
    /// @param args Arguments forwarded to `std::construct_at`.
    /// @note TrivialData implies trivially_destructible, so no destroy_at is needed.
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

    /// @brief Try to read the latest element via zero-copy visitor (lock-free).
    ///
    /// Performs an optimistic SeqLock read of the most recently written slot.
    /// If the read is consistent (no concurrent write detected), the visitor
    /// is considered successful and the reader's consumed index advances.
    ///
    /// @tparam F Callable with signature `void(const T&)`.
    /// @param visitor Callback invoked with a const reference to the slot data.
    /// @return `true` if a consistent read was obtained; `false` if no new
    ///         data is available or the read was torn by a concurrent write.
    /// @note Reader-side only. Exactly one thread may call consume/pop/peek.
    template <typename F>
        requires std::invocable<F, const T&>
    [[nodiscard]] bool try_consume_latest(F&& visitor) noexcept {
        // 1. 获取当前最新的全局索引 (Acquire)
        uint64_t idx =
            global_index_.load(std::memory_order_acquire);  // PERF: 48.83%

        const uint64_t last_read = reader_.last_global_index_.load(std::memory_order_relaxed);
        if (idx <= last_read) {
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
        if (actual_idx <= last_read) {
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
            reader_.last_global_index_.store(actual_idx, std::memory_order_relaxed);
            return true;
        }

        return false;
    }

    /// @brief Try to read the latest element into @p out by value copy.
    /// @param out [out] Destination object, written only on success.
    /// @return `true` if a consistent read was obtained; `false` otherwise.
    [[nodiscard]] bool try_pop_latest(T& out) noexcept {
        return try_consume_latest([&out](const T& data) { out = data; });
    }

    /// @brief Try to read the latest element, returning it as an optional.
    /// @return The element if a consistent read was obtained; `std::nullopt` otherwise.
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
        const uint64_t last_read = reader_.last_global_index_.load(std::memory_order_relaxed);
        if (idx <= last_read) return false;

        const Slot& s = slots_[idx & (Capacity - 1)];
        uint64_t seq1 = s.seq_.load(std::memory_order_acquire);
        if (is_locked(seq1)) [[unlikely]] return false;

        uint64_t actual_idx = decode_idx(seq1);
        if (actual_idx <= last_read) return false;

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
    // noexcept: steady_clock::now() is guaranteed not to throw on
    // Linux/macOS/Windows. If this assumption breaks on an exotic
    // platform, std::terminate will be called — preferable to
    // exception-based unwinding in a lock-free hot path.
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

    /// @brief Blocking zero-copy read (spins until a consistent read succeeds).
    ///
    /// Equivalent to calling `try_consume_latest` in a spin loop with
    /// `cpu_relax()` between attempts.
    ///
    /// @tparam F Callable with signature `void(const T&)`.
    /// @param visitor Callback invoked with a const reference to the slot data.
    /// @warning Spins indefinitely -- use only on CPU-pinned threads with a
    ///          known-active producer. Prefer `try_consume_latest_for` when
    ///          unbounded blocking is unacceptable.
    template <typename F>
        requires std::invocable<F, const T&>
    void consume_latest(F&& visitor) noexcept {
        while (!try_consume_latest(std::forward<F>(visitor))) {
            cpu_relax();
        }
    }

    /// @brief Blocking value-copy read (spins until success).
    /// @param out [out] Destination object.
    void pop_latest(T& out) noexcept {
        consume_latest([&out](const T& data) { out = data; });
    }

    /// @brief Blocking read returning the element by value (spins until success).
    /// @return The latest element.
    [[nodiscard]] T pop_latest() noexcept {
        T out;
        pop_latest(out);
        return out;
    }

    // ===========================================================================
    // Reader 带超时操作
    // ===========================================================================

    /// @brief Zero-copy read with timeout (spins until success or deadline).
    ///
    /// Suitable for non-pinned threads where unbounded spinning is unacceptable.
    ///
    /// @tparam F   Callable with signature `void(const T&)`.
    /// @tparam Rep, Period `std::chrono::duration` parameters.
    /// @param visitor Callback invoked on success.
    /// @param timeout Maximum wall-clock wait.
    /// @return `true` if a consistent read was obtained; `false` on timeout.
    // noexcept: steady_clock::now() is guaranteed not to throw on
    // Linux/macOS/Windows. If this assumption breaks on an exotic
    // platform, std::terminate will be called — preferable to
    // exception-based unwinding in a lock-free hot path.
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

    /// @brief Value-copy read with timeout.
    /// @param out [out] Destination object, written only on success.
    /// @param timeout Maximum wall-clock wait.
    /// @return `true` if a consistent read was obtained; `false` on timeout.
    template <typename Rep, typename Period>
    [[nodiscard]] bool try_pop_latest_for(
        T& out, std::chrono::duration<Rep, Period> timeout) noexcept {
        return try_consume_latest_for(
            [&out](const T& data) { out = data; }, timeout);
    }

    /// @brief Read with timeout, returning the element as an optional.
    /// @param timeout Maximum wall-clock wait.
    /// @return The element on success; `std::nullopt` on timeout.
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

    /// @brief Reset the queue, discarding all unread data.
    ///
    /// Advances the reader's consumed index to match the writer's current
    /// position so that subsequent reads see no pending data.
    ///
    /// @warning Not thread-safe. Call only when no concurrent readers/writers
    ///          are active (e.g. during reconnection or initialization).
    void clear() noexcept {
        // 将 reader 的 last_global_index_ 追赶到 writer 的当前位置，
        // 使后续 try_consume_latest 认为没有新数据。
        uint64_t idx = global_index_.load(std::memory_order_acquire);
        reader_.last_global_index_.store(idx, std::memory_order_relaxed);
    }

    /// @brief Return the fixed buffer capacity.
    /// @return Compile-time constant `Capacity`.
    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return Capacity;
    }

    /// Approximate number of unread entries (for monitoring/debugging only).
    /// The result may be stale by the time it is read in a concurrent context.
    [[nodiscard]] size_t size_approx() const noexcept {
        uint64_t written = global_index_.load(std::memory_order_relaxed);
        uint64_t read    = reader_.last_global_index_.load(std::memory_order_relaxed);
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
        uint64_t read = reader_.last_global_index_.load(std::memory_order_relaxed);
        if (written <= read + Capacity) return 0;
        return written - read - Capacity;
    }

    /// Total number of successful reads performed since construction.
    /// Useful for monitoring consumer throughput and computing discard rates
    /// (write_count - read_count = unconsumed + overwritten).
    /// @note Only accurate when called from the reader thread.
    [[nodiscard]] uint64_t read_count() const noexcept {
        return reader_.last_global_index_.load(std::memory_order_relaxed);
    }

    /// Alias for the standalone EvictingQueueStats type.
    using Stats = EvictingQueueStats;

    /// Take a point-in-time statistics snapshot.
    /// Uses acquire loads to produce a more consistent cross-field snapshot
    /// than calling each accessor independently with relaxed ordering.
    [[nodiscard]] Stats stats() const noexcept {
        // Acquire ordering ensures we see a consistent view of both counters.
        uint64_t written = global_index_.load(std::memory_order_acquire);
        uint64_t read    = reader_.last_global_index_.load(std::memory_order_acquire);
        uint64_t pending = (written >= read) ? (written - read) : 0;
        size_t   cur_sz  = static_cast<size_t>(
            std::min(pending, static_cast<uint64_t>(Capacity)));
        uint64_t overwritten = (written > read + Capacity)
                                   ? (written - read - Capacity) : 0;
        return Stats{
            .total_pushed = written,
            .total_popped = read,
            .overwritten  = overwritten,
            .current_size = cur_sz,
            .capacity     = Capacity,
        };
    }
};

/// @brief Single-slot SeqLock specialization of EvictingQueue (Capacity == 1).
///
/// A classic sequence-lock over a single data element. The writer is
/// wait-free (increments an even/odd sequence counter around the write);
/// the reader is lock-free (optimistic read with seq validation).
///
/// Memory layout (each zone on its own cache line):
/// @code
///   SeqLock Zone  -- seq_      (even = idle, odd = write-in-progress)
///   Reader Zone   -- last_seq_ (last successfully read sequence number)
///   Data Zone     -- data_     (the stored element)
/// @endcode
///
/// @tparam T Element type -- must satisfy TrivialData.
template <typename T>
    requires TrivialData<T>
class alignas(Align<T>) EvictingQueue<T, 1> {
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
                  "SeqLock requires lock-free std::atomic<uint64_t>");

   private:
    // 偶数=空闲，奇数=正在写入
    alignas(Align<T>) std::atomic<uint64_t> seq_{0};
    // Atomic: written by reader thread, read by monitoring functions from any thread.
    alignas(Align<T>) std::atomic<uint64_t> last_seq_{0};
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

    /// @brief Zero-copy write via visitor pattern (wait-free, Capacity=1).
    ///
    /// Locks the slot (seq becomes odd), invokes @p writer_func to
    /// populate the data in-place, then unlocks (seq becomes even).
    ///
    /// @tparam F Callable with signature `void(T& slot)`.
    /// @param writer_func Callback that initialises or modifies the data.
    /// @note Writer-side only. Exactly one thread may call produce/push/emplace.
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

        // 3. Unlock (Seq=Even) — must be release store (not relaxed+fence)
        // to guarantee all prior writes are visible before the reader sees
        // the even sequence number. A relaxed store after a release fence is
        // not equivalent on ARM64.
        seq_.store(seq + 2, std::memory_order_release);
    }

    /// @brief Write a new element by copy or move (wait-free, Capacity=1).
    /// @tparam U Type assignable to `T&`.
    /// @param val Value to store.
    template <typename U>
        requires std::is_assignable_v<T&, U>
    void push(U&& val) noexcept {
        produce([&](T& slot) { slot = std::forward<U>(val); });
    }

    /// @brief Emplace-construct a new element in-place (wait-free, Capacity=1).
    /// @tparam Args Constructor argument types for `T`.
    /// @param args Arguments forwarded to `std::construct_at`.
    /// @note TrivialData implies trivially_destructible, so no destroy_at is needed.
    template <typename... Args>
        requires std::is_constructible_v<T, Args...>
    void emplace(Args&&... args) noexcept {
        produce([&](T& slot) {
            std::construct_at(&slot, std::forward<Args>(args)...);
        });
    }

    /// @brief Batch write N items using a visitor pattern (Capacity=1).
    ///
    /// Calls `visitor(T& slot, size_t index)` for each of the N items.
    /// Each write is individually sequenced (wait-free).
    ///
    /// @param count   Number of items to write.
    /// @param visitor Callable with signature `void(T& slot, size_t index)`.
    /// @warning For Capacity=1, only the last item will be visible to the reader.
    template <typename F>
        requires std::invocable<F, T&, size_t>
    void produce_n(size_t count, F&& visitor) noexcept {
        for (size_t i = 0; i < count; ++i) {
            produce([&](T& slot) {
                std::invoke(std::forward<F>(visitor), slot, i);
            });
        }
    }

    /// @brief Batch push N items from a span (Capacity=1).
    /// @param items Elements to write sequentially.
    /// @warning For Capacity=1, only the last item will be visible to the reader.
    void push_n(std::span<const T> items) noexcept {
        produce_n(items.size(), [&](T& slot, size_t i) {
            slot = items[i];
        });
    }

    // ===========================================================================
    // Reader
    // ===========================================================================

    /// @brief Try to read the latest element via zero-copy visitor (lock-free, Capacity=1).
    ///
    /// Performs an optimistic SeqLock read. Returns `false` if no new data
    /// exists or the read was torn by a concurrent write.
    ///
    /// @tparam F Callable with signature `void(const T&)`.
    /// @param visitor Callback invoked with a const reference to the data.
    /// @return `true` if a consistent read was obtained; `false` otherwise.
    template <typename F>
        requires std::invocable<F, const T&>
    [[nodiscard]] bool try_consume_latest(F&& visitor) noexcept {
        // 1. 读取开始版本号 (Acquire)
        // PERF: 45.42%
        uint64_t seq0 = seq_.load(std::memory_order_acquire);
        const uint64_t cached_last = last_seq_.load(std::memory_order_relaxed);
        if (seq0 <= cached_last || (seq0 & 1)) return false;

        // 2. 读取数据
        std::invoke(std::forward<F>(visitor), data_);

        // 3. Load-Load Barrier
        std::atomic_thread_fence(std::memory_order_acquire);

        // 4. 验证结束版本号
        uint64_t seq1 = seq_.load(std::memory_order_relaxed);
        if (seq0 == seq1) {
            last_seq_.store(seq1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    /// @brief Try to read the latest element into @p out by value copy (Capacity=1).
    /// @param out [out] Destination object, written only on success.
    /// @return `true` on consistent read; `false` otherwise.
    [[nodiscard]] bool try_pop_latest(T& out) noexcept {
        return try_consume_latest([&out](const T& slot) { out = slot; });
    }

    /// @brief Try to read the latest element, returning it as an optional (Capacity=1).
    /// @return The element on success; `std::nullopt` otherwise.
    [[nodiscard]] std::optional<T> try_pop_latest() noexcept {
        std::optional<T> res;
        if (try_consume_latest([&res](const T& slot) { res.emplace(slot); })) {
            return res;
        }
        return std::nullopt;
    }

    /// @brief Zero-copy non-consuming peek (Capacity=1, visitor pattern).
    ///
    /// Reads the latest data without advancing `last_seq_`, so subsequent
    /// `try_consume_latest` will still return the same value. Useful for
    /// pre-inspecting message type before deciding whether to consume.
    ///
    /// @tparam F Callable with signature `void(const T&)`.
    /// @param visitor Callback invoked with a const reference to the data.
    /// @return `true` on consistent read; `false` otherwise.
    template <typename F>
        requires std::invocable<F, const T&>
    [[nodiscard]] bool try_peek_latest(F&& visitor) noexcept {
        uint64_t seq0 = seq_.load(std::memory_order_acquire);
        if (seq0 <= last_seq_.load(std::memory_order_relaxed) || (seq0 & 1)) return false;

        std::invoke(std::forward<F>(visitor), data_);

        std::atomic_thread_fence(std::memory_order_acquire);
        uint64_t seq1 = seq_.load(std::memory_order_relaxed);

        return seq0 == seq1;
        // Note: last_seq_ is NOT advanced
    }

    /// @brief Non-consuming peek by value copy (Capacity=1).
    /// @param out [out] Destination object, written only on success.
    /// @return `true` on consistent read; `false` otherwise.
    [[nodiscard]] bool try_peek_latest(T& out) noexcept {
        return try_peek_latest([&out](const T& data) { out = data; });
    }

    /// @brief Non-consuming peek returning an optional (Capacity=1).
    /// @return The element on success; `std::nullopt` otherwise.
    [[nodiscard]] std::optional<T> try_peek_latest() noexcept {
        std::optional<T> res;
        if (try_peek_latest([&res](const T& data) { res.emplace(data); })) {
            return res;
        }
        return std::nullopt;
    }

    /// @brief Non-consuming peek with timeout (zero-copy visitor, Capacity=1).
    /// @tparam F Callable with signature `void(const T&)`.
    /// @tparam Rep, Period `std::chrono::duration` parameters.
    /// @param visitor Callback invoked on success.
    /// @param timeout Maximum wall-clock wait.
    /// @return `true` on success; `false` on timeout.
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

    /// @brief Non-consuming peek with timeout by value copy (Capacity=1).
    /// @param out [out] Destination object.
    /// @param timeout Maximum wall-clock wait.
    /// @return `true` on success; `false` on timeout.
    template <typename Rep, typename Period>
    [[nodiscard]] bool try_peek_latest_for(
        T& out, std::chrono::duration<Rep, Period> timeout) noexcept {
        return try_peek_latest_for(
            [&out](const T& data) { out = data; }, timeout);
    }

    /// @brief Non-consuming peek with timeout, returning an optional (Capacity=1).
    /// @param timeout Maximum wall-clock wait.
    /// @return The element on success; `std::nullopt` on timeout.
    template <typename Rep, typename Period>
    [[nodiscard]] std::optional<T> try_peek_latest_for(
        std::chrono::duration<Rep, Period> timeout) noexcept {
        std::optional<T> res;
        (void)try_peek_latest_for(
            [&res](const T& data) { res.emplace(data); }, timeout);
        return res;
    }

    /// @brief Blocking zero-copy read (spins until success, Capacity=1).
    /// @tparam F Callable with signature `void(const T&)`.
    /// @param visitor Callback invoked with a const reference to the data.
    /// @warning Spins indefinitely -- use only on pinned threads.
    template <typename F>
        requires std::invocable<F, const T&>
    void consume_latest(F&& visitor) noexcept {
        while (!try_consume_latest(std::forward<F>(visitor))) {
            cpu_relax();
        }
    }

    /// @brief Blocking value-copy read (spins until success, Capacity=1).
    /// @param out [out] Destination object.
    void pop_latest(T& out) noexcept {
        consume_latest([&out](const T& slot) { out = slot; });
    }

    /// @brief Blocking read returning the element by value (Capacity=1).
    /// @return The latest element.
    [[nodiscard]] T pop_latest() noexcept {
        T out;
        pop_latest(out);
        return out;
    }

    // ===========================================================================
    // Reader 带超时操作
    // ===========================================================================

    /// @brief Zero-copy read with timeout (Capacity=1).
    /// @tparam F Callable with signature `void(const T&)`.
    /// @tparam Rep, Period `std::chrono::duration` parameters.
    /// @param visitor Callback invoked on success.
    /// @param timeout Maximum wall-clock wait.
    /// @return `true` on success; `false` on timeout.
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

    /// @brief Value-copy read with timeout (Capacity=1).
    /// @param out [out] Destination object.
    /// @param timeout Maximum wall-clock wait.
    /// @return `true` on success; `false` on timeout.
    template <typename Rep, typename Period>
    [[nodiscard]] bool try_pop_latest_for(
        T& out, std::chrono::duration<Rep, Period> timeout) noexcept {
        return try_consume_latest_for(
            [&out](const T& data) { out = data; }, timeout);
    }

    /// @brief Read with timeout, returning the element as an optional (Capacity=1).
    /// @param timeout Maximum wall-clock wait.
    /// @return The element on success; `std::nullopt` on timeout.
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

    /// @brief Reset the queue, discarding all unread data (Capacity=1).
    /// @warning Not thread-safe. Call only when no concurrent readers/writers.
    void clear() noexcept {
        uint64_t seq = seq_.load(std::memory_order_acquire);
        last_seq_.store(seq, std::memory_order_relaxed);
    }

    /// @brief Return the fixed capacity (always 1).
    [[nodiscard]] static constexpr size_t capacity() noexcept { return 1; }

    /// @brief Approximate number of unread entries (0 or 1 for single-slot).
    /// @return 0 if no unread data or a write is in progress; 1 otherwise.
    /// @note The result may be stale by the time it is read in a concurrent context.
    [[nodiscard]] size_t size_approx() const noexcept {
        uint64_t seq = seq_.load(std::memory_order_relaxed);
        // If writer is mid-write (odd seq) or no new data since last read, empty
        if ((seq & 1) || seq <= last_seq_.load(std::memory_order_relaxed)) return 0;
        return 1;
    }

    /// @brief Check if there are no unread entries (approximate).
    [[nodiscard]] bool empty() const noexcept { return size_approx() == 0; }

    /// @brief Check if the queue is at capacity (Capacity=1: equivalent to `!empty()`).
    /// @note Approximate -- suitable for monitoring, not synchronization.
    [[nodiscard]] bool full() const noexcept { return size_approx() >= 1; }

    /// @brief Total number of writes performed since construction.
    /// @note Relaxed load -- safe to call from any thread for approximate monitoring.
    [[nodiscard]] uint64_t write_count() const noexcept {
        // seq_ increments by 2 per write (odd→even), so total writes = seq / 2
        return seq_.load(std::memory_order_relaxed) / 2;
    }

    /// @brief Approximate number of writes that overwrote unread data.
    ///
    /// Uses actual read count for accuracy (consistent with the primary
    /// template's formula: `max(0, writes - reads - Capacity)`).
    ///
    /// @return Estimated overwrite count.
    /// @note Approximate -- derived from relaxed atomic loads.
    [[nodiscard]] uint64_t overwrite_count_approx() const noexcept {
        uint64_t writes = write_count();
        uint64_t reads  = read_count();
        if (writes <= reads + 1) return 0;
        return writes - reads - 1;
    }

    /// @brief Total number of successful reads since construction (Capacity=1).
    /// @note Only accurate when called from the reader thread.
    [[nodiscard]] uint64_t read_count() const noexcept {
        // seq increments by 2 per write; last_seq_ tracks the seq after last read
        return last_seq_.load(std::memory_order_relaxed) / 2;
    }

    /// @brief Alias for the standalone EvictingQueueStats type.
    using Stats = EvictingQueueStats;

    /// @brief Take a point-in-time statistics snapshot (Capacity=1).
    /// @return EvictingQueueStats with current counters.
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

/// @brief std::formatter specialization for EvictingQueueStats.
///
/// Delegates to EvictingQueueStats::dump() so that the stats can be
/// used directly with `std::format` and `std::print`.
template <>
struct std::formatter<eph::containers::EvictingQueueStats>
    : std::formatter<std::string> {
    auto format(const eph::containers::EvictingQueueStats& s,
                std::format_context& ctx) const {
        return std::formatter<std::string>::format(s.dump(), ctx);
    }
};
