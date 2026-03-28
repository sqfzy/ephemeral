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

// ===========================================================================
// 批量写入
// ===========================================================================

TEST(EvictingQueueBytesBatch, PushN_BasicOperation) {
    EvictingQueueBytes<64, 8> queue;

    std::array<uint8_t, 3> p1{0x01, 0x02, 0x03};
    std::array<uint8_t, 2> p2{0xAA, 0xBB};
    std::array<uint8_t, 1> p3{0xFF};

    std::span<const uint8_t> payloads[] = {p1, p2, p3};

    EXPECT_TRUE(queue.push_n(payloads, 3));
    EXPECT_EQ(queue.total_pushed(), 3u);

    // Read the latest (should be p3, the last written)
    std::array<uint8_t, 64> out{};
    auto len = queue.try_pop_latest(out);
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(*len, 1u);
    EXPECT_EQ(out[0], 0xFF);
}

TEST(EvictingQueueBytesBatch, PushNWts_WithTimestamps) {
    EvictingQueueBytes<64, 8> queue;

    std::array<uint8_t, 2> p1{0x01, 0x02};
    std::array<uint8_t, 2> p2{0x03, 0x04};

    std::span<const uint8_t> payloads[] = {p1, p2};
    uint64_t timestamps[] = {100, 200};

    EXPECT_TRUE(queue.push_n_wts(payloads, timestamps, 2));
    EXPECT_EQ(queue.total_pushed(), 2u);

    // Latest should be p2 with ts=200
    std::array<uint8_t, 64> out{};
    uint64_t ts = 0;
    uint32_t discarded = 0;
    auto len = queue.try_pop_latest_wts(out, ts, discarded);
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(*len, 2u);
    EXPECT_EQ(out[0], 0x03);
    EXPECT_EQ(ts, 200u);
}

TEST(EvictingQueueBytesBatch, PushN_FailsOnOversizedPayload) {
    EvictingQueueBytes<4, 8> queue;

    std::array<uint8_t, 5> too_large{};  // exceeds MaxDataSize=4
    std::array<uint8_t, 2> ok{};

    std::span<const uint8_t> payloads[] = {ok, too_large};
    EXPECT_FALSE(queue.push_n(payloads, 2));
    EXPECT_EQ(queue.total_pushed(), 0u);  // nothing pushed
}

TEST(EvictingQueueBytesBatch, PushN_EvictsOldData) {
    EvictingQueueBytes<64, 4> queue;  // capacity 4

    // Push 6 items — first 2 get evicted
    std::array<uint8_t, 1> items[6];
    std::span<const uint8_t> payloads[6];
    for (int i = 0; i < 6; ++i) {
        items[i] = {static_cast<uint8_t>(i)};
        payloads[i] = items[i];
    }

    EXPECT_TRUE(queue.push_n(payloads, 6));
    EXPECT_EQ(queue.total_pushed(), 6u);

    // Latest should be the last item (5)
    std::array<uint8_t, 64> out{};
    auto len = queue.try_pop_latest(out);
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(*len, 1u);
    EXPECT_EQ(out[0], 5u);
}

// ===========================================================================
// Peek 操作
// ===========================================================================

TEST(EvictingQueueBytesPeek, TryPeekLatest_EmptyQueue) {
    EvictingQueueBytes<64, 4> queue;

    std::array<uint8_t, 64> out{};
    auto len = queue.try_peek_latest(out);
    EXPECT_FALSE(len.has_value());
}

TEST(EvictingQueueBytesPeek, TryPeekLatest_ReadsWithoutUpdatingDiscardCounter) {
    EvictingQueueBytes<64, 4> queue;

    std::array<uint8_t, 3> payload{0xAA, 0xBB, 0xCC};
    ASSERT_TRUE(queue.try_push_wts(payload, 100));

    // Peek — should read data without updating last_pop_id_
    std::array<uint8_t, 64> out{};
    auto len = queue.try_peek_latest(out);
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(*len, 3u);
    EXPECT_EQ(out[0], 0xAA);
    EXPECT_EQ(out[1], 0xBB);
    EXPECT_EQ(out[2], 0xCC);

    // Peek again — same data, no side effects
    std::array<uint8_t, 64> out2{};
    auto len2 = queue.try_peek_latest(out2);
    ASSERT_TRUE(len2.has_value());
    EXPECT_EQ(*len2, 3u);

    // Now consume — discard counter should be 0 since peek didn't advance reader
    uint64_t ts = 0;
    uint32_t discarded = 999;  // sentinel
    std::array<uint8_t, 64> out3{};
    auto len3 = queue.try_pop_latest_wts(out3, ts, discarded);
    ASSERT_TRUE(len3.has_value());
    EXPECT_EQ(*len3, 3u);
    EXPECT_EQ(ts, 100u);
    EXPECT_EQ(discarded, 0u);  // no discards: peek didn't affect tracking
}

