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

// ===========================================================================
// Timed operations
// ===========================================================================

TEST(BoundedQueueBytesTimed, TryPushForSucceeds) {
    BoundedQueueBytes<64, 4> queue;
    std::array<uint8_t, 8> payload{};
    payload.fill(0xAB);
    EXPECT_TRUE(queue.try_push_for(payload, std::chrono::milliseconds(10)));
    std::array<uint8_t, 64> out{};
    auto len = queue.try_pop(out);
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(*len, 8u);
    EXPECT_EQ(out[0], 0xAB);
}

TEST(BoundedQueueBytesTimed, TryPushForTimesOutOnFull) {
    BoundedQueueBytes<64, 2> queue;
    std::array<uint8_t, 8> payload{};
    ASSERT_TRUE(queue.try_push(payload));
    ASSERT_TRUE(queue.try_push(payload));
    ASSERT_TRUE(queue.full());

    auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(queue.try_push_for(payload, std::chrono::milliseconds(20)));
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, std::chrono::milliseconds(15));
}

TEST(BoundedQueueBytesTimed, TryPopForTimesOutOnEmpty) {
    BoundedQueueBytes<64, 4> queue;
    std::array<uint8_t, 64> out{};
    auto start = std::chrono::steady_clock::now();
    auto len = queue.try_pop_for(out, std::chrono::milliseconds(20));
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_FALSE(len.has_value());
    EXPECT_GE(elapsed, std::chrono::milliseconds(15));
}

TEST(BoundedQueueBytesTimed, TryConsumeForSucceeds) {
    BoundedQueueBytes<64, 4> queue;
    std::array<uint8_t, 4> payload = {1, 2, 3, 4};
    ASSERT_TRUE(queue.try_push(payload));

    size_t captured_len = 0;
    bool ok = queue.try_consume_for([&](std::span<const uint8_t> data) {
        captured_len = data.size();
    }, std::chrono::milliseconds(10));
    EXPECT_TRUE(ok);
    EXPECT_EQ(captured_len, 4u);
}

TEST(BoundedQueueBytesTimed, TryPushWtsForWithTimestamp) {
    BoundedQueueBytes<64, 4> queue;
    std::array<uint8_t, 4> payload = {5, 6, 7, 8};
    EXPECT_TRUE(queue.try_push_wts_for(payload, 12345, std::chrono::milliseconds(10)));

    uint64_t ts = 0;
    bool ok = queue.try_consume_wts_for([&](std::span<const uint8_t> data, uint64_t t) {
        ts = t;
        EXPECT_EQ(data.size(), 4u);
    }, std::chrono::milliseconds(10));
    EXPECT_TRUE(ok);
    EXPECT_EQ(ts, 12345u);
}

TEST(BoundedQueueBytesTimed, TryPopWtsForSuccess) {
    BoundedQueueBytes<64, 4> queue;
    std::array<uint8_t, 3> payload = {0xAA, 0xBB, 0xCC};
    EXPECT_TRUE(queue.try_push_wts(payload, 42));

    std::array<uint8_t, 64> out{};
    uint64_t ts = 0;
    auto len = queue.try_pop_wts_for(out, ts, std::chrono::milliseconds(10));
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(*len, 3u);
    EXPECT_EQ(out[0], 0xAA);
    EXPECT_EQ(out[1], 0xBB);
    EXPECT_EQ(out[2], 0xCC);
    EXPECT_EQ(ts, 42u);
}

TEST(BoundedQueueBytesTimed, TryPopWtsForTimeout) {
    BoundedQueueBytes<64, 4> queue;

    std::array<uint8_t, 64> out{};
    uint64_t ts = 0;
    auto len = queue.try_pop_wts_for(out, ts, std::chrono::milliseconds(5));
    EXPECT_FALSE(len.has_value());
}

// ===========================================================================
// 批量操作
// ===========================================================================

