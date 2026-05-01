#include <gtest/gtest.h>

#include "eph/utils/audit_log.hpp"

#include <thread>
#include <vector>

using namespace eph::utils;

// Small capacity for testing (must be power of 2)
using TestLog = AuditLog<64>;

TEST(AuditLog, InitiallyEmpty) {
    TestLog log;
    EXPECT_EQ(log.total_count(), 0);
    EXPECT_EQ(log.count(), 0);
    EXPECT_EQ(log.latest(), nullptr);
}

TEST(AuditLog, RecordSingleEntry) {
    TestLog log;
    (void)log.record(AuditEvent::NewOrder, 1001, 50000.0, 1.5, Side::Buy, 0);
    EXPECT_EQ(log.total_count(), 1);
    EXPECT_EQ(log.count(), 1);

    const auto* e = log.latest();
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->event, AuditEvent::NewOrder);
    EXPECT_EQ(e->order_id, 1001);
    EXPECT_DOUBLE_EQ(e->price, 50000.0);
    EXPECT_DOUBLE_EQ(e->quantity, 1.5);
    EXPECT_EQ(e->side, Side::Buy);
    EXPECT_EQ(e->venue_id, 0);
    EXPECT_GT(e->tsc, 0u);
}

TEST(AuditLog, RecordMultipleEntries) {
    TestLog log;
    (void)log.record(AuditEvent::NewOrder, 1, 100.0, 10.0, Side::Buy, 1);
    (void)log.record(AuditEvent::Acknowledged, 1, 100.0, 10.0, Side::Buy, 1);
    (void)log.record(AuditEvent::Fill, 1, 100.0, 10.0, Side::Buy, 1, 100.05, 10.0);

    EXPECT_EQ(log.count(), 3);
    const auto* latest = log.latest();
    ASSERT_NE(latest, nullptr);
    EXPECT_EQ(latest->event, AuditEvent::Fill);
    EXPECT_DOUBLE_EQ(latest->fill_price, 100.05);
    EXPECT_DOUBLE_EQ(latest->fill_qty, 10.0);
}

TEST(AuditLog, AtOffsetReturnsCorrectEntry) {
    TestLog log;
    for (uint64_t i = 0; i < 5; ++i) {
        (void)log.record(AuditEvent::NewOrder, i, static_cast<double>(i), 1.0, Side::Buy, 0);
    }

    // at(0) = most recent (order_id=4), at(4) = oldest (order_id=0)
    for (uint64_t i = 0; i < 5; ++i) {
        const auto* e = log.at(i);
        ASSERT_NE(e, nullptr);
        EXPECT_EQ(e->order_id, 4 - i);
    }

    // Beyond count returns nullptr
    EXPECT_EQ(log.at(5), nullptr);
    EXPECT_EQ(log.at(100), nullptr);
}

TEST(AuditLog, RingBufferWrapsAround) {
    TestLog log;  // Capacity = 64

    // Write 100 entries (wraps around)
    for (uint64_t i = 0; i < 100; ++i) {
        (void)log.record(AuditEvent::NewOrder, i, 0.0, 0.0, Side::Buy, 0);
    }

    EXPECT_EQ(log.total_count(), 100);
    EXPECT_EQ(log.count(), 64);  // capped at capacity

    // Most recent should be order_id=99
    const auto* latest = log.latest();
    ASSERT_NE(latest, nullptr);
    EXPECT_EQ(latest->order_id, 99);

    // Oldest visible should be order_id=36 (100-64)
    const auto* oldest = log.at(63);
    ASSERT_NE(oldest, nullptr);
    EXPECT_EQ(oldest->order_id, 36);
}

TEST(AuditLog, FillEventRecordsFillFields) {
    TestLog log;
    (void)log.record(AuditEvent::PartialFill, 42, 100.0, 10.0, Side::Sell, 2, 100.10, 3.5);

    const auto* e = log.latest();
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->event, AuditEvent::PartialFill);
    EXPECT_EQ(e->side, Side::Sell);
    EXPECT_EQ(e->venue_id, 2);
    EXPECT_DOUBLE_EQ(e->fill_price, 100.10);
    EXPECT_DOUBLE_EQ(e->fill_qty, 3.5);
}

TEST(AuditLog, SessionEvents) {
    TestLog log;
    (void)log.record(AuditEvent::SessionLogon, 0, 0.0, 0.0, Side::Buy, 1);
    (void)log.record(AuditEvent::ConnectionUp, 0, 0.0, 0.0, Side::Buy, 1);

    EXPECT_EQ(log.count(), 2);
    EXPECT_EQ(log.at(0)->event, AuditEvent::ConnectionUp);
    EXPECT_EQ(log.at(1)->event, AuditEvent::SessionLogon);
}

TEST(AuditLog, TscTimestampsAreMonotonic) {
    TestLog log;
    (void)log.record(AuditEvent::NewOrder, 1, 0.0, 0.0, Side::Buy, 0);
    (void)log.record(AuditEvent::NewOrder, 2, 0.0, 0.0, Side::Buy, 0);

    const auto* e1 = log.at(1);  // older
    const auto* e2 = log.at(0);  // newer
    ASSERT_NE(e1, nullptr);
    ASSERT_NE(e2, nullptr);
    EXPECT_GE(e2->tsc, e1->tsc);
}

