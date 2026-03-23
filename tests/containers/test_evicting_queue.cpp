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

// ============================================================================
// try_peek_latest — non-consuming read
// ============================================================================

TYPED_TEST(EvictingQueueTest, TryPeekLatestOnEmptyReturnsFalse) {
    TypeParam queue;
    TestData out{};
    EXPECT_FALSE(queue.try_peek_latest(out));
}

TYPED_TEST(EvictingQueueTest, TryPeekLatestOptionalOnEmptyReturnsNullopt) {
    TypeParam queue;
    EXPECT_FALSE(queue.try_peek_latest().has_value());
}

TYPED_TEST(EvictingQueueTest, TryPeekLatestReadsWithoutConsuming) {
    TypeParam queue;
    TestData d{.seq = 42};
    queue.push(d);

    // Peek should succeed and return the data
    TestData peeked{};
    EXPECT_TRUE(queue.try_peek_latest(peeked));
    EXPECT_EQ(peeked.seq, 42u);

    // Subsequent consume should still return the same data (not consumed by peek)
    TestData consumed{};
    EXPECT_TRUE(queue.try_pop_latest(consumed));
    EXPECT_EQ(consumed.seq, 42u);
}

TYPED_TEST(EvictingQueueTest, TryPeekLatestRepeatedReturnsStaleAfterConsume) {
    TypeParam queue;
    TestData d{.seq = 1};
    queue.push(d);

    // Peek twice — both should return the same value
    auto v1 = queue.try_peek_latest();
    auto v2 = queue.try_peek_latest();
    ASSERT_TRUE(v1.has_value());
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(v1->seq, 1u);
    EXPECT_EQ(v2->seq, 1u);

    // Consume
    (void)queue.try_pop_latest();

    // Peek should now fail (no new data)
    EXPECT_FALSE(queue.try_peek_latest().has_value());
}

TYPED_TEST(EvictingQueueTest, TryPeekLatestVisitorPatternZeroCopy) {
    TypeParam queue;
    TestData d{.seq = 77};
    queue.push(d);

    uint32_t captured_seq = 0;
    bool ok = queue.try_peek_latest([&](const TestData& data) {
        captured_seq = data.seq;
    });
    EXPECT_TRUE(ok);
    EXPECT_EQ(captured_seq, 77u);

    // Element should still be consumable after visitor peek
    TestData consumed{};
    EXPECT_TRUE(queue.try_pop_latest(consumed));
    EXPECT_EQ(consumed.seq, 77u);
}

TYPED_TEST(EvictingQueueTest, TryPeekLatestSeesNewDataAfterPush) {
    TypeParam queue;
    TestData d1{.seq = 10};
    queue.push(d1);

    auto v1 = queue.try_peek_latest();
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(v1->seq, 10u);

    // Consume, then push new data
    (void)queue.try_pop_latest();

    TestData d2{.seq = 20};
    queue.push(d2);

    auto v2 = queue.try_peek_latest();
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(v2->seq, 20u);
}

TYPED_TEST(EvictingQueueTest, TryPeekLatestAfterClearAndPush) {
    TypeParam queue;
    TestData d{.seq = 1};
    queue.push(d);

    queue.clear();
    EXPECT_FALSE(queue.try_peek_latest().has_value());

    d.seq = 2;
    queue.push(d);
    auto v = queue.try_peek_latest();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->seq, 2u);
}

TYPED_TEST(EvictingQueueTest, TryPeekLatestAllOverloadsConsistent) {
    TypeParam queue;
    TestData d{.seq = 55};
    queue.push(d);

    // Visitor overload
    uint32_t visitor_seq = 0;
    bool ok1 = queue.try_peek_latest([&](const TestData& data) {
        visitor_seq = data.seq;
    });

    // Value-copy overload
    TestData ref_out{};
    bool ok2 = queue.try_peek_latest(ref_out);

    // Optional overload
    auto opt = queue.try_peek_latest();

    EXPECT_TRUE(ok1);
    EXPECT_TRUE(ok2);
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(visitor_seq, 55u);
    EXPECT_EQ(ref_out.seq, 55u);
    EXPECT_EQ(opt->seq, 55u);
}

