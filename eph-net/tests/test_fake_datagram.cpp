/// @file test_fake_datagram.cpp
/// Unit tests for `eph::net::test::FakeDatagram`.

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
    uint16_t last_len = 0;
    fd.on_datagram = [&](const uint8_t*, uint16_t n, const en::SocketAddr& s) {
        ++calls;
        last_len = n;
        last_src = s;
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
    std::vector<uint16_t> sizes;
    fd.on_datagram = [&](const uint8_t*, uint16_t n, const en::SocketAddr&) {
        sizes.push_back(n);
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
