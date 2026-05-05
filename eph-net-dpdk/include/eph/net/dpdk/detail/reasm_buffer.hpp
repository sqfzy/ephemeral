#pragma once

/// @file detail/reasm_buffer.hpp
/// Reassembly ring buffer for the DPDK TCP stream codec drain loop.
///
/// Extracted from `eph/net/dpdk/tcp_stream.hpp` (T2.2 partial split,
/// 2026-05-05). Self-contained — no DPDK / TLS / TcpSession deps,
/// only `<vector>` and `<cstring>`. Tests under
/// `tests/test_dpdk_fault_tolerance.cpp`,
/// `tests/test_dpdk_reasm_overflow.cpp`,
/// `tests/test_dpdk_drain.cpp` already exercise it via the
/// `eph::net::dpdk::detail::ReasmBuffer` symbol they import.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace eph::net::dpdk::detail {

/// @brief Reassembly buffer for the codec decode loop. Bytes dispatched
///        in from the Poller append here, then the codec drains them
///        incrementally. Implemented as a simple std::vector<uint8_t>
///        with a front cursor.
class ReasmBuffer {
public:
    explicit ReasmBuffer(std::size_t cap = 256 * 1024) { buf_.resize(cap); }

    [[nodiscard]] std::size_t readable() const noexcept { return tail_ - head_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return buf_.size(); }
    [[nodiscard]] std::size_t writable_capacity() const noexcept {
        return buf_.size() - tail_;
    }
    [[nodiscard]] const uint8_t* read_ptr() const noexcept {
        return buf_.data() + head_;
    }
    [[nodiscard]] uint8_t* writable_ptr() noexcept { return buf_.data() + tail_; }

    void commit_write(std::size_t n) noexcept { tail_ += n; }
    void consume(std::size_t n) noexcept {
        if (n >= readable()) {
            head_ = tail_;  // clamp — never push head_ past tail_
        } else {
            head_ += n;
        }
    }
    void compact() noexcept {
        if (head_ == 0) return;
        if (readable() == 0) {
            head_ = tail_ = 0;
            return;
        }
        std::memmove(buf_.data(), buf_.data() + head_, readable());
        tail_ -= head_;
        head_  = 0;
    }
    /// @brief Append `n` bytes from `src`, compacting first if needed.
    /// @return true on success, false if there is not enough room even
    ///         after compaction.
    [[nodiscard]] bool append(const uint8_t* src, std::size_t n) noexcept {
        if (writable_capacity() < n) {
            compact();
            if (writable_capacity() < n) return false;
        }
        std::memcpy(writable_ptr(), src, n);
        commit_write(n);
        return true;
    }

private:
    std::vector<uint8_t> buf_;
    std::size_t          head_{0};
    std::size_t          tail_{0};
};

}  // namespace eph::net::dpdk::detail