// Concurrent stress test: writer pushes rapidly while reader peeks
TEST(EvictingQueueStress, ConcurrentPeekNeverReturnsTornData) {
    // Use Capacity=4 for the stress test
    EvictingQueue<TestData, 4> queue;
    constexpr uint32_t kIterations = 500'000;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> peek_successes{0};
    std::atomic<uint64_t> peek_failures{0};
    std::atomic<uint64_t> torn_reads{0};

    // Writer thread: push items with seq = i, payload filled with i
    std::thread writer([&] {
        for (uint32_t i = 1; i <= kIterations; ++i) {
            queue.produce([&](TestData& slot) {
                slot.seq = i;
                slot.payload.fill(i);
            });
        }
        stop.store(true, std::memory_order_release);
    });

    // Reader thread: peek and verify data consistency
    std::thread reader([&] {
        while (!stop.load(std::memory_order_acquire) ||
               queue.try_peek_latest().has_value()) {
            TestData peeked{};
            if (queue.try_peek_latest(peeked)) {
                // Verify consistency: all payload values must match seq
                bool consistent = true;
                for (auto v : peeked.payload) {
                    if (v != peeked.seq) {
                        consistent = false;
                        break;
                    }
                }
                if (consistent) {
                    peek_successes.fetch_add(1, std::memory_order_relaxed);
                } else {
                    torn_reads.fetch_add(1, std::memory_order_relaxed);
                }
                // Consume to allow seeing new data
                (void)queue.try_pop_latest();
            } else {
                peek_failures.fetch_add(1, std::memory_order_relaxed);
                eph::utils::cpu_relax();
            }
        }
    });

    writer.join();
    reader.join();

    // CRITICAL: No torn reads should ever be observed
    EXPECT_EQ(torn_reads.load(), 0u)
        << "Detected torn reads in concurrent try_peek_latest!";
    // Should have had some successes
    EXPECT_GT(peek_successes.load(), 0u);
}

// ===========================================================================
// try_peek_latest_for — non-consuming read with timeout
// ===========================================================================

TYPED_TEST(EvictingQueueTest, try_peek_latest_for_returns_false_on_empty_queue_timeout) {
    TypeParam queue;
    TestData out{};
    // Very short timeout — should return false quickly on empty queue
    EXPECT_FALSE(queue.try_peek_latest_for(out, std::chrono::microseconds(10)));
}

TYPED_TEST(EvictingQueueTest, try_peek_latest_for_returns_data_when_available) {
    TypeParam queue;
    TestData d{.seq = 42};
    queue.push(d);

    TestData out{};
    EXPECT_TRUE(queue.try_peek_latest_for(out, std::chrono::milliseconds(10)));
    EXPECT_EQ(out.seq, 42u);
}

TYPED_TEST(EvictingQueueTest, try_peek_latest_for_does_not_consume) {
    TypeParam queue;
    TestData d{.seq = 99};
    queue.push(d);

    // Peek twice — both should succeed with same data
    auto v1 = queue.try_peek_latest_for(std::chrono::milliseconds(10));
    auto v2 = queue.try_peek_latest_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(v1.has_value());
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(v1->seq, 99u);
    EXPECT_EQ(v2->seq, 99u);

    // Consume should still work after peeks
    auto consumed = queue.try_pop_latest();
    ASSERT_TRUE(consumed.has_value());
    EXPECT_EQ(consumed->seq, 99u);
}

