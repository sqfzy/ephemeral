#include <gtest/gtest.h>
#include <atomic>
#include <array>
#include <print>
#include <thread>

#include "eph/containers/evicting_queue.hpp"

using eph::containers::EvictingQueue;

// 定义一个满足 TrivialData 的测试负载结构
// 总大小 256 字节，便于模拟真实的缓存行竞争和内存撕裂检测
struct TestData {
    uint32_t seq{0};
    std::array<uint32_t, 63> payload{};

    bool operator==(const TestData& other) const = default;
};

// 1. 定义类型化测试夹具，测试单槽位特化 (1) 和多槽位 (4)
template <typename T>
class EvictingQueueTest : public ::testing::Test {};

using QueueTypes = ::testing::Types<
    EvictingQueue<TestData, 1>,
    EvictingQueue<TestData, 4>
>;

TYPED_TEST_SUITE(EvictingQueueTest, QueueTypes);

// 2. emplace 测试
TYPED_TEST(EvictingQueueTest, EmplaceBasic) {
    TypeParam queue;

    // emplace 默认构造
    queue.emplace();
    auto res = queue.try_pop_latest();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->seq, 0u);

    // emplace 后 pop 再 emplace 验证覆盖
    TestData expected;
    expected.seq = 99;
    expected.payload.fill(99);
    queue.emplace(expected);
    auto res2 = queue.try_pop_latest();
    ASSERT_TRUE(res2.has_value());
    EXPECT_EQ(res2.value(), expected);
}

// 3. 单线程基本 push, pop_latest 测试
TYPED_TEST(EvictingQueueTest, SingleThreadBasicPushPop) {
    TypeParam queue;
    
    TestData data;
    data.seq = 1;
    data.payload.fill(42); 
    
    // 基本推入
    queue.push(data);

    // 基本弹出
    auto res = queue.try_pop_latest();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res.value().seq, 1);
    
    // 严格验证：整块内存必须与原数据完全一致
    EXPECT_EQ(res.value(), data);

    // 验证无新数据时返回空
    auto res2 = queue.try_pop_latest();
    EXPECT_FALSE(res2.has_value());
}

// 3. 单线程 push 发生覆盖后的 try_pop_latest 测试
TYPED_TEST(EvictingQueueTest, SingleThreadEviction) {
    TypeParam queue;
    const size_t push_count = 10; // 推入次数大于最大测试容量(4)，必然发生数据覆盖驱逐

    // 连续推入触发覆盖
    for (uint32_t i = 1; i <= push_count; ++i) {
        TestData data;
        data.seq = i;
        data.payload.fill(i);
        queue.push(data);
    }

    // 弹出，验证结果为最新推入的数据
    auto res = queue.try_pop_latest();
    ASSERT_TRUE(res.has_value());
    
    // 严格验证：期望全部数据均为 push_count 的状态
    EXPECT_EQ(res.value().seq, push_count);
    EXPECT_EQ(res.value().payload[0], push_count);

    // 再次弹出应为空
    auto res2 = queue.try_pop_latest();
    EXPECT_FALSE(res2.has_value());
}

// 4. 多线程满负荷推入弹出的压力测试
TYPED_TEST(EvictingQueueTest, MultiThreadStress) {
    TypeParam queue;
    const uint32_t total_messages = 1'000'000;
    std::atomic<bool> producer_done{false};

    // 生产者线程：全速推入单调递增的整数序列
    std::thread producer([&]() {
        for (uint32_t i = 1; i <= total_messages; ++i) {
            TestData data;
            data.seq = i;
            
            // 严格验证构造：剩余的 payload 填充与序列号 i 强相关的数据
            // 这样可以确保如果发生并发内存撕裂，后面的字节一定对不上
            for (size_t j = 0; j < data.payload.size(); ++j) {
                data.payload[j] = i + j;
            }
            queue.push(data);
        }
        producer_done.store(true, std::memory_order_release);
    });

    uint32_t last_received = 0;
    uint32_t received_count = 0;

    auto verify_payload = [&](const TestData& data) {
        // 验证读取的数据序列必须严格递增，说明没有乱序和旧数据复现
        EXPECT_GT(data.seq, last_received);
        
        // 严格验证 payload 部分：检查每个字段是否匹配对应的序列号模式
        bool is_valid = true;
        for (size_t j = 0; j < data.payload.size(); ++j) {
            if (data.payload[j] != data.seq + j) {
                is_valid = false;
                break;
            }
        }
        EXPECT_TRUE(is_valid) << "Data corruption / Torn read detected at sequence: " << data.seq;
        
        last_received = data.seq;
        received_count++;
    };

    // 消费者线程：循环读取最新数据
    std::thread consumer([&]() {
        while (!producer_done.load(std::memory_order_acquire)) {
            auto res = queue.try_pop_latest();
            if (res.has_value()) {
                verify_payload(res.value());
            }
        }
        
        // 生产结束后，把队列中最后的残留数据读取完
        while (true) {
            auto res = queue.try_pop_latest();
            if (res.has_value()) {
                verify_payload(res.value());
            } else {
                break;
            }
        }
    });

    producer.join();
    consumer.join();

    // 最终一定能获取到生产者发出的最后一条消息
    EXPECT_EQ(last_received, total_messages);
    std::println("Multi-thread stress test done. Processed {} out of {} messages.", received_count, total_messages);
}

