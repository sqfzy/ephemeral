#include <gtest/gtest.h>
#include <array>
#include <print>
#include <span>
#include <thread>
#include <vector>

#include "eph/containers/bounded_queue.hpp"

using eph::containers::BoundedQueue;

// 定义满足 TrivialData 的测试负载
struct BoundedTestData {
    uint32_t seq{0};
    std::array<uint32_t, 63> payload{};

    bool operator==(const BoundedTestData& other) const = default;
};

template <typename T>
class BoundedQueueTest : public ::testing::Test {};

// 测试容量为 1, 2, 1024 的队列
// Capacity=1 is the degenerate case where mask_=0 and the queue is
// immediately full after a single push. This exercises the boundary
// where every element maps to index 0 in the ring buffer.
using BoundedQueueTypes = ::testing::Types<
    BoundedQueue<BoundedTestData, 1>,
    BoundedQueue<BoundedTestData, 2>,
    BoundedQueue<BoundedTestData, 1024>
>;

TYPED_TEST_SUITE(BoundedQueueTest, BoundedQueueTypes);

// 1. emplace 测试
TYPED_TEST(BoundedQueueTest, EmplaceBasic) {
    TypeParam queue;

    // emplace 默认构造
    EXPECT_TRUE(queue.try_emplace());
    auto res = queue.try_pop();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->seq, 0u);

    // emplace 拷贝构造
    BoundedTestData expected;
    expected.seq = 77;
    expected.payload.fill(77);
    EXPECT_TRUE(queue.try_emplace(expected));
    auto res2 = queue.try_pop();
    ASSERT_TRUE(res2.has_value());
    EXPECT_EQ(res2.value(), expected);

    // 阻塞式 emplace
    queue.emplace(expected);
    auto res3 = queue.try_pop();
    ASSERT_TRUE(res3.has_value());
    EXPECT_EQ(res3.value(), expected);
}

// 2. 单线程基本操作
TYPED_TEST(BoundedQueueTest, SingleThreadBasic) {
    TypeParam queue;
    BoundedTestData data;
    data.seq = 100;
    data.payload.fill(0xAA);

    EXPECT_TRUE(queue.try_push(data));
    EXPECT_EQ(queue.size(), 1);

    auto res = queue.try_pop();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->seq, 100);
    EXPECT_EQ(res->payload[0], 0xAA);
    EXPECT_TRUE(queue.empty());
}

// 2. 边界条件：Full & Empty
TYPED_TEST(BoundedQueueTest, FullEmptyStatus) {
    TypeParam queue;
    BoundedTestData data;
    
    // 填满队列
    for (size_t i = 0; i < TypeParam::capacity(); ++i) {
        EXPECT_TRUE(queue.try_push(data));
    }
    
    EXPECT_TRUE(queue.full());
    EXPECT_FALSE(queue.try_push(data)); // 应该失败

    // 清空队列
    for (size_t i = 0; i < TypeParam::capacity(); ++i) {
        EXPECT_TRUE(queue.try_pop().has_value());
    }
    
    EXPECT_TRUE(queue.empty());
    EXPECT_FALSE(queue.try_pop().has_value());
}

// available_write / available_read queries
TYPED_TEST(BoundedQueueTest, AvailableWriteReadQueries) {
    TypeParam queue;
    const size_t cap = TypeParam::capacity();
    BoundedTestData data;

    // Empty queue: full write capacity, no read available
    EXPECT_EQ(queue.available_write(), cap);
    EXPECT_EQ(queue.available_read(), 0u);

    // Push half
    size_t half = cap / 2;
    for (size_t i = 0; i < half; ++i) {
        EXPECT_TRUE(queue.try_push(data));
    }
    EXPECT_EQ(queue.available_write(), cap - half);
    EXPECT_EQ(queue.available_read(), half);

    // Fill completely
    for (size_t i = half; i < cap; ++i) {
        EXPECT_TRUE(queue.try_push(data));
    }
    EXPECT_EQ(queue.available_write(), 0u);
    EXPECT_EQ(queue.available_read(), cap);

    // Pop one
    EXPECT_TRUE(queue.try_pop().has_value());
    EXPECT_EQ(queue.available_write(), 1u);
    EXPECT_EQ(queue.available_read(), cap - 1);

    // Clear
    queue.clear();
    EXPECT_EQ(queue.available_write(), cap);
    EXPECT_EQ(queue.available_read(), 0u);
}

// 3. 批量操作 try_push_n / try_pop_n
TYPED_TEST(BoundedQueueTest, BatchPushPopBasic) {
    TypeParam queue;
    const size_t cap = TypeParam::capacity();

    // 构造批量数据
    std::vector<BoundedTestData> batch(cap);
    for (size_t i = 0; i < cap; ++i) {
        batch[i].seq = static_cast<uint32_t>(i + 1);
        batch[i].payload.fill(static_cast<uint32_t>(i));
    }

    // 批量推入
    EXPECT_TRUE(queue.try_push_n(std::span<const BoundedTestData>{batch}));
    EXPECT_TRUE(queue.full());

    // 队列满时再推入应失败
    EXPECT_FALSE(queue.try_push_n(std::span<const BoundedTestData>{batch.data(), 1}));

    // 批量弹出
    std::vector<BoundedTestData> out(cap);
    size_t popped = queue.try_pop_n(std::span<BoundedTestData>{out});
    EXPECT_EQ(popped, cap);
    EXPECT_TRUE(queue.empty());

    // 验证数据
    for (size_t i = 0; i < cap; ++i) {
        EXPECT_EQ(out[i].seq, i + 1);
        EXPECT_EQ(out[i].payload[0], static_cast<uint32_t>(i));
    }
}

// 4. 批量操作边界条件：空队列 pop_n、零长度 push_n
TYPED_TEST(BoundedQueueTest, BatchEdgeCases) {
    TypeParam queue;

    // 空队列 pop_n 应返回 0
    std::vector<BoundedTestData> out(4);
    EXPECT_EQ(queue.try_pop_n(std::span<BoundedTestData>{out}), 0u);

    // 零长度 push_n 应成功
    EXPECT_TRUE(queue.try_push_n(std::span<const BoundedTestData>{}));

    // 零长度 pop_n 应返回 0
    EXPECT_EQ(queue.try_pop_n(std::span<BoundedTestData>{}), 0u);

    // 推入超过容量的批量数据应失败
    std::vector<BoundedTestData> too_big(TypeParam::capacity() + 1);
    EXPECT_FALSE(queue.try_push_n(std::span<const BoundedTestData>{too_big}));
    EXPECT_TRUE(queue.empty());
}