TYPED_TEST(EvictingQueueTest, try_peek_latest_for_visitor_receives_data_before_timeout) {
    TypeParam queue;
    TestData d{.seq = 7};
    queue.push(d);

    uint32_t seen_seq = 0;
    bool ok = queue.try_peek_latest_for(
        [&](const TestData& data) { seen_seq = data.seq; },
        std::chrono::milliseconds(10));
    EXPECT_TRUE(ok);
    EXPECT_EQ(seen_seq, 7u);
}

TYPED_TEST(EvictingQueueTest, try_peek_latest_for_visitor_returns_false_on_timeout) {
    TypeParam queue;
    uint32_t seen_seq = 0;
    bool ok = queue.try_peek_latest_for(
        [&](const TestData& data) { seen_seq = data.seq; },
        std::chrono::microseconds(10));
    EXPECT_FALSE(ok);
    EXPECT_EQ(seen_seq, 0u);
}

TEST(EvictingQueueTest_PeekFor, delayed_write_unblocks_peek) {
    EvictingQueue<TestData, 4> queue;

    std::thread writer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        queue.push(TestData{.seq = 123});
    });

    TestData out{};
    bool ok = queue.try_peek_latest_for(out, std::chrono::milliseconds(100));
    EXPECT_TRUE(ok);
    EXPECT_EQ(out.seq, 123u);

    writer.join();
}

TYPED_TEST(EvictingQueueTest, try_peek_latest_for_zero_timeout_on_empty) {
    TypeParam queue;
    TestData out{};
    // Zero timeout should return false immediately without spinning
    EXPECT_FALSE(queue.try_peek_latest_for(out, std::chrono::nanoseconds(0)));
}

TYPED_TEST(EvictingQueueTest, try_peek_latest_for_does_not_advance_read_count) {
    TypeParam queue;
    TestData d{.seq = 55};
    queue.push(d);

    // Peek should not advance read counter
    TestData out{};
    EXPECT_TRUE(queue.try_peek_latest_for(out, std::chrono::milliseconds(1)));
    EXPECT_EQ(out.seq, 55u);
    EXPECT_EQ(queue.read_count(), 0u);

    // Consume should still get the same data
    auto consumed = queue.try_pop_latest();
    ASSERT_TRUE(consumed.has_value());
    EXPECT_EQ(consumed->seq, 55u);
    EXPECT_EQ(queue.read_count(), 1u);
}

TYPED_TEST(EvictingQueueTest, try_peek_latest_for_sees_newer_write_after_push) {
    TypeParam queue;
    queue.push(TestData{.seq = 1});

    // Peek sees first data
    auto v1 = queue.try_peek_latest_for(std::chrono::milliseconds(1));
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(v1->seq, 1u);

    // Push new data (without consuming first)
    queue.push(TestData{.seq = 2});

    // Peek now sees the newer data
    auto v2 = queue.try_peek_latest_for(std::chrono::milliseconds(1));
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(v2->seq, 2u);
}

// ===========================================================================
// overwrite_count_approx() — data loss monitoring
// ===========================================================================

TYPED_TEST(EvictingQueueTest, OverwriteCountStartsAtZero) {
    TypeParam queue;
    EXPECT_EQ(queue.overwrite_count_approx(), 0u);
}

TYPED_TEST(EvictingQueueTest, OverwriteCountZeroWhenReaderKeepsUp) {
    TypeParam queue;
    TestData d{};

    for (uint32_t i = 1; i <= 5; ++i) {
        d.seq = i;
        queue.push(d);
        // Consume immediately — reader keeps up, no overwrites
        auto val = queue.try_pop_latest();
        ASSERT_TRUE(val.has_value());
    }
    EXPECT_EQ(queue.overwrite_count_approx(), 0u);
}

