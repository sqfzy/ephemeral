/// @file test_dpdk_reasm_overflow.cpp
/// Unit tests for `eph::net::dpdk::detail::ReasmBuffer` overflow
/// semantics. The surrounding `DpdkTcpStream` integration is NOT tested
/// here — that path requires real EAL + NIC setup — but the primitive
/// being guarded is the `append()` return value plus
/// the reassembly buffer's default capacity (256 KiB as of this fix;
/// HFT L2 orderbook snapshots routinely exceed the previous 64 KiB
/// default).
///
/// Covered cases:
///   * append() returns false when the request exceeds the writable
///     capacity even after compaction (this is the "reliability bug"
///     entry point that the stream's error path must treat as a hard
///     failure, not a silent drop).
///   * append() succeeds once consumed bytes free up compactable room.
///   * append() with a payload exactly equal to capacity at the fresh
///     buffer is accepted without surprise off-by-one rejection.
///   * The default capacity is the new 256 KiB, not the legacy 64 KiB.
///   * An explicit smaller capacity is honored.
///
/// These are primitive-level so they pin the contract without dragging
/// in the whole TCP state machine. The DpdkTcpStream integration is
/// covered implicitly by live E2E benchmarks (`lat tcp --dpdk` etc.) and
/// the existing `test_dpdk_tcp_stream.cpp` build checks.

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

// Pull the ReasmBuffer definition. It lives inside the TcpStream header
// under `eph::net::dpdk::detail`.
#include "eph/net/dpdk/tcp_stream.hpp"

namespace {

using ReasmBuffer = ::eph::net::dpdk::detail::ReasmBuffer;

// Helper: fill a vector with a byte pattern derived from its index so
// subsequent corruption checks (if any) can detect shifted windows.
[[nodiscard]] std::vector<uint8_t> make_pattern(std::size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = static_cast<uint8_t>(seed + (i & 0xFF));
    }
    return v;
}

} // namespace

TEST(DpdkReasmOverflow, AppendReturnsFalseWhenCapacityExceeded) {
    // Use an explicit small buffer so the test does not need to
    // allocate 256 KiB just to exceed it.
    constexpr std::size_t kCap = 4096;
    ReasmBuffer buf{kCap};
    ASSERT_EQ(buf.capacity(), kCap);
    ASSERT_EQ(buf.readable(), 0u);

    auto first = make_pattern(kCap, 0xA0);
    EXPECT_TRUE(buf.append(first.data(), first.size()));
    EXPECT_EQ(buf.readable(), kCap);

    // One more byte past full capacity must fail; there is nothing to
    // compact (head_ == 0), so the buffer cannot make room.
    uint8_t tail = 0xFF;
    EXPECT_FALSE(buf.append(&tail, 1));
    EXPECT_EQ(buf.readable(), kCap);
}

TEST(DpdkReasmOverflow, AppendSucceedsAfterCompact) {
    constexpr std::size_t kCap = 4096;
    ReasmBuffer buf{kCap};

    // Fill to capacity.
    auto filler = make_pattern(kCap, 0x10);
    ASSERT_TRUE(buf.append(filler.data(), filler.size()));

    // Consume half the buffer so compaction can reclaim the prefix.
    buf.consume(kCap / 2);
    EXPECT_EQ(buf.readable(), kCap / 2);

    // Appending a block that fits only after compaction must succeed.
    auto extra = make_pattern(kCap / 2, 0x80);
    EXPECT_TRUE(buf.append(extra.data(), extra.size()));
    EXPECT_EQ(buf.readable(), kCap);
}

TEST(DpdkReasmOverflow, AppendExactlyAtCapacitySucceeds) {
    constexpr std::size_t kCap = 8192;
    ReasmBuffer buf{kCap};

    auto block = make_pattern(kCap, 0x55);
    EXPECT_TRUE(buf.append(block.data(), block.size()));
    EXPECT_EQ(buf.readable(), kCap);

    // A zero-length append on a full buffer is a no-op and must succeed.
    EXPECT_TRUE(buf.append(nullptr, 0));
}

TEST(DpdkReasmOverflow, DefaultCapacityIs256KiB) {
    ReasmBuffer buf{};
    EXPECT_EQ(buf.capacity(), 256u * 1024u);
}

TEST(DpdkReasmOverflow, ExplicitSmallCapacityHonoured) {
    ReasmBuffer buf{1024};
    EXPECT_EQ(buf.capacity(), 1024u);
    auto block = make_pattern(1024, 0x01);
    EXPECT_TRUE(buf.append(block.data(), block.size()));
    uint8_t extra = 0xEE;
    EXPECT_FALSE(buf.append(&extra, 1));
}
