#pragma once

/// @file ring_buffer.hpp
/// @brief Fixed-size circular buffer for tick history lookback.
///
/// Designed for single-writer, any-reader use in HFT signal pipelines.
/// Enables lookback-based signals such as "price N ticks ago" or
/// "VWAP over the last 100 ticks".
///
/// Capacity must be a power of two (enforced at compile time) so that
/// modular index arithmetic reduces to a bitmask — no division on the
/// hot path.

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <optional>
#include <type_traits>

namespace eph::containers {

/// Fixed-size circular buffer for tick history.
///
/// Single-writer, any-reader (no mutex). T must be trivially copyable.
/// Capacity must be a power of two so index masking replaces modulo.
///
/// Thread safety: one writer may call push() concurrently with any number
/// of readers calling at()/front()/back()/count()/full()/empty().
/// `head_` and `count_` are atomic to support this pattern.
/// Writers use relaxed stores; readers use acquire loads to observe a
/// consistent count relative to the data already written.
///
/// @tparam T        Element type (must be trivially copyable).
/// @tparam Capacity Buffer size — must be a power of two.
template <typename T, std::size_t Capacity>
    requires (std::is_trivially_copyable_v<T> && std::has_single_bit(Capacity))
class RingBuffer {
public:
    static constexpr std::size_t capacity = Capacity;

    /// Push a new element, overwriting the oldest when full.
    /// @note Writer-side only. Exactly one thread may call push().
    void push(const T& item) noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        data_[h & kMask] = item;
        // Release store on head_ so that readers who acquire-load head_
        // are guaranteed to see the data written above.
        head_.store(h + 1, std::memory_order_release);
        const std::size_t c = count_.load(std::memory_order_relaxed);
        if (c < Capacity) {
            count_.store(c + 1, std::memory_order_release);
        }
    }

    /// Get element at @p offset from the newest.
    ///   0 = most recent, 1 = second most recent, etc.
    /// Returns std::nullopt if offset >= count().
    [[nodiscard]] std::optional<T> at(std::size_t offset) const noexcept {
        const std::size_t c = count_.load(std::memory_order_acquire);
        if (offset >= c) {
            return std::nullopt;
        }
        // head_ points one past the last written slot.
        // Most recent element is at (head_ - 1), so offset 0 maps there.
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t idx = (h - 1 - offset) & kMask;
        return data_[idx];
    }

    /// Most recent element.
    [[nodiscard]] std::optional<T> front() const noexcept {
        return at(0);
    }

    /// Oldest element in the buffer.
    [[nodiscard]] std::optional<T> back() const noexcept {
        const std::size_t c = count_.load(std::memory_order_acquire);
        if (c == 0) {
            return std::nullopt;
        }
        return at(c - 1);
    }

    /// Number of elements currently stored (up to Capacity).
    [[nodiscard]] std::size_t count() const noexcept {
        return count_.load(std::memory_order_acquire);
    }

    /// Whether the buffer has reached its fixed capacity.
    [[nodiscard]] bool full() const noexcept {
        return count_.load(std::memory_order_acquire) == Capacity;
    }

    /// Whether the buffer contains no elements.
    [[nodiscard]] bool empty() const noexcept {
        return count_.load(std::memory_order_acquire) == 0;
    }

    /// Remove all elements and reset write position.
    /// @warning Not thread-safe. Call only when no concurrent readers/writers.
    void clear() noexcept {
        head_.store(0, std::memory_order_relaxed);
        count_.store(0, std::memory_order_relaxed);
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    std::array<T, Capacity> data_{};
    std::atomic<std::size_t> head_{0};   ///< Next write position (monotonically increasing).
    std::atomic<std::size_t> count_{0};  ///< Elements currently in the buffer.
};

} // namespace eph::containers