TEST(EvictingQueueOverwrite, MultiSlotOverwriteTracking) {
    EvictingQueue<TestData, 4> queue;
    TestData d{};

    // Write 4 entries (fills capacity, no overwrite yet)
    for (uint32_t i = 0; i < 4; ++i) {
        d.seq = i;
        queue.push(d);
    }
    EXPECT_EQ(queue.overwrite_count_approx(), 0u);

    // Write 3 more without reading — overwrites 3 entries
    for (uint32_t i = 4; i < 7; ++i) {
        d.seq = i;
        queue.push(d);
    }
    EXPECT_EQ(queue.overwrite_count_approx(), 3u);
}

TEST(EvictingQueueOverwrite, SingleSlotOverwriteAccuracy) {
    EvictingQueue<TestData, 1> queue;
    TestData d{};

    // Write 1 entry — no overwrite (buffer holds it)
    d.seq = 1;
    queue.push(d);
    EXPECT_EQ(queue.overwrite_count_approx(), 0u);

    // Consume the entry
    auto val = queue.try_pop_latest();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(queue.overwrite_count_approx(), 0u);

    // Write 5 entries without reading — 4 overwrites (5 writes, buffer holds 1)
    for (uint32_t i = 2; i <= 6; ++i) {
        d.seq = i;
        queue.push(d);
    }
    // read_count = 1 (last consumed was write #1), write_count = 6
    // overwrite = 6 - 1 - 1 = 4
    EXPECT_EQ(queue.write_count(), 6u);
    EXPECT_EQ(queue.read_count(), 1u);
    EXPECT_EQ(queue.overwrite_count_approx(), 4u);

    // Consume latest (jumps reader to write #6)
    val = queue.try_pop_latest();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(val->seq, 6u);
    // read_count = 6, write_count = 6, overwrite = max(0, 6 - 6 - 1) = 0
    EXPECT_EQ(queue.overwrite_count_approx(), 0u);
}

TEST(EvictingQueueOverwrite, SingleSlotNoOverwriteWhenAllConsumed) {
    EvictingQueue<TestData, 1> queue;
    TestData d{};

    // Write and consume 5 entries — no overwrites
    for (uint32_t i = 1; i <= 5; ++i) {
        d.seq = i;
        queue.push(d);
        auto val = queue.try_pop_latest();
        ASSERT_TRUE(val.has_value());
    }
    EXPECT_EQ(queue.overwrite_count_approx(), 0u);
    EXPECT_EQ(queue.write_count(), 5u);
    EXPECT_EQ(queue.read_count(), 5u);
}

TEST(EvictingQueueOverwrite, SingleSlotAllOverwritten) {
    EvictingQueue<TestData, 1> queue;
    TestData d{};

    // Write 10 entries without ever reading
    for (uint32_t i = 1; i <= 10; ++i) {
        d.seq = i;
        queue.push(d);
    }
    // 10 writes, 0 reads, 1 buffered → 9 overwrites
    EXPECT_EQ(queue.overwrite_count_approx(), 9u);
    EXPECT_EQ(queue.write_count(), 10u);
    EXPECT_EQ(queue.read_count(), 0u);
}

// ---------------------------------------------------------------------------
// Stats snapshot
// ---------------------------------------------------------------------------

TEST(EvictingQueueStats, MultiSlotStatsSnapshot) {
    EvictingQueue<TestData, 4> queue;
    TestData d{};

    for (uint32_t i = 1; i <= 6; ++i) {
        d.seq = i;
        queue.push(d);
    }
    // Read 2 entries
    queue.try_consume_latest([](const TestData&) {});
    queue.try_consume_latest([](const TestData&) {});

    auto s = queue.stats();
    EXPECT_EQ(s.total_pushed, 6u);
    EXPECT_EQ(s.capacity, 4u);
}

TEST(EvictingQueueStats, SingleSlotStatsSnapshot) {
    EvictingQueue<TestData, 1> queue;
    TestData d{.seq = 42};
    queue.push(d);

    auto s = queue.stats();
    EXPECT_EQ(s.total_pushed, 1u);
    EXPECT_EQ(s.total_popped, 0u);
    EXPECT_EQ(s.overwritten, 0u);
    EXPECT_EQ(s.capacity, 1u);
}