TEST(AuditLog, DumpProducesOutput) {
    TestLog log;
    (void)log.record(AuditEvent::NewOrder, 1, 50000.0, 1.5, Side::Buy, 0);
    (void)log.record(AuditEvent::Fill, 1, 50000.0, 1.5, Side::Buy, 0, 50001.0, 1.5);

    std::string output = log.dump();
    EXPECT_FALSE(output.empty());
    EXPECT_NE(output.find("NEW_ORDER"), std::string::npos);
    EXPECT_NE(output.find("FILL"), std::string::npos);
}

TEST(AuditLog, FlushToFile) {
    TestLog log;
    (void)log.record(AuditEvent::NewOrder, 1, 100.0, 5.0, Side::Buy, 0);
    (void)log.record(AuditEvent::Acknowledged, 1, 100.0, 5.0, Side::Buy, 0);

    auto path = "/tmp/test_audit_flush.bin";
    size_t written = log.flush_to_file(path);
    EXPECT_EQ(written, 2);

    // Verify file size
    FILE* f = std::fopen(path, "rb");
    ASSERT_NE(f, nullptr);
    std::fseek(f, 0, SEEK_END);
    auto file_size = std::ftell(f);
    std::fclose(f);
    EXPECT_EQ(file_size, 2 * static_cast<long>(sizeof(AuditEntry)));

    std::remove(path);
}

TEST(AuditLog, AuditEventNameCoversAllValues) {
    EXPECT_EQ(audit_event_name(AuditEvent::NewOrder), "NEW_ORDER");
    EXPECT_EQ(audit_event_name(AuditEvent::Acknowledged), "ACK");
    EXPECT_EQ(audit_event_name(AuditEvent::PartialFill), "PARTIAL_FILL");
    EXPECT_EQ(audit_event_name(AuditEvent::Fill), "FILL");
    EXPECT_EQ(audit_event_name(AuditEvent::CancelRequest), "CANCEL_REQ");
    EXPECT_EQ(audit_event_name(AuditEvent::Canceled), "CANCELED");
    EXPECT_EQ(audit_event_name(AuditEvent::Rejected), "REJECTED");
    EXPECT_EQ(audit_event_name(AuditEvent::Amended), "AMENDED");
    EXPECT_EQ(audit_event_name(AuditEvent::SessionLogon), "SESSION_LOGON");
    EXPECT_EQ(audit_event_name(AuditEvent::SessionLogout), "SESSION_LOGOUT");
    EXPECT_EQ(audit_event_name(AuditEvent::ConnectionUp), "CONN_UP");
    EXPECT_EQ(audit_event_name(AuditEvent::ConnectionDown), "CONN_DOWN");
    EXPECT_EQ(audit_event_name(AuditEvent::KillSwitch), "KILL_SWITCH");
}

TEST(AuditLog, AuditEventNameReturnsUnknownForOutOfRangeValue) {
    // KillSwitch is the highest legal enumerator (12). Any uint8_t past it
    // is undefined enum territory — `audit_event_name` must fall through
    // the switch and return the "UNKNOWN" sentinel rather than alias to
    // a real event. This guards forward-compat (a future enum addition
    // shipped in a header without a matching switch arm is the more
    // realistic regression than "user constructs garbage value").
    auto bogus = static_cast<AuditEvent>(99);
    EXPECT_EQ(audit_event_name(bogus), "UNKNOWN");

    // 255 is the uint8_t max — distinct from the 99 case so a bug that
    // accidentally returns the *last* matched arm (KillSwitch=12) for
    // anything > kMax also fails here, not just the 99 case.
    auto bogus_max = static_cast<AuditEvent>(255);
    EXPECT_EQ(audit_event_name(bogus_max), "UNKNOWN");
}

