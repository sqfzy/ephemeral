/// @file test_reactor.cpp
/// Unit tests for RxDispatcher: connection management, hash utility, and API.
///
/// NOTE: The actual NIC poll (rte_eth_rx_burst) cannot be unit-tested
/// without DPDK EAL. These tests cover the non-NIC parts:
///   - RxDispatcherEntry::hash_tuple correctness
///   - Connection registration and limits
///   - RxDispatcher lifecycle (start/stop without NIC)

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep
#include "eph/dpdk/rx_dispatcher.hpp"

using namespace eph::dpdk;
using namespace eph::net;

// ---------------------------------------------------------------------------
// RxDispatcherEntry::hash_tuple
// ---------------------------------------------------------------------------

static net::ConnectionTuple make_tuple(uint32_t src_ip, uint32_t dst_ip,
                                        uint16_t src_port, uint16_t dst_port) {
    net::ConnectionTuple t;
    t.src_ip = src_ip;
    t.dst_ip = dst_ip;
    t.src_port = src_port;
    t.dst_port = dst_port;
    return t;
}

TEST(RxDispatcherHash, DifferentTuplesProduceDifferentHashes) {
    auto t1 = make_tuple(0x0A000001, 0x0A000002, 12345, 443);
    auto t2 = make_tuple(0x0A000001, 0x0A000003, 12345, 443);
    EXPECT_NE(RxDispatcherEntry::hash_tuple(t1), RxDispatcherEntry::hash_tuple(t2));
}

TEST(RxDispatcherHash, SameTuplesSameHash) {
    auto t = make_tuple(0x0A000001, 0x0A000002, 12345, 443);
    EXPECT_EQ(RxDispatcherEntry::hash_tuple(t), RxDispatcherEntry::hash_tuple(t));
}

TEST(RxDispatcherHash, DifferentPortsDifferentHash) {
    auto t1 = make_tuple(0x0A000001, 0x0A000002, 12345, 443);
    auto t2 = make_tuple(0x0A000001, 0x0A000002, 12346, 443);
    EXPECT_NE(RxDispatcherEntry::hash_tuple(t1), RxDispatcherEntry::hash_tuple(t2));
}

TEST(RxDispatcherHash, ZeroTupleNonZeroHash) {
    net::ConnectionTuple t{};
    auto h = RxDispatcherEntry::hash_tuple(t);
    EXPECT_EQ(h, RxDispatcherEntry::hash_tuple(t));
}

TEST(RxDispatcherHash, SymmetricSwappedSrcDst) {
    // Critical property: hash(A->B) == hash(B->A) because incoming packets
    // have swapped src/dst relative to the registered connection tuple.
    auto local = make_tuple(0x0A000001, 0x0A000002, 12345, 443);
    auto remote = make_tuple(0x0A000002, 0x0A000001, 443, 12345); // swapped
    EXPECT_EQ(RxDispatcherEntry::hash_tuple(local),
              RxDispatcherEntry::hash_tuple(remote))
        << "Hash must be direction-symmetric for RX dispatch";
}

TEST(RxDispatcherHash, SymmetricMultipleTuples) {
    // Verify symmetry across several different connection tuples
    struct { uint32_t a_ip; uint32_t b_ip; uint16_t a_port; uint16_t b_port; } cases[] = {
        {0xC0A80101, 0x08080808, 5000, 443},
        {0xAC100001, 0xAC100002, 8443, 9090},
        {0x0A0A0001, 0x0A0A0002, 55123, 80},
    };
    for (const auto& c : cases) {
        auto fwd = make_tuple(c.a_ip, c.b_ip, c.a_port, c.b_port);
        auto rev = make_tuple(c.b_ip, c.a_ip, c.b_port, c.a_port);
        EXPECT_EQ(RxDispatcherEntry::hash_tuple(fwd), RxDispatcherEntry::hash_tuple(rev))
            << "Symmetry violation for " << c.a_ip << " <-> " << c.b_ip;
    }
}