// 5. 批量操作 try_pop_n 的尽力而为语义
TYPED_TEST(BoundedQueueTest, BatchPopPartial) {
    TypeParam queue;

    // 推入少于请求量的数据
    BoundedTestData d;
    d.seq = 42;
    EXPECT_TRUE(queue.try_push(d));

    // 请求多个但只能读到 1 个
    std::vector<BoundedTestData> out(4);
    size_t popped = queue.try_pop_n(std::span<BoundedTestData>{out});
    EXPECT_EQ(popped, 1u);
    EXPECT_EQ(out[0].seq, 42u);
    EXPECT_TRUE(queue.empty());
}

// 6. 多线程批量操作压力测试
TYPED_TEST(BoundedQueueTest, BatchMultiThreadStress) {
    TypeParam queue;
    const uint32_t total_batches = 100'000;
    // Batch size must not exceed queue capacity (Capacity=1 is valid).
    constexpr size_t batch_size = std::min(size_t{2}, TypeParam::capacity());

    std::thread producer([&]() {
        std::vector<BoundedTestData> batch(batch_size);
        for (uint32_t i = 0; i < total_batches; ++i) {
            for (size_t j = 0; j < batch_size; ++j) {
                batch[j].seq = i * batch_size + static_cast<uint32_t>(j) + 1;
                batch[j].payload.fill(batch[j].seq);
            }
            while (!queue.try_push_n(std::span<const BoundedTestData>{batch})) {
                eph::utils::cpu_relax();
            }
        }
    });

    uint32_t next_expected = 1;
    std::thread consumer([&]() {
        std::vector<BoundedTestData> out(batch_size);
        uint32_t remaining = total_batches * batch_size;
        while (remaining > 0) {
            size_t got = queue.try_pop_n(std::span<BoundedTestData>{out});
            for (size_t i = 0; i < got; ++i) {
                EXPECT_EQ(out[i].seq, next_expected);
                EXPECT_EQ(out[i].payload[0], next_expected);
                next_expected++;
            }
            remaining -= static_cast<uint32_t>(got);
            if (got == 0) eph::utils::cpu_relax();
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(next_expected - 1, total_batches * batch_size);
    std::println("BoundedQueue batch stress test done. Processed {} messages.", total_batches * batch_size);
}

// 7. 多线程压力测试：验证不可丢弃性和顺序
TYPED_TEST(BoundedQueueTest, MultiThreadStress) {
    TypeParam queue;
    const uint32_t total_messages = 1'000'000; // 不可丢弃队列，量级视内存而定

    std::thread producer([&]() {
        for (uint32_t i = 1; i <= total_messages; ++i) {
            BoundedTestData data;
            data.seq = i;
            // 填充特定模式以便校验
            for (size_t j = 0; j < data.payload.size(); ++j) {
                data.payload[j] = i ^ j;
            }
            // 使用阻塞 push 确保所有数据都进入队列
            queue.push(data);
        }
    });

    uint32_t next_expected = 1;
    std::thread consumer([&]() {
        while (next_expected <= total_messages) {
            BoundedTestData data = queue.pop();
            
            // 验证顺序：由于是 SPSC，顺序必须严格一致
            if (data.seq != next_expected) {
                ADD_FAILURE() << "Out of order! Expected: " << next_expected << " Got: " << data.seq;
                break;
            }
            
            // 验证数据完整性
            for (size_t j = 0; j < data.payload.size(); ++j) {
                if (data.payload[j] != (data.seq ^ j)) {
                    ADD_FAILURE() << "Data corruption at seq: " << data.seq;
                    break;
                }
            }
            next_expected++;
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(next_expected - 1, total_messages);
    std::println("BoundedQueue stress test done. Processed {} messages.", total_messages);
}

// clear() 测试：清空后队列为空且可以继续使用
TYPED_TEST(BoundedQueueTest, ClearResetsQueue) {
    TypeParam queue;
    BoundedTestData data;

    // 填入若干元素
    for (uint32_t i = 0; i < std::min<size_t>(queue.capacity(), 8); ++i) {
        data.seq = i;
        ASSERT_TRUE(queue.try_push(data));
    }
    EXPECT_FALSE(queue.empty());

    // clear 后应为空
    queue.clear();
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.size(), 0u);
    EXPECT_FALSE(queue.full());

    // clear 后应能正常写入和读取
    data.seq = 999;
    EXPECT_TRUE(queue.try_push(data));
    auto res = queue.try_pop();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->seq, 999u);
}

// clear() 在已空队列上调用不应出错
TYPED_TEST(BoundedQueueTest, ClearOnEmptyQueue) {
    TypeParam queue;
    queue.clear();
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.size(), 0u);

    BoundedTestData data;
    data.seq = 1;
    EXPECT_TRUE(queue.try_push(data));
    auto res = queue.try_pop();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->seq, 1u);
}

// clear() 在满队列上调用
TYPED_TEST(BoundedQueueTest, ClearOnFullQueue) {
    TypeParam queue;
    BoundedTestData data;

    // 填满
    for (size_t i = 0; i < queue.capacity(); ++i) {
        data.seq = static_cast<uint32_t>(i);
        ASSERT_TRUE(queue.try_push(data));
    }
    EXPECT_TRUE(queue.full());

    queue.clear();
    EXPECT_TRUE(queue.empty());
    EXPECT_FALSE(queue.full());

    // 填满后 clear 再填满应正常工作
    for (size_t i = 0; i < queue.capacity(); ++i) {
        data.seq = static_cast<uint32_t>(i + 1000);
        ASSERT_TRUE(queue.try_push(data));
    }
    EXPECT_TRUE(queue.full());

    auto res = queue.try_pop();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->seq, 1000u);
}

// ---------------------------------------------------------------------------
// try_produce_n tests
// ---------------------------------------------------------------------------

TYPED_TEST(BoundedQueueTest, TryProduceN_BasicOperation) {
    TypeParam queue;
    constexpr size_t n = std::min(size_t{4}, queue.capacity());

    bool ok = queue.try_produce_n(n, [](BoundedTestData& slot, size_t i) {
        slot.seq = static_cast<uint32_t>(i + 100);
    });
    EXPECT_TRUE(ok);
    EXPECT_EQ(queue.size(), n);

    for (size_t i = 0; i < n; ++i) {
        auto val = queue.try_pop();
        ASSERT_TRUE(val.has_value());
        EXPECT_EQ(val->seq, static_cast<uint32_t>(i + 100));
    }
    EXPECT_TRUE(queue.empty());
}

TYPED_TEST(BoundedQueueTest, TryProduceN_EmptyBatch) {
    TypeParam queue;
    EXPECT_TRUE(queue.try_produce_n(0, [](BoundedTestData&, size_t) {}));
    EXPECT_TRUE(queue.empty());
}

TYPED_TEST(BoundedQueueTest, TryProduceN_FailsWhenInsufficientSpace) {
    TypeParam queue;
    // Fill queue
    for (size_t i = 0; i < queue.capacity(); ++i) {
        BoundedTestData d;
        d.seq = static_cast<uint32_t>(i);
        ASSERT_TRUE(queue.try_push(d));
    }
    // Batch produce should fail (queue full)
    bool ok = queue.try_produce_n(1, [](BoundedTestData& slot, size_t) {
        slot.seq = 999;
    });
    EXPECT_FALSE(ok);
    // Original data should be intact
    auto val = queue.try_pop();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(val->seq, 0u);
}

TYPED_TEST(BoundedQueueTest, TryProduceN_FillsExactCapacity) {
    TypeParam queue;
    bool ok = queue.try_produce_n(queue.capacity(),
        [](BoundedTestData& slot, size_t i) {
            slot.seq = static_cast<uint32_t>(i);
        });
    EXPECT_TRUE(ok);
    EXPECT_TRUE(queue.full());

    // Verify all elements
    for (size_t i = 0; i < queue.capacity(); ++i) {
        auto val = queue.try_pop();
        ASSERT_TRUE(val.has_value());
        EXPECT_EQ(val->seq, static_cast<uint32_t>(i));
    }
}

// ===========================================================================
// Blocking batch operations: push_n / produce_n
// ===========================================================================

TYPED_TEST(BoundedQueueTest, PushN_BlockingBatchWrite) {
    TypeParam queue;
    const size_t n = std::min(size_t{4}, queue.capacity());

    std::vector<BoundedTestData> batch(n);
    for (size_t i = 0; i < n; ++i) {
        batch[i].seq = static_cast<uint32_t>(i + 100);
    }

    // push_n should succeed immediately on empty queue
    queue.push_n(std::span<const BoundedTestData>{batch});

    // Verify all elements
    for (size_t i = 0; i < n; ++i) {
        auto val = queue.try_pop();
        ASSERT_TRUE(val.has_value());
        EXPECT_EQ(val->seq, static_cast<uint32_t>(i + 100));
    }
    EXPECT_TRUE(queue.empty());
}

TYPED_TEST(BoundedQueueTest, ProduceN_BlockingBatchVisitor) {
    TypeParam queue;
    const size_t n = std::min(size_t{4}, queue.capacity());

    queue.produce_n(n, [](BoundedTestData& slot, size_t i) {
        slot.seq = static_cast<uint32_t>(i + 200);
    });

    for (size_t i = 0; i < n; ++i) {
        auto val = queue.try_pop();
        ASSERT_TRUE(val.has_value());
        EXPECT_EQ(val->seq, static_cast<uint32_t>(i + 200));
    }
    EXPECT_TRUE(queue.empty());
}

// ===========================================================================
// Timed operations
// ===========================================================================

TEST(BoundedQueueTimed, TryPushForSucceedsOnEmptyQueue) {
    BoundedQueue<BoundedTestData, 4> queue;
    BoundedTestData d{.seq = 42};
    EXPECT_TRUE(queue.try_push_for(d, std::chrono::milliseconds(10)));
    auto val = queue.try_pop();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(val->seq, 42u);
}

TEST(BoundedQueueTimed, TryPushForTimesOutOnFullQueue) {
    BoundedQueue<BoundedTestData, 2> queue;
    BoundedTestData d{.seq = 1};
    ASSERT_TRUE(queue.try_push(d));
    d.seq = 2;
    ASSERT_TRUE(queue.try_push(d));
    ASSERT_TRUE(queue.full());

    auto start = std::chrono::steady_clock::now();
    d.seq = 3;
    EXPECT_FALSE(queue.try_push_for(d, std::chrono::milliseconds(20)));
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, std::chrono::milliseconds(15));
}