// 5. 空队列读取应返回 false / nullopt
TYPED_TEST(EvictingQueueTest, EmptyQueueReturnsEmpty) {
    TypeParam queue;

    // try_pop_latest on fresh queue
    auto res = queue.try_pop_latest();
    EXPECT_FALSE(res.has_value());

    // try_consume_latest should also fail
    bool consumed = queue.try_consume_latest([](const TestData&) {
        FAIL() << "Should not be called on empty queue";
    });
    EXPECT_FALSE(consumed);
}

// 6. 连续读取无新数据时持续返回 false
TYPED_TEST(EvictingQueueTest, RepeatedReadWithoutNewDataReturnsFalse) {
    TypeParam queue;

    TestData data;
    data.seq = 1;
    data.payload.fill(1);
    queue.push(data);

    // First read succeeds
    auto res1 = queue.try_pop_latest();
    ASSERT_TRUE(res1.has_value());
    EXPECT_EQ(res1->seq, 1u);

    // Subsequent reads without new push return false
    for (int i = 0; i < 100; ++i) {
        EXPECT_FALSE(queue.try_pop_latest().has_value());
    }

    // After another push, read should succeed again
    data.seq = 2;
    data.payload.fill(2);
    queue.push(data);

    auto res2 = queue.try_pop_latest();
    ASSERT_TRUE(res2.has_value());
    EXPECT_EQ(res2->seq, 2u);
}

// 7. Capacity=2 边界：推入 2 个后覆盖第 1 个
TEST(EvictingQueueCapacity2, OverwriteAndReadLatest) {
    EvictingQueue<TestData, 2> queue;

    TestData d1, d2, d3;
    d1.seq = 1; d1.payload.fill(1);
    d2.seq = 2; d2.payload.fill(2);
    d3.seq = 3; d3.payload.fill(3);

    queue.push(d1);
    queue.push(d2);

    // Both slots occupied, read latest = d2
    auto res = queue.try_pop_latest();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->seq, 2u);

    // Now push d3, which overwrites the oldest slot
    queue.push(d3);
    auto res2 = queue.try_pop_latest();
    ASSERT_TRUE(res2.has_value());
    EXPECT_EQ(res2->seq, 3u);
    EXPECT_EQ(res2->payload[0], 3u);
}

// 8. Rapid overwrite: many pushes between reads, last one always wins
TYPED_TEST(EvictingQueueTest, RapidOverwriteLatestWins) {
    TypeParam queue;

    // Push 1000 values without reading
    for (uint32_t i = 1; i <= 1000; ++i) {
        TestData data;
        data.seq = i;
        data.payload.fill(i);
        queue.push(data);
    }

    // Only the latest should be returned
    auto res = queue.try_pop_latest();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->seq, 1000u);
    EXPECT_EQ(res->payload[0], 1000u);
}

// clear() 测试：清空后 try_consume_latest 返回 false，直到新数据写入
TYPED_TEST(EvictingQueueTest, ClearDiscardsUnreadData) {
    TypeParam queue;
    TestData data;
    data.seq = 42;
    data.payload.fill(42);
    queue.push(data);

    // clear 后不应读到旧数据
    queue.clear();
    EXPECT_FALSE(queue.try_pop_latest().has_value());

    // 写入新数据后应正常读取
    data.seq = 99;
    data.payload.fill(99);
    queue.push(data);
    auto res = queue.try_pop_latest();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->seq, 99u);
}

// clear() 在未写入任何数据的空队列上安全调用
TYPED_TEST(EvictingQueueTest, ClearOnFreshQueue) {
    TypeParam queue;
    queue.clear();
    EXPECT_FALSE(queue.try_pop_latest().has_value());

    TestData data;
    data.seq = 1;
    queue.push(data);
    auto res = queue.try_pop_latest();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->seq, 1u);
}

