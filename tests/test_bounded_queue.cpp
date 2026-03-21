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

// 测试容量为 2, 1024 的队列
using BoundedQueueTypes = ::testing::Types<
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
    constexpr size_t batch_size = 2;  // 使用最小容量兼容的批量大小

    std::thread producer([&]() {
        std::array<BoundedTestData, batch_size> batch{};
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
        std::array<BoundedTestData, batch_size> out{};
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