TEST(BoundedQueueTimed, TryPopForSucceedsOnNonEmptyQueue) {
    BoundedQueue<BoundedTestData, 4> queue;
    BoundedTestData d{.seq = 99};
    queue.push(d);
    BoundedTestData out{};
    EXPECT_TRUE(queue.try_pop_for(out, std::chrono::milliseconds(10)));
    EXPECT_EQ(out.seq, 99u);
}

TEST(BoundedQueueTimed, TryPopForTimesOutOnEmptyQueue) {
    BoundedQueue<BoundedTestData, 4> queue;
    auto start = std::chrono::steady_clock::now();
    auto val = queue.try_pop_for(std::chrono::milliseconds(20));
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_FALSE(val.has_value());
    EXPECT_GE(elapsed, std::chrono::milliseconds(15));
}

TEST(BoundedQueueTimed, TryPushForSucceedsAfterConsumerDrains) {
    BoundedQueue<BoundedTestData, 2> queue;
    BoundedTestData d{.seq = 1};
    queue.push(d);
    d.seq = 2;
    queue.push(d);
    ASSERT_TRUE(queue.full());

    // Consumer drains after 10ms
    std::thread consumer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        (void)queue.pop();
    });

    d.seq = 3;
    EXPECT_TRUE(queue.try_push_for(d, std::chrono::milliseconds(200)));
    consumer.join();

    // Drain and verify
    auto v1 = queue.pop();
    auto v2 = queue.pop();
    EXPECT_EQ(v1.seq, 2u);
    EXPECT_EQ(v2.seq, 3u);
}

TEST(BoundedQueueTimed, TryPopForSucceedsAfterProducerPushes) {
    BoundedQueue<BoundedTestData, 4> queue;
    ASSERT_TRUE(queue.empty());

    // Producer pushes after 10ms
    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        BoundedTestData d{.seq = 77};
        queue.push(d);
    });

    BoundedTestData out{};
    EXPECT_TRUE(queue.try_pop_for(out, std::chrono::milliseconds(200)));
    EXPECT_EQ(out.seq, 77u);
    producer.join();
}