TEST(EvictingQueueStats, dump_contains_key_info) {
    EvictingQueue<TestData, 4> queue;
    queue.push(TestData{.seq = 1});
    queue.push(TestData{.seq = 2});
    queue.push(TestData{.seq = 3});

    auto s = queue.stats();
    auto dump = s.dump();

    EXPECT_NE(dump.find("EvictingQueue"), std::string::npos);
    EXPECT_NE(dump.find("capacity: 4"), std::string::npos);
    EXPECT_NE(dump.find("total_pushed: 3"), std::string::npos);
}

TEST(EvictingQueueStats, to_json_is_valid_format) {
    EvictingQueue<TestData, 4> queue;
    queue.push(TestData{.seq = 1});

    auto s = queue.stats();
    auto json = s.to_json();

    EXPECT_NE(json.find("\"capacity\":4"), std::string::npos);
    EXPECT_NE(json.find("\"total_pushed\":1"), std::string::npos);
    EXPECT_NE(json.find("\"overwritten\":"), std::string::npos);
}

TEST(EvictingQueueStats, single_slot_dump_works) {
    EvictingQueue<TestData, 1> queue;
    queue.push(TestData{.seq = 1});
    queue.push(TestData{.seq = 2});  // overwrites

    auto s = queue.stats();
    auto dump = s.dump();

    EXPECT_NE(dump.find("EvictingQueue"), std::string::npos);
    EXPECT_NE(dump.find("capacity: 1"), std::string::npos);
    EXPECT_NE(dump.find("total_pushed: 2"), std::string::npos);
}

TEST(EvictingQueueStats, dump_zero_pushes_shows_zero_loss) {
    EvictingQueue<TestData, 4> queue;
    auto s = queue.stats();
    auto dump = s.dump();
    EXPECT_NE(dump.find("total_pushed: 0"), std::string::npos);
    EXPECT_NE(dump.find("0.0% loss"), std::string::npos);
}

TEST(EvictingQueueStats, to_json_single_slot_has_all_fields) {
    EvictingQueue<TestData, 1> queue;
    queue.push(TestData{.seq = 1});
    auto s = queue.stats();
    auto json = s.to_json();

    EXPECT_NE(json.find("\"capacity\":1"), std::string::npos);
    EXPECT_NE(json.find("\"total_pushed\":1"), std::string::npos);
    EXPECT_NE(json.find("\"total_popped\":"), std::string::npos);
    EXPECT_NE(json.find("\"overwritten\":"), std::string::npos);
    EXPECT_NE(json.find("\"current_size\":"), std::string::npos);
}

TEST(EvictingQueueStats, operator_minus_computes_interval_delta) {
    EvictingQueue<TestData, 4> queue;
    queue.push(TestData{.seq = 1});
    queue.push(TestData{.seq = 2});
    TestData out{};
    queue.try_pop_latest(out);  // read up to global_index=2
    auto s1 = queue.stats();

    queue.push(TestData{.seq = 3});
    queue.try_pop_latest(out);  // read up to global_index=3
    auto s2 = queue.stats();

    auto delta = s2 - s1;
    EXPECT_EQ(delta.total_pushed, 1u);  // 1 new push
    EXPECT_EQ(delta.total_popped, 1u);  // reader advanced by 1
    EXPECT_EQ(delta.current_size, s2.current_size);
    EXPECT_EQ(delta.capacity, 4u);
}

TEST(EvictingQueueStats, operator_eq_compares_all_fields) {
    EvictingQueue<TestData, 4> queue;
    auto s1 = queue.stats();
    auto s2 = queue.stats();
    EXPECT_EQ(s1, s2);

    queue.push(TestData{.seq = 1});
    auto s3 = queue.stats();
    EXPECT_NE(s1, s3);
}

