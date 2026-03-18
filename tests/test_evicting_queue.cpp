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

// 2. 单线程基本 push, pop_latest 测试
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