TEST(BoundedQueueBytesBatch, TryPushN_BasicOperation) {
    BoundedQueueBytes<64, 8> queue;

    std::array<uint8_t, 3> p1{0x01, 0x02, 0x03};
    std::array<uint8_t, 2> p2{0xAA, 0xBB};
    std::array<uint8_t, 1> p3{0xFF};

    std::span<const uint8_t> payloads[] = {p1, p2, p3};

    EXPECT_TRUE(queue.try_push_n(payloads, 3));
    EXPECT_EQ(queue.size(), 3u);

    // Verify in order
    std::array<uint8_t, 64> out{};
    auto len = queue.try_pop(out);
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(*len, 3u);
    EXPECT_EQ(out[0], 0x01);

    len = queue.try_pop(out);
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(*len, 2u);
    EXPECT_EQ(out[0], 0xAA);

    len = queue.try_pop(out);
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(*len, 1u);
    EXPECT_EQ(out[0], 0xFF);
}

TEST(BoundedQueueBytesBatch, TryPushNWts_WithTimestamps) {
    BoundedQueueBytes<64, 8> queue;

    std::array<uint8_t, 2> p1{0x01, 0x02};
    std::array<uint8_t, 2> p2{0x03, 0x04};

    std::span<const uint8_t> payloads[] = {p1, p2};
    uint64_t timestamps[] = {100, 200};

    EXPECT_TRUE(queue.try_push_n_wts(payloads, timestamps, 2));
    EXPECT_EQ(queue.size(), 2u);

    std::array<uint8_t, 64> out{};
    uint64_t ts = 0;
    auto len = queue.try_pop_wts(out, ts);
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(ts, 100u);
    EXPECT_EQ(out[0], 0x01);

    len = queue.try_pop_wts(out, ts);
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(ts, 200u);
    EXPECT_EQ(out[0], 0x03);
}

TEST(BoundedQueueBytesBatch, TryPushN_FailsOnOversizedPayload) {
    BoundedQueueBytes<4, 8> queue;

    std::array<uint8_t, 5> too_large{};  // exceeds MaxDataSize=4
    std::array<uint8_t, 2> ok{};

    std::span<const uint8_t> payloads[] = {ok, too_large};
    EXPECT_FALSE(queue.try_push_n(payloads, 2));
    EXPECT_TRUE(queue.empty());  // all-or-nothing: nothing pushed
}

TEST(BoundedQueueBytesBatch, TryPushN_FailsOnFullQueue) {
    BoundedQueueBytes<64, 4> queue;

    // Fill with 4 items first
    for (int i = 0; i < 4; ++i) {
        std::array<uint8_t, 1> p{static_cast<uint8_t>(i)};
        ASSERT_TRUE(queue.try_push(p));
    }
    EXPECT_TRUE(queue.full());

    std::array<uint8_t, 1> p{0xFF};
    std::span<const uint8_t> payloads[] = {p};
    EXPECT_FALSE(queue.try_push_n(payloads, 1));
}

TEST(BoundedQueueBytesBatch, TryConsumeN_BasicOperation) {
    BoundedQueueBytes<64, 8> queue;

    // Push 3 items
    for (uint8_t i = 0; i < 3; ++i) {
        std::array<uint8_t, 1> p{i};
        ASSERT_TRUE(queue.try_push(p));
    }

    std::vector<uint8_t> consumed;
    size_t n = queue.try_consume_n(2, [&](std::span<const uint8_t> data, size_t idx) {
        EXPECT_EQ(idx, consumed.size());
        ASSERT_EQ(data.size(), 1u);
        consumed.push_back(data[0]);
    });

    EXPECT_EQ(n, 2u);
    ASSERT_EQ(consumed.size(), 2u);
    EXPECT_EQ(consumed[0], 0u);
    EXPECT_EQ(consumed[1], 1u);
    EXPECT_EQ(queue.size(), 1u);
}