TYPED_TEST(EvictingQueueTest, WriteCountStartsAtZero) {
    TypeParam queue;
    EXPECT_EQ(queue.write_count(), 0u);
}

TYPED_TEST(EvictingQueueTest, WriteCountIncrementsOnPush) {
    TypeParam queue;
    TestData data{};

    for (uint32_t i = 1; i <= 10; ++i) {
        data.seq = i;
        queue.push(data);
        EXPECT_EQ(queue.write_count(), i);
    }
}

TYPED_TEST(EvictingQueueTest, WriteCountUnaffectedByClear) {
    TypeParam queue;
    TestData data{};

    data.seq = 1;
    queue.push(data);
    data.seq = 2;
    queue.push(data);
    EXPECT_EQ(queue.write_count(), 2u);

    queue.clear();
    // write_count persists across clear — it's a lifetime counter
    EXPECT_EQ(queue.write_count(), 2u);

    data.seq = 3;
    queue.push(data);
    EXPECT_EQ(queue.write_count(), 3u);
}

// ─────────────────────────────────────────────────────────────────────────────
// size_approx / empty observability
// ─────────────────────────────────────────────────────────────────────────────

TEST(EvictingQueueTest, SizeApproxReflectsUnreadEntries) {
    EvictingQueue<TestData, 4> queue;

    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.size_approx(), 0u);

    TestData d{};
    d.seq = 1;
    queue.push(d);
    EXPECT_EQ(queue.size_approx(), 1u);
    EXPECT_FALSE(queue.empty());

    d.seq = 2;
    queue.push(d);
    EXPECT_EQ(queue.size_approx(), 2u);

    // Consume latest
    auto out = queue.try_pop_latest();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->seq, 2u);
    // After consuming latest, all entries are considered read
    EXPECT_EQ(queue.size_approx(), 0u);
    EXPECT_TRUE(queue.empty());
}

TEST(EvictingQueueTest, ProduceNBatchWrite) {
    EvictingQueue<TestData, 8> queue;

    // Batch-write 3 items using visitor pattern
    queue.produce_n(3, [](TestData& slot, size_t i) {
        slot.seq = static_cast<uint32_t>(i + 10);
    });

    // Latest should be the last written (index 2, seq=12)
    auto out = queue.try_pop_latest();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->seq, 12u);
}

TEST(EvictingQueueTest, PushNBatchWriteFromSpan) {
    EvictingQueue<TestData, 8> queue;

    TestData items[3] = {};
    items[0].seq = 1;
    items[1].seq = 2;
    items[2].seq = 3;
    queue.push_n(std::span<const TestData>(items, 3));

    auto out = queue.try_pop_latest();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->seq, 3u);
}

// ===========================================================================
// Timed operations
// ===========================================================================

TEST(EvictingQueueTimed, TryPopLatestForSucceedsWithData) {
    EvictingQueue<TestData, 4> queue;
    TestData d{.seq = 42};
    queue.push(d);
    auto val = queue.try_pop_latest_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(val->seq, 42u);
}

TEST(EvictingQueueTimed, TryPopLatestForTimesOutOnEmpty) {
    EvictingQueue<TestData, 4> queue;
    auto start = std::chrono::steady_clock::now();
    auto val = queue.try_pop_latest_for(std::chrono::milliseconds(20));
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_FALSE(val.has_value());
    EXPECT_GE(elapsed, std::chrono::milliseconds(15));
}

TEST(EvictingQueueTimed, TryPopLatestForSucceedsAfterProducerWrites) {
    EvictingQueue<TestData, 4> queue;

    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        TestData d{.seq = 88};
        queue.push(d);
    });

    TestData out{};
    EXPECT_TRUE(queue.try_pop_latest_for(out, std::chrono::milliseconds(200)));
    EXPECT_EQ(out.seq, 88u);
    producer.join();
}

TEST(EvictingQueueTimed, TryConsumeLatestForVisitorPattern) {
    EvictingQueue<TestData, 4> queue;
    TestData d{.seq = 55};
    queue.push(d);

    uint32_t captured = 0;
    bool ok = queue.try_consume_latest_for(
        [&](const TestData& data) { captured = data.seq; },
        std::chrono::milliseconds(10));
    EXPECT_TRUE(ok);
    EXPECT_EQ(captured, 55u);
}

TEST(EvictingQueueTimed, SingleSlotSpecializationTimedOp) {
    EvictingQueue<TestData, 1> queue;
    TestData d{.seq = 77};
    queue.push(d);
    auto val = queue.try_pop_latest_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(val->seq, 77u);
}