TEST(EvictingQueueStats, single_slot_operator_minus_works) {
    EvictingQueue<TestData, 1> queue;
    queue.push(TestData{.seq = 1});
    auto s1 = queue.stats();

    queue.push(TestData{.seq = 2});  // overwrites
    queue.push(TestData{.seq = 3});  // overwrites again
    auto s2 = queue.stats();

    auto delta = s2 - s1;
    EXPECT_EQ(delta.total_pushed, 2u);
    EXPECT_GE(delta.overwritten, 1u);
}

// ===========================================================================
// Timeout variants: try_peek_latest_for / try_consume_latest_for / try_pop_latest_for
// ===========================================================================

using namespace std::chrono_literals;

// --- try_peek_latest_for ---

TEST(EvictingQueueTimeout, peek_latest_for_returns_immediately_when_data_available) {
    EvictingQueue<TestData, 4> queue;
    queue.push(TestData{.seq = 42});

    TestData out{};
    EXPECT_TRUE(queue.try_peek_latest_for(out, 1s));
    EXPECT_EQ(out.seq, 42u);
}

TEST(EvictingQueueTimeout, peek_latest_for_visitor_returns_immediately_when_data_available) {
    EvictingQueue<TestData, 4> queue;
    queue.push(TestData{.seq = 99});

    uint32_t seen = 0;
    EXPECT_TRUE(queue.try_peek_latest_for(
        [&seen](const TestData& d) { seen = d.seq; }, 1s));
    EXPECT_EQ(seen, 99u);
}

TEST(EvictingQueueTimeout, peek_latest_for_optional_returns_immediately_when_data_available) {
    EvictingQueue<TestData, 4> queue;
    queue.push(TestData{.seq = 55});

    auto result = queue.try_peek_latest_for(1s);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->seq, 55u);
}

TEST(EvictingQueueTimeout, peek_latest_for_times_out_on_empty_queue) {
    EvictingQueue<TestData, 4> queue;

    TestData out{};
    auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(queue.try_peek_latest_for(out, 5ms));
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, 5ms);
}

TEST(EvictingQueueTimeout, peek_latest_for_optional_returns_nullopt_on_empty) {
    EvictingQueue<TestData, 4> queue;
    auto result = queue.try_peek_latest_for(5ms);
    EXPECT_FALSE(result.has_value());
}

TEST(EvictingQueueTimeout, peek_latest_for_does_not_consume_data) {
    EvictingQueue<TestData, 4> queue;
    queue.push(TestData{.seq = 10});

    // Peek should not consume
    TestData out{};
    EXPECT_TRUE(queue.try_peek_latest_for(out, 1s));
    EXPECT_EQ(out.seq, 10u);

    // Data should still be available for consume
    TestData out2{};
    EXPECT_TRUE(queue.try_pop_latest(out2));
    EXPECT_EQ(out2.seq, 10u);
}

TEST(EvictingQueueTimeout, peek_latest_for_zero_timeout_acts_like_try) {
    EvictingQueue<TestData, 4> queue;
    TestData out{};
    EXPECT_FALSE(queue.try_peek_latest_for(out, 0ms));

    queue.push(TestData{.seq = 1});
    EXPECT_TRUE(queue.try_peek_latest_for(out, 0ms));
    EXPECT_EQ(out.seq, 1u);
}

// --- try_consume_latest_for ---

TEST(EvictingQueueTimeout, consume_latest_for_returns_immediately_when_data_available) {
    EvictingQueue<TestData, 4> queue;
    queue.push(TestData{.seq = 77});

    uint32_t seen = 0;
    EXPECT_TRUE(queue.try_consume_latest_for(
        [&seen](const TestData& d) { seen = d.seq; }, 1s));
    EXPECT_EQ(seen, 77u);
}

TEST(EvictingQueueTimeout, consume_latest_for_times_out_on_empty_queue) {
    EvictingQueue<TestData, 4> queue;

    uint32_t seen = 0;
    auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(queue.try_consume_latest_for(
        [&seen](const TestData& d) { seen = d.seq; }, 5ms));
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, 5ms);
    EXPECT_EQ(seen, 0u);
}