TEST(BoundedQueueBytesBatch, TryConsumeNWts_WithTimestamps) {
    BoundedQueueBytes<64, 8> queue;

    for (uint8_t i = 0; i < 3; ++i) {
        std::array<uint8_t, 1> p{i};
        ASSERT_TRUE(queue.try_push_wts(p, static_cast<uint64_t>(i * 100)));
    }

    std::vector<std::pair<uint8_t, uint64_t>> consumed;
    size_t n = queue.try_consume_n_wts(3,
        [&](std::span<const uint8_t> data, uint64_t ts, size_t idx) {
            EXPECT_EQ(idx, consumed.size());
            consumed.emplace_back(data[0], ts);
        });

    EXPECT_EQ(n, 3u);
    ASSERT_EQ(consumed.size(), 3u);
    EXPECT_EQ(consumed[0].first, 0u);
    EXPECT_EQ(consumed[0].second, 0u);
    EXPECT_EQ(consumed[1].first, 1u);
    EXPECT_EQ(consumed[1].second, 100u);
    EXPECT_EQ(consumed[2].first, 2u);
    EXPECT_EQ(consumed[2].second, 200u);
}

TEST(BoundedQueueBytesBatch, TryConsumeN_EmptyQueue) {
    BoundedQueueBytes<64, 8> queue;

    size_t n = queue.try_consume_n(4, [](std::span<const uint8_t>, size_t) {
        FAIL() << "Should not be called on empty queue";
    });
    EXPECT_EQ(n, 0u);
}

// ===========================================================================
// try_consume_all / try_consume_all_wts
// ===========================================================================

TEST(BoundedQueueBytesBatch, TryConsumeAll_DrainsEntireQueue) {
    BoundedQueueBytes<64, 8> queue;

    // Push 5 messages
    for (uint8_t i = 0; i < 5; ++i) {
        std::array<uint8_t, 1> payload{i};
        ASSERT_TRUE(queue.try_push(payload));
    }
    ASSERT_EQ(queue.size(), 5u);

    std::vector<uint8_t> consumed;
    size_t n = queue.try_consume_all([&](std::span<const uint8_t> data, size_t idx) {
        ASSERT_EQ(data.size(), 1u);
        EXPECT_EQ(idx, consumed.size());
        consumed.push_back(data[0]);
    });

    EXPECT_EQ(n, 5u);
    EXPECT_TRUE(queue.empty());
    ASSERT_EQ(consumed.size(), 5u);
    for (uint8_t i = 0; i < 5; ++i) {
        EXPECT_EQ(consumed[i], i);
    }
}

TEST(BoundedQueueBytesBatch, TryConsumeAll_EmptyQueue) {
    BoundedQueueBytes<64, 8> queue;

    size_t n = queue.try_consume_all([](std::span<const uint8_t>, size_t) {
        FAIL() << "Should not be called on empty queue";
    });
    EXPECT_EQ(n, 0u);
}

TEST(BoundedQueueBytesBatch, TryConsumeAllWts_DrainsWithTimestamps) {
    BoundedQueueBytes<64, 8> queue;

    for (uint8_t i = 0; i < 3; ++i) {
        std::array<uint8_t, 1> payload{i};
        ASSERT_TRUE(queue.try_push_wts(payload, 1000 + i));
    }

    std::vector<std::pair<uint8_t, uint64_t>> consumed;
    size_t n = queue.try_consume_all_wts(
        [&](std::span<const uint8_t> data, uint64_t ts, size_t) {
            ASSERT_EQ(data.size(), 1u);
            consumed.emplace_back(data[0], ts);
        });

    EXPECT_EQ(n, 3u);
    EXPECT_TRUE(queue.empty());
    ASSERT_EQ(consumed.size(), 3u);
    for (uint8_t i = 0; i < 3; ++i) {
        EXPECT_EQ(consumed[i].first, i);
        EXPECT_EQ(consumed[i].second, 1000u + i);
    }
}

// ===========================================================================
// Peek 操作
// ===========================================================================

TEST(BoundedQueueBytesPeek, TryPeek_EmptyQueue) {
    BoundedQueueBytes<64, 4> queue;

    std::array<uint8_t, 64> out{};
    auto len = queue.try_peek(out);
    EXPECT_FALSE(len.has_value());
}

