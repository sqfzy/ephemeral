#pragma once

/// @file bounded_queue.hpp
/// @brief Lock-free bounded SPSC queue with back-pressure.
///
/// Provides a fixed-capacity, single-producer single-consumer FIFO queue
/// where writes **block or fail** when the queue is full (no data is ever
/// silently dropped). The implementation uses a classic two-index ring
/// buffer with shadow copies of the remote index to minimise cross-core
/// cache-line traffic.
///
/// Capacity must be a power of two so index masking replaces modulo on
/// the hot path.

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "eph/core/log.hpp"
#include "eph/containers/concepts.hpp"
#include "eph/utils/alignment.hpp"
#include "eph/utils/cpu.hpp"

namespace eph::containers {

namespace detail {
inline spdlog::logger* bounded_queue_logger() {
    static spdlog::logger* l = ::eph::log::get("containers.bounded_queue");
    return l;
}
} // namespace detail

using eph::utils::Align;
using eph::utils::cpu_relax;

// ---------------------------------------------------------------------------
// Standalone Stats type (enables std::formatter specialization)
// ---------------------------------------------------------------------------

/// @brief Point-in-time statistics snapshot for BoundedQueue monitoring.
///
/// All values are approximate (derived from relaxed/acquire atomic loads).
/// Defined as a standalone (non-nested) type so that a `std::formatter`
/// specialization can be provided without knowing the queue's template
/// arguments. Obtain a snapshot via `BoundedQueue::stats()`.
struct BoundedQueueStats {
    size_t total_pushed;   ///< Total items ever pushed (monotonic)
    size_t total_popped;   ///< Total items ever popped (monotonic)
    size_t current_size;   ///< Approximate current occupancy
    size_t capacity;       ///< Fixed capacity

    /// @brief Multi-line human-readable dump for logging/debugging.
    /// @return Formatted string including capacity, utilization, and counters.
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

    /// @brief JSON-formatted stats for monitoring system integration.
    /// @return Compact single-line JSON string.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"capacity\":{},\"current_size\":{},\"total_pushed\":{},\"total_popped\":{}}}",
            capacity, current_size, total_pushed, total_popped);
    }

    /// @brief Compute delta between two snapshots for interval-based monitoring.
    ///
    /// Usage: `auto delta = stats_t2 - stats_t1;`
    ///
    /// @param lhs Later snapshot (e.g. at time T2).
    /// @param rhs Earlier snapshot (e.g. at time T1).
    /// @return Per-field difference; `current_size` and `capacity` are taken from @p lhs.
    [[nodiscard]] friend BoundedQueueStats operator-(const BoundedQueueStats& lhs,
                                                     const BoundedQueueStats& rhs) noexcept {
        return BoundedQueueStats{
            .total_pushed = lhs.total_pushed >= rhs.total_pushed ? lhs.total_pushed - rhs.total_pushed : 0,
            .total_popped = lhs.total_popped >= rhs.total_popped ? lhs.total_popped - rhs.total_popped : 0,
            .current_size = lhs.current_size,  // point-in-time, not diffable
            .capacity     = lhs.capacity,
        };
    }

    /// @brief Items popped per second over a measurement interval.
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

    [[nodiscard]] friend bool operator==(const BoundedQueueStats&,
                                          const BoundedQueueStats&) = default;
};