TEST(BoundedQueueTimed, TryProduceForVisitorPattern) {
    BoundedQueue<BoundedTestData, 4> queue;
    bool ok = queue.try_produce_for(
        [](BoundedTestData& slot) { slot.seq = 55; },
        std::chrono::milliseconds(10));
    EXPECT_TRUE(ok);
    auto val = queue.try_pop();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(val->seq, 55u);
}

TEST(BoundedQueueTimed, TryConsumeForVisitorPattern) {
    BoundedQueue<BoundedTestData, 4> queue;
    BoundedTestData d{.seq = 66};
    queue.push(d);

    uint32_t captured = 0;
    bool ok = queue.try_consume_for(
        [&](const BoundedTestData& data) { captured = data.seq; },
        std::chrono::milliseconds(10));
    EXPECT_TRUE(ok);
    EXPECT_EQ(captured, 66u);
}

TEST(BoundedQueueTimed, TryEmplaceForSucceeds) {
    BoundedQueue<BoundedTestData, 4> queue;
    EXPECT_TRUE(queue.try_emplace_for(std::chrono::milliseconds(10)));
    auto val = queue.try_pop();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(val->seq, 0u); // default-constructed
}

TEST(BoundedQueueTimed, ZeroTimeoutActsLikeTry) {
    BoundedQueue<BoundedTestData, 2> queue;
    BoundedTestData d{.seq = 1};
    // Push succeeds immediately on empty queue even with zero timeout
    EXPECT_TRUE(queue.try_push_for(d, std::chrono::milliseconds(0)));
    d.seq = 2;
    EXPECT_TRUE(queue.try_push_for(d, std::chrono::milliseconds(0)));
    // Queue full, zero timeout should fail immediately
    d.seq = 3;
    EXPECT_FALSE(queue.try_push_for(d, std::chrono::milliseconds(0)));
}

TEST(BoundedQueueBlocking, PushN_SpinsUntilSpaceAvailable) {
    BoundedQueue<BoundedTestData, 4> queue;

    // Fill queue to capacity
    for (uint32_t i = 0; i < 4; ++i) {
        BoundedTestData d;
        d.seq = i;
        queue.push(d);
    }
    ASSERT_TRUE(queue.full());

    std::vector<BoundedTestData> batch(2);
    batch[0].seq = 10;
    batch[1].seq = 11;

    // push_n should block until consumer frees space
    std::atomic<bool> done{false};
    std::thread writer([&] {
        queue.push_n(std::span<const BoundedTestData>{batch});
        done.store(true, std::memory_order_release);
    });

    // Small delay to let writer spin
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_FALSE(done.load(std::memory_order_acquire));

    // Free 2 slots so push_n can complete
    (void)queue.pop();
    (void)queue.pop();

    writer.join();
    EXPECT_TRUE(done.load(std::memory_order_acquire));

    // Drain remaining elements: 2 old + 2 new
    auto v1 = queue.pop(); // old[2]
    auto v2 = queue.pop(); // old[3]
    auto v3 = queue.pop(); // new[0]
    auto v4 = queue.pop(); // new[1]
    EXPECT_EQ(v1.seq, 2u);
    EXPECT_EQ(v2.seq, 3u);
    EXPECT_EQ(v3.seq, 10u);
    EXPECT_EQ(v4.seq, 11u);
}

// ===========================================================================
// Timed batch operations
// ===========================================================================

TYPED_TEST(BoundedQueueTest, TimedBatchPushSuccess) {
    TypeParam queue;
    // Batch size must not exceed capacity (Capacity=1 caps batch at 1).
    const size_t n = std::min<size_t>(2, TypeParam::capacity());
    std::vector<BoundedTestData> batch(n);
    for (size_t i = 0; i < n; ++i) {
        batch[i].seq = static_cast<uint32_t>(42 + i);
    }

    bool ok = queue.try_push_n_for(
        std::span<const BoundedTestData>{batch},
        std::chrono::milliseconds(10));
    EXPECT_TRUE(ok);
    EXPECT_EQ(queue.size(), n);

    for (size_t i = 0; i < n; ++i) {
        auto v = queue.pop();
        EXPECT_EQ(v.seq, static_cast<uint32_t>(42 + i));
    }
}

TYPED_TEST(BoundedQueueTest, TimedBatchPushTimeoutOnFull) {
    TypeParam queue;
    const size_t cap = queue.capacity();

    // Fill the queue
    for (size_t i = 0; i < cap; ++i) {
        BoundedTestData d;
        d.seq = static_cast<uint32_t>(i);
        ASSERT_TRUE(queue.try_push(d));
    }

    // Batch push should timeout on full queue
    std::vector<BoundedTestData> batch(2);
    bool ok = queue.try_push_n_for(
        std::span<const BoundedTestData>{batch},
        std::chrono::milliseconds(5));
    EXPECT_FALSE(ok);
}

TYPED_TEST(BoundedQueueTest, TimedBatchPushWaitsForSpace) {
    // Use capacity-2 queue for deterministic test
    BoundedQueue<BoundedTestData, 4> queue;

    // Fill 3 of 4 slots
    for (uint32_t i = 0; i < 3; ++i) {
        BoundedTestData d;
        d.seq = i;
        ASSERT_TRUE(queue.try_push(d));
    }

    // Batch of 2 needs 2 free slots, only 1 available
    std::vector<BoundedTestData> batch(2);
    batch[0].seq = 100;
    batch[1].seq = 101;

    std::atomic<bool> done{false};
    std::thread writer([&] {
        bool ok = queue.try_push_n_for(
            std::span<const BoundedTestData>{batch},
            std::chrono::milliseconds(200));
        EXPECT_TRUE(ok);
        done.store(true, std::memory_order_release);
    });

    // Let writer spin briefly
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_FALSE(done.load(std::memory_order_acquire));

    // Free 2 slots so batch push succeeds
    (void)queue.pop();
    (void)queue.pop();

    writer.join();
    EXPECT_TRUE(done.load(std::memory_order_acquire));
}

TYPED_TEST(BoundedQueueTest, TimedBatchProduceNSuccess) {
    TypeParam queue;
    const size_t n = std::min<size_t>(queue.capacity(), 2);

    bool ok = queue.try_produce_n_for(
        n,
        [](BoundedTestData& slot, size_t idx) {
            slot.seq = static_cast<uint32_t>(idx + 10);
        },
        std::chrono::milliseconds(10));
    EXPECT_TRUE(ok);
    EXPECT_EQ(queue.size(), n);

    for (size_t i = 0; i < n; ++i) {
        EXPECT_EQ(queue.pop().seq, static_cast<uint32_t>(i + 10));
    }
}

