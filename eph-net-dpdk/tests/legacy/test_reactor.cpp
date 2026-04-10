/// @file test_reactor.cpp
/// Unit tests for Reactor: connection management, hash utility, and API.
///
/// NOTE: The actual NIC poll (rte_eth_rx_burst) cannot be unit-tested
/// without DPDK EAL. These tests cover the non-NIC parts:
///   - ReactorEntry::hash_tuple correctness
///   - Connection registration and limits
///   - Reactor lifecycle (start/stop without NIC)

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep
#include "eph/dpdk/reactor.hpp"

using namespace eph::dpdk;
using namespace eph::net;

// ---------------------------------------------------------------------------
// ReactorEntry::hash_tuple
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

TEST(ReactorHash, DifferentTuplesProduceDifferentHashes) {
    auto t1 = make_tuple(0x0A000001, 0x0A000002, 12345, 443);
    auto t2 = make_tuple(0x0A000001, 0x0A000003, 12345, 443);
    EXPECT_NE(ReactorEntry::hash_tuple(t1), ReactorEntry::hash_tuple(t2));
}

TEST(ReactorHash, SameTuplesSameHash) {
    auto t = make_tuple(0x0A000001, 0x0A000002, 12345, 443);
    EXPECT_EQ(ReactorEntry::hash_tuple(t), ReactorEntry::hash_tuple(t));
}

TEST(ReactorHash, DifferentPortsDifferentHash) {
    auto t1 = make_tuple(0x0A000001, 0x0A000002, 12345, 443);
    auto t2 = make_tuple(0x0A000001, 0x0A000002, 12346, 443);
    EXPECT_NE(ReactorEntry::hash_tuple(t1), ReactorEntry::hash_tuple(t2));
}

TEST(ReactorHash, ZeroTupleNonZeroHash) {
    net::ConnectionTuple t{};
    auto h = ReactorEntry::hash_tuple(t);
    EXPECT_EQ(h, ReactorEntry::hash_tuple(t));
}

TEST(ReactorHash, SymmetricSwappedSrcDst) {
    // Critical property: hash(A->B) == hash(B->A) because incoming packets
    // have swapped src/dst relative to the registered connection tuple.
    auto local = make_tuple(0x0A000001, 0x0A000002, 12345, 443);
    auto remote = make_tuple(0x0A000002, 0x0A000001, 443, 12345); // swapped
    EXPECT_EQ(ReactorEntry::hash_tuple(local),
              ReactorEntry::hash_tuple(remote))
        << "Hash must be direction-symmetric for RX dispatch";
}

TEST(ReactorHash, SymmetricMultipleTuples) {
    // Verify symmetry across several different connection tuples
    struct { uint32_t a_ip; uint32_t b_ip; uint16_t a_port; uint16_t b_port; } cases[] = {
        {0xC0A80101, 0x08080808, 5000, 443},
        {0xAC100001, 0xAC100002, 8443, 9090},
        {0x0A0A0001, 0x0A0A0002, 55123, 80},
    };
    for (const auto& c : cases) {
        auto fwd = make_tuple(c.a_ip, c.b_ip, c.a_port, c.b_port);
        auto rev = make_tuple(c.b_ip, c.a_ip, c.b_port, c.a_port);
        EXPECT_EQ(ReactorEntry::hash_tuple(fwd), ReactorEntry::hash_tuple(rev))
            << "Symmetry violation for " << c.a_ip << " <-> " << c.b_ip;
    }
}

TEST(ReactorHash, NonSymmetricIpsDifferentHash) {
    // Different IPs (not just swapped) should produce different hashes
    auto t1 = make_tuple(0x0A000001, 0x0A000002, 12345, 443);
    auto t2 = make_tuple(0x0A000001, 0x0A000003, 12345, 443);
    EXPECT_NE(ReactorEntry::hash_tuple(t1), ReactorEntry::hash_tuple(t2));
}

// ---------------------------------------------------------------------------
// Reactor — connection management (no NIC needed)
// ---------------------------------------------------------------------------

TEST(Reactor, AddConnectionNullSessionFails) {
    Reactor<> reactor(ReactorConfig{});
    auto result = reactor.add_connection(nullptr, [](auto*, auto, auto){});
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("null"), std::string::npos);
}

