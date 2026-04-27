/// @file test_fake_datagram.cpp
/// Unit tests for `eph::net::test::FakeDatagram`.

#include <span>

#include <gtest/gtest.h>

#include "eph/net/test/fake_datagram.hpp"

namespace en  = eph::net;
namespace ent = eph::net::test;

namespace {

// Helper: build a SocketAddr from four octets + port.
constexpr en::SocketAddr addr(uint8_t a, uint8_t b, uint8_t c, uint8_t d,
                              uint16_t port) noexcept {
    return en::SocketAddr{en::Ipv4Addr{a, b, c, d}, port};
}

} // namespace

TEST(FakeDatagram, InitialStateIsDetached) {
    ent::FakeDatagram fd;
    EXPECT_FALSE(fd.is_attached());
    EXPECT_TRUE(fd.joined_groups().empty());
    EXPECT_TRUE(fd.collect_tx().empty());
}

TEST(FakeDatagram, SendBeforeAttachReturnsNotAttached) {
    ent::FakeDatagram fd;
    const uint8_t data[] = {1, 2, 3};
    auto r = fd.send_to(data, addr(10, 0, 0, 1, 1000));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::NotAttached);
}

TEST(FakeDatagram, SendAfterAttachRecordsEachDatagram) {
    ent::FakeDatagram fd;
    fd.set_attached(true);
    const uint8_t d1[] = {1, 2};
    const uint8_t d2[] = {3, 4, 5};
    ASSERT_TRUE(fd.send_to(d1, addr(1, 2, 3, 4, 1000)).has_value());
    ASSERT_TRUE(fd.send_to(d2, addr(5, 6, 7, 8, 2000)).has_value());
    const auto& tx = fd.collect_tx();
    ASSERT_EQ(tx.size(), 2u);
    EXPECT_EQ(tx[0].dst.port, 1000);
    EXPECT_EQ(tx[0].data.size(), 2u);
    EXPECT_EQ(tx[1].dst.port, 2000);
    EXPECT_EQ(tx[1].data.size(), 3u);
}

TEST(FakeDatagram, InjectDatagramThenPollOnceFiresOnDatagram) {
    ent::FakeDatagram fd;
    int calls = 0;
    en::SocketAddr last_src{};
    std::size_t last_len = 0;
    fd.on_datagram = [&](std::span<const uint8_t> app_datagram,
                         const en::SocketAddr& peer) {
        ++calls;
        last_len = app_datagram.size();
        last_src = peer;
    };
    const uint8_t data[] = {9, 9, 9, 9};
    fd.inject_datagram(data, addr(192, 168, 0, 1, 12345));
    EXPECT_EQ(fd.poll_once_(), 1u);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(last_len, 4);
    EXPECT_EQ(last_src, addr(192, 168, 0, 1, 12345));
}

TEST(FakeDatagram, MultipleInjectedDatagramsDeliverInOrder) {
    ent::FakeDatagram fd;
    std::vector<std::size_t> sizes;
    fd.on_datagram = [&](std::span<const uint8_t> app_datagram,
                         const en::SocketAddr&) {
        sizes.push_back(app_datagram.size());
    };
    const uint8_t d1[] = {1};
    const uint8_t d2[] = {1, 2};
    const uint8_t d3[] = {1, 2, 3};
    fd.inject_datagram(d1, addr(1, 1, 1, 1, 10));
    fd.inject_datagram(d2, addr(1, 1, 1, 1, 11));
    fd.inject_datagram(d3, addr(1, 1, 1, 1, 12));
    EXPECT_EQ(fd.poll_once_(), 3u);
    ASSERT_EQ(sizes.size(), 3u);
    EXPECT_EQ(sizes[0], 1);
    EXPECT_EQ(sizes[1], 2);
    EXPECT_EQ(sizes[2], 3);
}

TEST(FakeDatagram, JoinAndLeaveMulticast) {
    ent::FakeDatagram fd;
    auto g1 = addr(239, 1, 2, 3, 5000);
    auto g2 = addr(239, 1, 2, 4, 5000);
    ASSERT_TRUE(fd.join_multicast(g1).has_value());
    ASSERT_TRUE(fd.join_multicast(g2).has_value());
    ASSERT_EQ(fd.joined_groups().size(), 2u);
    ASSERT_TRUE(fd.leave_multicast(g1).has_value());
    ASSERT_EQ(fd.joined_groups().size(), 1u);
    EXPECT_EQ(fd.joined_groups()[0], g2);
}