TYPED_TEST(BoundedQueueTest, TimedBatchPopSuccess) {
    TypeParam queue;
    const size_t count = std::min<size_t>(queue.capacity(), 4);

    // Push elements up to capacity
    for (uint32_t i = 0; i < count; ++i) {
        BoundedTestData d;
        d.seq = i;
        ASSERT_TRUE(queue.try_push(d));
    }

    std::array<BoundedTestData, 4> out{};
    size_t n = queue.try_pop_n_for(
        std::span<BoundedTestData>{out.data(), count},
        std::chrono::milliseconds(10));
    EXPECT_EQ(n, count);
    for (uint32_t i = 0; i < n; ++i) {
        EXPECT_EQ(out[i].seq, i);
    }
}

TYPED_TEST(BoundedQueueTest, TimedBatchPopTimeoutOnEmpty) {
    TypeParam queue;

    std::array<BoundedTestData, 2> out{};
    size_t n = queue.try_pop_n_for(
        std::span<BoundedTestData>{out},
        std::chrono::milliseconds(5));
    EXPECT_EQ(n, 0u);
}

TYPED_TEST(BoundedQueueTest, TimedBatchPopWaitsForData) {
    BoundedQueue<BoundedTestData, 4> queue;

    std::array<BoundedTestData, 2> out{};
    std::atomic<size_t> popped{0};

    std::thread reader([&] {
        size_t n = queue.try_pop_n_for(
            std::span<BoundedTestData>{out},
            std::chrono::milliseconds(200));
        popped.store(n, std::memory_order_release);
    });

    // Let reader spin briefly
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_EQ(popped.load(std::memory_order_acquire), 0u);

    // Push data so reader can proceed
    BoundedTestData d;
    d.seq = 77;
    queue.push(d);
    d.seq = 78;
    queue.push(d);

    reader.join();
    EXPECT_GE(popped.load(std::memory_order_acquire), 1u);
}

TYPED_TEST(BoundedQueueTest, TimedBatchPushZeroLength) {
    TypeParam queue;

    // Zero-length batch should succeed immediately
    bool ok = queue.try_push_n_for(
        std::span<const BoundedTestData>{},
        std::chrono::milliseconds(0));
    EXPECT_TRUE(ok);
    EXPECT_TRUE(queue.empty());
}

TYPED_TEST(BoundedQueueTest, TimedBatchPopZeroLength) {
    TypeParam queue;

    // Zero-length pop should return 0 immediately
    size_t n = queue.try_pop_n_for(
        std::span<BoundedTestData>{},
        std::chrono::milliseconds(0));
    EXPECT_EQ(n, 0u);
}

// ===========================================================================
// try_consume_n / consume_n / try_consume_n_for — 批量零拷贝消费
// ===========================================================================

TYPED_TEST(BoundedQueueTest, TryConsumeN_BasicOperation) {
    TypeParam queue;
    const size_t cap = TypeParam::capacity();

    // Push cap items
    for (uint32_t i = 0; i < static_cast<uint32_t>(cap); ++i) {
        BoundedTestData d;
        d.seq = i + 1;
        ASSERT_TRUE(queue.try_push(d));
    }

    // Consume at most (cap - 1) via visitor, leaving 1
    const size_t to_consume = cap - 1;
    std::vector<uint32_t> consumed;
    size_t n = queue.try_consume_n(to_consume, [&](const BoundedTestData& slot, size_t idx) {
        consumed.push_back(slot.seq);
        EXPECT_EQ(idx, consumed.size() - 1);
    });
    EXPECT_EQ(n, to_consume);
    ASSERT_EQ(consumed.size(), to_consume);
    for (size_t i = 0; i < to_consume; ++i) {
        EXPECT_EQ(consumed[i], static_cast<uint32_t>(i + 1));
    }

    // 1 item still in queue
    EXPECT_EQ(queue.size(), 1u);
    auto rem = queue.try_pop();
    ASSERT_TRUE(rem.has_value());
    EXPECT_EQ(rem->seq, static_cast<uint32_t>(cap));
}

TYPED_TEST(BoundedQueueTest, TryConsumeN_EmptyQueue) {
    TypeParam queue;

    size_t n = queue.try_consume_n(4, [](const BoundedTestData&, size_t) {
        FAIL() << "Visitor should not be called on empty queue";
    });
    EXPECT_EQ(n, 0u);
}

TYPED_TEST(BoundedQueueTest, TryConsumeN_ZeroCount) {
    TypeParam queue;
    BoundedTestData d{};
    d.seq = 1;
    queue.try_push(d);

    size_t n = queue.try_consume_n(0, [](const BoundedTestData&, size_t) {
        FAIL() << "Visitor should not be called with n=0";
    });
    EXPECT_EQ(n, 0u);
    EXPECT_EQ(queue.size(), 1u);  // nothing consumed
}

TYPED_TEST(BoundedQueueTest, TryConsumeN_MoreThanAvailable) {
    TypeParam queue;

    // Push up to 2 items (capped at capacity), request 100
    const size_t pushed = std::min<size_t>(2, TypeParam::capacity());
    for (uint32_t i = 0; i < pushed; ++i) {
        BoundedTestData d;
        d.seq = i;
        ASSERT_TRUE(queue.try_push(d));
    }

    size_t n = queue.try_consume_n(100, [](const BoundedTestData& slot, size_t idx) {
        EXPECT_EQ(slot.seq, static_cast<uint32_t>(idx));
    });
    EXPECT_EQ(n, pushed);
    EXPECT_TRUE(queue.empty());
}

TYPED_TEST(BoundedQueueTest, ConsumeN_BlockingBatchVisitor) {
    using Queue = BoundedQueue<BoundedTestData, 1024>;
    Queue queue;

    constexpr uint32_t kBatch = 8;

    // Producer thread pushes kBatch items after a short delay
    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        for (uint32_t i = 0; i < kBatch; ++i) {
            BoundedTestData d;
            d.seq = i;
            queue.push(d);
        }
    });

    // Blocking consume_n: should spin until data arrives
    std::vector<uint32_t> consumed;
    size_t n = queue.consume_n(kBatch, [&](const BoundedTestData& slot, size_t) {
        consumed.push_back(slot.seq);
    });

    EXPECT_GE(n, 1u);  // At least 1 element consumed
    EXPECT_LE(n, kBatch);

    // Drain remaining
    while (consumed.size() < kBatch) {
        queue.consume([&](const BoundedTestData& slot) {
            consumed.push_back(slot.seq);
        });
    }

    producer.join();

    for (uint32_t i = 0; i < kBatch; ++i) {
        EXPECT_EQ(consumed[i], i);
    }
}