TEST(EvictingQueueTimeout, consume_latest_for_consumes_data) {
    EvictingQueue<TestData, 4> queue;
    queue.push(TestData{.seq = 33});

    uint32_t seen = 0;
    EXPECT_TRUE(queue.try_consume_latest_for(
        [&seen](const TestData& d) { seen = d.seq; }, 1s));
    EXPECT_EQ(seen, 33u);

    // Second consume should fail (data consumed)
    EXPECT_FALSE(queue.try_consume_latest_for(
        [](const TestData&) {}, 5ms));
}

TEST(EvictingQueueTimeout, consume_latest_for_wakes_when_data_arrives) {
    EvictingQueue<TestData, 4> queue;

    std::atomic<uint32_t> seen{0};
    std::thread consumer([&] {
        queue.try_consume_latest_for(
            [&seen](const TestData& d) { seen.store(d.seq); }, 500ms);
    });

    std::this_thread::sleep_for(10ms);
    queue.push(TestData{.seq = 88});
    consumer.join();
    EXPECT_EQ(seen.load(), 88u);
}

// --- try_pop_latest_for ---

TEST(EvictingQueueTimeout, pop_latest_for_ref_returns_immediately_when_data_available) {
    EvictingQueue<TestData, 4> queue;
    queue.push(TestData{.seq = 44});

    TestData out{};
    EXPECT_TRUE(queue.try_pop_latest_for(out, 1s));
    EXPECT_EQ(out.seq, 44u);
}

TEST(EvictingQueueTimeout, pop_latest_for_optional_returns_immediately_when_data_available) {
    EvictingQueue<TestData, 4> queue;
    queue.push(TestData{.seq = 66});

    auto result = queue.try_pop_latest_for(1s);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->seq, 66u);
}

TEST(EvictingQueueTimeout, pop_latest_for_ref_times_out_on_empty) {
    EvictingQueue<TestData, 4> queue;
    TestData out{};
    EXPECT_FALSE(queue.try_pop_latest_for(out, 5ms));
}

TEST(EvictingQueueTimeout, pop_latest_for_optional_returns_nullopt_on_empty) {
    EvictingQueue<TestData, 4> queue;
    auto result = queue.try_pop_latest_for(5ms);
    EXPECT_FALSE(result.has_value());
}

TEST(EvictingQueueTimeout, pop_latest_for_consumes_data) {
    EvictingQueue<TestData, 4> queue;
    queue.push(TestData{.seq = 11});

    auto r1 = queue.try_pop_latest_for(1s);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->seq, 11u);

    // Should be empty now
    auto r2 = queue.try_pop_latest_for(5ms);
    EXPECT_FALSE(r2.has_value());
}

TEST(EvictingQueueTimeout, pop_latest_for_zero_timeout_acts_like_try) {
    EvictingQueue<TestData, 4> queue;
    auto r = queue.try_pop_latest_for(0ms);
    EXPECT_FALSE(r.has_value());

    queue.push(TestData{.seq = 22});
    r = queue.try_pop_latest_for(0ms);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->seq, 22u);
}

// --- Single-slot timeout behavior ---

TEST(EvictingQueueTimeout, single_slot_peek_for_works) {
    EvictingQueue<TestData, 1> queue;
    queue.push(TestData{.seq = 100});

    auto result = queue.try_peek_latest_for(1s);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->seq, 100u);
}

TEST(EvictingQueueTimeout, single_slot_pop_for_overwrite_gets_latest) {
    EvictingQueue<TestData, 1> queue;
    queue.push(TestData{.seq = 1});
    queue.push(TestData{.seq = 2});  // overwrites

    auto result = queue.try_pop_latest_for(1s);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->seq, 2u);
}
