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

// ─── Sub-phase 9.8: supplemental concept conformance tests ────────────────
//
// Port of baseline `test_tcp_concept.cpp` (46 cases) re-expressed against
// the v3.3 `Stream` / `Datagram` / `Pollable` / `Poller` concepts. Each
// TEST body is mostly a `static_assert` wall; gtest exists here purely to
// anchor the translation unit so CI counts the compile-time checks.

// ── Near-miss types used by the negative conformance tests ──────────────
namespace {

// Missing `state()`: close to Stream but not quite.
struct MissingState {
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
};

// Missing `send()`: fails Stream but still a Pollable.
struct MissingSend {
    struct PacketView {};
    using CodecType = void;
    using OnMessage = std::function<void(const uint8_t*, uint16_t)>;
    OnMessage on_message;
    std::size_t poll_once_() noexcept { return 0; }
    bool is_attached_() const noexcept { return false; }
    void* native_handle() noexcept { return nullptr; }
    std::expected<void, eph::core::ErrorInfo>
        close_gracefully() noexcept { return {}; }
    bool is_attached() const noexcept { return false; }
    en::TcpState state() const noexcept { return en::TcpState::Closed; }
};

// Missing `on_message` field entirely.
struct MissingOnMessage {
    struct PacketView {};
    using CodecType = void;
    using OnMessage = std::function<void(const uint8_t*, uint16_t)>;
    std::size_t poll_once_() noexcept { return 0; }
    bool is_attached_() const noexcept { return false; }
    void* native_handle() noexcept { return nullptr; }
    std::expected<std::size_t, eph::core::ErrorInfo>
        send(std::span<const uint8_t>) { return 0; }
    std::expected<void, eph::core::ErrorInfo>
        close_gracefully() noexcept { return {}; }
    bool is_attached() const noexcept { return false; }
    en::TcpState state() const noexcept { return en::TcpState::Closed; }
};

// Wrong signature: send() returns void instead of expected<size_t>.
struct WrongSendSignature {
    struct PacketView {};
    using CodecType = void;
    using OnMessage = std::function<void(const uint8_t*, uint16_t)>;
    OnMessage on_message;
    std::size_t poll_once_() noexcept { return 0; }
    bool is_attached_() const noexcept { return false; }
    void* native_handle() noexcept { return nullptr; }
    void send(std::span<const uint8_t>) {}
    std::expected<void, eph::core::ErrorInfo>
        close_gracefully() noexcept { return {}; }
    bool is_attached() const noexcept { return false; }
    en::TcpState state() const noexcept { return en::TcpState::Closed; }
};

// Missing `native_handle`: fails Pollable outright.
struct MissingNativeHandle {
    struct PacketView {};
    std::size_t poll_once_() noexcept { return 0; }
    bool is_attached_() const noexcept { return false; }
};

// Missing `poll_once_`: fails Pollable outright.
struct MissingPollOnce {
    struct PacketView {};
    bool is_attached_() const noexcept { return false; }
    void* native_handle() noexcept { return nullptr; }
};

// Missing nested `PacketView` type: fails Pollable.
struct MissingPacketView {
    std::size_t poll_once_() noexcept { return 0; }
    bool is_attached_() const noexcept { return false; }
    void* native_handle() noexcept { return nullptr; }
};

// A Datagram near-miss: missing join_multicast.
struct MissingJoinMulticast {
    struct PacketView {};
    using CodecType  = void;
    using OnDatagram = std::function<void(const uint8_t*, uint16_t,
                                          const en::SocketAddr&)>;
    OnDatagram on_datagram;
    std::size_t poll_once_() noexcept { return 0; }
    bool is_attached_() const noexcept { return false; }
    void* native_handle() noexcept { return nullptr; }
    std::expected<std::size_t, eph::core::ErrorInfo>
        send_to(std::span<const uint8_t>, const en::SocketAddr&) { return 0; }
    bool is_attached() const noexcept { return false; }
    // No join_multicast / leave_multicast.
};

// A type that only implements poll() but no add/remove → Poller-basic only.
struct PollOnlyPoller {
    std::size_t poll() noexcept { return 0; }
};

} // namespace

// ── Test 1–4: FakeStream is Pollable, Stream, and Pollable-subsumes-Stream.
TEST(Concepts, Supplemental_FakeStreamIsPollable) {
    static_assert(en::Pollable<ent::FakeStream>);
    SUCCEED();
}
TEST(Concepts, Supplemental_FakeStreamIsStream) {
    static_assert(en::Stream<ent::FakeStream>);
    SUCCEED();
}
TEST(Concepts, Supplemental_FakeDatagramIsPollable) {
    static_assert(en::Pollable<ent::FakeDatagram>);
    SUCCEED();
}
TEST(Concepts, Supplemental_FakeDatagramIsDatagram) {
    static_assert(en::Datagram<ent::FakeDatagram>);
    SUCCEED();
}

// ── Test 5–6: Concept subsumption (Stream ⊂ Pollable, Datagram ⊂ Pollable).
TEST(Concepts, Supplemental_StreamImpliesPollable) {
    // If a type satisfies Stream, it must also satisfy Pollable — checked
    // here with a concept-constrained function template.
    constexpr auto accepts_any_pollable = []<en::Pollable P>() { return true; };
    static_assert(accepts_any_pollable.template operator()<ent::FakeStream>());
    SUCCEED();
}
TEST(Concepts, Supplemental_DatagramImpliesPollable) {
    constexpr auto accepts_any_pollable = []<en::Pollable P>() { return true; };
    static_assert(accepts_any_pollable.template operator()<ent::FakeDatagram>());
    SUCCEED();
}