TEST(BoundedQueueTimed, TryConsumeNForSucceedsImmediately) {
    BoundedQueue<BoundedTestData, 256> queue;

    for (uint32_t i = 0; i < 5; ++i) {
        BoundedTestData d;
        d.seq = i + 10;
        queue.try_push(d);
    }

    std::vector<uint32_t> consumed;
    size_t n = queue.try_consume_n_for(3,
        [&](const BoundedTestData& slot, size_t) {
            consumed.push_back(slot.seq);
        },
        std::chrono::milliseconds(100));

    EXPECT_EQ(n, 3u);
    ASSERT_EQ(consumed.size(), 3u);
    EXPECT_EQ(consumed[0], 10u);
    EXPECT_EQ(consumed[1], 11u);
    EXPECT_EQ(consumed[2], 12u);
}

TEST(BoundedQueueTimed, TryConsumeNForTimesOut) {
    BoundedQueue<BoundedTestData, 256> queue;

    auto start = std::chrono::steady_clock::now();
    size_t n = queue.try_consume_n_for(4,
        [](const BoundedTestData&, size_t) {
            FAIL() << "Should not be called on timeout";
        },
        std::chrono::milliseconds(10));
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(n, 0u);
    EXPECT_GE(elapsed, std::chrono::milliseconds(10));
}

TEST(BoundedQueueTimed, TryConsumeNForWaitsForData) {
    BoundedQueue<BoundedTestData, 256> queue;

    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        for (uint32_t i = 0; i < 4; ++i) {
            BoundedTestData d;
            d.seq = i;
            queue.try_push(d);
        }
    });

    std::vector<uint32_t> consumed;
    size_t n = queue.try_consume_n_for(4,
        [&](const BoundedTestData& slot, size_t) {
            consumed.push_back(slot.seq);
        },
        std::chrono::milliseconds(500));

    producer.join();

    EXPECT_GE(n, 1u);
    EXPECT_LE(n, 4u);
    for (size_t i = 0; i < consumed.size(); ++i) {
        EXPECT_EQ(consumed[i], static_cast<uint32_t>(i));
    }
}

// ===========================================================================
// try_peek
// ===========================================================================

TEST(BoundedQueue, try_peek_returns_false_on_empty_queue) {
    BoundedQueue<BoundedTestData, 4> q;
    BoundedTestData out;
    EXPECT_FALSE(q.try_peek(out));
}

TEST(BoundedQueue, try_peek_optional_returns_nullopt_on_empty) {
    BoundedQueue<BoundedTestData, 4> q;
    EXPECT_FALSE(q.try_peek().has_value());
}

TEST(BoundedQueue, try_peek_reads_head_without_consuming) {
    BoundedQueue<BoundedTestData, 4> q;
    BoundedTestData d1{.seq = 42};
    BoundedTestData d2{.seq = 99};
    (void)q.try_push(d1);
    (void)q.try_push(d2);

    BoundedTestData peeked;
    EXPECT_TRUE(q.try_peek(peeked));
    EXPECT_EQ(peeked.seq, 42u);
    EXPECT_EQ(q.size(), 2u);  // Nothing consumed

    // Peek again — same element
    BoundedTestData peeked2;
    EXPECT_TRUE(q.try_peek(peeked2));
    EXPECT_EQ(peeked2.seq, 42u);
    EXPECT_EQ(q.size(), 2u);
}

TEST(BoundedQueue, try_peek_then_pop_advances_head) {
    BoundedQueue<BoundedTestData, 4> q;
    BoundedTestData d1{.seq = 10};
    BoundedTestData d2{.seq = 20};
    (void)q.try_push(d1);
    (void)q.try_push(d2);

    // Peek sees first element
    auto peeked = q.try_peek();
    ASSERT_TRUE(peeked.has_value());
    EXPECT_EQ(peeked->seq, 10u);

    // Pop consumes it
    auto popped = q.try_pop();
    ASSERT_TRUE(popped.has_value());
    EXPECT_EQ(popped->seq, 10u);

    // Next peek sees second element
    peeked = q.try_peek();
    ASSERT_TRUE(peeked.has_value());
    EXPECT_EQ(peeked->seq, 20u);
}

TEST(BoundedQueue, try_peek_on_full_queue) {
    BoundedQueue<BoundedTestData, 4> q;
    for (uint32_t i = 0; i < 4; ++i) {
        (void)q.try_push(BoundedTestData{.seq = i});
    }
    EXPECT_TRUE(q.full());

    BoundedTestData peeked;
    EXPECT_TRUE(q.try_peek(peeked));
    EXPECT_EQ(peeked.seq, 0u);
    EXPECT_TRUE(q.full());  // Still full — nothing consumed
}

TEST(BoundedQueue, try_peek_visitor_returns_false_on_empty) {
    BoundedQueue<BoundedTestData, 4> q;
    bool visited = false;
    EXPECT_FALSE(q.try_peek([&](const BoundedTestData&) { visited = true; }));
    EXPECT_FALSE(visited);
}

TEST(BoundedQueue, try_peek_visitor_reads_without_consuming) {
    BoundedQueue<BoundedTestData, 4> q;
    (void)q.try_push(BoundedTestData{.seq = 77});
    (void)q.try_push(BoundedTestData{.seq = 88});

    uint32_t captured = 0;
    EXPECT_TRUE(q.try_peek([&](const BoundedTestData& d) { captured = d.seq; }));
    EXPECT_EQ(captured, 77u);
    EXPECT_EQ(q.size(), 2u);  // nothing consumed

    // Peek again — same element
    captured = 0;
    EXPECT_TRUE(q.try_peek([&](const BoundedTestData& d) { captured = d.seq; }));
    EXPECT_EQ(captured, 77u);
}

// ---------------------------------------------------------------------------
// Stats snapshot
// ---------------------------------------------------------------------------