TEST(EvictingQueueBytesPeek, TryPeekLatestWts_ReturnsTimestamp) {
    EvictingQueueBytes<64, 4> queue;

    std::array<uint8_t, 2> payload{0x01, 0x02};
    ASSERT_TRUE(queue.try_push_wts(payload, 42));

    std::array<uint8_t, 64> out{};
    uint64_t ts = 0;
    auto len = queue.try_peek_latest_wts(out, ts);
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(*len, 2u);
    EXPECT_EQ(ts, 42u);
    EXPECT_EQ(out[0], 0x01);
}

TEST(EvictingQueueBytesPeek, TryPeekLatestVisit_ZeroCopy) {
    EvictingQueueBytes<64, 4> queue;

    std::array<uint8_t, 4> payload{0xDE, 0xAD, 0xBE, 0xEF};
    ASSERT_TRUE(queue.try_push(payload));

    bool visited = false;
    bool ok = queue.try_peek_latest_visit([&](std::span<const uint8_t> data) {
        visited = true;
        EXPECT_EQ(data.size(), 4u);
        EXPECT_EQ(data[0], 0xDE);
        EXPECT_EQ(data[3], 0xEF);
    });
    EXPECT_TRUE(ok);
    EXPECT_TRUE(visited);
}

TEST(EvictingQueueBytesPeek, TryPeekLatestVisitWts_ZeroCopyWithTimestamp) {
    EvictingQueueBytes<64, 4> queue;

    std::array<uint8_t, 2> payload{0x42, 0x43};
    ASSERT_TRUE(queue.try_push_wts(payload, 777));

    bool visited = false;
    bool ok = queue.try_peek_latest_visit_wts(
        [&](std::span<const uint8_t> data, uint64_t ts) {
            visited = true;
            EXPECT_EQ(data.size(), 2u);
            EXPECT_EQ(data[0], 0x42);
            EXPECT_EQ(ts, 777u);
        });
    EXPECT_TRUE(ok);
    EXPECT_TRUE(visited);
}

