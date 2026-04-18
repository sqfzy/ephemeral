/// @file test_test_poller.cpp
/// Unit tests for `eph::net::test::TestPoller`.

#include <span>

#include <gtest/gtest.h>

#include "eph/net/test/fake_datagram.hpp"
#include "eph/net/test/fake_stream.hpp"
#include "eph/net/test/test_poller.hpp"

namespace en  = eph::net;
namespace ent = eph::net::test;

// ---------------------------------------------------------------------------
// add / remove bookkeeping
// ---------------------------------------------------------------------------

TEST(TestPoller, AddNullptrFails) {
    ent::TestPoller<ent::FakeStream> p;
    auto r = p.add(nullptr);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
}

TEST(TestPoller, AddFlipsAttachedFlag) {
    ent::TestPoller<ent::FakeStream> p;
    auto fs = ent::FakeStream::create();
    ASSERT_FALSE(fs->is_attached());
    ASSERT_TRUE(p.add(fs.get()).has_value());
    EXPECT_TRUE(fs->is_attached());
    EXPECT_EQ(p.size(), 1u);
    EXPECT_TRUE(p.contains(fs.get()));
}

TEST(TestPoller, DuplicateAddFails) {
    ent::TestPoller<ent::FakeStream> p;
    auto fs = ent::FakeStream::create();
    ASSERT_TRUE(p.add(fs.get()).has_value());
    auto r = p.add(fs.get());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
}

TEST(TestPoller, RemoveClearsAttachedFlag) {
    ent::TestPoller<ent::FakeStream> p;
    auto fs = ent::FakeStream::create();
    ASSERT_TRUE(p.add(fs.get()).has_value());
    ASSERT_TRUE(p.remove(fs.get()).has_value());
    EXPECT_FALSE(fs->is_attached());
    EXPECT_EQ(p.size(), 0u);
}

TEST(TestPoller, RemoveUnregisteredFails) {
    ent::TestPoller<ent::FakeStream> p;
    auto fs = ent::FakeStream::create();
    auto r = p.remove(fs.get());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
}

// ---------------------------------------------------------------------------
// poll() drives registered pollables
// ---------------------------------------------------------------------------

TEST(TestPoller, PollDrivesAllRegisteredStreams) {
    ent::TestPoller<ent::FakeStream> p;
    auto a = ent::FakeStream::create();
    auto b = ent::FakeStream::create();
    int a_calls = 0, b_calls = 0;
    a->on_message = [&](std::span<const uint8_t>) { ++a_calls; };
    b->on_message = [&](std::span<const uint8_t>) { ++b_calls; };
    ASSERT_TRUE(p.add(a.get()).has_value());
    ASSERT_TRUE(p.add(b.get()).has_value());

    const uint8_t data[] = {1, 2, 3};
    a->inject_rx(data);
    b->inject_rx(data);
    EXPECT_EQ(p.poll(), 2u);
    EXPECT_EQ(a_calls, 1);
    EXPECT_EQ(b_calls, 1);

    // Second poll: nothing queued — returns 0.
    EXPECT_EQ(p.poll(), 0u);
}

TEST(TestPoller, PollOnEmptyReturnsZero) {
    ent::TestPoller<ent::FakeStream> p;
    EXPECT_EQ(p.poll(), 0u);
    EXPECT_EQ(p.poll(std::chrono::milliseconds{100}), 0u);
}

TEST(TestPoller, CallbackRemovingSelfDoesNotCrash) {
    // The snapshot-then-iterate design in TestPoller::poll guarantees that a
    // pollable can remove itself from inside its own poll_once_ callback
    // without invalidating the iteration.
    ent::TestPoller<ent::FakeStream> p;
    auto a = ent::FakeStream::create();
    auto b = ent::FakeStream::create();
    ASSERT_TRUE(p.add(a.get()).has_value());
    ASSERT_TRUE(p.add(b.get()).has_value());

    bool removed = false;
    a->on_message = [&](std::span<const uint8_t>) {
        if (!removed) {
            (void)p.remove(a.get());
            removed = true;
        }
    };
    int b_calls = 0;
    b->on_message = [&](std::span<const uint8_t>) { ++b_calls; };

    const uint8_t data[] = {42};
    a->inject_rx(data);
    b->inject_rx(data);
    EXPECT_NO_FATAL_FAILURE((void)p.poll());
    // a removed itself mid-poll; b still got its callback.
    EXPECT_TRUE(removed);
    EXPECT_EQ(b_calls, 1);
    EXPECT_EQ(p.size(), 1u);
    EXPECT_FALSE(p.contains(a.get()));
    EXPECT_TRUE(p.contains(b.get()));
}

// ---------------------------------------------------------------------------
// Datagram specialization
// ---------------------------------------------------------------------------

TEST(TestPoller, DrivesFakeDatagram) {
    ent::TestPoller<ent::FakeDatagram> p;
    auto fd = ent::FakeDatagram::create();
    int calls = 0;
    fd->on_datagram = [&](std::span<const uint8_t>, const en::SocketAddr&) {
        ++calls;
    };
    ASSERT_TRUE(p.add(fd.get()).has_value());
    const uint8_t data[] = {7};
    fd->inject_datagram(data, en::SocketAddr{en::Ipv4Addr{1, 2, 3, 4}, 500});
    fd->inject_datagram(data, en::SocketAddr{en::Ipv4Addr{1, 2, 3, 4}, 501});
    EXPECT_EQ(p.poll(), 2u);
    EXPECT_EQ(calls, 2);
}

TEST(TestPoller, TimeoutOverloadIsIdentical) {
    ent::TestPoller<ent::FakeStream> p;
    auto fs = ent::FakeStream::create();
    ASSERT_TRUE(p.add(fs.get()).has_value());
    const uint8_t data[] = {1};
    fs->inject_rx(data);
    EXPECT_EQ(p.poll(std::chrono::milliseconds{0}), 1u);
}