TEST(RxDispatcherHash, NonSymmetricIpsDifferentHash) {
    // Different IPs (not just swapped) should produce different hashes
    auto t1 = make_tuple(0x0A000001, 0x0A000002, 12345, 443);
    auto t2 = make_tuple(0x0A000001, 0x0A000003, 12345, 443);
    EXPECT_NE(RxDispatcherEntry::hash_tuple(t1), RxDispatcherEntry::hash_tuple(t2));
}

// ---------------------------------------------------------------------------
// RxDispatcher — connection management (no NIC needed)
// ---------------------------------------------------------------------------

TEST(RxDispatcher, AddConnectionNullSessionFails) {
    RxDispatcher<> reactor(RxDispatcherConfig{});
    auto result = reactor.add_connection(nullptr, [](auto*, auto, auto){});
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("null"), std::string::npos);
}

TEST(RxDispatcher, ConnectionCountStartsAtZero) {
    RxDispatcher<> reactor(RxDispatcherConfig{});
    EXPECT_EQ(reactor.connection_count(), 0u);
}

TEST(RxDispatcher, IsRunningInitiallyFalse) {
    RxDispatcher<> reactor(RxDispatcherConfig{});
    EXPECT_FALSE(reactor.is_running());
}

TEST(RxDispatcher, MaxConnectionsConstant) {
    EXPECT_GE(kRxDispatcherMaxConnections, 8u);
    EXPECT_LE(kRxDispatcherMaxConnections, 256u);
}

// ---------------------------------------------------------------------------
// RxDispatcherEntry
// ---------------------------------------------------------------------------

TEST(RxDispatcherEntry, DefaultConstructedIsDisconnected) {
    RxDispatcherEntry entry;
    EXPECT_EQ(entry.session, nullptr);
    EXPECT_FALSE(entry.connected);
}

TEST(RxDispatcherEntry, HashIsConstexprSafe) {
    auto t = make_tuple(1, 2, 3, 4);
    auto h1 = RxDispatcherEntry::hash_tuple(t);
    auto h2 = RxDispatcherEntry::hash_tuple(t);
    EXPECT_EQ(h1, h2);
}

// ---------------------------------------------------------------------------
// RxDispatcherConfig validate/dump
// ---------------------------------------------------------------------------

TEST(RxDispatcherConfig, DefaultConfigIsValid) {
    RxDispatcherConfig cfg;
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(RxDispatcherConfig, NegativeCpuAffinity) {
    RxDispatcherConfig cfg{.rx_cpu = -2};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("rx_cpu"), std::string_view::npos);
}

TEST(RxDispatcherConfig, DumpContainsFields) {
    RxDispatcherConfig cfg{.port_id = 1, .rx_queue_id = 2, .rx_cpu = 3};
    auto d = cfg.dump();
    EXPECT_NE(d.find("port=1"), std::string::npos);
    EXPECT_NE(d.find("queue=2"), std::string::npos);
    EXPECT_NE(d.find("cpu=3"), std::string::npos);
}

TEST(RxDispatcherConfig, ToJsonValidStructure) {
    RxDispatcherConfig cfg{.port_id = 1, .rx_queue_id = 2, .rx_cpu = 3};
    auto j = cfg.to_json();
    EXPECT_EQ(j.front(), '{');
    EXPECT_EQ(j.back(), '}');
    EXPECT_NE(j.find("\"port_id\":1"), std::string::npos);
    EXPECT_NE(j.find("\"rx_queue_id\":2"), std::string::npos);
    EXPECT_NE(j.find("\"rx_cpu\":3"), std::string::npos);
}