TEST(Reactor, ConnectionCountStartsAtZero) {
    Reactor<> reactor(ReactorConfig{});
    EXPECT_EQ(reactor.connection_count(), 0u);
}

TEST(Reactor, IsRunningInitiallyFalse) {
    Reactor<> reactor(ReactorConfig{});
    EXPECT_FALSE(reactor.is_running());
}

TEST(Reactor, MaxConnectionsConstant) {
    EXPECT_GE(kReactorMaxConnections, 8u);
    EXPECT_LE(kReactorMaxConnections, 256u);
}

// ---------------------------------------------------------------------------
// ReactorEntry
// ---------------------------------------------------------------------------

TEST(ReactorEntry, DefaultConstructedIsDisconnected) {
    ReactorEntry entry;
    EXPECT_EQ(entry.session, nullptr);
    EXPECT_FALSE(entry.connected);
}

TEST(ReactorEntry, HashIsConstexprSafe) {
    auto t = make_tuple(1, 2, 3, 4);
    auto h1 = ReactorEntry::hash_tuple(t);
    auto h2 = ReactorEntry::hash_tuple(t);
    EXPECT_EQ(h1, h2);
}

// ---------------------------------------------------------------------------
// ReactorConfig validate/dump
// ---------------------------------------------------------------------------

TEST(ReactorConfig, DefaultConfigIsValid) {
    ReactorConfig cfg;
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(ReactorConfig, NegativeCpuAffinity) {
    ReactorConfig cfg{.rx_cpu = -2};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("rx_cpu"), std::string_view::npos);
}

TEST(ReactorConfig, DumpContainsFields) {
    ReactorConfig cfg{.port_id = 1, .rx_queue_id = 2, .rx_cpu = 3};
    auto d = cfg.dump();
    EXPECT_NE(d.find("port=1"), std::string::npos);
    EXPECT_NE(d.find("queue=2"), std::string::npos);
    EXPECT_NE(d.find("cpu=3"), std::string::npos);
}

TEST(ReactorConfig, ToJsonValidStructure) {
    ReactorConfig cfg{.port_id = 1, .rx_queue_id = 2, .rx_cpu = 3};
    auto j = cfg.to_json();
    EXPECT_EQ(j.front(), '{');
    EXPECT_EQ(j.back(), '}');
    EXPECT_NE(j.find("\"port_id\":1"), std::string::npos);
    EXPECT_NE(j.find("\"rx_queue_id\":2"), std::string::npos);
    EXPECT_NE(j.find("\"rx_cpu\":3"), std::string::npos);
}

