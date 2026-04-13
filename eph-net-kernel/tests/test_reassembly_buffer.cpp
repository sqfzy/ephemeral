/// @file test_reassembly_buffer.cpp
/// Unit tests for ReassemblyBuffer — linear RX buffer with head/tail compaction.
///
/// This is the core data structure backing KernelTcpStream's recv→codec pipeline.
/// Tests cover all public operations: write, read, consume, compact, and clear,
/// including boundary conditions and typical streaming patterns.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <vector>

#include "eph/net/kernel/detail/reassembly_buffer.hpp"

using eph::net::kernel::detail::ReassemblyBuffer;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST(ReassemblyBuffer, initial_state_is_empty) {
    ReassemblyBuffer buf(1024);
    EXPECT_EQ(buf.capacity(), 1024u);
    EXPECT_EQ(buf.readable(), 0u);
    EXPECT_EQ(buf.writable_capacity(), 1024u);
}

TEST(ReassemblyBuffer, capacity_one) {
    ReassemblyBuffer buf(1);
    EXPECT_EQ(buf.capacity(), 1u);
    EXPECT_EQ(buf.readable(), 0u);
    EXPECT_EQ(buf.writable_capacity(), 1u);
}

// ---------------------------------------------------------------------------
// Write + read basic
// ---------------------------------------------------------------------------

TEST(ReassemblyBuffer, write_then_read) {
    ReassemblyBuffer buf(64);

    // Simulate recv() writing 4 bytes
    std::memcpy(buf.writable_ptr(), "ABCD", 4);
    buf.commit_write(4);

    EXPECT_EQ(buf.readable(), 4u);
    EXPECT_EQ(buf.writable_capacity(), 60u);
    EXPECT_EQ(std::memcmp(buf.read_ptr(), "ABCD", 4), 0);
}

TEST(ReassemblyBuffer, fill_to_capacity) {
    ReassemblyBuffer buf(8);

    std::memcpy(buf.writable_ptr(), "12345678", 8);
    buf.commit_write(8);

    EXPECT_EQ(buf.readable(), 8u);
    EXPECT_EQ(buf.writable_capacity(), 0u);
    EXPECT_EQ(std::memcmp(buf.read_ptr(), "12345678", 8), 0);
}

// ---------------------------------------------------------------------------
// Consume
// ---------------------------------------------------------------------------

TEST(ReassemblyBuffer, consume_partial) {
    ReassemblyBuffer buf(16);
    std::memcpy(buf.writable_ptr(), "hello world", 11);
    buf.commit_write(11);

    buf.consume(6); // consume "hello "
    EXPECT_EQ(buf.readable(), 5u);
    EXPECT_EQ(std::memcmp(buf.read_ptr(), "world", 5), 0);
}

TEST(ReassemblyBuffer, consume_all_resets_offsets) {
    ReassemblyBuffer buf(16);
    std::memcpy(buf.writable_ptr(), "ABCD", 4);
    buf.commit_write(4);

    buf.consume(4);
    EXPECT_EQ(buf.readable(), 0u);
    // After consuming all bytes, offsets should reset to 0 (full capacity available)
    EXPECT_EQ(buf.writable_capacity(), 16u);
}

TEST(ReassemblyBuffer, consume_more_than_readable_clamps) {
    ReassemblyBuffer buf(16);
    std::memcpy(buf.writable_ptr(), "AB", 2);
    buf.commit_write(2);

    buf.consume(100); // more than readable
    EXPECT_EQ(buf.readable(), 0u);
}

TEST(ReassemblyBuffer, consume_zero_is_noop) {
    ReassemblyBuffer buf(16);
    std::memcpy(buf.writable_ptr(), "AB", 2);
    buf.commit_write(2);

    buf.consume(0);
    EXPECT_EQ(buf.readable(), 2u);
    EXPECT_EQ(std::memcmp(buf.read_ptr(), "AB", 2), 0);
}

// ---------------------------------------------------------------------------
// Compact
// ---------------------------------------------------------------------------

TEST(ReassemblyBuffer, compact_moves_data_to_front) {
    ReassemblyBuffer buf(16);

    // Write 10 bytes
    std::memcpy(buf.writable_ptr(), "0123456789", 10);
    buf.commit_write(10);

    // Consume first 6 → "456789" remaining, head=6
    buf.consume(6);
    EXPECT_EQ(buf.readable(), 4u);
    // writable_capacity is only 6 (16 - 10)
    EXPECT_EQ(buf.writable_capacity(), 6u);

    // Compact → "456789" moves to front
    buf.compact();
    EXPECT_EQ(buf.readable(), 4u);
    EXPECT_EQ(buf.writable_capacity(), 12u); // 16 - 4
    EXPECT_EQ(std::memcmp(buf.read_ptr(), "6789", 4), 0);
}

TEST(ReassemblyBuffer, compact_empty_buffer_is_noop) {
    ReassemblyBuffer buf(16);
    buf.compact();
    EXPECT_EQ(buf.readable(), 0u);
    EXPECT_EQ(buf.writable_capacity(), 16u);
}