TEST(BoundedQueue, stats_empty_queue_returns_zeroes) {
    BoundedQueue<BoundedTestData, 4> q;
    auto s = q.stats();
    EXPECT_EQ(s.total_pushed, 0u);
    EXPECT_EQ(s.total_popped, 0u);
    EXPECT_EQ(s.current_size, 0u);
    EXPECT_EQ(s.capacity, 4u);
}

TEST(BoundedQueue, stats_tracks_push_and_pop) {
    BoundedQueue<BoundedTestData, 4> q;
    (void)q.try_push(BoundedTestData{.seq = 1});
    (void)q.try_push(BoundedTestData{.seq = 2});
    (void)q.try_push(BoundedTestData{.seq = 3});

    auto s1 = q.stats();
    EXPECT_EQ(s1.total_pushed, 3u);
    EXPECT_EQ(s1.total_popped, 0u);
    EXPECT_EQ(s1.current_size, 3u);

    BoundedTestData out;
    q.try_pop(out);
    q.try_pop(out);

    auto s2 = q.stats();
    EXPECT_EQ(s2.total_pushed, 3u);
    EXPECT_EQ(s2.total_popped, 2u);
    EXPECT_EQ(s2.current_size, 1u);
}

TEST(BoundedQueue, stats_after_clear_shows_zero_current_size) {
    BoundedQueue<BoundedTestData, 4> q;
    (void)q.try_push(BoundedTestData{.seq = 1});
    (void)q.try_push(BoundedTestData{.seq = 2});
    q.clear();

    auto s = q.stats();
    // clear() advances head to match tail, so total_pushed stays but current_size=0
    EXPECT_EQ(s.total_pushed, 2u);
    EXPECT_EQ(s.current_size, 0u);
}

TEST(BoundedQueue, stats_after_wraparound) {
    BoundedQueue<BoundedTestData, 2> q;
    BoundedTestData out;
    // Push 2 (fill), pop 2, push 2 more → indices wrap around capacity
    (void)q.try_push(BoundedTestData{.seq = 1});
    (void)q.try_push(BoundedTestData{.seq = 2});
    q.try_pop(out);
    q.try_pop(out);
    (void)q.try_push(BoundedTestData{.seq = 3});
    (void)q.try_push(BoundedTestData{.seq = 4});

    auto s = q.stats();
    EXPECT_EQ(s.total_pushed, 4u);
    EXPECT_EQ(s.total_popped, 2u);
    EXPECT_EQ(s.current_size, 2u);
}

TEST(BoundedQueueStats, dump_contains_key_info) {
    BoundedQueue<BoundedTestData, 4> q;
    (void)q.try_push(BoundedTestData{.seq = 1});
    (void)q.try_push(BoundedTestData{.seq = 2});

    auto s = q.stats();
    auto dump = s.dump();

    EXPECT_NE(dump.find("BoundedQueue"), std::string::npos);
    EXPECT_NE(dump.find("capacity: 4"), std::string::npos);
    EXPECT_NE(dump.find("current_size: 2"), std::string::npos);
    EXPECT_NE(dump.find("total_pushed: 2"), std::string::npos);
    EXPECT_NE(dump.find("total_popped: 0"), std::string::npos);
}

TEST(BoundedQueueStats, to_json_is_valid_format) {
    BoundedQueue<BoundedTestData, 4> q;
    (void)q.try_push(BoundedTestData{.seq = 1});

    auto s = q.stats();
    auto json = s.to_json();

    EXPECT_NE(json.find("\"capacity\":4"), std::string::npos);
    EXPECT_NE(json.find("\"current_size\":1"), std::string::npos);
    EXPECT_NE(json.find("\"total_pushed\":1"), std::string::npos);
    EXPECT_NE(json.find("\"total_popped\":0"), std::string::npos);
}

TEST(BoundedQueueStats, dump_empty_queue_shows_zero_utilization) {
    BoundedQueue<BoundedTestData, 4> q;
    auto s = q.stats();
    auto dump = s.dump();
    EXPECT_NE(dump.find("current_size: 0"), std::string::npos);
    EXPECT_NE(dump.find("0.0% full"), std::string::npos);
}

TEST(BoundedQueueStats, dump_full_queue_shows_100_percent) {
    BoundedQueue<BoundedTestData, 2> q;
    (void)q.try_push(BoundedTestData{.seq = 1});
    (void)q.try_push(BoundedTestData{.seq = 2});
    auto s = q.stats();
    auto dump = s.dump();
    EXPECT_NE(dump.find("current_size: 2"), std::string::npos);
    EXPECT_NE(dump.find("100.0% full"), std::string::npos);
}

TEST(BoundedQueueStats, operator_minus_computes_interval_delta) {
    BoundedQueue<BoundedTestData, 4> q;
    (void)q.try_push(BoundedTestData{.seq = 1});
    (void)q.try_push(BoundedTestData{.seq = 2});
    auto s1 = q.stats();

    (void)q.try_push(BoundedTestData{.seq = 3});
    BoundedTestData tmp;
    q.pop(tmp);
    auto s2 = q.stats();

    auto delta = s2 - s1;
    EXPECT_EQ(delta.total_pushed, 1u);  // 1 new push
    EXPECT_EQ(delta.total_popped, 1u);  // 1 new pop
    EXPECT_EQ(delta.current_size, s2.current_size);  // point-in-time
    EXPECT_EQ(delta.capacity, 4u);
}

TEST(BoundedQueueStats, operator_eq_compares_all_fields) {
    BoundedQueue<BoundedTestData, 4> q;
    auto s1 = q.stats();
    auto s2 = q.stats();
    EXPECT_EQ(s1, s2);

    (void)q.try_push(BoundedTestData{.seq = 1});
    auto s3 = q.stats();
    EXPECT_NE(s1, s3);
}

TEST(BoundedQueueStats, std_formatter_produces_dump_output) {
    BoundedQueue<BoundedTestData, 4> q;
    (void)q.try_push(BoundedTestData{.seq = 1});
    auto s = q.stats();

    auto formatted = std::format("{}", s);
    EXPECT_EQ(formatted, s.dump());
    EXPECT_NE(formatted.find("BoundedQueue::Stats:"), std::string::npos);
    EXPECT_NE(formatted.find("total_pushed: 1"), std::string::npos);
}

TEST(BoundedQueueStats, std_formatter_empty_queue) {
    BoundedQueue<BoundedTestData, 4> q;
    auto s = q.stats();
    auto formatted = std::format("{}", s);
    EXPECT_NE(formatted.find("current_size: 0"), std::string::npos);
}