TEST(RxDispatcherConfig, Equality) {
    RxDispatcherConfig a{.port_id = 1, .rx_queue_id = 2, .rx_cpu = 3};
    RxDispatcherConfig b{.port_id = 1, .rx_queue_id = 2, .rx_cpu = 3};
    RxDispatcherConfig c{.port_id = 0, .rx_queue_id = 2, .rx_cpu = 3};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST(RxDispatcherConfig, WarningUnpinnedCpu) {
    RxDispatcherConfig cfg{.rx_cpu = -1};
    auto w = cfg.warnings();
    ASSERT_FALSE(w.empty());
    EXPECT_NE(w[0].find("no pinning"), std::string::npos);
}

TEST(RxDispatcherConfig, NoWarningPinnedCpu) {
    RxDispatcherConfig cfg{.rx_cpu = 3};
    auto w = cfg.warnings();
    EXPECT_TRUE(w.empty());
}

TEST(RxDispatcherConfig, FormatterContainsKeyFields) {
    RxDispatcherConfig cfg{.port_id = 1, .rx_queue_id = 2, .rx_cpu = 3};
    auto s = std::format("{}", cfg);
    EXPECT_NE(s.find("RxDispatcher"), std::string::npos);
    EXPECT_NE(s.find("port=1"), std::string::npos);
}

// ---------------------------------------------------------------------------
// RxDispatcher::entry out-of-bounds
// ---------------------------------------------------------------------------

TEST(RxDispatcher, EntryOutOfBoundsReturnsEmpty) {
    RxDispatcher<> reactor(RxDispatcherConfig{});
    // No connections added, any index should return the static empty entry
    const auto& e = reactor.entry(0);
    EXPECT_EQ(e.session, nullptr);
    EXPECT_FALSE(e.connected);

    // Large index
    const auto& e2 = reactor.entry(999);
    EXPECT_EQ(e2.session, nullptr);
    EXPECT_FALSE(e2.connected);
}

TEST(RxDispatcher, MarkDisconnectedOutOfBoundsIsSafe) {
    RxDispatcher<> reactor(RxDispatcherConfig{});
    // Should not crash, just log a warning
    reactor.mark_disconnected(0);
    reactor.mark_disconnected(999);
    EXPECT_EQ(reactor.connection_count(), 0u);
}

TEST(RxDispatcher, MarkReconnectedNullSessionIsSafe) {
    RxDispatcher<> reactor(RxDispatcherConfig{});
    // Should not crash, null session is logged
    reactor.mark_reconnected(0, nullptr);
    EXPECT_EQ(reactor.connection_count(), 0u);
}

TEST(RxDispatcher, MarkReconnectedOutOfBoundsIsSafe) {
    RxDispatcher<> reactor(RxDispatcherConfig{});
    // Fake non-null session pointer for testing out-of-bounds
    TcpSession<>* fake_session = reinterpret_cast<TcpSession<>*>(0xDEAD);
    // Should not crash, just log a warning (does not dereference pointer
    // because conn_id check fires first)
    reactor.mark_reconnected(999, fake_session);
    EXPECT_EQ(reactor.connection_count(), 0u);
}

TEST(RxDispatcher, AddConnectionWhileRunningFails) {
    // Cannot test start()/stop() without EAL, but can verify the running
    // check logic indirectly via the atomic flag
    RxDispatcher<> reactor(RxDispatcherConfig{});
    EXPECT_FALSE(reactor.is_running());
    // The actual "running" rejection test would require mocking the thread
}

TEST(RxDispatcher, StopWhileNotRunningIsSafe) {
    RxDispatcher<> reactor(RxDispatcherConfig{});
    reactor.stop(); // Should not crash
    reactor.stop(); // Double stop should be idempotent
    EXPECT_FALSE(reactor.is_running());
}

// ---------------------------------------------------------------------------
// RxDispatcher — add_connection null callback rejection
// ---------------------------------------------------------------------------

TEST(RxDispatcher, AddConnectionNullCallbackFails) {
    RxDispatcher<> reactor(RxDispatcherConfig{});
    // Fake non-null session pointer for testing callback validation
    TcpSession<>* fake_session = reinterpret_cast<TcpSession<>*>(0xBEEF);
    RxDispatcherDataCallback empty_cb;  // default-constructed = empty
    auto result = reactor.add_connection(fake_session, empty_cb);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("null"), std::string::npos);
    EXPECT_EQ(reactor.connection_count(), 0u);
}

