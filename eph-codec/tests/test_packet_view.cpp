/// @file test_packet_view.cpp
/// Compile-time + runtime conformance for the formal
/// `eph::core::PacketView` concept declared in
/// `eph/core/packet_view.hpp`.
///
/// Verifies:
///   1. The codec-side `eph::codec::SpanPacketView` satisfies the concept.
///   2. A naive trivial type that *doesn't* expose `writable_data` is
///      correctly rejected.
///   3. The cursor semantics (trim_front / trim_back / length / data) match
///      the documented contract for the SpanPacketView reference impl.

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

#include "eph/codec/detail/span_packet_view.hpp"
#include "eph/core/packet_view.hpp"

namespace ec  = eph::core;
namespace ecd = eph::codec;

// ─── Compile-time conformance ───────────────────────────────────────────────

static_assert(ec::PacketView<ecd::SpanPacketView>,
              "SpanPacketView must satisfy eph::core::PacketView");

// A type that doesn't expose `writable_data()` should be rejected. This is
// the basic negative-test that proves the concept actually constrains.
struct NotAView {
    const uint8_t* data() const noexcept { return nullptr; }
    size_t length() const noexcept { return 0; }
    void trim_front(size_t) noexcept {}
    void trim_back(size_t) noexcept {}
    uint64_t arrival_tsc() const noexcept { return 0; }
};
static_assert(!ec::PacketView<NotAView>,
              "A type missing writable_data must NOT satisfy PacketView");

// Pin the OTHER required members too — a refactor that loosened the concept
// (e.g. dropped `trim_back` because no in-tree caller used it) would otherwise
// silently regress the contract. Each negative type exercises exactly one
// missing member so a future failure points straight at the regression.

struct MissingDataView {
    uint8_t* writable_data() noexcept { return nullptr; }
    size_t length() const noexcept { return 0; }
    void trim_front(size_t) noexcept {}
    void trim_back(size_t) noexcept {}
    uint64_t arrival_tsc() const noexcept { return 0; }
};
static_assert(!ec::PacketView<MissingDataView>,
              "PacketView must require const data()");

struct MissingLengthView {
    uint8_t* writable_data() noexcept { return nullptr; }
    const uint8_t* data() const noexcept { return nullptr; }
    void trim_front(size_t) noexcept {}
    void trim_back(size_t) noexcept {}
    uint64_t arrival_tsc() const noexcept { return 0; }
};
static_assert(!ec::PacketView<MissingLengthView>,
              "PacketView must require length()");

struct MissingTrimFrontView {
    uint8_t* writable_data() noexcept { return nullptr; }
    const uint8_t* data() const noexcept { return nullptr; }
    size_t length() const noexcept { return 0; }
    void trim_back(size_t) noexcept {}
    uint64_t arrival_tsc() const noexcept { return 0; }
};
static_assert(!ec::PacketView<MissingTrimFrontView>,
              "PacketView must require trim_front()");

struct MissingTrimBackView {
    uint8_t* writable_data() noexcept { return nullptr; }
    const uint8_t* data() const noexcept { return nullptr; }
    size_t length() const noexcept { return 0; }
    void trim_front(size_t) noexcept {}
    uint64_t arrival_tsc() const noexcept { return 0; }
};
static_assert(!ec::PacketView<MissingTrimBackView>,
              "PacketView must require trim_back()");

struct MissingArrivalTscView {
    uint8_t* writable_data() noexcept { return nullptr; }
    const uint8_t* data() const noexcept { return nullptr; }
    size_t length() const noexcept { return 0; }
    void trim_front(size_t) noexcept {}
    void trim_back(size_t) noexcept {}
};
static_assert(!ec::PacketView<MissingArrivalTscView>,
              "PacketView must require arrival_tsc()");

// ─── Runtime cursor semantics on SpanPacketView ─────────────────────────────

TEST(PacketViewConcept, SpanPacketViewBasicCursor) {
    std::array<uint8_t, 8> bytes{0, 1, 2, 3, 4, 5, 6, 7};
    ecd::SpanPacketView v(bytes.data(), bytes.size(), /*tsc=*/12345ULL);

    EXPECT_EQ(v.length(), 8u);
    EXPECT_EQ(v.data()[0], 0u);
    EXPECT_EQ(v.arrival_tsc(), 12345ULL);

    v.trim_front(2);
    EXPECT_EQ(v.length(), 6u);
    EXPECT_EQ(v.data()[0], 2u);

    v.trim_back(1);
    EXPECT_EQ(v.length(), 5u);
    EXPECT_EQ(v.data()[v.length() - 1], 6u);

    // writable_data() should alias the same byte data() points at.
    EXPECT_EQ(v.writable_data(), const_cast<uint8_t*>(v.data()));
}

TEST(PacketViewConcept, SpanPacketViewIsTriviallyMutated) {
    std::array<uint8_t, 4> bytes{10, 20, 30, 40};
    ecd::SpanPacketView v(bytes.data(), bytes.size());
    // In-place mutation through writable_data() must persist in the storage.
    v.writable_data()[1] = 99;
    EXPECT_EQ(bytes[1], 99u);
}