TEST(BoundedQueueBytesPeek, TryPeek_ReadsWithoutConsuming) {
    BoundedQueueBytes<64, 4> queue;

    std::array<uint8_t, 3> payload{0xAA, 0xBB, 0xCC};
    ASSERT_TRUE(queue.try_push(payload));
    EXPECT_EQ(queue.size(), 1u);

    // Peek should return the data without consuming
    std::array<uint8_t, 64> out{};
    auto len = queue.try_peek(out);
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(*len, 3u);
    EXPECT_EQ(out[0], 0xAA);
    EXPECT_EQ(out[1], 0xBB);
    EXPECT_EQ(out[2], 0xCC);

    // Queue should still have 1 element
    EXPECT_EQ(queue.size(), 1u);

    // Peek again — same data
    std::array<uint8_t, 64> out2{};
    auto len2 = queue.try_peek(out2);
    ASSERT_TRUE(len2.has_value());
    EXPECT_EQ(*len2, 3u);
    EXPECT_EQ(out2[0], 0xAA);

    // Pop should get the same data and consume it
    std::array<uint8_t, 64> out3{};
    auto len3 = queue.try_pop(out3);
    ASSERT_TRUE(len3.has_value());
    EXPECT_EQ(*len3, 3u);
    EXPECT_EQ(out3[0], 0xAA);
    EXPECT_TRUE(queue.empty());
}

TEST(BoundedQueueBytesPeek, TryPeekWts_ReturnsTimestamp) {
    BoundedQueueBytes<64, 4> queue;

    std::array<uint8_t, 2> payload{0x01, 0x02};
    uint64_t ts_in = 9999;
    ASSERT_TRUE(queue.try_push_wts(payload, ts_in));

    std::array<uint8_t, 64> out{};
    uint64_t ts_out = 0;
    auto len = queue.try_peek_wts(out, ts_out);
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(*len, 2u);
    EXPECT_EQ(ts_out, ts_in);
    EXPECT_EQ(out[0], 0x01);

    // Still in queue
    EXPECT_EQ(queue.size(), 1u);
}

TEST(BoundedQueueBytesPeek, TryPeekVisit_ZeroCopy) {
    BoundedQueueBytes<64, 4> queue;

    std::array<uint8_t, 4> payload{0xDE, 0xAD, 0xBE, 0xEF};
    ASSERT_TRUE(queue.try_push(payload));

    bool visited = false;
    bool ok = queue.try_peek_visit([&](std::span<const uint8_t> data) {
        visited = true;
        EXPECT_EQ(data.size(), 4u);
        EXPECT_EQ(data[0], 0xDE);
        EXPECT_EQ(data[3], 0xEF);
    });
    EXPECT_TRUE(ok);
    EXPECT_TRUE(visited);
    EXPECT_EQ(queue.size(), 1u);  // not consumed
}

TEST(BoundedQueueBytesPeek, TryPeekVisitWts_ZeroCopyWithTimestamp) {
    BoundedQueueBytes<64, 4> queue;

    std::array<uint8_t, 2> payload{0x42, 0x43};
    ASSERT_TRUE(queue.try_push_wts(payload, 12345));

    bool visited = false;
    bool ok = queue.try_peek_visit_wts([&](std::span<const uint8_t> data, uint64_t ts) {
        visited = true;
        EXPECT_EQ(data.size(), 2u);
        EXPECT_EQ(data[0], 0x42);
        EXPECT_EQ(ts, 12345u);
    });
    EXPECT_TRUE(ok);
    EXPECT_TRUE(visited);
    EXPECT_EQ(queue.size(), 1u);
}