TEST(ReassemblyBuffer, compact_already_at_front_is_noop) {
    ReassemblyBuffer buf(16);
    std::memcpy(buf.writable_ptr(), "ABCD", 4);
    buf.commit_write(4);

    // head is already 0, compact should be a no-op
    buf.compact();
    EXPECT_EQ(buf.readable(), 4u);
    EXPECT_EQ(std::memcmp(buf.read_ptr(), "ABCD", 4), 0);
}

TEST(ReassemblyBuffer, compact_after_full_consume_resets) {
    ReassemblyBuffer buf(16);
    std::memcpy(buf.writable_ptr(), "ABCD", 4);
    buf.commit_write(4);
    buf.consume(4);

    // head == tail but head > 0 won't happen here because consume(all) resets
    // Test that compact on an empty buffer (head==tail==0) is safe
    buf.compact();
    EXPECT_EQ(buf.readable(), 0u);
    EXPECT_EQ(buf.writable_capacity(), 16u);
}

// ---------------------------------------------------------------------------
// Clear
// ---------------------------------------------------------------------------

TEST(ReassemblyBuffer, clear_resets_to_empty) {
    ReassemblyBuffer buf(32);
    std::memcpy(buf.writable_ptr(), "test data", 9);
    buf.commit_write(9);
    buf.consume(4);

    buf.clear();
    EXPECT_EQ(buf.readable(), 0u);
    EXPECT_EQ(buf.writable_capacity(), 32u);
}

// ---------------------------------------------------------------------------
// Streaming pattern: repeated write/consume/compact cycles
// ---------------------------------------------------------------------------

TEST(ReassemblyBuffer, streaming_pattern_write_consume_compact) {
    ReassemblyBuffer buf(32);

    // Simulate a steady stream of 8-byte messages
    for (int round = 0; round < 100; ++round) {
        // Make room if needed
        if (buf.writable_capacity() < 8) {
            buf.compact();
        }
        ASSERT_GE(buf.writable_capacity(), 8u)
            << "round=" << round << " readable=" << buf.readable();

        // Write 8 bytes
        uint8_t msg[8];
        std::iota(msg, msg + 8, static_cast<uint8_t>(round));
        std::memcpy(buf.writable_ptr(), msg, 8);
        buf.commit_write(8);

        // Consume the message
        ASSERT_GE(buf.readable(), 8u);
        EXPECT_EQ(buf.read_ptr()[0], static_cast<uint8_t>(round));
        buf.consume(8);
    }

    EXPECT_EQ(buf.readable(), 0u);
}

TEST(ReassemblyBuffer, streaming_pattern_partial_consume) {
    // Simulate: write 10 bytes, consume 7, write 10 bytes, consume 7...
    // This stresses compaction because the head advances by 7 each round
    // while 3 bytes remain pending. Buffer must be large enough to hold
    // the maximum pending data (3 bytes/round accumulate until a "drain"
    // or reset). With 256 bytes the buffer can sustain many rounds.
    ReassemblyBuffer buf(256);

    for (int round = 0; round < 50; ++round) {
        if (buf.writable_capacity() < 10) {
            buf.compact();
        }
        ASSERT_GE(buf.writable_capacity(), 10u)
            << "round=" << round << " readable=" << buf.readable()
            << " writable=" << buf.writable_capacity();

        uint8_t msg[10];
        std::fill_n(msg, 10, static_cast<uint8_t>(round));
        std::memcpy(buf.writable_ptr(), msg, 10);
        buf.commit_write(10);

        // Consume 7 of the 10 (3 remain pending)
        buf.consume(7);
    }

    // 3 bytes/round × 50 rounds = 150 bytes pending
    EXPECT_EQ(buf.readable(), 150u);
}

// ---------------------------------------------------------------------------
// Edge: zero-capacity buffer
// ---------------------------------------------------------------------------

TEST(ReassemblyBuffer, zero_capacity_buffer) {
    ReassemblyBuffer buf(0);
    EXPECT_EQ(buf.capacity(), 0u);
    EXPECT_EQ(buf.readable(), 0u);
    EXPECT_EQ(buf.writable_capacity(), 0u);
    buf.compact();
    buf.clear();
    EXPECT_EQ(buf.readable(), 0u);
}

// ---------------------------------------------------------------------------
// Data integrity after compaction
// ---------------------------------------------------------------------------

TEST(ReassemblyBuffer, data_integrity_after_compact) {
    ReassemblyBuffer buf(256);

    // Write a known pattern
    std::vector<uint8_t> pattern(100);
    std::iota(pattern.begin(), pattern.end(), 0);
    std::memcpy(buf.writable_ptr(), pattern.data(), 100);
    buf.commit_write(100);

    // Consume first 70 bytes, leaving bytes 70-99
    buf.consume(70);
    EXPECT_EQ(buf.readable(), 30u);

    // Compact
    buf.compact();
    EXPECT_EQ(buf.readable(), 30u);

    // Verify data is preserved exactly
    for (int i = 0; i < 30; ++i) {
        EXPECT_EQ(buf.read_ptr()[i], static_cast<uint8_t>(70 + i))
            << "i=" << i;
    }

    // Should be able to write more now
    EXPECT_EQ(buf.writable_capacity(), 226u); // 256 - 30
}
