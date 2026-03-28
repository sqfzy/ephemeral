#include <gtest/gtest.h>

#include "eph/containers/ring_buffer.hpp"

using eph::containers::RingBuffer;

// ---------------------------------------------------------------------------
// Basic push / read
// ---------------------------------------------------------------------------

TEST(RingBuffer, push_and_read_single_element) {
    RingBuffer<int, 4> rb;
    EXPECT_TRUE(rb.empty());
    EXPECT_EQ(rb.count(), 0u);

    rb.push(42);

    EXPECT_EQ(rb.count(), 1u);
    EXPECT_FALSE(rb.empty());
    EXPECT_FALSE(rb.full());

    auto val = rb.front();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, 42);
}

TEST(RingBuffer, push_multiple_and_read_back) {
    RingBuffer<int, 8> rb;
    for (int i = 0; i < 5; ++i) {
        rb.push(i * 10);
    }
    EXPECT_EQ(rb.count(), 5u);

    // front() = most recent = 40
    EXPECT_EQ(*rb.front(), 40);
    // back() = oldest = 0
    EXPECT_EQ(*rb.back(), 0);
}

// ---------------------------------------------------------------------------
// at() — offset from newest
// ---------------------------------------------------------------------------

TEST(RingBuffer, at_returns_elements_newest_first) {
    RingBuffer<int, 4> rb;
    rb.push(10);
    rb.push(20);
    rb.push(30);

    EXPECT_EQ(*rb.at(0), 30);  // most recent
    EXPECT_EQ(*rb.at(1), 20);
    EXPECT_EQ(*rb.at(2), 10);  // oldest
    EXPECT_FALSE(rb.at(3).has_value());  // out of range
}

// ---------------------------------------------------------------------------
// Wraparound — overwrite oldest
// ---------------------------------------------------------------------------

TEST(RingBuffer, wraparound_overwrites_oldest) {
    RingBuffer<int, 4> rb;

    // Fill to capacity
    for (int i = 0; i < 4; ++i) {
        rb.push(i);
    }
    EXPECT_TRUE(rb.full());
    EXPECT_EQ(rb.count(), 4u);
    EXPECT_EQ(*rb.front(), 3);
    EXPECT_EQ(*rb.back(), 0);

    // Push one more — overwrites element 0
    rb.push(99);
    EXPECT_TRUE(rb.full());
    EXPECT_EQ(rb.count(), 4u);
    EXPECT_EQ(*rb.front(), 99);
    EXPECT_EQ(*rb.back(), 1);  // 0 was evicted

    // Verify full contents after wraparound
    EXPECT_EQ(*rb.at(0), 99);
    EXPECT_EQ(*rb.at(1), 3);
    EXPECT_EQ(*rb.at(2), 2);
    EXPECT_EQ(*rb.at(3), 1);
}

TEST(RingBuffer, wraparound_many_overwrites) {
    // Push Capacity + N elements and verify only the last Capacity survive.
    constexpr std::size_t N = 16;
    RingBuffer<int, N> rb;

    for (int i = 0; i < 100; ++i) {
        rb.push(i);
    }

    EXPECT_TRUE(rb.full());
    EXPECT_EQ(rb.count(), N);
    EXPECT_EQ(*rb.front(), 99);         // most recent
    EXPECT_EQ(*rb.back(), 100 - static_cast<int>(N));  // oldest surviving
}

// ---------------------------------------------------------------------------
// Empty buffer returns nullopt
// ---------------------------------------------------------------------------

TEST(RingBuffer, empty_buffer_returns_nullopt) {
    RingBuffer<double, 4> rb;

    EXPECT_FALSE(rb.front().has_value());
    EXPECT_FALSE(rb.back().has_value());
    EXPECT_FALSE(rb.at(0).has_value());
}

// ---------------------------------------------------------------------------
// full() flag
// ---------------------------------------------------------------------------

TEST(RingBuffer, full_flag_transitions) {
    RingBuffer<int, 2> rb;

    EXPECT_FALSE(rb.full());
    rb.push(1);
    EXPECT_FALSE(rb.full());
    rb.push(2);
    EXPECT_TRUE(rb.full());
    rb.push(3);  // overwrite
    EXPECT_TRUE(rb.full());
}

// ---------------------------------------------------------------------------
// clear()
// ---------------------------------------------------------------------------

TEST(RingBuffer, clear_resets_state) {
    RingBuffer<int, 4> rb;
    rb.push(1);
    rb.push(2);
    rb.push(3);

    rb.clear();

    EXPECT_TRUE(rb.empty());
    EXPECT_EQ(rb.count(), 0u);
    EXPECT_FALSE(rb.full());
    EXPECT_FALSE(rb.front().has_value());
    EXPECT_FALSE(rb.back().has_value());

    // Can push again after clear
    rb.push(99);
    EXPECT_EQ(rb.count(), 1u);
    EXPECT_EQ(*rb.front(), 99);
}

// ---------------------------------------------------------------------------
// Struct element type (trivially copyable aggregate)
// ---------------------------------------------------------------------------

struct Tick {
    double price;
    double qty;
    uint64_t ts;
    bool operator==(const Tick&) const = default;
};
static_assert(std::is_trivially_copyable_v<Tick>);

TEST(RingBuffer, works_with_struct_elements) {
    RingBuffer<Tick, 4> rb;

    rb.push(Tick{100.5, 1.0, 1000});
    rb.push(Tick{101.0, 2.0, 1001});
    rb.push(Tick{99.5,  0.5, 1002});

    EXPECT_EQ(*rb.front(), (Tick{99.5, 0.5, 1002}));
    EXPECT_EQ(*rb.back(),  (Tick{100.5, 1.0, 1000}));
}

// ---------------------------------------------------------------------------
// Capacity-1 edge case (smallest allowed power of two)
// ---------------------------------------------------------------------------

TEST(RingBuffer, capacity_one_ring_buffer) {
    RingBuffer<int, 1> rb;

    EXPECT_TRUE(rb.empty());
    rb.push(42);
    EXPECT_TRUE(rb.full());
    EXPECT_EQ(rb.count(), 1u);
    EXPECT_EQ(*rb.front(), 42);
    EXPECT_EQ(*rb.back(), 42);

    rb.push(99);  // overwrite
    EXPECT_EQ(rb.count(), 1u);
    EXPECT_EQ(*rb.front(), 99);
}

// ---------------------------------------------------------------------------
// Static capacity constant accessible
// ---------------------------------------------------------------------------

TEST(RingBuffer, capacity_constant_is_correct) {
    RingBuffer<int, 64> rb;
    EXPECT_EQ(rb.capacity, 64u);
}
