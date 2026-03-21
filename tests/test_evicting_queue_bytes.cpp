#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <numeric>
#include <print>
#include <thread>
#include <vector>
#include <algorithm>

#include "eph/containers/evicting_queue_bytes.hpp"

using eph::containers::EvictingQueueBytes;

// 1. 定义类型化测试夹具，测试容量分别为 1, 4 的队列
template <typename T>
class EvictingQueueBytesTest : public ::testing::Test {};

using QueueTypes = ::testing::Types<
    EvictingQueueBytes<256, 1>,
    EvictingQueueBytes<256, 4>
>;

TYPED_TEST_SUITE(EvictingQueueBytesTest, QueueTypes);

// 2. 单线程基本 try_push, try_pop 测试 (使用 256 字节 payload)
TYPED_TEST(EvictingQueueBytesTest, SingleThreadBasicPushPop) {
    TypeParam queue;
    
    // 构造满载 256 字节的数据，填充 0~255
    std::vector<uint8_t> payload(256);
    std::iota(payload.begin(), payload.end(), 0); 
    
    // 基本推入
    EXPECT_TRUE(queue.try_push(payload));

    // 基本弹出
    std::vector<uint8_t> out_buf(256, 0);
    auto res = queue.try_pop_latest(out_buf);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res.value(), 256);
    
    // 严格验证：整块内存必须与原 payload 完全一致
    EXPECT_TRUE(std::ranges::equal(out_buf, payload));

    // 验证无新数据时返回空
    auto res2 = queue.try_pop_latest(out_buf);
    EXPECT_FALSE(res2.has_value());
}

// 3. 单线程 try_push 发生覆盖后的 try_pop 测试 (使用 256 字节 payload)
TYPED_TEST(EvictingQueueBytesTest, SingleThreadEviction) {
    TypeParam queue;
    const size_t push_count = 10; // 推入次数大于最大测试容量(4)，必然发生数据覆盖驱逐

    // 连续推入触发覆盖，每次推入 256 字节全为 i 的数据
    for (uint8_t i = 0; i < push_count; ++i) {
        std::vector<uint8_t> payload(256, i);
        EXPECT_TRUE(queue.try_push(payload));
    }

    // 弹出，验证结果为最新推入的数据
    std::vector<uint8_t> out_buf(256, 0);
    auto res = queue.try_pop_latest(out_buf);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res.value(), 256);
    
    // 严格验证：期望全部 256 字节均为 push_count - 1
    std::vector<uint8_t> expected_payload(256, push_count - 1);
    EXPECT_TRUE(std::ranges::equal(out_buf, expected_payload));

    // 再次弹出应为空
    auto res2 = queue.try_pop_latest(out_buf);
    EXPECT_FALSE(res2.has_value());
}

