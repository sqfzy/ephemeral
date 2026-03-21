#include <gtest/gtest.h>

#include <span>
#include <thread>
#include <vector>

#include "eph/containers/bounded_queue_bytes.hpp"

using eph::containers::BoundedQueueBytes;

template <typename T>
class BoundedQueueBytesTest : public ::testing::Test {};

using ByteQueueTypes =
    ::testing::Types<BoundedQueueBytes<256, 16>, BoundedQueueBytes<512, 128> >;

TYPED_TEST_SUITE(BoundedQueueBytesTest, ByteQueueTypes);

// 1. 基本字节流读写与时间戳测试
TYPED_TEST(BoundedQueueBytesTest, BasicBytesAndTS) {
    TypeParam queue;
    uint8_t raw_data[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02};
    uint64_t input_ts = 123456789;

    EXPECT_TRUE(queue.try_push_wts(raw_data, input_ts));

    uint8_t out_buf[16];
    uint64_t out_ts = 0;
    auto bytes_read = queue.try_pop_wts(out_buf, out_ts);

    ASSERT_TRUE(bytes_read.has_value());
    EXPECT_EQ(*bytes_read, sizeof(raw_data));
    EXPECT_EQ(out_ts, input_ts);
    EXPECT_EQ(std::memcmp(raw_data, out_buf, sizeof(raw_data)), 0);
}

// 2. 溢出检查 (Payload > MaxDataSize)
TYPED_TEST(BoundedQueueBytesTest, OverflowProtection) {
    TypeParam queue;
    std::vector<uint8_t> giant_payload(1024, 0xFF);

    // 尝试推入超过 MaxDataSize 的数据应返回 false
    EXPECT_FALSE(queue.try_push(giant_payload));
}

// 3. 多线程生产者-消费者压力测试 (字节流模式)
TYPED_TEST(BoundedQueueBytesTest, MultiThreadByteStress) {
    TypeParam queue;
    const uint32_t total_messages = 1'000'000;

    std::thread producer([&]() {
        for (uint32_t i = 1; i <= total_messages; ++i) {
            uint8_t buf[64];
            // 写入序列号和重复模式
            std::memcpy(buf, &i, sizeof(i));
            std::memset(buf + sizeof(i), static_cast<uint8_t>(i & 0xFF), 60);

            // 阻塞推入（返回值仅表示 payload 是否过大，此处已知不会）
            [[maybe_unused]] bool ok = queue.push_wts(std::span{buf, 64}, static_cast<uint64_t>(i));
        }
    });

    uint32_t last_seq = 0;
    std::thread consumer([&]() {
        uint8_t read_buf[64];
        uint64_t ts = 0;
        for (uint32_t i = 1; i <= total_messages; ++i) {
            uint32_t len = queue.pop_wts(read_buf, ts);

            uint32_t current_seq;
            std::memcpy(&current_seq, read_buf, sizeof(current_seq));

            // 校验顺序、长度、时间戳和 payload
            EXPECT_EQ(len, 64);
            EXPECT_EQ(current_seq, i);
            EXPECT_EQ(ts, static_cast<uint64_t>(i));

            bool pattern_ok = true;
            for (size_t j = sizeof(uint32_t); j < 64; ++j) {
                if (read_buf[j] != static_cast<uint8_t>(i & 0xFF)) {
                    pattern_ok = false;
                    break;
                }
            }
            EXPECT_TRUE(pattern_ok);
            last_seq = current_seq;
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(last_seq, total_messages);
}

// clear() 测试
TEST(BoundedQueueBytesTest, ClearResetsQueue) {
    BoundedQueueBytes<64, 8> queue;

    std::array<uint8_t, 64> payload{};
    payload.fill(0xAB);
    EXPECT_TRUE(queue.try_push(payload));
    EXPECT_FALSE(queue.empty());

    queue.clear();
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.size(), 0u);

    // 写入新数据后应正常工作
    payload.fill(0xCD);
    EXPECT_TRUE(queue.try_push(payload));
    std::array<uint8_t, 64> out{};
    auto res = queue.try_pop(out);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(out[0], 0xCD);
}
