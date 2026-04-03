#include <atomic>
#include <thread>

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

// ---------------------------------------------------------------------------
// SPSC concurrent correctness tests
// ---------------------------------------------------------------------------

TEST(RingBuffer, spsc_single_producer_single_reader_no_data_loss) {
    // One writer pushes N items, one reader verifies they arrive in order.
    // RingBuffer overwrites oldest when full, so with capacity=1024 and
    // N=10000, the reader should see all items >= N - 1024.
    constexpr size_t kCapacity = 1024;
    constexpr int kTotal = 10000;
    RingBuffer<int, kCapacity> rb;

    std::atomic<bool> writer_done{false};

    std::thread writer([&] {
        for (int i = 0; i < kTotal; ++i) {
            rb.push(i);
        }
        writer_done.store(true, std::memory_order_release);
    });

    // Reader: spin until writer is done, then verify the ring contents.
    // The ring should contain the last kCapacity values.
    writer.join();

    EXPECT_TRUE(rb.full());
    EXPECT_EQ(rb.count(), kCapacity);

    // Verify newest element is kTotal - 1
    auto newest = rb.front();
    ASSERT_TRUE(newest.has_value());
    EXPECT_EQ(*newest, kTotal - 1);

    // Verify oldest element is kTotal - kCapacity
    auto oldest = rb.back();
    ASSERT_TRUE(oldest.has_value());
    EXPECT_EQ(*oldest, kTotal - static_cast<int>(kCapacity));

    // Verify entire sequence is contiguous and in order
    for (size_t i = 0; i < kCapacity; ++i) {
        auto val = rb.at(i);
        ASSERT_TRUE(val.has_value()) << "at(" << i << ") is nullopt";
        EXPECT_EQ(*val, kTotal - 1 - static_cast<int>(i))
            << "at(" << i << ") mismatch";
    }
}

TEST(RingBuffer, spsc_concurrent_read_during_write_sees_consistent_data) {
    // Reader concurrently reads while writer pushes. The reader must never
    // see a partially-written value (which would indicate a data race).
    // We use a struct with redundant fields to detect tearing.
    struct Canary {
        uint32_t value;
        uint32_t check;  // Must always equal ~value
    };
    static_assert(std::is_trivially_copyable_v<Canary>);

    constexpr size_t kCapacity = 256;
    constexpr int kTotal = 50000;
    RingBuffer<Canary, kCapacity> rb;

    std::atomic<bool> done{false};
    std::atomic<int> torn_count{0};

    std::thread writer([&] {
        for (int i = 0; i < kTotal; ++i) {
            rb.push(Canary{static_cast<uint32_t>(i),
                           ~static_cast<uint32_t>(i)});
        }
        done.store(true, std::memory_order_release);
    });

    std::thread reader([&] {
        while (!done.load(std::memory_order_acquire)) {
            auto val = rb.front();
            if (val.has_value()) {
                // Check for torn read: value and check must be consistent
                if (val->check != ~val->value) {
                    torn_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    });

    writer.join();
    reader.join();

    EXPECT_EQ(torn_count.load(), 0) << "Detected torn reads in concurrent SPSC";
}

TEST(RingBuffer, spsc_reader_sees_monotonic_front_values) {
    // The front() value must be monotonically non-decreasing as the writer
    // pushes increasing values. A regression would indicate missing
    // acquire/release ordering.
    constexpr size_t kCapacity = 128;
    constexpr int kTotal = 20000;
    RingBuffer<int, kCapacity> rb;

    std::atomic<bool> done{false};
    std::atomic<int> regression_count{0};

    std::thread writer([&] {
        for (int i = 0; i < kTotal; ++i) {
            rb.push(i);
        }
        done.store(true, std::memory_order_release);
    });

    std::thread reader([&] {
        int last_seen = -1;
        while (!done.load(std::memory_order_acquire)) {
            auto val = rb.front();
            if (val.has_value()) {
                if (*val < last_seen) {
                    regression_count.fetch_add(1, std::memory_order_relaxed);
                }
                last_seen = *val;
            }
        }
    });

    writer.join();
    reader.join();

    EXPECT_EQ(regression_count.load(), 0)
        << "front() showed non-monotonic values (memory ordering issue)";
}

TEST(RingBuffer, spsc_count_never_exceeds_capacity) {
    // Verify that count() never exceeds capacity during concurrent
    // push operations — would indicate a race in the count_ update.
    constexpr size_t kCapacity = 64;
    constexpr int kTotal = 30000;
    RingBuffer<int, kCapacity> rb;

    std::atomic<bool> done{false};
    std::atomic<int> overflow_count{0};

    std::thread writer([&] {
        for (int i = 0; i < kTotal; ++i) {
            rb.push(i);
        }
        done.store(true, std::memory_order_release);
    });

    std::thread reader([&] {
        while (!done.load(std::memory_order_acquire)) {
            size_t c = rb.count();
            if (c > kCapacity) {
                overflow_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    writer.join();
    reader.join();

    EXPECT_EQ(overflow_count.load(), 0)
        << "count() exceeded capacity during concurrent writes";
}