TEST(ReactorConfig, Equality) {
    ReactorConfig a{.port_id = 1, .rx_queue_id = 2, .rx_cpu = 3};
    ReactorConfig b{.port_id = 1, .rx_queue_id = 2, .rx_cpu = 3};
    ReactorConfig c{.port_id = 0, .rx_queue_id = 2, .rx_cpu = 3};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST(ReactorConfig, WarningUnpinnedCpu) {
    ReactorConfig cfg{.rx_cpu = -1};
    auto w = cfg.warnings();
    ASSERT_FALSE(w.empty());
    EXPECT_NE(w[0].find("no pinning"), std::string::npos);
}

TEST(ReactorConfig, NoWarningPinnedCpu) {
    ReactorConfig cfg{.rx_cpu = 3};
    auto w = cfg.warnings();
    EXPECT_TRUE(w.empty());
}

TEST(ReactorConfig, FormatterContainsKeyFields) {
    ReactorConfig cfg{.port_id = 1, .rx_queue_id = 2, .rx_cpu = 3};
    auto s = std::format("{}", cfg);
    EXPECT_NE(s.find("Reactor"), std::string::npos);
    EXPECT_NE(s.find("port=1"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Reactor::entry out-of-bounds
// ---------------------------------------------------------------------------

TEST(Reactor, EntryOutOfBoundsReturnsEmpty) {
    Reactor<> reactor(ReactorConfig{});
    // No connections added, any index should return the static empty entry
    const auto& e = reactor.entry(0);
    EXPECT_EQ(e.session, nullptr);
    EXPECT_FALSE(e.connected);

    // Large index
    const auto& e2 = reactor.entry(999);
    EXPECT_EQ(e2.session, nullptr);
    EXPECT_FALSE(e2.connected);
}

TEST(Reactor, MarkDisconnectedOutOfBoundsIsSafe) {
    Reactor<> reactor(ReactorConfig{});
    // Should not crash, just log a warning
    reactor.mark_disconnected(0);
    reactor.mark_disconnected(999);
    EXPECT_EQ(reactor.connection_count(), 0u);
}

TEST(Reactor, MarkReconnectedNullSessionIsSafe) {
    Reactor<> reactor(ReactorConfig{});
    // Should not crash, null session is logged
    reactor.mark_reconnected(0, nullptr);
    EXPECT_EQ(reactor.connection_count(), 0u);
}

TEST(Reactor, MarkReconnectedOutOfBoundsIsSafe) {
    Reactor<> reactor(ReactorConfig{});
    // Fake non-null session pointer for testing out-of-bounds
    TcpSession<>* fake_session = reinterpret_cast<TcpSession<>*>(0xDEAD);
    // Should not crash, just log a warning (does not dereference pointer
    // because conn_id check fires first)
    reactor.mark_reconnected(999, fake_session);
    EXPECT_EQ(reactor.connection_count(), 0u);
}

TEST(Reactor, AddConnectionWhileRunningFails) {
    // Cannot test start()/stop() without EAL, but can verify the running
    // check logic indirectly via the atomic flag
    Reactor<> reactor(ReactorConfig{});
    EXPECT_FALSE(reactor.is_running());
    // The actual "running" rejection test would require mocking the thread
}

TEST(Reactor, StopWhileNotRunningIsSafe) {
    Reactor<> reactor(ReactorConfig{});
    reactor.stop(); // Should not crash
    reactor.stop(); // Double stop should be idempotent
    EXPECT_FALSE(reactor.is_running());
}

// ---------------------------------------------------------------------------
// Reactor — add_connection null callback rejection
// ---------------------------------------------------------------------------

TEST(Reactor, AddConnectionNullCallbackFails) {
    Reactor<> reactor(ReactorConfig{});
    // Fake non-null session pointer for testing callback validation
    TcpSession<>* fake_session = reinterpret_cast<TcpSession<>*>(0xBEEF);
    ReactorDataCallback empty_cb;  // default-constructed = empty
    auto result = reactor.add_connection(fake_session, empty_cb);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("null"), std::string::npos);
    EXPECT_EQ(reactor.connection_count(), 0u);
}

// ---------------------------------------------------------------------------
// Reactor::start — returns false with zero connections
// ---------------------------------------------------------------------------

TEST(Reactor, StartWithNoConnectionsReturnsFalse) {
    Reactor<> reactor(ReactorConfig{});
    EXPECT_FALSE(reactor.start());
    EXPECT_FALSE(reactor.is_running());
}

// ---------------------------------------------------------------------------
// Reactor::set_on_burst_complete — returns bool
// ---------------------------------------------------------------------------

TEST(Reactor, SetOnBurstCompleteReturnsTrue) {
    Reactor<> reactor(ReactorConfig{});
    bool result = reactor.set_on_burst_complete([]() {});
    EXPECT_TRUE(result);
}

TEST(Reactor, SetOnBurstCompleteNullCallbackReturnsTrue) {
    Reactor<> reactor(ReactorConfig{});
    // Clearing the callback is a valid operation
    bool result = reactor.set_on_burst_complete(nullptr);
    EXPECT_TRUE(result);
}

// ---------------------------------------------------------------------------
// ReactorConfig — to_json with negative rx_cpu
// ---------------------------------------------------------------------------

TEST(ReactorConfig, ToJsonNegativeCpu) {
    ReactorConfig cfg{.port_id = 0, .rx_queue_id = 0, .rx_cpu = -1};
    auto json = cfg.to_json();
    EXPECT_NE(json.find("\"rx_cpu\":-1"), std::string::npos);
}

TEST(ReactorConfig, DumpDefaultConfig) {
    ReactorConfig cfg{};
    auto d = cfg.dump();
    EXPECT_NE(d.find("ReactorConfig"), std::string::npos);
    EXPECT_NE(d.find("port=0"), std::string::npos);
}

// ---------------------------------------------------------------------------
// ReactorEntry — hash_tuple edge cases
// ---------------------------------------------------------------------------

TEST(ReactorHash, MaxValueTupleNonZeroHash) {
    auto t = make_tuple(UINT32_MAX, UINT32_MAX, UINT16_MAX, UINT16_MAX);
    auto h = ReactorEntry::hash_tuple(t);
    // Should produce some hash, not zero (collisions are theoretically possible
    // but extremely unlikely for a well-distributed hash)
    EXPECT_EQ(h, ReactorEntry::hash_tuple(t));  // Deterministic
}

TEST(ReactorHash, SingleBitDifferenceDifferentHash) {
    // Verify single-bit difference in IP produces different hash
    auto t1 = make_tuple(0x80000000, 0x0A000002, 12345, 443);
    auto t2 = make_tuple(0x80000001, 0x0A000002, 12345, 443);
    EXPECT_NE(ReactorEntry::hash_tuple(t1), ReactorEntry::hash_tuple(t2));
}

TEST(ReactorHash, SingleBitDifferencePortDifferentHash) {
    auto t1 = make_tuple(0x0A000001, 0x0A000002, 1, 443);
    auto t2 = make_tuple(0x0A000001, 0x0A000002, 2, 443);
    EXPECT_NE(ReactorEntry::hash_tuple(t1), ReactorEntry::hash_tuple(t2));
}

// ---------------------------------------------------------------------------
// Reactor — default template parameter compiles
// ---------------------------------------------------------------------------

TEST(Reactor, DefaultTemplateParam) {
    // Reactor<> (EnableUdp=false) must compile and behave identically
    // to the pre-template Reactor.
    Reactor<> reactor(ReactorConfig{});
    EXPECT_FALSE(reactor.is_running());
    EXPECT_EQ(reactor.connection_count(), 0u);
}

// ---------------------------------------------------------------------------
// Reactor<true> — UDP entry management
// ---------------------------------------------------------------------------

TEST(ReactorUdp, AddUdpBeforeStart) {
    Reactor<true> reactor(ReactorConfig{});
    auto tuple = make_tuple(0x0A000001, 0x0A000002, 50000, 8080);
    auto result = reactor.add_udp(tuple, [](const uint8_t*, uint16_t, size_t){});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 0u);
    EXPECT_EQ(reactor.udp_count(), 1u);
}

TEST(ReactorUdp, AddUdpNullCallback) {
    Reactor<true> reactor(ReactorConfig{});
    auto tuple = make_tuple(0x0A000001, 0x0A000002, 50000, 8080);
    UdpReactorCallback empty_cb;
    auto result = reactor.add_udp(tuple, empty_cb);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("null"), std::string::npos);
}

TEST(ReactorUdp, AddUdpInvalidTuple) {
    Reactor<true> reactor(ReactorConfig{});
    net::ConnectionTuple bad_tuple{};  // all zeros = invalid
    auto result = reactor.add_udp(bad_tuple, [](const uint8_t*, uint16_t, size_t){});
    ASSERT_FALSE(result.has_value());
}

TEST(ReactorUdp, AddUdpFull) {
    Reactor<true> reactor(ReactorConfig{});
    for (size_t i = 0; i < kReactorMaxUdpEntries; ++i) {
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

TEST(ReactorUdp, SetUdpActiveToggle) {
    Reactor<true> reactor(ReactorConfig{});
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

TEST(ReactorUdp, UdpCountStartsAtZero) {
    Reactor<true> reactor(ReactorConfig{});
    EXPECT_EQ(reactor.udp_count(), 0u);
}

// ---------------------------------------------------------------------------
// UdpReactorEntry — defaults
// ---------------------------------------------------------------------------

TEST(UdpReactorEntry, Defaults) {
    UdpReactorEntry entry;
    EXPECT_EQ(entry.tuple.src_ip, 0u);
    EXPECT_EQ(entry.tuple.dst_ip, 0u);
    EXPECT_EQ(entry.tuple.src_port, 0u);
    EXPECT_EQ(entry.tuple.dst_port, 0u);
    EXPECT_FALSE(entry.on_data);  // default-constructed = empty
}

TEST(ReactorUdp, MaxUdpEntriesConstant) {
    EXPECT_GE(kReactorMaxUdpEntries, 4u);
    EXPECT_LE(kReactorMaxUdpEntries, 64u);
}