// ── Test 7–8: TestPoller satisfies Poller and PollerOf<FakeStream/Dgram>.
TEST(Concepts, Supplemental_TestPollerIsPoller) {
    static_assert(en::Poller<ent::TestPoller<ent::FakeStream>>);
    static_assert(en::Poller<ent::TestPoller<ent::FakeDatagram>>);
    SUCCEED();
}
TEST(Concepts, Supplemental_PollerOfFakeStream) {
    static_assert(en::PollerOf<ent::TestPoller<ent::FakeStream>,
                               ent::FakeStream>);
    static_assert(en::PollerOf<ent::TestPoller<ent::FakeDatagram>,
                               ent::FakeDatagram>);
    SUCCEED();
}

// ── Test 9–11: Negative conformance — plain data types fail all concepts.
TEST(Concepts, Supplemental_IntIsNotPollable) {
    static_assert(!en::Pollable<int>);
    static_assert(!en::Stream<int>);
    static_assert(!en::Datagram<int>);
    static_assert(!en::Poller<int>);
    SUCCEED();
}
TEST(Concepts, Supplemental_VoidIsNotStream) {
    static_assert(!en::Stream<void>);
    SUCCEED();
}
TEST(Concepts, Supplemental_DoubleIsNotDatagram) {
    static_assert(!en::Datagram<double>);
    SUCCEED();
}

// ── Test 12–16: Negative conformance — near-miss types fail Stream/Datagram.
TEST(Concepts, Supplemental_MissingStateFailsStream) {
    static_assert(en::Pollable<MissingState>);
    static_assert(!en::Stream<MissingState>);
    SUCCEED();
}
TEST(Concepts, Supplemental_MissingSendFailsStream) {
    static_assert(en::Pollable<MissingSend>);
    static_assert(!en::Stream<MissingSend>);
    SUCCEED();
}
TEST(Concepts, Supplemental_MissingOnMessageFailsStream) {
    static_assert(en::Pollable<MissingOnMessage>);
    static_assert(!en::Stream<MissingOnMessage>);
    SUCCEED();
}
TEST(Concepts, Supplemental_WrongSendSignatureFailsStream) {
    static_assert(en::Pollable<WrongSendSignature>);
    static_assert(!en::Stream<WrongSendSignature>);
    SUCCEED();
}
TEST(Concepts, Supplemental_MissingJoinMulticastFailsDatagram) {
    static_assert(en::Pollable<MissingJoinMulticast>);
    static_assert(!en::Datagram<MissingJoinMulticast>);
    SUCCEED();
}

// ── Test 17–19: Negative conformance — types that can't even be Pollable.
TEST(Concepts, Supplemental_MissingNativeHandleFailsPollable) {
    static_assert(!en::Pollable<MissingNativeHandle>);
    SUCCEED();
}
TEST(Concepts, Supplemental_MissingPollOnceFailsPollable) {
    static_assert(!en::Pollable<MissingPollOnce>);
    SUCCEED();
}
TEST(Concepts, Supplemental_MissingPacketViewFailsPollable) {
    static_assert(!en::Pollable<MissingPacketView>);
    SUCCEED();
}

// ── Test 20–22: Poller shape checks.
TEST(Concepts, Supplemental_PollOnlyTypeSatisfiesBasePoller) {
    // The base `Poller` concept only requires `poll()`. A type that has
    // just `poll()` should satisfy the base concept but will fail
    // `PollerOf<P, Obj>` because it lacks add/remove.
    static_assert(en::Poller<PollOnlyPoller>);
    SUCCEED();
}
TEST(Concepts, Supplemental_PollOnlyFailsPollerOf) {
    static_assert(!en::PollerOf<PollOnlyPoller, ent::FakeStream>);
    SUCCEED();
}
TEST(Concepts, Supplemental_MismatchedPollerOfFails) {
    // TestPoller<FakeStream> should NOT be a PollerOf for FakeDatagram —
    // add<FakeDatagram*> would fail because the template class is
    // parameterized on FakeStream. This is a real contract: a poller
    // instantiated for one pollable type cannot drive an unrelated type.
    static_assert(
        !en::PollerOf<ent::TestPoller<ent::FakeStream>, ent::FakeDatagram>);
    SUCCEED();
}

// ── Test 23: Runtime — TestPoller.remove() on unregistered fake errors.
TEST(Concepts, Supplemental_TestPollerRemoveUnregisteredFails) {
    auto poller = ent::TestPoller<ent::FakeStream>::create();
    auto fake = ent::FakeStream::create();
    auto r = poller->remove(fake.get());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
}

// ── Test 24: Runtime — TestPoller.add(nullptr) errors.
TEST(Concepts, Supplemental_TestPollerAddNullFails) {
    auto poller = ent::TestPoller<ent::FakeStream>::create();
    auto r = poller->add(nullptr);
    ASSERT_FALSE(r.has_value());
}

// ── Test 25: Runtime — FakeStream state accessor works across transitions.
TEST(Concepts, Supplemental_FakeStreamStateMutable) {
    auto fake = ent::FakeStream::create();
    EXPECT_EQ(fake->state(), en::TcpState::Established);
    fake->set_state(en::TcpState::Closed);
    EXPECT_EQ(fake->state(), en::TcpState::Closed);
}
