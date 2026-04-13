/// @file test_posix_io.cpp
/// Unit tests for posix_io.hpp — send_all / recv_exact byte-level I/O.
///
/// Uses socketpair(AF_UNIX, SOCK_STREAM) so tests run without network access.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include "eph/net/posix_io.hpp"

using namespace eph::net::posix;

// ---------------------------------------------------------------------------
// RAII wrapper for socketpair
// ---------------------------------------------------------------------------

class SocketPairTest : public ::testing::Test {
protected:
    int fds_[2] = {-1, -1};

    void SetUp() override {
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds_), 0)
            << "socketpair() failed: " << std::strerror(errno);
    }

    void TearDown() override {
        if (fds_[0] >= 0) ::close(fds_[0]);
        if (fds_[1] >= 0) ::close(fds_[1]);
    }

    int sender() const { return fds_[0]; }
    int receiver() const { return fds_[1]; }
};

// ---------------------------------------------------------------------------
// send_all / recv_exact — basic round-trip
// ---------------------------------------------------------------------------

TEST_F(SocketPairTest, round_trip_single_byte) {
    uint8_t tx = 0xAB;
    ASSERT_TRUE(send_all(sender(), &tx, 1));
    uint8_t rx = 0;
    ASSERT_TRUE(recv_exact(receiver(), &rx, 1));
    EXPECT_EQ(rx, 0xAB);
}

TEST_F(SocketPairTest, round_trip_multi_byte) {
    const char msg[] = "hello posix_io";
    ASSERT_TRUE(send_all(sender(), msg, sizeof(msg)));

    char buf[sizeof(msg)]{};
    ASSERT_TRUE(recv_exact(receiver(), buf, sizeof(msg)));
    EXPECT_STREQ(buf, msg);
}

TEST_F(SocketPairTest, round_trip_large_buffer) {
    // Send 64 KiB — likely requires multiple send/recv iterations
    constexpr size_t kSize = 65536;
    std::vector<uint8_t> tx(kSize);
    for (size_t i = 0; i < kSize; ++i) tx[i] = static_cast<uint8_t>(i & 0xFF);

    // send in a thread to avoid blocking (socket buffer may be smaller)
    std::thread writer([&] {
        EXPECT_TRUE(send_all(sender(), tx.data(), tx.size()));
    });

    std::vector<uint8_t> rx(kSize, 0);
    ASSERT_TRUE(recv_exact(receiver(), rx.data(), rx.size()));
    writer.join();

    EXPECT_EQ(tx, rx);
}

// ---------------------------------------------------------------------------
// send_all — zero length
// ---------------------------------------------------------------------------

TEST_F(SocketPairTest, send_all_zero_length_succeeds) {
    EXPECT_TRUE(send_all(sender(), nullptr, 0));
}

// ---------------------------------------------------------------------------
// recv_exact — zero length
// ---------------------------------------------------------------------------

TEST_F(SocketPairTest, recv_exact_zero_length_succeeds) {
    EXPECT_TRUE(recv_exact(receiver(), nullptr, 0));
}

// ---------------------------------------------------------------------------
// recv_exact — peer close detection
// ---------------------------------------------------------------------------

TEST_F(SocketPairTest, recv_exact_returns_false_on_peer_close) {
    // Close sender side
    ::close(sender());
    fds_[0] = -1;

    uint8_t buf[4]{};
    EXPECT_FALSE(recv_exact(receiver(), buf, 4));
}

TEST_F(SocketPairTest, recv_exact_partial_then_peer_close) {
    // Send only 2 bytes then close
    uint8_t tx[2] = {0x01, 0x02};
    ASSERT_TRUE(send_all(sender(), tx, 2));
    ::close(sender());
    fds_[0] = -1;

    // Try to read 4 bytes — should fail after getting only 2
    uint8_t buf[4]{};
    EXPECT_FALSE(recv_exact(receiver(), buf, 4));
    // The first 2 bytes should still have been written
    EXPECT_EQ(buf[0], 0x01);
    EXPECT_EQ(buf[1], 0x02);
}

// ---------------------------------------------------------------------------
// send_all — bad fd
// ---------------------------------------------------------------------------

TEST_F(SocketPairTest, send_all_returns_false_on_bad_fd) {
    uint8_t data = 0x42;
    EXPECT_FALSE(send_all(-1, &data, 1));
}

// ---------------------------------------------------------------------------
// recv_exact — bad fd
// ---------------------------------------------------------------------------

TEST_F(SocketPairTest, recv_exact_returns_false_on_bad_fd) {
    uint8_t buf[1];
    EXPECT_FALSE(recv_exact(-1, buf, 1));
}

// ---------------------------------------------------------------------------
// Multiple sequential send/recv
// ---------------------------------------------------------------------------

TEST_F(SocketPairTest, sequential_messages) {
    for (int i = 0; i < 100; ++i) {
        uint32_t val = static_cast<uint32_t>(i * 7);
        ASSERT_TRUE(send_all(sender(), &val, sizeof(val)));

        uint32_t got = 0;
        ASSERT_TRUE(recv_exact(receiver(), &got, sizeof(got)));
        EXPECT_EQ(got, val);
    }
}

// ---------------------------------------------------------------------------
// send_all after peer closes receiver (broken pipe)
// ---------------------------------------------------------------------------

TEST_F(SocketPairTest, send_all_returns_false_after_peer_close) {
    ::close(receiver());
    fds_[1] = -1;

    // The first send might succeed (buffered), but eventually we should fail
    std::array<uint8_t, 4096> buf{};
    buf.fill(0xFF);
    bool failed = false;
    for (int i = 0; i < 100; ++i) {
        if (!send_all(sender(), buf.data(), buf.size())) {
            failed = true;
            break;
        }
    }
    EXPECT_TRUE(failed);
}