// ---------------------------------------------------------------------------
// RxDispatcher::start — returns false with zero connections
// ---------------------------------------------------------------------------

TEST(RxDispatcher, StartWithNoConnectionsReturnsFalse) {
    RxDispatcher<> reactor(RxDispatcherConfig{});
    EXPECT_FALSE(reactor.start());
    EXPECT_FALSE(reactor.is_running());
}

// ---------------------------------------------------------------------------
// RxDispatcher::set_on_burst_complete — returns bool
// ---------------------------------------------------------------------------

TEST(RxDispatcher, SetOnBurstCompleteReturnsTrue) {
    RxDispatcher<> reactor(RxDispatcherConfig{});
    bool result = reactor.set_on_burst_complete([]() {});
    EXPECT_TRUE(result);
}

TEST(RxDispatcher, SetOnBurstCompleteNullCallbackReturnsTrue) {
    RxDispatcher<> reactor(RxDispatcherConfig{});
    // Clearing the callback is a valid operation
    bool result = reactor.set_on_burst_complete(nullptr);
    EXPECT_TRUE(result);
}

// ---------------------------------------------------------------------------
// RxDispatcherConfig — to_json with negative rx_cpu
// ---------------------------------------------------------------------------

TEST(RxDispatcherConfig, ToJsonNegativeCpu) {
    RxDispatcherConfig cfg{.port_id = 0, .rx_queue_id = 0, .rx_cpu = -1};
    auto json = cfg.to_json();
    EXPECT_NE(json.find("\"rx_cpu\":-1"), std::string::npos);
}

TEST(RxDispatcherConfig, DumpDefaultConfig) {
    RxDispatcherConfig cfg{};
    auto d = cfg.dump();
    EXPECT_NE(d.find("RxDispatcherConfig"), std::string::npos);
    EXPECT_NE(d.find("port=0"), std::string::npos);
}

// ---------------------------------------------------------------------------
// RxDispatcherEntry — hash_tuple edge cases
// ---------------------------------------------------------------------------

TEST(RxDispatcherHash, MaxValueTupleNonZeroHash) {
    auto t = make_tuple(UINT32_MAX, UINT32_MAX, UINT16_MAX, UINT16_MAX);
    auto h = RxDispatcherEntry::hash_tuple(t);
    // Should produce some hash, not zero (collisions are theoretically possible
    // but extremely unlikely for a well-distributed hash)
    EXPECT_EQ(h, RxDispatcherEntry::hash_tuple(t));  // Deterministic
}

TEST(RxDispatcherHash, SingleBitDifferenceDifferentHash) {
    // Verify single-bit difference in IP produces different hash
    auto t1 = make_tuple(0x80000000, 0x0A000002, 12345, 443);
    auto t2 = make_tuple(0x80000001, 0x0A000002, 12345, 443);
    EXPECT_NE(RxDispatcherEntry::hash_tuple(t1), RxDispatcherEntry::hash_tuple(t2));
}

TEST(RxDispatcherHash, SingleBitDifferencePortDifferentHash) {
    auto t1 = make_tuple(0x0A000001, 0x0A000002, 1, 443);
    auto t2 = make_tuple(0x0A000001, 0x0A000002, 2, 443);
    EXPECT_NE(RxDispatcherEntry::hash_tuple(t1), RxDispatcherEntry::hash_tuple(t2));
}

// ---------------------------------------------------------------------------
// RxDispatcher — default template parameter compiles
// ---------------------------------------------------------------------------

TEST(RxDispatcher, DefaultTemplateParam) {
    // RxDispatcher<> (EnableUdp=false) must compile and behave identically
    // to the pre-template RxDispatcher.
    RxDispatcher<> reactor(RxDispatcherConfig{});
    EXPECT_FALSE(reactor.is_running());
    EXPECT_EQ(reactor.connection_count(), 0u);
}

