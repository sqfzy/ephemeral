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
/// @warning NOT thread-safe for concurrent readers. `head_` and `count_`
///          are non-atomic. Use external synchronization if reading from
///          a different thread than the writer.
///
/// @tparam T        Element type (must be trivially copyable).
/// @tparam Capacity Buffer size — must be a power of two.
template <typename T, std::size_t Capacity>
    requires (std::is_trivially_copyable_v<T> && std::has_single_bit(Capacity))
class RingBuffer {
public:
    static constexpr std::size_t capacity = Capacity;

    /// Push a new element, overwriting the oldest when full.
    void push(const T& item) noexcept {
        data_[head_ & kMask] = item;
        ++head_;
        if (count_ < Capacity) {
            ++count_;
        }
    }

    /// Get element at @p offset from the newest.
    ///   0 = most recent, 1 = second most recent, etc.
    /// Returns std::nullopt if offset >= count().
    [[nodiscard]] std::optional<T> at(std::size_t offset) const noexcept {
        if (offset >= count_) {
            return std::nullopt;
        }
        // head_ points one past the last written slot.
        // Most recent element is at (head_ - 1), so offset 0 maps there.
        const std::size_t idx = (head_ - 1 - offset) & kMask;
        return data_[idx];
    }

    /// Most recent element.
    [[nodiscard]] std::optional<T> front() const noexcept {
        return at(0);
    }

    /// Oldest element in the buffer.
    [[nodiscard]] std::optional<T> back() const noexcept {
        if (count_ == 0) {
            return std::nullopt;
        }
        return at(count_ - 1);
    }

    /// Number of elements currently stored (up to Capacity).
    [[nodiscard]] std::size_t count() const noexcept {
        return count_;
    }

    /// Whether the buffer has reached its fixed capacity.
    [[nodiscard]] bool full() const noexcept {
        return count_ == Capacity;
    }

    /// Whether the buffer contains no elements.
    [[nodiscard]] bool empty() const noexcept {
        return count_ == 0;
    }

    /// Remove all elements and reset write position.
    void clear() noexcept {
        head_  = 0;
        count_ = 0;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    std::array<T, Capacity> data_{};
    std::size_t head_  = 0;   ///< Next write position (monotonically increasing).
    std::size_t count_ = 0;   ///< Elements currently in the buffer.
};

} // namespace eph::containers
