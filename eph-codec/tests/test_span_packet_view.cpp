/// @file test_span_packet_view.cpp
/// Unit tests for `eph::codec::SpanPacketView`.
///
/// SpanPacketView is the canonical PacketView used inside eph-codec test
/// fixtures so codecs can be exercised independently of any backend
/// (kernel SpanView, DPDK MbufView). Until now its trim
/// methods were exercised only indirectly through codec tests — a
/// regression in the clamp logic would surface as a confusing oversize
/// span inside one of those codec tests rather than a focused failure.
///
/// These tests pin the same trim-clamp invariants
/// that test_span_view.cpp covers for the kernel sibling, so either
/// implementation drifting from the shared PacketView contract is caught.

#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>

#include "eph/codec/detail/span_packet_view.hpp"
#include "eph/core/packet_view.hpp"

using eph::codec::SpanPacketView;

// ---------------------------------------------------------------------------
// Concept conformance
// ---------------------------------------------------------------------------

static_assert(eph::core::PacketView<SpanPacketView>,
              "SpanPacketView must satisfy eph::core::PacketView");

// ---------------------------------------------------------------------------
// Construction & basic accessors
// ---------------------------------------------------------------------------

TEST(SpanPacketView, ConstructionExposesData) {
    uint8_t buf[] = {0x10, 0x20, 0x30};
    SpanPacketView v(buf, sizeof(buf));
    EXPECT_EQ(v.data(), buf);
    EXPECT_EQ(v.writable_data(), buf);
    EXPECT_EQ(v.length(), 3u);
}

TEST(SpanPacketView, ZeroLengthEmptyView) {
    SpanPacketView v(nullptr, 0);
    EXPECT_EQ(v.length(), 0u);
}

TEST(SpanPacketView, WritableDataMatchesData) {
    uint8_t buf[] = {0xAB};
    SpanPacketView v(buf, sizeof(buf));
    EXPECT_EQ(v.writable_data(), v.data());
}

// ---------------------------------------------------------------------------
// trim_front / trim_back — partial / exact / over-trim clamp
// ---------------------------------------------------------------------------

TEST(SpanPacketView, TrimFrontPartialAdvancesHead) {
    uint8_t buf[] = {0x01, 0x02, 0x03, 0x04};
    SpanPacketView v(buf, sizeof(buf));
    v.trim_front(2);
    EXPECT_EQ(v.length(), 2u);
    EXPECT_EQ(v.data(), buf + 2);
    EXPECT_EQ(v.data()[0], 0x03);
}

TEST(SpanPacketView, TrimFrontExactCollapsesToEmpty) {
    uint8_t buf[] = {0x01, 0x02};
    SpanPacketView v(buf, sizeof(buf));
    v.trim_front(2);
    EXPECT_EQ(v.length(), 0u);
}

TEST(SpanPacketView, TrimFrontOverClampsAtLength) {
    uint8_t buf[] = {0x55, 0x66};
    SpanPacketView v(buf, sizeof(buf));
    v.trim_front(99);
    EXPECT_EQ(v.length(), 0u);
    EXPECT_EQ(v.data(), buf + 2);
}

TEST(SpanPacketView, TrimBackPartialPullsTail) {
    uint8_t buf[] = {0xAA, 0xBB, 0xCC};
    SpanPacketView v(buf, sizeof(buf));
    v.trim_back(1);
    EXPECT_EQ(v.length(), 2u);
    EXPECT_EQ(v.data(), buf);
}

TEST(SpanPacketView, TrimBackExactCollapsesToEmpty) {
    uint8_t buf[] = {0x77};
    SpanPacketView v(buf, sizeof(buf));
    v.trim_back(1);
    EXPECT_EQ(v.length(), 0u);
}

TEST(SpanPacketView, TrimBackOverClampsAtLength) {
    uint8_t buf[] = {0x88, 0x99};
    SpanPacketView v(buf, sizeof(buf));
    v.trim_back(50);
    EXPECT_EQ(v.length(), 0u);
}

TEST(SpanPacketView, SequentialTrimSharesShrinkingWindow) {
    uint8_t buf[] = {1, 2, 3, 4, 5};
    SpanPacketView v(buf, sizeof(buf));
    v.trim_front(2);   // length 3
    v.trim_back(99);   // clamp on residual 3
    EXPECT_EQ(v.length(), 0u);
}

// ---------------------------------------------------------------------------
// Type traits
// ---------------------------------------------------------------------------

TEST(SpanPacketView, IsTriviallyCopyableAndDestructible) {
    static_assert(std::is_trivially_copyable_v<SpanPacketView>);
    static_assert(std::is_trivially_destructible_v<SpanPacketView>);
    SUCCEED();
}