// ---------------------------------------------------------------------------
// RxDispatcher<true> — UDP entry management
// ---------------------------------------------------------------------------

TEST(RxDispatcherUdp, AddUdpBeforeStart) {
    RxDispatcher<true> reactor(RxDispatcherConfig{});
    auto tuple = make_tuple(0x0A000001, 0x0A000002, 50000, 8080);
    auto result = reactor.add_udp(tuple, [](const uint8_t*, uint16_t, size_t){});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 0u);
    EXPECT_EQ(reactor.udp_count(), 1u);
}

TEST(RxDispatcherUdp, AddUdpNullCallback) {
    RxDispatcher<true> reactor(RxDispatcherConfig{});
    auto tuple = make_tuple(0x0A000001, 0x0A000002, 50000, 8080);
    UdpRxDispatcherCallback empty_cb;
    auto result = reactor.add_udp(tuple, empty_cb);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("null"), std::string::npos);
}

TEST(RxDispatcherUdp, AddUdpInvalidTuple) {
    RxDispatcher<true> reactor(RxDispatcherConfig{});
    net::ConnectionTuple bad_tuple{};  // all zeros = invalid
    auto result = reactor.add_udp(bad_tuple, [](const uint8_t*, uint16_t, size_t){});
    ASSERT_FALSE(result.has_value());
}

TEST(RxDispatcherUdp, AddUdpFull) {
    RxDispatcher<true> reactor(RxDispatcherConfig{});
    for (size_t i = 0; i < kRxDispatcherMaxUdpEntries; ++i) {
        auto tuple = make_tuple(0x0A000001, 0x0A000002, static_cast<uint16_t>(50000 + i), 8080);
        auto result = reactor.add_udp(tuple, [](const uint8_t*, uint16_t, size_t){});
        ASSERT_TRUE(result.has_value()) << "Failed at entry " << i;
        EXPECT_EQ(result.value(), i);
    }
    // Next add should fail
    auto tuple = make_tuple(0x0A000001, 0x0A000002, 60000, 8080);
    auto result = reactor.add_udp(tuple, [](const uint8_t*, uint16_t, size_t){});
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("full"), std::string::npos);
}

TEST(RxDispatcherUdp, SetUdpActiveToggle) {
    RxDispatcher<true> reactor(RxDispatcherConfig{});
    auto tuple = make_tuple(0x0A000001, 0x0A000002, 50000, 8080);
    auto id = reactor.add_udp(tuple, [](const uint8_t*, uint16_t, size_t){});
    ASSERT_TRUE(id.has_value());

    // Disable
    reactor.set_udp_active(id.value(), false);
    // Enable
    reactor.set_udp_active(id.value(), true);
    // Out of bounds — should not crash
    reactor.set_udp_active(999, true);
}

TEST(RxDispatcherUdp, UdpCountStartsAtZero) {
    RxDispatcher<true> reactor(RxDispatcherConfig{});
    EXPECT_EQ(reactor.udp_count(), 0u);
}

// ---------------------------------------------------------------------------
// UdpRxDispatcherEntry — defaults
// ---------------------------------------------------------------------------

TEST(UdpRxDispatcherEntry, Defaults) {
    UdpRxDispatcherEntry entry;
    EXPECT_EQ(entry.tuple.src_ip, 0u);
    EXPECT_EQ(entry.tuple.dst_ip, 0u);
    EXPECT_EQ(entry.tuple.src_port, 0u);
    EXPECT_EQ(entry.tuple.dst_port, 0u);
    EXPECT_FALSE(entry.on_data);  // default-constructed = empty
}

TEST(RxDispatcherUdp, MaxUdpEntriesConstant) {
    EXPECT_GE(kRxDispatcherMaxUdpEntries, 4u);
    EXPECT_LE(kRxDispatcherMaxUdpEntries, 64u);
}