// 4. 多线程满负荷推入弹出的压力测试 (使用 256 字节 payload)
TYPED_TEST(EvictingQueueBytesTest, MultiThreadStress) {
    TypeParam queue;
    const uint32_t total_messages = 1'000'000;
    std::atomic<bool> producer_done{false};

    // 生产者线程：全速推入单调递增的整数，每次 256 字节满载
    std::thread producer([&]() {
        std::vector<uint8_t> payload(256); 
        for (uint32_t i = 1; i <= total_messages; ++i) {
            // 前 4 个字节用于存放序列号，以便消费者校验
            std::memcpy(payload.data(), &i, sizeof(uint32_t));
            
            // 严格验证构造：剩余的 252 字节填充与序列号 i 强相关的数据
            // 这样可以确保如果发生并发内存撕裂，后面的字节一定对不上
            for (size_t j = sizeof(uint32_t); j < 256; ++j) {
                payload[j] = static_cast<uint8_t>((i + j) & 0xFF);
            }
            [[maybe_unused]] bool ok = queue.try_push(payload);
        }
        producer_done.store(true, std::memory_order_release);
    });

    uint32_t last_received = 0;
    uint32_t received_count = 0;
    std::vector<uint8_t> out_buf(256, 0);

    auto verify_payload = [&](const std::vector<uint8_t>& buf, uint32_t& current_seq) {
        std::memcpy(&current_seq, buf.data(), sizeof(uint32_t));
        // 验证读取的数据序列必须严格递增，说明没有乱序和旧数据复现
        EXPECT_GT(current_seq, last_received);
        
        // 严格验证 payload 后半部分：检查每个字节是否匹配对应的序列号模式
        bool is_valid = true;
        for (size_t j = sizeof(uint32_t); j < 256; ++j) {
            if (buf[j] != static_cast<uint8_t>((current_seq + j) & 0xFF)) {
                is_valid = false;
                break;
            }
        }
        EXPECT_TRUE(is_valid) << "Data corruption / Torn read detected at sequence: " << current_seq;
        
        last_received = current_seq;
        received_count++;
    };

    // 消费者线程：循环读取最新数据
    std::thread consumer([&]() {
        while (!producer_done.load(std::memory_order_acquire)) {
            auto res = queue.try_pop_latest(out_buf);
            if (res.has_value()) {
                uint32_t current = 0;
                verify_payload(out_buf, current);
            }
        }
        
        // 生产结束后，把队列中最后的残留数据读取完
        while (true) {
            auto res = queue.try_pop_latest(out_buf);
            if (res.has_value()) {
                uint32_t current = 0;
                verify_payload(out_buf, current);
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

// clear() 测试
TEST(EvictingQueueBytesTest, ClearDiscardsData) {
    EvictingQueueBytes<256, 4> queue;

    std::vector<uint8_t> payload(256, 0xAB);
    EXPECT_TRUE(queue.try_push(payload));

    queue.clear();

    // clear 后不应读到旧数据
    std::vector<uint8_t> out(256, 0);
    auto res = queue.try_pop_latest(out);
    EXPECT_FALSE(res.has_value());

    // 写入新数据后应正常读取
    std::vector<uint8_t> new_payload(256, 0xCD);
    EXPECT_TRUE(queue.try_push(new_payload));
    res = queue.try_pop_latest(out);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(out[0], 0xCD);
}

TEST(EvictingQueueBytesTest, TotalPushedStartsAtZero) {
    EvictingQueueBytes<256, 4> queue;
    EXPECT_EQ(queue.total_pushed(), 0u);
}

TEST(EvictingQueueBytesTest, TotalPushedIncrementsOnPush) {
    EvictingQueueBytes<256, 4> queue;
    std::vector<uint8_t> payload(64, 0xAA);

    for (uint64_t i = 1; i <= 10; ++i) {
        EXPECT_TRUE(queue.try_push(payload));
        EXPECT_EQ(queue.total_pushed(), i);
    }
}

TEST(EvictingQueueBytesTest, TotalPushedNotIncrementedOnOversizedPayload) {
    EvictingQueueBytes<64, 4> queue;
    std::vector<uint8_t> oversized(65, 0xBB);

    EXPECT_FALSE(queue.try_push(oversized));
    EXPECT_EQ(queue.total_pushed(), 0u);
}

TEST(EvictingQueueBytesTest, TotalPushedPersistsAcrossClear) {
    EvictingQueueBytes<256, 4> queue;
    std::vector<uint8_t> payload(32, 0xCC);

    EXPECT_TRUE(queue.try_push(payload));
    EXPECT_TRUE(queue.try_push(payload));
    EXPECT_EQ(queue.total_pushed(), 2u);

    queue.clear();
    EXPECT_EQ(queue.total_pushed(), 2u);

    EXPECT_TRUE(queue.try_push(payload));
    EXPECT_EQ(queue.total_pushed(), 3u);
}

// ===========================================================================
// Timed operations
// ===========================================================================

TEST(EvictingQueueBytesTimed, TryPopLatestForSucceeds) {
    EvictingQueueBytes<64, 4> queue;
    std::array<uint8_t, 8> payload{};
    payload.fill(0xBB);
    ASSERT_TRUE(queue.try_push(payload));

    std::array<uint8_t, 64> out{};
    auto len = queue.try_pop_latest_for(out, std::chrono::milliseconds(10));
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(*len, 8u);
    EXPECT_EQ(out[0], 0xBB);
}

TEST(EvictingQueueBytesTimed, TryPopLatestForTimesOutOnEmpty) {
    EvictingQueueBytes<64, 4> queue;
    std::array<uint8_t, 64> out{};
    auto start = std::chrono::steady_clock::now();
    auto len = queue.try_pop_latest_for(out, std::chrono::milliseconds(20));
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_FALSE(len.has_value());
    EXPECT_GE(elapsed, std::chrono::milliseconds(15));
}

TEST(EvictingQueueBytesTimed, TryConsumeLatestForSucceeds) {
    EvictingQueueBytes<64, 4> queue;
    std::array<uint8_t, 4> payload = {10, 20, 30, 40};
    ASSERT_TRUE(queue.try_push(payload));

    size_t captured_len = 0;
    bool ok = queue.try_consume_latest_for([&](std::span<const uint8_t> data) {
        captured_len = data.size();
    }, std::chrono::milliseconds(10));
    EXPECT_TRUE(ok);
    EXPECT_EQ(captured_len, 4u);
}

TEST(EvictingQueueBytesTimed, TryPopLatestForSucceedsAfterWrite) {
    EvictingQueueBytes<64, 4> queue;

    std::thread writer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::array<uint8_t, 4> payload = {0xCC, 0xDD, 0xEE, 0xFF};
        (void)queue.try_push(payload);
    });

    std::array<uint8_t, 64> out{};
    auto len = queue.try_pop_latest_for(out, std::chrono::milliseconds(200));
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(*len, 4u);
    EXPECT_EQ(out[0], 0xCC);
    writer.join();
}

// ===========================================================================
// Timed _wts variants
// ===========================================================================

TEST(EvictingQueueBytesTimed, TryConsumeLatestWtsForSuccess) {
    EvictingQueueBytes<64, 4> queue;
    std::array<uint8_t, 4> payload = {1, 2, 3, 4};
    ASSERT_TRUE(queue.try_push_wts(payload, 999));

    uint64_t ts = 0;
    uint32_t discarded = 0;
    size_t captured_len = 0;

    bool ok = queue.try_consume_latest_wts_for(
        [&](std::span<const uint8_t> data, uint64_t t, uint32_t d) {
            captured_len = data.size();
            ts = t;
            discarded = d;
        },
        std::chrono::milliseconds(10));
    EXPECT_TRUE(ok);
    EXPECT_EQ(captured_len, 4u);
    EXPECT_EQ(ts, 999u);
    EXPECT_EQ(discarded, 0u);
}

TEST(EvictingQueueBytesTimed, TryConsumeLatestWtsForTimeout) {
    EvictingQueueBytes<64, 4> queue;

    bool ok = queue.try_consume_latest_wts_for(
        [](std::span<const uint8_t>, uint64_t, uint32_t) {},
        std::chrono::milliseconds(5));
    EXPECT_FALSE(ok);
}

TEST(EvictingQueueBytesTimed, TryPopLatestWtsForSuccess) {
    EvictingQueueBytes<64, 4> queue;
    std::array<uint8_t, 3> payload = {0xAA, 0xBB, 0xCC};
    ASSERT_TRUE(queue.try_push_wts(payload, 12345));

    std::array<uint8_t, 64> out{};
    uint64_t ts = 0;
    uint32_t discarded = 0;

    auto len = queue.try_pop_latest_wts_for(
        out, ts, discarded, std::chrono::milliseconds(10));
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(*len, 3u);
    EXPECT_EQ(out[0], 0xAA);
    EXPECT_EQ(out[1], 0xBB);
    EXPECT_EQ(out[2], 0xCC);
    EXPECT_EQ(ts, 12345u);
    EXPECT_EQ(discarded, 0u);
}

TEST(EvictingQueueBytesTimed, TryPopLatestWtsForTimeout) {
    EvictingQueueBytes<64, 4> queue;

    std::array<uint8_t, 64> out{};
    uint64_t ts = 0;
    uint32_t discarded = 0;

    auto len = queue.try_pop_latest_wts_for(
        out, ts, discarded, std::chrono::milliseconds(5));
    EXPECT_FALSE(len.has_value());
}

TEST(EvictingQueueBytesTimed, TryPopLatestWtsForTracksDiscarded) {
    EvictingQueueBytes<64, 2> queue;

    // Push 3 messages into capacity-2 queue (first gets evicted)
    std::array<uint8_t, 2> p1 = {1, 1};
    std::array<uint8_t, 2> p2 = {2, 2};
    std::array<uint8_t, 2> p3 = {3, 3};
    ASSERT_TRUE(queue.try_push_wts(p1, 100));
    ASSERT_TRUE(queue.try_push_wts(p2, 200));

    // Read once to establish baseline pop id
    std::array<uint8_t, 64> out{};
    uint64_t ts = 0;
    uint32_t discarded = 0;
    auto len = queue.try_pop_latest_wts_for(
        out, ts, discarded, std::chrono::milliseconds(10));
    ASSERT_TRUE(len.has_value());

    // Push more — evicts old data
    ASSERT_TRUE(queue.try_push_wts(p3, 300));
    ASSERT_TRUE(queue.try_push_wts(p1, 400));
    ASSERT_TRUE(queue.try_push_wts(p2, 500));

    len = queue.try_pop_latest_wts_for(
        out, ts, discarded, std::chrono::milliseconds(10));
    ASSERT_TRUE(len.has_value());
    // Some messages were discarded between reads
    EXPECT_GT(discarded, 0u);
}