TEST(EvictingQueueTimed, SingleSlotTimesOutOnEmpty) {
    EvictingQueue<TestData, 1> queue;
    auto start = std::chrono::steady_clock::now();
    auto val = queue.try_pop_latest_for(std::chrono::milliseconds(20));
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_FALSE(val.has_value());
    EXPECT_GE(elapsed, std::chrono::milliseconds(15));
}

TEST(EvictingQueueTest, SizeApproxCappedAtCapacity) {
    EvictingQueue<TestData, 2> queue;

    TestData d{};
    // Push 5 entries into capacity-2 queue (overwrites)
    for (int i = 0; i < 5; ++i) {
        d.seq = static_cast<uint32_t>(i);
        queue.push(d);
    }

    // size_approx should be capped at Capacity, not 5
    EXPECT_LE(queue.size_approx(), 2u);
}

// ---------------------------------------------------------------------------
// EvictingQueue<T,1> specialization: size_approx() and empty()
// ---------------------------------------------------------------------------

TEST(EvictingQueueSingleSlot, SizeApproxStartsAtZero) {
    EvictingQueue<TestData, 1> queue;
    EXPECT_EQ(queue.size_approx(), 0u);
    EXPECT_TRUE(queue.empty());
}

TEST(EvictingQueueSingleSlot, SizeApproxReflectsUnread) {
    EvictingQueue<TestData, 1> queue;

    TestData d{};
    d.seq = 42;
    queue.push(d);

    // After push, there's 1 unread entry
    EXPECT_EQ(queue.size_approx(), 1u);
    EXPECT_FALSE(queue.empty());

    // After consuming, back to 0
    auto val = queue.try_pop_latest();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(val->seq, 42u);
    EXPECT_EQ(queue.size_approx(), 0u);
    EXPECT_TRUE(queue.empty());
}

TEST(EvictingQueueSingleSlot, SizeApproxAfterClear) {
    EvictingQueue<TestData, 1> queue;

    TestData d{.seq = 1};
    queue.push(d);
    EXPECT_EQ(queue.size_approx(), 1u);

    queue.clear();
    EXPECT_EQ(queue.size_approx(), 0u);
    EXPECT_TRUE(queue.empty());
}

TEST(EvictingQueueSingleSlot, SizeApproxAfterMultipleOverwrites) {
    EvictingQueue<TestData, 1> queue;

    TestData d{};
    for (uint32_t i = 0; i < 10; ++i) {
        d.seq = i;
        queue.push(d);
    }

    // Still at most 1 unread
    EXPECT_EQ(queue.size_approx(), 1u);
    EXPECT_FALSE(queue.empty());
}

// ===========================================================================
// read_count() — consumer throughput metric
// ===========================================================================

TYPED_TEST(EvictingQueueTest, ReadCountStartsAtZero) {
    TypeParam queue;
    EXPECT_EQ(queue.read_count(), 0u);
}

TYPED_TEST(EvictingQueueTest, ReadCountIncrementsOnSuccessfulRead) {
    TypeParam queue;
    TestData d{};

    d.seq = 1;
    queue.push(d);
    EXPECT_EQ(queue.read_count(), 0u);

    auto res = queue.try_pop_latest();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(queue.read_count(), 1u);

    d.seq = 2;
    queue.push(d);
    res = queue.try_pop_latest();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(queue.read_count(), 2u);
}

TYPED_TEST(EvictingQueueTest, ReadCountUnchangedOnFailedRead) {
    TypeParam queue;

    // Failed reads on empty queue should not change read_count
    auto res = queue.try_pop_latest();
    EXPECT_FALSE(res.has_value());
    EXPECT_EQ(queue.read_count(), 0u);
}

TYPED_TEST(EvictingQueueTest, ReadCountAdvancedByClear) {
    TypeParam queue;
    TestData d{.seq = 1};
    queue.push(d);
    auto res = queue.try_pop_latest();
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(queue.read_count(), 1u);

    queue.clear();
    // clear() conceptually "consumes" all buffered data by advancing the reader
    // position to the writer position, so read_count may increase.
    EXPECT_GE(queue.read_count(), 1u);
}

TYPED_TEST(EvictingQueueTest, ReadCountTracksConsumptionAcrossMultipleReads) {
    TypeParam queue;
    TestData d{};

    for (uint32_t i = 1; i <= 5; ++i) {
        d.seq = i;
        queue.push(d);
        (void)queue.try_pop_latest();
    }

    EXPECT_EQ(queue.read_count(), 5u);
    EXPECT_EQ(queue.write_count(), 5u);
}
