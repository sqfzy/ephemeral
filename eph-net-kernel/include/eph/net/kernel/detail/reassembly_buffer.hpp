#pragma once

/// @file reassembly_buffer.hpp
/// Simple linear reassembly buffer used by `KernelTcpStream` between recv()
/// and the codec decode path.
///
/// Design: a flat byte vector with head/tail offsets. `read_ptr()` and
/// `readable()` expose the current window; `commit_write(n)` grows the tail
/// after a recv() drops bytes directly into `writable_ptr()`; `consume(n)`
/// advances the head after the codec has extracted bytes; `compact()` moves
/// the unread region back to offset 0 when the free tailroom drops below a
/// threshold.
///
/// This is intentionally not a ring buffer: codecs receive a contiguous span
/// and the StreamCodec contract wants a `PacketView` that advances
/// monotonically. The compaction step ensures `writable_capacity()` stays
/// non-zero as long as the buffer is not logically full.
///
/// All operations are noexcept and allocation-free after construction (the
/// backing storage is a single `std::vector<uint8_t>` sized once by the
/// constructor).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace eph::net::kernel::detail {

/// @brief Linear RX reassembly buffer with head/tail offsets and compaction.
class ReassemblyBuffer {
public:
    /// @brief Construct with a fixed capacity. `cap` should be >= the largest
    ///        expected frame the codec can emit, with headroom for partial
    ///        reads.
    explicit ReassemblyBuffer(std::size_t cap)
        : buf_(cap), head_(0), tail_(0) {}

    /// @brief Pointer to the first unread byte.
    [[nodiscard]] uint8_t* read_ptr() noexcept { return buf_.data() + head_; }

    /// @brief Number of unread bytes currently buffered.
    [[nodiscard]] std::size_t readable() const noexcept {
        return tail_ - head_;
    }

    /// @brief Pointer to the first writable byte at the tail. May be nullptr
    ///        if the buffer is currently full (caller must compact/drain).
    [[nodiscard]] uint8_t* writable_ptr() noexcept {
        return buf_.data() + tail_;
    }

    /// @brief Number of bytes available for writing at the tail.
    [[nodiscard]] std::size_t writable_capacity() const noexcept {
        return buf_.size() - tail_;
    }

    /// @brief Commit `n` bytes previously written via `writable_ptr()`.
    void commit_write(std::size_t n) noexcept { tail_ += n; }

    /// @brief Advance the head by `n` unread bytes.
    void consume(std::size_t n) noexcept {
        head_ = std::min(head_ + n, tail_);
        if (head_ == tail_) {
            head_ = 0;
            tail_ = 0;
        }
    }

    /// @brief Unconditionally move the unread window back to offset 0 so the
    ///        tail has room to grow again.
    ///
    /// Called internally by the Stream before every recv() so that a slow
    /// stream of tiny frames cannot starve the tail of writable bytes.
    /// No-op when `head_ == 0`. There is no "headroom threshold" knob —
    /// compaction is always free relative to the cost of growing tail
    /// against a wall, and the recv() path already pays a memmove on
    /// stale frames anyway.
    void compact() noexcept {
        if (head_ == 0) return;
        const std::size_t n = readable();
        if (n == 0) {
            head_ = 0;
            tail_ = 0;
            return;
        }
        std::memmove(buf_.data(), buf_.data() + head_, n);
        head_ = 0;
        tail_ = n;
    }

    /// @brief Total allocated capacity.
    [[nodiscard]] std::size_t capacity() const noexcept { return buf_.size(); }

    /// @brief Reset to empty without deallocating.
    void clear() noexcept {
        head_ = 0;
        tail_ = 0;
    }

private:
    std::vector<uint8_t> buf_;
    std::size_t          head_;
    std::size_t          tail_;
};

} // namespace eph::net::kernel::detail