TEST(AuditLog, MultiThreadedRecordMt) {
    AuditLog<1024> log;
    constexpr int N_THREADS = 4;
    constexpr int N_PER_THREAD = 100;

    std::vector<std::thread> threads;
    for (int t = 0; t < N_THREADS; ++t) {
        threads.emplace_back([&log, t] {
            for (int i = 0; i < N_PER_THREAD; ++i) {
                (void)log.record_mt(AuditEvent::NewOrder,
                    static_cast<uint64_t>(t * N_PER_THREAD + i),
                    100.0, 1.0, Side::Buy, static_cast<uint8_t>(t));
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(log.total_count(), N_THREADS * N_PER_THREAD);
}

TEST(AuditLog, DefaultCapacity) {
    // Verify the default template parameter compiles
    AuditLog<> log;
    EXPECT_EQ(log.capacity, 65536);
    (void)log.record(AuditEvent::NewOrder, 1, 0.0, 0.0, Side::Buy, 0);
    EXPECT_EQ(log.count(), 1);
}

TEST(AuditEntry, SizeIs64Bytes) {
    EXPECT_EQ(sizeof(AuditEntry), 64);
}

TEST(AuditEntry, AlignmentIsCacheLine) {
    // AuditEntry must be cache-line aligned to prevent false sharing
    // when stored in the ring buffer array.
    static_assert(alignof(AuditEntry) == 64,
                  "AuditEntry must have 64-byte alignment for cache-line isolation");
    EXPECT_EQ(alignof(AuditEntry), 64);
}

// ---------------------------------------------------------------------------
// Return value and error path tests
// ---------------------------------------------------------------------------

TEST(AuditLog, RecordReturnsTrueBeforeOverflow) {
    TestLog log;
    // First 64 records should return true (no overflow)
    for (uint64_t i = 0; i < 64; ++i) {
        bool ok = log.record(AuditEvent::NewOrder, i, 0.0, 0.0, Side::Buy, 0);
        EXPECT_TRUE(ok) << "record() should return true before capacity at i=" << i;
    }
}

TEST(AuditLog, RecordReturnsFalseOnOverflow) {
    TestLog log;
    // Fill to capacity
    for (uint64_t i = 0; i < 64; ++i) {
        (void)log.record(AuditEvent::NewOrder, i, 0.0, 0.0, Side::Buy, 0);
    }
    // 65th record should return false (overwriting oldest)
    bool ok = log.record(AuditEvent::NewOrder, 99, 0.0, 0.0, Side::Buy, 0);
    EXPECT_FALSE(ok);
}

TEST(AuditLog, RecordMtReturnsFalseOnOverflow) {
    AuditLog<64> log;
    for (uint64_t i = 0; i < 64; ++i) {
        (void)log.record_mt(AuditEvent::NewOrder, i, 0.0, 0.0, Side::Buy, 0);
    }
    bool ok = log.record_mt(AuditEvent::NewOrder, 99, 0.0, 0.0, Side::Buy, 0);
    EXPECT_FALSE(ok);
}

TEST(AuditLog, FlushToFileInvalidPathReturnsZero) {
    TestLog log;
    (void)log.record(AuditEvent::NewOrder, 1, 100.0, 1.0, Side::Buy, 0);
    // Writing to an invalid/non-existent directory should fail
    size_t written = log.flush_to_file("/nonexistent_dir_xyz/audit.bin");
    EXPECT_EQ(written, 0);
}

TEST(AuditLog, FlushToFileEmptyLogReturnsZero) {
    TestLog log;
    size_t written = log.flush_to_file("/tmp/test_audit_empty.bin");
    EXPECT_EQ(written, 0);
    std::remove("/tmp/test_audit_empty.bin");
}

TEST(AuditLog, DumpEmptyLog) {
    TestLog log;
    std::string output = log.dump();
    EXPECT_NE(output.find("0 entries"), std::string::npos);
}

TEST(AuditLog, DumpLimitedEntries) {
    TestLog log;
    for (uint64_t i = 0; i < 10; ++i) {
        (void)log.record(AuditEvent::NewOrder, i, 0.0, 0.0, Side::Buy, 0);
    }
    // Dump only 3 entries
    std::string output = log.dump(3);
    EXPECT_NE(output.find("showing 3"), std::string::npos);
}

TEST(AuditLog, EntryDumpContainsAllFields) {
    TestLog log;
    (void)log.record(AuditEvent::Fill, 42, 100.25, 5.0, Side::Sell, 3, 100.30, 5.0);
    const auto* e = log.latest();
    ASSERT_NE(e, nullptr);
    std::string dump = e->dump();
    EXPECT_NE(dump.find("FILL"), std::string::npos);
    EXPECT_NE(dump.find("42"), std::string::npos);
    EXPECT_NE(dump.find("SELL"), std::string::npos);
}

TEST(AuditEntry, DumpShowsBuyForBuySide) {
    AuditEntry entry{};
    entry.side = Side::Buy;
    entry.event = AuditEvent::NewOrder;
    std::string dump = entry.dump();
    EXPECT_NE(dump.find("BUY"), std::string::npos);
    // Must NOT contain SELL or ???
    EXPECT_EQ(dump.find("SELL"), std::string::npos);
    EXPECT_EQ(dump.find("???"), std::string::npos);
}

TEST(AuditEntry, DumpShowsSellForSellSide) {
    AuditEntry entry{};
    entry.side = Side::Sell;
    entry.event = AuditEvent::NewOrder;
    std::string dump = entry.dump();
    EXPECT_NE(dump.find("SELL"), std::string::npos);
    EXPECT_EQ(dump.find("BUY"), std::string::npos);
    EXPECT_EQ(dump.find("???"), std::string::npos);
}

TEST(AuditEntry, DumpShowsQuestionMarksForZeroedSide) {
    // A zeroed AuditEntry has side == 0 which is neither Buy(1) nor Sell(2).
    // dump() must NOT misrepresent it as "SELL".
    AuditEntry entry{};
    std::memset(&entry, 0, sizeof(entry));
    std::string dump = entry.dump();
    EXPECT_NE(dump.find("???"), std::string::npos)
        << "Zeroed side should display '???' not 'SELL'; dump=" << dump;
}