TEST(FakeDatagram, LeaveUnknownGroupIsOk) {
    // Leaving a group that was never joined is a no-op per the fake's
    // contract — we don't want tests to be noisy on cleanup paths.
    ent::FakeDatagram fd;
    auto g = addr(239, 0, 0, 1, 1234);
    EXPECT_TRUE(fd.leave_multicast(g).has_value());
}

TEST(FakeDatagram, NativeHandleIsStablePointer) {
    ent::FakeDatagram fd;
    EXPECT_EQ(fd.native_handle(), static_cast<void*>(&fd));
}

// ─────────────────────────────────────────────────────────────────────────────
// Error injection (round-46) — tests for `inject_send_error`,
// `inject_join_error`, `inject_leave_error`. Covers caller paths under
// arbitrary `core::ErrorInfo` codes, closing the FakeDatagram
// error-coverage gap flagged by the test-blind-spot review.
// ─────────────────────────────────────────────────────────────────────────────

TEST(FakeDatagram, InjectSendErrorPopsOnceFifo) {
    ent::FakeDatagram fd;
    fd.set_attached(true);

    fd.inject_send_error(eph::core::Error::WouldBlock,  "wb-first");
    fd.inject_send_error(eph::core::Error::BufferFull,  "bf-second");
    EXPECT_EQ(fd.pending_send_errors(), 2u);

    const uint8_t payload[] = {0xAA};
    auto dst = addr(10, 0, 0, 1, 1000);

    auto r1 = fd.send_to(payload, dst);
    ASSERT_FALSE(r1.has_value());
    EXPECT_EQ(r1.error().code, eph::core::Error::WouldBlock);
    EXPECT_STREQ(r1.error().detail, "wb-first");

    auto r2 = fd.send_to(payload, dst);
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code, eph::core::Error::BufferFull);

    // Drained — next send_to records a normal tx entry.
    auto r3 = fd.send_to(payload, dst);
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(fd.collect_tx().size(), 1u);
}

TEST(FakeDatagram, InjectSendErrorDoesNotConsumeWhenNotAttached) {
    // NotAttached short-circuits before the queue is inspected — the
    // injected error stays queued. Mirrors the precedence rule on
    // FakeStream and on the real KernelUdpSocket / DpdkUdpSocket
    // backends.
    ent::FakeDatagram fd;
    fd.inject_send_error(eph::core::Error::CodecBad, "should-stay-queued");

    const uint8_t payload[] = {0x01};
    auto r = fd.send_to(payload, addr(10, 0, 0, 1, 1000));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::NotAttached);
    EXPECT_EQ(fd.pending_send_errors(), 1u);
}

TEST(FakeDatagram, InjectJoinErrorReturnsExactlyOnce) {
    // First join_multicast pops the injected error; second call after
    // drain succeeds normally and records the group.
    ent::FakeDatagram fd;
    fd.inject_join_error(eph::core::Error::InvalidConfig,
                          "join: address family mismatch");

    auto g = addr(239, 1, 2, 3, 5000);
    auto r1 = fd.join_multicast(g);
    ASSERT_FALSE(r1.has_value());
    EXPECT_EQ(r1.error().code, eph::core::Error::InvalidConfig);
    EXPECT_STREQ(r1.error().detail, "join: address family mismatch");
    // Failed join must NOT have recorded the group.
    EXPECT_TRUE(fd.joined_groups().empty());

    // Second call drains queue → succeeds and records the group.
    auto r2 = fd.join_multicast(g);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(fd.joined_groups().size(), 1u);
    EXPECT_EQ(fd.joined_groups()[0], g);
}

TEST(FakeDatagram, InjectLeaveErrorDoesNotMutateJoinedList) {
    // A failed leave_multicast must not remove the group from the
    // internal `joined_` vector — caller observes the error and can
    // retry; the registry stays consistent.
    ent::FakeDatagram fd;
    auto g = addr(239, 4, 5, 6, 6000);
    ASSERT_TRUE(fd.join_multicast(g).has_value());
    ASSERT_EQ(fd.joined_groups().size(), 1u);

    fd.inject_leave_error(eph::core::Error::InvalidConfig,
                           "leave: igmp refused");
    auto r = fd.leave_multicast(g);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
    // Group still present — the leave was rejected before the vector
    // was touched.
    EXPECT_EQ(fd.joined_groups().size(), 1u);

    // After drain, leave succeeds and the group disappears.
    auto r2 = fd.leave_multicast(g);
    ASSERT_TRUE(r2.has_value());
    EXPECT_TRUE(fd.joined_groups().empty());
}