TEST(EvictingQueueBytesPeek, TryPeekLatest_TruncatesWhenBufferSmaller) {
    EvictingQueueBytes<64, 4> queue;

    std::array<uint8_t, 8> payload{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    ASSERT_TRUE(queue.try_push(payload));

    // Peek with undersized buffer — should truncate to 3 bytes
    std::array<uint8_t, 3> small_buf{};
    auto len = queue.try_peek_latest(small_buf);
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(*len, 3u);
    EXPECT_EQ(small_buf[0], 0x01);
    EXPECT_EQ(small_buf[1], 0x02);
    EXPECT_EQ(small_buf[2], 0x03);
}

TEST(EvictingQueueBytesPeek, TryPeekLatest_EvictionDoesNotAffectDiscardCounter) {
    EvictingQueueBytes<64, 2> queue;  // capacity 2

    // Push 3 items — first gets evicted
    std::array<uint8_t, 2> p1{1, 1}, p2{2, 2}, p3{3, 3};
    ASSERT_TRUE(queue.try_push_wts(p1, 100));
    ASSERT_TRUE(queue.try_push_wts(p2, 200));
    ASSERT_TRUE(queue.try_push_wts(p3, 300));  // evicts p1

    // Peek — should NOT update discard tracking
    std::array<uint8_t, 64> out{};
    auto peek_len = queue.try_peek_latest(out);
    ASSERT_TRUE(peek_len.has_value());
    EXPECT_EQ(out[0], 3u);  // latest data (p3)

    // Now consume — discard counter should reflect all skipped messages
    // from the beginning (since last_pop_id_ was never set by peek)
    uint64_t ts = 0;
    uint32_t discarded = 999;
    std::array<uint8_t, 64> out2{};
    auto len = queue.try_pop_latest_wts(out2, ts, discarded);
    ASSERT_TRUE(len.has_value());
    EXPECT_EQ(ts, 300u);
    // First consume after push: last_pop_id_ was 0, so discarded = 0
    // (first read always reports 0 discards per the implementation)
    EXPECT_EQ(discarded, 0u);
}

// ===========================================================================
// try_peek_latest_visit_for — non-consuming peek with timeout
// ===========================================================================

TYPED_TEST(EvictingQueueBytesTest, peek_visit_for_returns_false_on_empty_timeout) {
    TypeParam queue;
    bool called = false;
    bool ok = queue.try_peek_latest_visit_for(
        [&](std::span<const uint8_t>) { called = true; },
        std::chrono::microseconds(10));
    EXPECT_FALSE(ok);
    EXPECT_FALSE(called);
}

TYPED_TEST(EvictingQueueBytesTest, peek_visit_for_receives_data_before_timeout) {
    TypeParam queue;
    std::vector<uint8_t> payload{0x01, 0x02, 0x03};
    queue.try_push(payload);

    std::vector<uint8_t> received;
    bool ok = queue.try_peek_latest_visit_for(
        [&](std::span<const uint8_t> data) {
            received.assign(data.begin(), data.end());
        },
        std::chrono::milliseconds(10));
    EXPECT_TRUE(ok);
    EXPECT_EQ(received, payload);
}

TYPED_TEST(EvictingQueueBytesTest, peek_visit_for_does_not_consume) {
    TypeParam queue;
    std::vector<uint8_t> payload{0xAA, 0xBB};
    queue.try_push(payload);

    // Peek twice
    size_t peek_count = 0;
    for (int i = 0; i < 2; ++i) {
        queue.try_peek_latest_visit_for(
            [&](std::span<const uint8_t> data) {
                ++peek_count;
                EXPECT_EQ(data.size(), 2u);
            },
            std::chrono::milliseconds(1));
    }
    EXPECT_EQ(peek_count, 2u);

    // Consume should still succeed
    std::vector<uint8_t> out(256);
    auto len = queue.try_pop_latest(out);
    EXPECT_TRUE(len.has_value());
}

TYPED_TEST(EvictingQueueBytesTest, peek_visit_wts_for_returns_correct_timestamp) {
    TypeParam queue;
    std::vector<uint8_t> payload{0x42};
    queue.try_push_wts(payload, 12345u);

    uint64_t seen_ts = 0;
    bool ok = queue.try_peek_latest_visit_wts_for(
        [&](std::span<const uint8_t> data, uint64_t ts) {
            EXPECT_EQ(data.size(), 1u);
            EXPECT_EQ(data[0], 0x42);
            seen_ts = ts;
        },
        std::chrono::milliseconds(10));
    EXPECT_TRUE(ok);
    EXPECT_EQ(seen_ts, 12345u);
}

TYPED_TEST(EvictingQueueBytesTest, peek_visit_wts_for_timeout_on_empty) {
    TypeParam queue;
    bool called = false;
    bool ok = queue.try_peek_latest_visit_wts_for(
        [&](std::span<const uint8_t>, uint64_t) { called = true; },
        std::chrono::microseconds(10));
    EXPECT_FALSE(ok);
    EXPECT_FALSE(called);
}

TYPED_TEST(EvictingQueueBytesTest, stats_reflects_push_and_consume) {
    TypeParam queue;

    // Empty queue stats
    auto s0 = queue.stats();
    EXPECT_EQ(s0.total_pushed, 0u);
    EXPECT_EQ(s0.last_pop_id, 0u);
    EXPECT_EQ(s0.capacity, queue.capacity());

    // Push a message
    std::vector<uint8_t> payload = {1, 2, 3, 4};
    queue.try_push(payload);

    auto s1 = queue.stats();
    EXPECT_EQ(s1.total_pushed, 1u);

    // Consume it
    std::vector<uint8_t> buf(256);
    queue.try_pop_latest(buf);

    auto s2 = queue.stats();
    EXPECT_EQ(s2.total_pushed, 1u);
    EXPECT_GT(s2.last_pop_id, 0u);

    // dump() and to_json() should not crash
    EXPECT_FALSE(s2.dump().empty());
    EXPECT_FALSE(s2.to_json().empty());
}

TYPED_TEST(EvictingQueueBytesTest, stats_tracks_overwritten_after_eviction) {
    TypeParam queue;
    const size_t cap = queue.capacity();

    // Fill beyond capacity to trigger evictions
    for (size_t i = 0; i < cap + 5; ++i) {
        uint8_t byte = static_cast<uint8_t>(i & 0xFF);
        queue.try_push(std::span<const uint8_t>(&byte, 1));
    }

    auto s = queue.stats();
    EXPECT_EQ(s.total_pushed, cap + 5);
    // Overwritten count should reflect the evicted entries
    EXPECT_GT(s.total_overwritten, 0u);
    EXPECT_EQ(s.capacity, cap);
}

TYPED_TEST(EvictingQueueBytesTest, stats_operator_minus_diffs_counters) {
    TypeParam queue;

    // Push some messages
    std::vector<uint8_t> payload = {1, 2, 3};
    for (int i = 0; i < 5; ++i) {
        queue.try_push(payload);
    }
    auto s1 = queue.stats();

    // Push more
    for (int i = 0; i < 3; ++i) {
        queue.try_push(payload);
    }
    auto s2 = queue.stats();

    auto delta = s2 - s1;
    EXPECT_EQ(delta.total_pushed, 3u);
    // Point-in-time fields take the lhs value
    EXPECT_EQ(delta.current_size, s2.current_size);
    EXPECT_EQ(delta.capacity, s2.capacity);
    EXPECT_EQ(delta.last_pop_id, s2.last_pop_id);
}

TYPED_TEST(EvictingQueueBytesTest, stats_equality_compares_all_fields) {
    TypeParam queue;

    auto s1 = queue.stats();
    auto s2 = queue.stats();
    EXPECT_EQ(s1, s2);

    // Push to change stats
    std::vector<uint8_t> payload = {1};
    queue.try_push(payload);
    auto s3 = queue.stats();
    EXPECT_NE(s1, s3);
}

TEST(EvictingQueueBytesStats, StdFormatterProducesDumpOutput) {
    EvictingQueueBytes<64, 4> queue;
    std::vector<uint8_t> payload = {1, 2, 3};
    queue.try_push(payload);

    auto s = queue.stats();
    auto formatted = std::format("{}", s);
    EXPECT_EQ(formatted, s.dump());
    EXPECT_NE(formatted.find("EvictingQueueBytes::Stats:"), std::string::npos);
}

// ---------------------------------------------------------------------------
// throughput() and loss_rate() tests
// ---------------------------------------------------------------------------

TEST(EvictingQueueBytesStats, throughput_on_delta_snapshot) {
    EvictingQueueBytes<64, 4> queue;

    // Push 3 items and pop them
    std::vector<uint8_t> payload = {0xAA};
    (void)queue.try_push(payload);
    (void)queue.try_push(payload);
    (void)queue.try_push(payload);

    std::array<uint8_t, 64> out{};
    queue.try_pop_latest(out);

    auto s1 = queue.stats();

    // Push and pop 2 more
    (void)queue.try_push(payload);
    (void)queue.try_push(payload);
    queue.try_pop_latest(out);

    auto s2 = queue.stats();
    auto delta = s2 - s1;

    // delta.total_popped should reflect only the pops in the interval
    EXPECT_EQ(delta.total_pushed, 2u);
    // total_popped delta should be > 0 (items were popped in interval)
    EXPECT_GT(delta.total_popped, 0u);
    // total_popped delta should NOT equal the absolute s2.total_popped
    EXPECT_LT(delta.total_popped, s2.total_popped);

    // Throughput over 1 second (1e9 ns) should equal the delta pop count
    double tp = delta.throughput(1'000'000'000);
    EXPECT_DOUBLE_EQ(tp, static_cast<double>(delta.total_popped));
}

TEST(EvictingQueueBytesStats, throughput_zero_duration_returns_zero) {
    using Stats = eph::containers::EvictingQueueBytesStats;
    Stats s{.total_pushed = 100, .last_pop_id = 50, .total_popped = 50};
    EXPECT_DOUBLE_EQ(s.throughput(0), 0.0);
}

TEST(EvictingQueueBytesStats, loss_rate_no_overwrite_returns_zero) {
    using Stats = eph::containers::EvictingQueueBytesStats;
    Stats s{.total_pushed = 100, .total_overwritten = 0};
    EXPECT_DOUBLE_EQ(s.loss_rate(), 0.0);
}

TEST(EvictingQueueBytesStats, loss_rate_with_overwrite) {
    using Stats = eph::containers::EvictingQueueBytesStats;
    Stats s{.total_pushed = 100, .total_overwritten = 25};
    EXPECT_DOUBLE_EQ(s.loss_rate(), 0.25);
}

TEST(EvictingQueueBytesStats, loss_rate_no_pushes_returns_zero) {
    using Stats = eph::containers::EvictingQueueBytesStats;
    Stats s{.total_pushed = 0, .total_overwritten = 0};
    EXPECT_DOUBLE_EQ(s.loss_rate(), 0.0);
}