/// @brief Lock-free bounded SPSC (single-producer, single-consumer) queue.
///
/// Unlike EvictingQueue, this queue applies **back-pressure**: writes fail
/// (or spin-wait) when the queue is full, guaranteeing that no data is
/// silently dropped.
///
/// Memory layout (each zone on its own cache line):
/// @code
///   Writer Zone  -- tail_ (atomic write index) + shadow_head_ (cached read index)
///   Reader Zone  -- head_ (atomic read index)  + shadow_tail_ (cached write index)
///   Slots Zone   -- buffer_[0..Capacity-1]
/// @endcode
///
/// @note Blocking interfaces (push/pop/produce/consume) use pure
///       `cpu_relax()` spin-waiting, suitable for CPU-pinned threads with
///       brief contention. For queues that may remain full/empty for extended
///       periods, prefer the `try_*` family with a custom back-off strategy.
///
/// @tparam T        Element type -- must satisfy TrivialData.
/// @tparam Capacity Ring buffer size. Must be a power of two.
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
    // alignas on WriterLine/ReaderLine causes sizeof to be padded to
    // Align<T> (≥ CACHE_LINE_SIZE), guaranteeing the next member starts
    // on a fresh cache line. No manual padding is needed.
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

    /// @brief Try to write via zero-copy visitor pattern (non-blocking).
    ///
    /// @tparam F Callable with signature `void(T& slot)`.
    /// @param writer_func Callback that initialises the slot in-place.
    /// @return `true` on success; `false` if the queue is full.
    template <typename F>
        requires std::invocable<F, T&>
    [[nodiscard]] bool try_produce(F&& writer_func) noexcept {
        const size_t tail = writer_.tail_.load(std::memory_order_relaxed);

        // SPSC invariant: shadow_head_ is only accessed by the single writer
        // thread, so it needs no atomic protection. It caches the last observed
        // value of head_ to avoid an expensive cross-core acquire load on the
        // fast path. We only reload head_ when the cached value indicates full.
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

    /// @brief Try to emplace-construct an element (non-blocking).
    /// @tparam Args Constructor argument types for `T`.
    /// @param args Arguments forwarded to `std::construct_at`.
    /// @return `true` on success; `false` if the queue is full.
    template <typename... Args>
        requires std::is_constructible_v<T, Args...>
    [[nodiscard]] bool try_emplace(Args&&... args) noexcept {
        return try_produce([&](T& slot) {
            std::construct_at(&slot, std::forward<Args>(args)...);
        });
    }

    /// @brief Try to push an element by copy or move (non-blocking).
    /// @tparam U Type assignable to `T&`.
    /// @param data Source value (lvalue or rvalue).
    /// @return `true` on success; `false` if the queue is full.
    template <typename U>
        requires std::is_assignable_v<T&, U>
    [[nodiscard]] bool try_push(U&& data) noexcept {
        return try_produce([&](T& slot) { slot = std::forward<U>(data); });
    }

    /// @brief Batch push with all-or-nothing semantics (non-blocking).
    ///
    /// Commits N elements with a single atomic store, amortising the cost
    /// of per-element `try_push` calls.
    ///
    /// @param data Span of elements to write.
    /// @return `true` if all elements were enqueued; `false` if insufficient
    ///         space (no elements written).
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

    /// @brief Batch zero-copy write via visitor (all-or-nothing, non-blocking).
    ///
    /// Reserves N contiguous slots, invokes `writer(slot, index)` for each,
    /// then publishes all elements with a single release store. Avoids
    /// constructing a temporary array, eliminating hot-path heap allocation.
    ///
    /// @tparam F Callable with signature `void(T& slot, size_t index)`.
    /// @param n      Number of elements to write.
    /// @param writer Callback invoked for each slot; `index` is in [0, n).
    /// @return `true` if all elements were enqueued; `false` if insufficient space.
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

    /// @brief Blocking zero-copy write (spins until space is available).
    /// @tparam F Callable with signature `void(T& slot)`.
    /// @param writer Callback that initialises the slot.
    /// @warning Spins indefinitely -- use only on pinned threads.
    template <typename F>
        requires std::invocable<F, T&>
    void produce(F&& writer) noexcept {
        while (!try_produce(std::forward<F>(writer))) {
            cpu_relax();
        }
    }

    /// @brief Blocking push by copy or move (spins until space is available).
    /// @tparam U Type assignable to `T&`.
    /// @param data Source value.
    template <typename U>
        requires std::is_assignable_v<T&, U>
    void push(U&& data) noexcept {
        while (!try_push(std::forward<U>(data))) {
            cpu_relax();
        }
    }

    /// @brief Blocking emplace-construct (spins until space is available).
    /// @tparam Args Constructor argument types for `T`.
    /// @param args Arguments forwarded to `std::construct_at`.
    template <typename... Args>
        requires std::is_constructible_v<T, Args...>
    void emplace(Args&&... args) noexcept {
        while (!try_emplace(std::forward<Args>(args)...)) {
            cpu_relax();
        }
    }

    /// @brief Blocking batch push (all-or-nothing, spins until space is available).
    ///
    /// Spins until contiguous space for all elements is available, then
    /// publishes atomically with a single release store.
    ///
    /// @param data Span of elements to write.
    void push_n(std::span<const T> data) noexcept {
        while (!try_push_n(data)) {
            cpu_relax();
        }
    }

    /// @brief Blocking batch zero-copy write via visitor (all-or-nothing).
    ///
    /// Spins until contiguous space for all elements is available, invokes
    /// `writer(slot, index)` for each, then publishes atomically.
    ///
    /// @tparam F Callable with signature `void(T& slot, size_t index)`.
    /// @param n      Number of elements to write.
    /// @param writer Callback invoked for each slot; `index` is in [0, n).
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

    /// @brief Zero-copy write with timeout.
    ///
    /// Spins until space is available or the deadline expires. Suitable for
    /// non-pinned threads that need a middle ground between immediate return
    /// and unbounded blocking.
    ///
    /// @tparam F Callable with signature `void(T& slot)`.
    /// @tparam Rep, Period `std::chrono::duration` parameters.
    /// @param writer_func Callback that initialises the slot.
    /// @param timeout Maximum wall-clock wait.
    /// @return `true` on success; `false` on timeout.
    // noexcept: steady_clock::now() is guaranteed not to throw on
    // Linux/macOS/Windows. If this assumption breaks on an exotic
    // platform, std::terminate will be called — preferable to
    // exception-based unwinding in a lock-free hot path.
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

    /// @brief Push with timeout by copy or move.
    /// @tparam U Type assignable to `T&`.
    /// @tparam Rep, Period `std::chrono::duration` parameters.
    /// @param data Source value.
    /// @param timeout Maximum wall-clock wait.
    /// @return `true` on success; `false` on timeout.
    template <typename U, typename Rep, typename Period>
        requires std::is_assignable_v<T&, U>
    [[nodiscard]] bool try_push_for(U&& data,
                                     std::chrono::duration<Rep, Period> timeout) noexcept {
        return try_produce_for([&](T& slot) { slot = std::forward<U>(data); }, timeout);
    }

    /// @brief Emplace with timeout.
    /// @tparam Rep, Period `std::chrono::duration` parameters.
    /// @tparam Args Constructor argument types for `T`.
    /// @param timeout Maximum wall-clock wait.
    /// @param args Arguments forwarded to `std::construct_at`.
    /// @return `true` on success; `false` on timeout.
    template <typename Rep, typename Period, typename... Args>
        requires std::is_constructible_v<T, Args...>
    [[nodiscard]] bool try_emplace_for(std::chrono::duration<Rep, Period> timeout,
                                       Args&&... args) noexcept {
        return try_produce_for(
            [&](T& slot) { std::construct_at(&slot, std::forward<Args>(args)...); },
            timeout);
    }

    /// @brief Batch push with timeout (all-or-nothing).
    ///
    /// Spins until contiguous space for all elements is available or the
    /// deadline expires.
    ///
    /// @param data Span of elements to write.
    /// @param timeout Maximum wall-clock wait.
    /// @return `true` if all elements were enqueued; `false` on timeout (nothing written).
    // noexcept: steady_clock::now() is guaranteed not to throw on
    // Linux/macOS/Windows. If this assumption breaks on an exotic
    // platform, std::terminate will be called — preferable to
    // exception-based unwinding in a lock-free hot path.
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

    /// @brief Batch zero-copy write with timeout (all-or-nothing, visitor pattern).
    ///
    /// Spins until contiguous space for N elements is available or the
    /// deadline expires. On success, invokes `writer(slot, index)` for each
    /// slot and publishes atomically.
    ///
    /// @tparam F Callable with signature `void(T& slot, size_t index)`.
    /// @tparam Rep, Period `std::chrono::duration` parameters.
    /// @param n       Number of elements to write.
    /// @param writer  Callback invoked for each slot; `index` is in [0, n).
    /// @param timeout Maximum wall-clock wait.
    /// @return `true` if all elements were enqueued; `false` on timeout.
    // noexcept: steady_clock::now() is guaranteed not to throw on
    // Linux/macOS/Windows. If this assumption breaks on an exotic
    // platform, std::terminate will be called — preferable to
    // exception-based unwinding in a lock-free hot path.
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

    /// @brief Try to consume the head element via zero-copy visitor (non-blocking).
    ///
    /// @tparam F Callable with signature `void(const T& data)`.
    /// @param visitor Callback invoked with a const reference to the head element.
    /// @return `true` if an element was consumed; `false` if the queue is empty.
    /// @note Reader-side only. Exactly one thread may call consume/pop/peek.
    template <typename F>
        requires std::invocable<F, const T&>
    [[nodiscard]] bool try_consume(F&& visitor) noexcept {
        const size_t head = reader_.head_.load(std::memory_order_relaxed);

        if (reader_.shadow_tail_ == head) {
            const size_t tail = writer_.tail_.load(std::memory_order_acquire);
            reader_.shadow_tail_ = tail;

            if (head == tail) {
                return false;
            }
        }

        // Shadow tail optimization: the cached shadow_tail_ was set by a
        // prior acquire load of writer_.tail_, which established happens-before
        // for all slots in [head, shadow_tail_). No fresh acquire is needed
        // for reads within this range — they are already visible.
        std::invoke(std::forward<F>(visitor), std::as_const(buffer_[head & mask_]));

        reader_.head_.store(head + 1, std::memory_order_release);
        return true;
    }

    /// @brief Zero-copy peek at the head element without consuming (reader-side only).
    ///
    /// Invokes @p visitor with the head element but does **not** advance the
    /// head pointer. Useful for pre-inspecting message type or routing logic
    /// before deciding whether to consume.
    ///
    /// @tparam F Callable with signature `void(const T& data)`.
    /// @param visitor Callback invoked with a const reference to the head.
    /// @return `true` if the queue is non-empty and the visitor was called;
    ///         `false` if the queue is empty.
    /// @note Multiple calls return the same element until `try_pop`/`consume`
    ///       advances the head.
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

    /// @brief Peek at the head element by value copy without consuming (reader-side only).
    ///
    /// @param out [out] Destination object, written only when the queue is non-empty.
    /// @return `true` if copied; `false` if the queue is empty.
    /// @note Multiple calls return the same element until `try_pop`/`consume`
    ///       advances the head.
    [[nodiscard]] bool try_peek(T& out) noexcept {
        return try_peek([&out](const T& data) { out = data; });
    }

    /// @brief Peek at the head element returning an optional (reader-side only).
    /// @return The head element if the queue is non-empty; `std::nullopt` otherwise.
    [[nodiscard]] std::optional<T> try_peek() noexcept {
        T val;
        if (try_peek(val)) return val;
        return std::nullopt;
    }

    /// @brief Try to pop the head element into @p out (non-blocking).
    /// @param out [out] Destination object, written only on success.
    /// @return `true` if an element was dequeued; `false` if empty.
    [[nodiscard]] bool try_pop(T& out) noexcept {
        return try_consume([&out](const T& data) { out = data; });
    }

    /// @brief Try to pop the head element, returning it as an optional.
    /// @return The element on success; `std::nullopt` if empty.
    [[nodiscard]] std::optional<T> try_pop() noexcept {
        std::optional<T> res;
        if (try_consume([&](const T& data) { res.emplace(data); })) {
            return res;
        }
        return std::nullopt;
    }

    /// @brief Batch pop with best-effort semantics (non-blocking).
    ///
    /// Consumes up to `min(available, out.size())` elements with a single
    /// atomic store to advance the head pointer.
    ///
    /// @param out Output span to receive the elements.
    /// @return Number of elements actually dequeued (0 if empty).
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

    /// @brief Batch zero-copy consume via visitor (best-effort, non-blocking).
    ///
    /// Consumes up to `min(n, available)` elements, invoking
    /// `visitor(slot, index)` for each, then advances the head pointer
    /// with a single release store. Mirrors `try_produce_n` on the write side.
    ///
    /// @tparam F Callable with signature `void(const T& slot, size_t index)`.
    /// @param n       Maximum number of elements to consume.
    /// @param visitor Callback invoked for each element; `index` is in [0, consumed).
    /// @return Number of elements actually consumed (0 if empty).
    template <typename F>
        requires std::invocable<F, const T&, size_t>
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
                        std::as_const(buffer_[(head + i) & mask_]), i);
        }

        reader_.head_.store(head + count, std::memory_order_release);
        return count;
    }

    /// @brief Blocking batch zero-copy consume (spins until at least one element).
    ///
    /// @tparam F Callable with signature `void(const T& slot, size_t index)`.
    /// @param n       Maximum number of elements to consume.
    /// @param visitor Callback invoked for each element.
    /// @return Number of elements consumed (>= 1).
    template <typename F>
        requires std::invocable<F, const T&, size_t>
    size_t consume_n(size_t n, F&& visitor) noexcept {
        size_t count;
        while ((count = try_consume_n(n, std::forward<F>(visitor))) == 0) {
            cpu_relax();
        }
        return count;
    }

    /// @brief Blocking zero-copy consume (spins until an element is available).
    /// @tparam F Callable with signature `void(const T&)`.
    /// @param visitor Callback invoked with a const reference to the head element.
    template <typename F>
        requires std::invocable<F, const T&>
    void consume(F&& visitor) noexcept {
        while (!try_consume(std::forward<F>(visitor))) {
            cpu_relax();
        }
    }

    /// @brief Blocking pop into @p out (spins until data is available).
    /// @param out [out] Destination object.
    void pop(T& out) noexcept {
        consume([&out](const T& data) { out = data; });
    }

    /// @brief Blocking pop returning the element by value.
    /// @return The dequeued element.
    [[nodiscard]] T pop() noexcept {
        T res;
        consume([&res](const T& data) { res = data; });
        return res;
    }

    // ===========================================================================
    // Reader 带超时操作
    // ===========================================================================

    /// @brief Zero-copy consume with timeout.
    ///
    /// Spins until an element is available or the deadline expires.
    ///
    /// @tparam F Callable with signature `void(const T&)`.
    /// @tparam Rep, Period `std::chrono::duration` parameters.
    /// @param visitor Callback invoked on success.
    /// @param timeout Maximum wall-clock wait.
    /// @return `true` on success; `false` on timeout.
    // noexcept: steady_clock::now() is guaranteed not to throw on
    // Linux/macOS/Windows. If this assumption breaks on an exotic
    // platform, std::terminate will be called — preferable to
    // exception-based unwinding in a lock-free hot path.
    template <typename F, typename Rep, typename Period>
        requires std::invocable<F, const T&>
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

    /// @brief Pop with timeout into @p out.
    /// @param out [out] Destination object.
    /// @param timeout Maximum wall-clock wait.
    /// @return `true` on success; `false` on timeout.
    template <typename Rep, typename Period>
    [[nodiscard]] bool try_pop_for(T& out,
                                    std::chrono::duration<Rep, Period> timeout) noexcept {
        return try_consume_for([&out](const T& data) { out = data; }, timeout);
    }

    /// @brief Pop with timeout, returning the element as an optional.
    /// @param timeout Maximum wall-clock wait.
    /// @return The element on success; `std::nullopt` on timeout.
    template <typename Rep, typename Period>
    [[nodiscard]] std::optional<T> try_pop_for(
        std::chrono::duration<Rep, Period> timeout) noexcept {
        std::optional<T> res;
        (void)try_consume_for([&](const T& data) { res.emplace(data); }, timeout);
        return res;
    }

    /// @brief Batch pop with timeout (best-effort).
    ///
    /// Waits until at least one element is available (or timeout), then
    /// consumes up to `out.size()` elements in one batch. Unlike `try_pop_n`,
    /// this method spins when the queue is temporarily empty.
    ///
    /// @param out Output span to receive the elements.
    /// @param timeout Maximum wall-clock wait.
    /// @return Number of elements dequeued (0 only on timeout).
    // noexcept: steady_clock::now() is guaranteed not to throw on
    // Linux/macOS/Windows. If this assumption breaks on an exotic
    // platform, std::terminate will be called — preferable to
    // exception-based unwinding in a lock-free hot path.
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

    /// @brief Batch zero-copy consume with timeout (best-effort, visitor pattern).
    ///
    /// Waits until at least one element is available (or timeout), then
    /// consumes up to N elements, invoking `visitor(slot, index)` for each.
    ///
    /// @tparam F Callable with signature `void(const T& slot, size_t index)`.
    /// @tparam Rep, Period `std::chrono::duration` parameters.
    /// @param n       Maximum number of elements to consume.
    /// @param visitor Callback invoked for each element.
    /// @param timeout Maximum wall-clock wait.
    /// @return Number of elements consumed (0 only on timeout).
    // noexcept: steady_clock::now() is guaranteed not to throw on
    // Linux/macOS/Windows. If this assumption breaks on an exotic
    // platform, std::terminate will be called — preferable to
    // exception-based unwinding in a lock-free hot path.
    template <typename F, typename Rep, typename Period>
        requires std::invocable<F, const T&, size_t>
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

    /// @brief Best-effort drain: consume all elements visible at call time.
    ///
    /// Equivalent to `try_consume_n(Capacity, visitor)`. Useful for
    /// shutdown drain or periodic batch processing.
    ///
    /// @tparam F Callable with signature `void(const T& slot, size_t index)`.
    /// @param visitor Callback invoked for each element.
    /// @return Number of elements consumed (0 if empty).
    /// @note Does **not** guarantee the queue is empty afterward if the
    ///       producer is concurrently active.
    template <typename F>
        requires std::invocable<F, const T&, size_t>
    [[nodiscard]] size_t try_consume_all(F&& visitor) noexcept {
        return try_consume_n(Capacity, std::forward<F>(visitor));
    }

    // ===========================================================================
    // 状态查询
    // ===========================================================================

    /// @brief Discard all queued elements, resetting the queue to empty.
    ///
    /// Advances head to match tail and flushes shadow indices.
    ///
    /// @warning Not thread-safe. Call only when no concurrent readers/writers
    ///          are active (e.g. reconnection phase, initialization).
    void clear() noexcept {
        // 将 head 追赶到 tail，等效于消费所有元素但不执行任何回调。
        const size_t tail = writer_.tail_.load(std::memory_order_acquire);
        reader_.head_.store(tail, std::memory_order_release);
        // 刷新影子索引，使后续 try_produce/try_consume 看到一致状态
        writer_.shadow_head_ = tail;
        reader_.shadow_tail_ = tail;
    }

    /// @brief Approximate number of elements currently in the queue.
    /// @return Estimated count (not guaranteed consistent across threads).
    [[nodiscard]] size_t size() const noexcept {
        auto tail = writer_.tail_.load(std::memory_order_relaxed);
        auto head = reader_.head_.load(std::memory_order_relaxed);
        return tail - head;
    }

    /// @brief Check if the queue is empty (approximate).
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    /// @brief Check if the queue is full (approximate).
    [[nodiscard]] bool full() const noexcept { return size() >= Capacity; }

    /// @brief Return the fixed queue capacity.
    /// @return Compile-time constant `Capacity`.
    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return Capacity;
    }

    /// @brief Estimated remaining write capacity (`Capacity - size()`).
    ///
    /// Useful for pre-checking before batch writes:
    /// `if (q.available_write() >= n) q.try_push_n(...)`
    ///
    /// @return Approximate number of free slots.
    /// @note Not guaranteed consistent across threads.
    [[nodiscard]] size_t available_write() const noexcept {
        size_t used = size();
        return (used < Capacity) ? (Capacity - used) : 0;
    }

    /// @brief Estimated number of elements available to read (semantic alias for `size()`).
    /// @note Not guaranteed consistent across threads.
    [[nodiscard]] size_t available_read() const noexcept {
        return size();
    }

    // ===========================================================================
    // 可观测性
    // ===========================================================================

    /// @brief Alias for the standalone BoundedQueueStats type.
    using Stats = BoundedQueueStats;

    /// @brief Take a point-in-time statistics snapshot.
    ///
    /// Zero overhead -- derived from existing atomic indices, no extra counters.
    ///
    /// @return BoundedQueueStats with current values.
    [[nodiscard]] Stats stats() const noexcept {
        auto tail = writer_.tail_.load(std::memory_order_acquire);
        auto head = reader_.head_.load(std::memory_order_acquire);
        // Clamp to avoid unsigned underflow from stale relaxed reads
        // where head might appear > tail due to memory ordering.
        size_t size = (tail >= head) ? (tail - head) : 0;
        if (tail < head || size > Capacity) {
            EPH_LOG_DEBUG(detail::bounded_queue_logger(),"bounded_queue stats clamping: raw value out of expected range "
                         "(tail={}, head={}, raw_size={})", tail, head, size);
        }
        return Stats{
            .total_pushed = tail,
            .total_popped = head,
            .current_size = std::min(size, Capacity),
            .capacity     = Capacity,
        };
    }
};

}  // namespace eph::containers

/// @brief std::formatter specialization for BoundedQueueStats.
///
/// Delegates to BoundedQueueStats::dump() for use with `std::format`/`std::print`.
template <>
struct std::formatter<eph::containers::BoundedQueueStats>
    : std::formatter<std::string> {
    auto format(const eph::containers::BoundedQueueStats& s,
                std::format_context& ctx) const {
        return std::formatter<std::string>::format(s.dump(), ctx);
    }
};
