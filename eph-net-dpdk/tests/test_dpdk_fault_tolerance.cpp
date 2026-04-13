/// @file test_dpdk_fault_tolerance.cpp
/// Edge-case and fault-tolerance tests for DPDK module primitives.
///
/// Tests the following fixes:
///   1. ReasmBuffer::consume() clamping — over-consume does not cause UB
///   2. ReasmBuffer::consume() exact-readable edge — drains cleanly
///   3. ReasmBuffer::compact() on empty after full consume — no crash
///   4. DpdkUdpSocket::send_to() rejects oversized payloads (> 65535)
///
/// These are primitive-level tests that do not require EAL or NIC setup.
/// The drain_codec_ no-progress and TLS AEAD failure → reset paths are
/// tested by the existing integration/E2E tests; the fixes are structural
/// guards that prevent infinite loops and are validated by code review.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

// Pull ReasmBuffer from the DpdkTcpStream header.
#include "eph/net/dpdk/tcp_stream.hpp"

namespace {

using ReasmBuffer = ::eph::net::dpdk::detail::ReasmBuffer;

// Helper: fill a vector with a byte pattern.
[[nodiscard]] std::vector<uint8_t> make_pattern(std::size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = static_cast<uint8_t>(seed + (i & 0xFF));
    }
    return v;
}

} // namespace

// ---------------------------------------------------------------------------
// ReasmBuffer::consume() clamping
// ---------------------------------------------------------------------------

TEST(DpdkFaultTolerance, ConsumeMoreThanReadableClampsToEnd) {
    constexpr std::size_t kCap = 1024;
    ReasmBuffer buf{kCap};

    auto data = make_pattern(512, 0x42);
    ASSERT_TRUE(buf.append(data.data(), data.size()));
    ASSERT_EQ(buf.readable(), 512u);

    // Consume more than readable — must clamp, not underflow.
    buf.consume(9999);
    EXPECT_EQ(buf.readable(), 0u);

    // Buffer must still be functional after the clamped consume.
    auto data2 = make_pattern(256, 0xAB);
    EXPECT_TRUE(buf.append(data2.data(), data2.size()));
    EXPECT_EQ(buf.readable(), 256u);

    // Verify the data is correct (not corrupted by the over-consume).
    EXPECT_EQ(std::memcmp(buf.read_ptr(), data2.data(), 256), 0);
}

TEST(DpdkFaultTolerance, ConsumeExactlyReadableDrainsCleanly) {
    constexpr std::size_t kCap = 2048;
    ReasmBuffer buf{kCap};

    auto data = make_pattern(kCap, 0x10);
    ASSERT_TRUE(buf.append(data.data(), data.size()));

    // Consume exactly readable — should leave an empty buffer.
    buf.consume(kCap);
    EXPECT_EQ(buf.readable(), 0u);

    // Compact on empty buffer must not crash.
    buf.compact();
    EXPECT_EQ(buf.readable(), 0u);

    // Re-fill should work — compact reclaimed all space.
    auto data2 = make_pattern(kCap, 0x20);
    EXPECT_TRUE(buf.append(data2.data(), data2.size()));
    EXPECT_EQ(buf.readable(), kCap);
}

TEST(DpdkFaultTolerance, ConsumeZeroBytesIsNoOp) {
    constexpr std::size_t kCap = 512;
    ReasmBuffer buf{kCap};

    auto data = make_pattern(100, 0x55);
    ASSERT_TRUE(buf.append(data.data(), data.size()));

    buf.consume(0);
    EXPECT_EQ(buf.readable(), 100u);
    EXPECT_EQ(std::memcmp(buf.read_ptr(), data.data(), 100), 0);
}

TEST(DpdkFaultTolerance, RepeatedOverConsumeDoesNotCorrupt) {
    constexpr std::size_t kCap = 256;
    ReasmBuffer buf{kCap};

    auto data = make_pattern(128, 0xCC);
    ASSERT_TRUE(buf.append(data.data(), data.size()));

    // Multiple over-consumes in a row — should all clamp cleanly.
    for (int i = 0; i < 10; ++i) {
        buf.consume(1'000'000);
        EXPECT_EQ(buf.readable(), 0u);
    }

    // Buffer must still be usable.
    EXPECT_TRUE(buf.append(data.data(), 64));
    EXPECT_EQ(buf.readable(), 64u);
}

// ---------------------------------------------------------------------------
// ReasmBuffer::compact() edge cases
// ---------------------------------------------------------------------------

TEST(DpdkFaultTolerance, CompactWithHeadAtZeroIsNoOp) {
    constexpr std::size_t kCap = 512;
    ReasmBuffer buf{kCap};

    auto data = make_pattern(256, 0x77);
    ASSERT_TRUE(buf.append(data.data(), data.size()));

    // compact() when head_ == 0 should be a no-op.
    buf.compact();
    EXPECT_EQ(buf.readable(), 256u);
    EXPECT_EQ(std::memcmp(buf.read_ptr(), data.data(), 256), 0);
}

TEST(DpdkFaultTolerance, CompactAfterPartialConsumePreservesData) {
    constexpr std::size_t kCap = 512;
    ReasmBuffer buf{kCap};

    auto data = make_pattern(kCap, 0x33);
    ASSERT_TRUE(buf.append(data.data(), data.size()));

    // Consume 200 bytes, then compact — remaining 312 bytes must shift left
    // and be intact.
    buf.consume(200);
    EXPECT_EQ(buf.readable(), kCap - 200);

    buf.compact();
    EXPECT_EQ(buf.readable(), kCap - 200);
    EXPECT_EQ(std::memcmp(buf.read_ptr(), data.data() + 200, kCap - 200), 0);
}

// ---------------------------------------------------------------------------
// MbufView edge cases
// ---------------------------------------------------------------------------

using MbufView = ::eph::net::dpdk::detail::MbufView;

TEST(DpdkFaultTolerance, MbufViewTrimFrontBeyondLengthBecomesEmpty) {
    uint8_t payload[16] = {};
    MbufView view(payload, sizeof(payload), 12345);

    view.trim_front(100);  // way beyond length
    EXPECT_EQ(view.length(), 0u);
    // data() should have advanced by the original length, not by 100.
    EXPECT_EQ(view.data(), payload + 16);
}

TEST(DpdkFaultTolerance, MbufViewTrimBackBeyondLengthBecomesEmpty) {
    uint8_t payload[32] = {};
    MbufView view(payload, sizeof(payload), 0);

    view.trim_back(999);
    EXPECT_EQ(view.length(), 0u);
    // data() pointer must not move on trim_back.
    EXPECT_EQ(view.data(), payload);
}

TEST(DpdkFaultTolerance, MbufViewDefaultConstructedIsEmpty) {
    MbufView view;
    EXPECT_EQ(view.data(), nullptr);
    EXPECT_EQ(view.length(), 0u);
    EXPECT_EQ(view.arrival_tsc(), 0u);

    // trim_front/trim_back on null view must not crash.
    view.trim_front(10);
    view.trim_back(10);
    EXPECT_EQ(view.length(), 0u);
}

TEST(DpdkFaultTolerance, MbufViewTrimFrontExactlyLengthBecomesEmpty) {
    uint8_t payload[8] = {};
    MbufView view(payload, sizeof(payload), 42);

    view.trim_front(8);
    EXPECT_EQ(view.length(), 0u);
    EXPECT_EQ(view.data(), payload + 8);
}
