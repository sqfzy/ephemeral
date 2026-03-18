#include <gtest/gtest.h>
#include <array>
#include <print>
#include <thread>

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

// 1. 单线程基本操作
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

// 3. 多线程压力测试：验证不可丢弃性和顺序
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