TEST(BoundedQueueBytesPeek, TryPeek_TruncatesWhenBufferSmaller) {
    BoundedQueueBytes<64, 4> queue;

    std::array<uint8_t, 8> payload{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    ASSERT_TRUE(queue.try_push(payload));

    // Peek with undersized buffer — should truncate to 3 bytes
    std::array<uint8_t, 3> small_buf{};
    auto len = queue.try_peek(small_buf);
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(*len, 3u);
    EXPECT_EQ(small_buf[0], 0x01);
    EXPECT_EQ(small_buf[1], 0x02);
    EXPECT_EQ(small_buf[2], 0x03);

    // Queue still has the full message
    EXPECT_EQ(queue.size(), 1u);
}

// ===========================================================================
// Stats tests
// ===========================================================================

TEST(BoundedQueueBytesStats, EmptyQueueStats) {
    BoundedQueueBytes<64, 4> queue;
    auto s = queue.stats();
    EXPECT_EQ(s.total_pushed, 0u);
    EXPECT_EQ(s.total_popped, 0u);
    EXPECT_EQ(s.current_size, 0u);
    EXPECT_EQ(s.capacity, 4u);
}

TEST(BoundedQueueBytesStats, StatsAfterPushPop) {
    BoundedQueueBytes<64, 4> queue;
    std::array<uint8_t, 3> payload{0x01, 0x02, 0x03};

    ASSERT_TRUE(queue.try_push(payload));
    ASSERT_TRUE(queue.try_push(payload));

    auto s1 = queue.stats();
    EXPECT_EQ(s1.total_pushed, 2u);
    EXPECT_EQ(s1.total_popped, 0u);
    EXPECT_EQ(s1.current_size, 2u);

    std::array<uint8_t, 64> out{};
    ASSERT_TRUE(queue.try_pop(out).has_value());

    auto s2 = queue.stats();
    EXPECT_EQ(s2.total_pushed, 2u);
    EXPECT_EQ(s2.total_popped, 1u);
    EXPECT_EQ(s2.current_size, 1u);
}

TEST(BoundedQueueBytesStats, StatsDiffOperator) {
    BoundedQueueBytes<64, 8> queue;
    auto s1 = queue.stats();

    std::array<uint8_t, 2> payload{0xAA, 0xBB};
    for (int i = 0; i < 5; ++i)
        ASSERT_TRUE(queue.try_push(payload));

    std::array<uint8_t, 64> out{};
    ASSERT_TRUE(queue.try_pop(out).has_value());
    ASSERT_TRUE(queue.try_pop(out).has_value());

    auto s2 = queue.stats();
    auto delta = s2 - s1;

    EXPECT_EQ(delta.total_pushed, 5u);
    EXPECT_EQ(delta.total_popped, 2u);
    EXPECT_EQ(delta.current_size, 3u);  // point-in-time from s2
    EXPECT_EQ(delta.capacity, 8u);
}

TEST(BoundedQueueBytesStats, StatsEquality) {
    BoundedQueueBytes<64, 4> queue;
    auto s1 = queue.stats();
    auto s2 = queue.stats();
    EXPECT_EQ(s1, s2);

    std::array<uint8_t, 2> payload{0x01, 0x02};
    ASSERT_TRUE(queue.try_push(payload));
    auto s3 = queue.stats();
    EXPECT_NE(s1, s3);
}

TEST(BoundedQueueBytesStats, StatsDumpContainsKey) {
    BoundedQueueBytes<64, 4> queue;
    std::array<uint8_t, 2> payload{0x01, 0x02};
    ASSERT_TRUE(queue.try_push(payload));

    auto s = queue.stats();
    auto dump = s.dump();
    EXPECT_NE(dump.find("BoundedQueueBytes"), std::string::npos);
    EXPECT_NE(dump.find("capacity: 4"), std::string::npos);
    EXPECT_NE(dump.find("total_pushed: 1"), std::string::npos);
}

TEST(BoundedQueueBytesStats, StatsToJsonValid) {
    BoundedQueueBytes<64, 4> queue;
    std::array<uint8_t, 2> payload{0x01, 0x02};
    ASSERT_TRUE(queue.try_push(payload));

    auto s = queue.stats();
    auto json = s.to_json();
    EXPECT_NE(json.find("\"capacity\":4"), std::string::npos);
    EXPECT_NE(json.find("\"total_pushed\":1"), std::string::npos);
    EXPECT_NE(json.find("\"total_popped\":0"), std::string::npos);
}

TEST(BoundedQueueBytesStats, StdFormatterProducesDumpOutput) {
    BoundedQueueBytes<64, 4> queue;
    std::array<uint8_t, 2> payload{0x01, 0x02};
    ASSERT_TRUE(queue.try_push(payload));

    auto s = queue.stats();
    auto formatted = std::format("{}", s);
    EXPECT_EQ(formatted, s.dump());
    EXPECT_NE(formatted.find("BoundedQueueBytes::Stats:"), std::string::npos);
}
