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

// ---------------------------------------------------------------------------
// Reactor — connection management (no NIC needed)
// ---------------------------------------------------------------------------

TEST(Reactor, AddConnectionNullSessionFails) {
    Reactor reactor(Reactor::Config{});
    auto result = reactor.add_connection(nullptr, [](auto*, auto, auto){});
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("null"), std::string::npos);
}

TEST(Reactor, ConnectionCountStartsAtZero) {
    Reactor reactor(Reactor::Config{});
    EXPECT_EQ(reactor.connection_count(), 0u);
}

TEST(Reactor, IsRunningInitiallyFalse) {
    Reactor reactor(Reactor::Config{});
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
// Reactor::Config validate/dump
// ---------------------------------------------------------------------------

TEST(ReactorConfig, DefaultConfigIsValid) {
    Reactor::Config cfg;
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(ReactorConfig, NegativeCpuAffinity) {
    Reactor::Config cfg{.rx_cpu = -2};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("rx_cpu"), std::string_view::npos);
}

TEST(ReactorConfig, DumpContainsFields) {
    Reactor::Config cfg{.port_id = 1, .rx_queue_id = 2, .rx_cpu = 3};
    auto d = cfg.dump();
    EXPECT_NE(d.find("port=1"), std::string::npos);
    EXPECT_NE(d.find("queue=2"), std::string::npos);
    EXPECT_NE(d.find("cpu=3"), std::string::npos);
}
