/// @file test_concepts.cpp
/// Compile-time + runtime sanity checks for the v3.3 `eph::net` concepts.
///
/// The real teeth of the file is the `static_assert` block: if a fake or a
/// concept drifts out of sync, compilation fails here. The gtest
/// `TEST(…)` bodies exist mostly to anchor the translation unit as a
/// test target so the compile-time checks are actually exercised by CI.

#include <gtest/gtest.h>

#include "eph/net/concepts.hpp"
#include "eph/net/test/fake_datagram.hpp"
#include "eph/net/test/fake_stream.hpp"
#include "eph/net/test/test_poller.hpp"

namespace en  = eph::net;
namespace ent = eph::net::test;

// ─── Positive conformance ─────────────────────────────────────────────────
// FakeStream/FakeDatagram already embed their own static_asserts inside the
// header, but we repeat them here as a translation-unit-local smoke test.

static_assert(en::Pollable<ent::FakeStream>,
              "FakeStream must satisfy Pollable");
static_assert(en::Pollable<ent::FakeDatagram>,
              "FakeDatagram must satisfy Pollable");

static_assert(en::Stream<ent::FakeStream>,
              "FakeStream must satisfy Stream");
static_assert(en::Datagram<ent::FakeDatagram>,
              "FakeDatagram must satisfy Datagram");

// Subsumption: Stream / Datagram refine Pollable, so any Stream type
// must also satisfy the Pollable concept. Evaluated purely at the type
// level so FakeStream/FakeDatagram don't need to be literal types.
template <class P> constexpr bool accepts_pollable_type() requires en::Pollable<P>
    { return true; }
static_assert(accepts_pollable_type<ent::FakeStream>());
static_assert(accepts_pollable_type<ent::FakeDatagram>());

// TestPoller<FakeStream> and TestPoller<FakeDatagram> are the two canonical
// Poller instantiations. Verify they both satisfy the Poller concept (just
// the base shape — the PollerOf refinement is a separate check below).
static_assert(en::Poller<ent::TestPoller<ent::FakeStream>>,
              "TestPoller<FakeStream> must satisfy Poller");
static_assert(en::Poller<ent::TestPoller<ent::FakeDatagram>>,
              "TestPoller<FakeDatagram> must satisfy Poller");

// PollerOf refinement: does TestPoller<FakeStream> accept FakeStream*?
static_assert(en::PollerOf<ent::TestPoller<ent::FakeStream>, ent::FakeStream>,
              "TestPoller<FakeStream> must accept FakeStream*");
static_assert(en::PollerOf<ent::TestPoller<ent::FakeDatagram>, ent::FakeDatagram>,
              "TestPoller<FakeDatagram> must accept FakeDatagram*");

// ─── Negative conformance ─────────────────────────────────────────────────
// Arbitrary unrelated types must NOT satisfy the concepts.

static_assert(!en::Pollable<int>);
static_assert(!en::Stream<int>);
static_assert(!en::Datagram<int>);
static_assert(!en::Poller<int>);

// A nearly-conforming type that only implements half the contract should
// still fail. We make `AlmostStream` deliberately missing `state()`.
struct AlmostStream {
    struct PacketView {};
    using CodecType = void;
    using OnMessage = std::function<void(const uint8_t*, uint16_t)>;
    OnMessage on_message;
    std::size_t poll_once_() noexcept { return 0; }
    bool is_attached_() const noexcept { return false; }
    void* native_handle() noexcept { return nullptr; }
    std::expected<std::size_t, eph::core::ErrorInfo>
        send(std::span<const uint8_t>) { return 0; }
    std::expected<void, eph::core::ErrorInfo>
        close_gracefully() noexcept { return {}; }
    bool is_attached() const noexcept { return false; }
    // Deliberately NO `state()` method.
};

static_assert(en::Pollable<AlmostStream>,
              "AlmostStream should still satisfy Pollable (sanity)");
static_assert(!en::Stream<AlmostStream>,
              "AlmostStream missing state() must fail the Stream concept");

// ─── Runtime smoke ────────────────────────────────────────────────────────

TEST(Concepts, FakeStreamRoundTripsThroughTestPoller) {
    auto poller = ent::TestPoller<ent::FakeStream>::create();
    auto fake = ent::FakeStream::create();
    std::vector<uint8_t> captured;
    fake->on_message = [&](const uint8_t* p, uint16_t n) {
        captured.assign(p, p + n);
    };
    ASSERT_TRUE(poller->add(fake.get()).has_value());
    EXPECT_TRUE(fake->is_attached());

    const uint8_t bytes[] = {0xDE, 0xAD, 0xBE, 0xEF};
    fake->inject_rx(bytes);
    EXPECT_EQ(poller->poll(), 1u);
    ASSERT_EQ(captured.size(), 4u);
    EXPECT_EQ(captured[0], 0xDE);
    EXPECT_EQ(captured[3], 0xEF);
}

TEST(Concepts, FakeDatagramRoundTripsThroughTestPoller) {
    auto poller = ent::TestPoller<ent::FakeDatagram>::create();
    auto fake = ent::FakeDatagram::create();
    bool saw = false;
    fake->on_datagram = [&](const uint8_t*, uint16_t n, const en::SocketAddr& src) {
        saw = true;
        EXPECT_EQ(n, 3u);
        EXPECT_EQ(src.port, 9999);
    };
    ASSERT_TRUE(poller->add(fake.get()).has_value());

    const uint8_t bytes[] = {1, 2, 3};
    fake->inject_datagram(bytes, en::SocketAddr{en::Ipv4Addr{10, 0, 0, 1}, 9999});
    EXPECT_EQ(poller->poll(), 1u);
    EXPECT_TRUE(saw);
}