TEST(BoundedQueueStats, throughput_computes_items_per_second) {
    eph::containers::BoundedQueueStats delta{
        .total_pushed = 1000,
        .total_popped = 1000,
        .current_size = 0,
        .capacity = 64,
    };
    // 1000 items in 0.5 seconds = 2000 items/sec
    EXPECT_DOUBLE_EQ(delta.throughput(500'000'000), 2000.0);
}

TEST(BoundedQueueStats, throughput_zero_duration_returns_zero) {
    eph::containers::BoundedQueueStats delta{
        .total_pushed = 100,
        .total_popped = 100,
        .current_size = 0,
        .capacity = 64,
    };
    EXPECT_DOUBLE_EQ(delta.throughput(0), 0.0);
}

// ===========================================================================
// try_consume_all — drain all available elements
// ===========================================================================

TYPED_TEST(BoundedQueueTest, try_consume_all_empty_queue_returns_zero) {
    TypeParam queue;
    size_t n = queue.try_consume_all([](const BoundedTestData&, size_t) {
        FAIL() << "Visitor should not be called on empty queue";
    });
    EXPECT_EQ(n, 0u);
}

TYPED_TEST(BoundedQueueTest, try_consume_all_drains_all_elements) {
    TypeParam queue;
    const size_t cap = TypeParam::capacity();

    // Fill the queue
    for (size_t i = 0; i < cap; ++i) {
        BoundedTestData d;
        d.seq = static_cast<uint32_t>(i);
        ASSERT_TRUE(queue.try_push(d));
    }
    EXPECT_EQ(queue.size(), cap);

    std::vector<uint32_t> consumed;
    size_t n = queue.try_consume_all([&](const BoundedTestData& slot, size_t idx) {
        consumed.push_back(slot.seq);
        EXPECT_EQ(idx, consumed.size() - 1);
    });

    EXPECT_EQ(n, cap);
    EXPECT_EQ(consumed.size(), cap);
    EXPECT_TRUE(queue.empty());

    // Verify order
    for (size_t i = 0; i < consumed.size(); ++i) {
        EXPECT_EQ(consumed[i], static_cast<uint32_t>(i));
    }
}

TYPED_TEST(BoundedQueueTest, try_consume_all_partial_fill) {
    TypeParam queue;
    // Push 3 elements (less than capacity)
    const size_t count = std::min<size_t>(3, TypeParam::capacity());
    for (size_t i = 0; i < count; ++i) {
        BoundedTestData d;
        d.seq = static_cast<uint32_t>(i + 100);
        ASSERT_TRUE(queue.try_push(d));
    }

    std::vector<uint32_t> consumed;
    size_t n = queue.try_consume_all([&](const BoundedTestData& slot, size_t) {
        consumed.push_back(slot.seq);
    });

    EXPECT_EQ(n, count);
    EXPECT_TRUE(queue.empty());
    for (size_t i = 0; i < count; ++i) {
        EXPECT_EQ(consumed[i], static_cast<uint32_t>(i + 100));
    }
}

TEST(BoundedQueueConsumeAll, works_after_head_wraps_around) {
    // Test try_consume_all after multiple produce/consume cycles that
    // cause the internal head/tail pointers to wrap around the ring buffer.
    BoundedQueue<BoundedTestData, 4> queue;

    // Cycle through the buffer several times to advance head/tail
    for (int cycle = 0; cycle < 10; ++cycle) {
        for (int i = 0; i < 4; ++i) {
            BoundedTestData d;
            d.seq = static_cast<uint32_t>(cycle * 100 + i);
            ASSERT_TRUE(queue.try_push(d));
        }
        // Drain all
        size_t n = queue.try_consume_all([](const BoundedTestData&, size_t) {});
        EXPECT_EQ(n, 4u);
    }

    // Now push 2 elements and consume_all — should work correctly
    // even though head/tail are at high offsets
    for (int i = 0; i < 2; ++i) {
        BoundedTestData d;
        d.seq = static_cast<uint32_t>(999 + i);
        ASSERT_TRUE(queue.try_push(d));
    }

    std::vector<uint32_t> consumed;
    size_t n = queue.try_consume_all([&](const BoundedTestData& slot, size_t idx) {
        consumed.push_back(slot.seq);
        EXPECT_EQ(idx, consumed.size() - 1);
    });

    EXPECT_EQ(n, 2u);
    EXPECT_EQ(consumed[0], 999u);
    EXPECT_EQ(consumed[1], 1000u);
}

TEST(BoundedQueueConsumeAll, capacity_one_queue) {
    BoundedQueue<BoundedTestData, 1> queue;

    BoundedTestData d;
    d.seq = 42;
    ASSERT_TRUE(queue.try_push(d));

    std::vector<uint32_t> consumed;
    size_t n = queue.try_consume_all([&](const BoundedTestData& slot, size_t idx) {
        consumed.push_back(slot.seq);
        EXPECT_EQ(idx, 0u);
    });

    EXPECT_EQ(n, 1u);
    EXPECT_EQ(consumed[0], 42u);
    EXPECT_TRUE(queue.empty());
}

// ---------------------------------------------------------------------------
// Stats safety: current_size never exceeds capacity
// ---------------------------------------------------------------------------
TEST(BoundedQueueStats, current_size_never_exceeds_capacity) {
    constexpr size_t kCap = 4;
    BoundedQueue<BoundedTestData, kCap> q;

    // Empty
    EXPECT_LE(q.stats().current_size, kCap);

    // Fill completely
    for (size_t i = 0; i < kCap; ++i) {
        (void)q.try_push(BoundedTestData{.seq = static_cast<uint32_t>(i)});
        auto s = q.stats();
        EXPECT_LE(s.current_size, s.capacity);
    }

    // Overfill attempt (should fail, but stats must still be sane)
    EXPECT_FALSE(q.try_push(BoundedTestData{.seq = 99}));
    auto s = q.stats();
    EXPECT_LE(s.current_size, s.capacity);
    EXPECT_EQ(s.current_size, kCap);

    // Drain and verify at each step
    for (size_t i = 0; i < kCap; ++i) {
        BoundedTestData out;
        (void)q.try_pop(out);
        auto s2 = q.stats();
        EXPECT_LE(s2.current_size, s2.capacity);
    }
    EXPECT_EQ(q.stats().current_size, 0u);
}
