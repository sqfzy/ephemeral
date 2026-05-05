/// @file test_span_view.cpp
/// Unit tests for SpanView — the kernel-backend PacketView.
///
/// SpanView is one of three PacketView implementations (kernel SpanView,
/// DPDK MbufView, mock FakeStream::PacketView). It is the most direct
/// implementation — a contiguous head/tail cursor over a caller-owned
/// byte buffer — but lacked dedicated tests; the only coverage was an
/// indirect static_assert in test_kernel_tls_state.cpp.
///
/// These tests pin the trim-clamp invariants and arrival_tsc propagation
/// against the documented PacketView contract so a future drift surfaces
/// here rather than inside whichever codec test happened to use a too-
/// large trim argument.

#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>

#include "eph/core/packet_view.hpp"
#include "eph/net/kernel/detail/span_view.hpp"

using eph::net::kernel::detail::SpanView;

// ---------------------------------------------------------------------------
// Concept conformance — duplicates the in-header static_assert so a CI
// failure here pinpoints the test file rather than a transitive include.
// ---------------------------------------------------------------------------

static_assert(eph::core::PacketView<SpanView>,
              "SpanView must satisfy eph::core::PacketView");

// ---------------------------------------------------------------------------
// Construction & basic accessors
// ---------------------------------------------------------------------------

TEST(SpanView, ConstructionExposesData) {
    uint8_t buf[] = {0xAA, 0xBB, 0xCC};
    SpanView v(buf, sizeof(buf));
    EXPECT_EQ(v.data(), buf);
    EXPECT_EQ(v.writable_data(), buf);
    EXPECT_EQ(v.length(), 3u);
}

TEST(SpanView, ZeroLengthEmptyView) {
    // Allowed: payload may be nullptr iff len == 0 per ctor doc.
    SpanView v(nullptr, 0);
    EXPECT_EQ(v.length(), 0u);
}

TEST(SpanView, WritableDataMatchesData) {
    uint8_t buf[] = {0x01, 0x02};
    SpanView v(buf, sizeof(buf));
    EXPECT_EQ(v.writable_data(), v.data());
}

// ---------------------------------------------------------------------------
// trim_front / trim_back — exact, partial, and over-trim clamp
// ---------------------------------------------------------------------------

TEST(SpanView, TrimFrontPartialAdvancesHead) {
    uint8_t buf[] = {0x10, 0x20, 0x30, 0x40};
    SpanView v(buf, sizeof(buf));
    v.trim_front(2);
    EXPECT_EQ(v.length(), 2u);
    EXPECT_EQ(v.data(), buf + 2);
    EXPECT_EQ(v.data()[0], 0x30);
}

TEST(SpanView, TrimFrontExactCollapsesToEmpty) {
    uint8_t buf[] = {0xDE, 0xAD};
    SpanView v(buf, sizeof(buf));
    v.trim_front(2);
    EXPECT_EQ(v.length(), 0u);
}

TEST(SpanView, TrimFrontOverClampsAtLength) {
    // Without the clamp added in the file's commit-history fix, an
    // over-trim would drive head_ past tail_ and wrap length() to a huge
    // value; codecs accessing data()[i] would then read past buf and the
    // failure would only surface as a UBSan / sanitizer hit later.
    uint8_t buf[] = {0x01, 0x02};
    SpanView v(buf, sizeof(buf));
    v.trim_front(99);
    EXPECT_EQ(v.length(), 0u);
    EXPECT_EQ(v.data(), buf + 2);
}

TEST(SpanView, TrimBackPartialPullsTail) {
    uint8_t buf[] = {0x11, 0x22, 0x33, 0x44};
    SpanView v(buf, sizeof(buf));
    v.trim_back(1);
    EXPECT_EQ(v.length(), 3u);
    EXPECT_EQ(v.data(), buf);
    EXPECT_EQ(v.data()[2], 0x33);
}

TEST(SpanView, TrimBackExactCollapsesToEmpty) {
    uint8_t buf[] = {0x77, 0x88};
    SpanView v(buf, sizeof(buf));
    v.trim_back(2);
    EXPECT_EQ(v.length(), 0u);
}

TEST(SpanView, TrimBackOverClampsAtLength) {
    uint8_t buf[] = {0xFE};
    SpanView v(buf, sizeof(buf));
    v.trim_back(50);
    EXPECT_EQ(v.length(), 0u);
}

TEST(SpanView, TrimFrontThenTrimBackSharesShrunkWindow) {
    // Sequential trims must clamp on the residual window. A naive
    // `tail_ -= n` after a partial trim_front would wrap when n exceeds
    // the remaining length.
    uint8_t buf[] = {1, 2, 3, 4, 5, 6, 7, 8};
    SpanView v(buf, sizeof(buf));
    v.trim_front(5);
    EXPECT_EQ(v.length(), 3u);
    v.trim_back(99);  // clamp on residual 3, not original 8
    EXPECT_EQ(v.length(), 0u);
}

TEST(SpanView, TrimBackThenTrimFrontSharesShrunkWindow) {
    uint8_t buf[] = {1, 2, 3, 4, 5};
    SpanView v(buf, sizeof(buf));
    v.trim_back(2);     // [0,3) → length 3
    EXPECT_EQ(v.length(), 3u);
    v.trim_front(10);   // clamp on residual 3
    EXPECT_EQ(v.length(), 0u);
}

// ---------------------------------------------------------------------------
// arrival_tsc — propagates from ctor and stays stable across trims
// ---------------------------------------------------------------------------

TEST(SpanView, ArrivalTscDefaultsToZero) {
    uint8_t buf[] = {0x00};
    SpanView v(buf, sizeof(buf));
    EXPECT_EQ(v.arrival_tsc(), 0u);
}

TEST(SpanView, ArrivalTscPropagatesFromCtor) {
    uint8_t buf[] = {0x42};
    SpanView v(buf, sizeof(buf), /*tsc=*/0xFEEDFACEDEADBEEFull);
    EXPECT_EQ(v.arrival_tsc(), 0xFEEDFACEDEADBEEFull);
}

TEST(SpanView, ArrivalTscStableAcrossTrims) {
    // The timestamp pinpoints arrival, not the post-decode window — trim
    // ops must not perturb it.
    uint8_t buf[] = {1, 2, 3, 4};
    SpanView v(buf, sizeof(buf), 0x123456789ULL);
    v.trim_front(1);
    EXPECT_EQ(v.arrival_tsc(), 0x123456789ULL);
    v.trim_back(1);
    EXPECT_EQ(v.arrival_tsc(), 0x123456789ULL);
    v.trim_front(99);  // even on collapse
    EXPECT_EQ(v.arrival_tsc(), 0x123456789ULL);
}

// ---------------------------------------------------------------------------
// Move semantics — SpanView is a tiny aggregate-like struct over
// (uint8_t*, size_t, size_t, uint64_t); pin the standard shape so a
// future refactor that inadvertently introduces a non-trivial dtor
// or copy-blocking surfaces here.
// ---------------------------------------------------------------------------

TEST(SpanView, IsTriviallyCopyableAndDestructible) {
    static_assert(std::is_trivially_copyable_v<SpanView>);
    static_assert(std::is_trivially_destructible_v<SpanView>);
    SUCCEED();
}
