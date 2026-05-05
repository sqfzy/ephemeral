/// @file test_dpdk_udp_multicast.cpp
/// Surface tests for `DpdkUdpSocket::join_multicast`,
/// `leave_multicast`, and `connect_to`.
///
/// We do not exercise the live RX path here — that requires a vfio-pci
/// NIC and is covered by `tests/integration/dpdk_e2e`. What we DO check:
///
///   1. The Datagram concept still holds for the new method bodies.
///   2. Helper free-functions used by the implementation
///      (`is_multicast_ip`, `multicast_mac_from_ip`) produce the
///      RFC 1112 expected MACs for a known multicast group.
///   3. `join_multicast` rejects a non-multicast IP with InvalidConfig.
///      We can't construct a real DpdkUdpSocket without a NIC, but the
///      validation logic is testable through the `is_multicast_ip`
///      free function that the method delegates to.
///
/// Real apply-to-NIC testing happens in the integration suite.

#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep

#include "eph/codec/raw_datagram_codec.hpp"
#include "eph/dpdk/multicast.hpp"
#include "eph/net/concepts.hpp"
#include "eph/net/dpdk/udp_socket.hpp"
#include "eph/net/socket_addr.hpp"

namespace edpk = eph::net::dpdk;
namespace ec  = eph::codec;
namespace en  = eph::net;

using McastSock = edpk::DpdkUdpSocket<ec::RawDatagramCodec>;

// Concept conformance still holds with the new method bodies.
static_assert(en::Datagram<McastSock>,
              "DpdkUdpSocket must still satisfy Datagram concept");

TEST(DpdkUdpMulticast, MulticastIpRangeCheck) {
    EXPECT_TRUE(::eph::dpdk::is_multicast_ip(0xE0000001u));   // 224.0.0.1
    EXPECT_TRUE(::eph::dpdk::is_multicast_ip(0xEF010203u));   // 239.1.2.3
    EXPECT_FALSE(::eph::dpdk::is_multicast_ip(0x0A000001u));  // 10.0.0.1
    EXPECT_FALSE(::eph::dpdk::is_multicast_ip(0xFFFFFFFFu));  // broadcast (240+)
}

TEST(DpdkUdpMulticast, McastMacFromIpRfc1112) {
    // 224.0.0.1 -> 01:00:5e:00:00:01 (RFC 1112)
    auto mac = ::eph::dpdk::multicast_mac_from_ip(0xE0000001u);
    EXPECT_EQ(mac.addr_bytes[0], 0x01);
    EXPECT_EQ(mac.addr_bytes[1], 0x00);
    EXPECT_EQ(mac.addr_bytes[2], 0x5e);
    EXPECT_EQ(mac.addr_bytes[3], 0x00);
    EXPECT_EQ(mac.addr_bytes[4], 0x00);
    EXPECT_EQ(mac.addr_bytes[5], 0x01);

    // 239.1.2.3 -> 01:00:5e:01:02:03 (low 23 bits of group IP)
    auto mac2 = ::eph::dpdk::multicast_mac_from_ip(0xEF010203u);
    EXPECT_EQ(mac2.addr_bytes[3], 0x01);  // 0x81 & 0x7F = 0x01
    EXPECT_EQ(mac2.addr_bytes[4], 0x02);
    EXPECT_EQ(mac2.addr_bytes[5], 0x03);
}

TEST(DpdkUdpMulticast, SocketAddrToBe32MatchesIpFormat) {
    // Sanity: SocketAddr::to_be32() returns the host-int representation
    // expected by is_multicast_ip().
    en::SocketAddr addr{en::Ipv4Addr{224, 0, 0, 1}, 30000};
    EXPECT_TRUE(::eph::dpdk::is_multicast_ip(addr.ip.to_be32()));

    en::SocketAddr unicast{en::Ipv4Addr{10, 0, 0, 1}, 30000};
    EXPECT_FALSE(::eph::dpdk::is_multicast_ip(unicast.ip.to_be32()));
}

// MulticastGroup::validate covers four reject paths plus the happy
// path. Pin them to lock the structural contract before any
// optimisation reorders the checks.

TEST(DpdkUdpMulticast, GroupValidateAcceptsMinimalGroup) {
    ::eph::dpdk::MulticastGroup g{};
    g.group_ip   = 0xE0000001u;   // 224.0.0.1
    g.group_port = 30000;
    EXPECT_TRUE(g.validate().empty());
}

TEST(DpdkUdpMulticast, GroupValidateRejectsZeroGroupIp) {
    ::eph::dpdk::MulticastGroup g{};
    g.group_port = 30000;
    auto err = g.validate();
    ASSERT_FALSE(err.empty());
    EXPECT_NE(err.find("group_ip"), std::string_view::npos)
        << "expected group_ip rejection, got: " << err;
}

TEST(DpdkUdpMulticast, GroupValidateRejectsNonMulticastGroupIp) {
    ::eph::dpdk::MulticastGroup g{};
    g.group_ip   = 0x0A000001u;   // 10.0.0.1 — unicast
    g.group_port = 30000;
    auto err = g.validate();
    ASSERT_FALSE(err.empty());
    EXPECT_NE(err.find("multicast range"), std::string_view::npos)
        << "expected multicast-range rejection, got: " << err;
}

TEST(DpdkUdpMulticast, GroupValidateRejectsZeroGroupPort) {
    ::eph::dpdk::MulticastGroup g{};
    g.group_ip = 0xE0010203u;
    auto err = g.validate();
    ASSERT_FALSE(err.empty());
    EXPECT_NE(err.find("group_port"), std::string_view::npos);
}

TEST(DpdkUdpMulticast, GroupValidateRejectsMulticastSourceIp) {
    // SSM filter sourcing FROM a multicast address is impossible —
    // process_packet would silently drop every datagram.
    ::eph::dpdk::MulticastGroup g{};
    g.group_ip   = 0xE0010203u;
    g.group_port = 30000;
    g.source_ip  = 0xE0000001u;   // 224.0.0.1
    auto err = g.validate();
    ASSERT_FALSE(err.empty());
    EXPECT_NE(err.find("multicast"), std::string_view::npos)
        << "expected multicast-source rejection, got: " << err;
}

TEST(DpdkUdpMulticast, GroupValidateRejectsBroadcastSourceIp) {
    // 255.255.255.255 also can't validly source a real datagram.
    ::eph::dpdk::MulticastGroup g{};
    g.group_ip   = 0xE0010203u;
    g.group_port = 30000;
    g.source_ip  = 0xFFFFFFFFu;
    auto err = g.validate();
    ASSERT_FALSE(err.empty());
    EXPECT_NE(err.find("255.255.255.255"), std::string_view::npos);
}

TEST(DpdkUdpMulticast, GroupValidateAcceptsLegitimateUnicastSource) {
    ::eph::dpdk::MulticastGroup g{};
    g.group_ip   = 0xE0010203u;
    g.group_port = 30000;
    g.source_ip  = 0x0A000001u;   // 10.0.0.1 — unicast
    EXPECT_TRUE(g.validate().empty());
}

// MulticastGroup::warnings() — three advisory warning paths. Pin
// each so a future refactor that drops one is caught.
TEST(DpdkUdpMulticast, GroupWarningsLocalNetworkControlBlock) {
    // 224.0.0.x is the local network control block (RFC 5771 §4).
    // Note: the warnings() check is `(group_ip >> 8) == 0xE00000`,
    // which corresponds to `224.0.0.0/24` exactly.
    ::eph::dpdk::MulticastGroup g{};
    g.group_ip   = 0xE0000005u;   // 224.0.0.5
    g.group_port = 30000;
    auto w = g.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("local network control") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found) << "expected local-network-control warning";
}

TEST(DpdkUdpMulticast, GroupWarningsLoopbackSourceIp) {
    ::eph::dpdk::MulticastGroup g{};
    g.group_ip   = 0xE0010203u;
    g.group_port = 30000;
    g.source_ip  = 0x7F000001u;   // 127.0.0.1
    auto w = g.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("loopback") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found) << "expected loopback-source warning";
}

TEST(DpdkUdpMulticast, GroupWarningsWellKnownPort) {
    ::eph::dpdk::MulticastGroup g{};
    g.group_ip   = 0xE0010203u;
    g.group_port = 80;
    auto w = g.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("well-known") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found) << "expected well-known-port warning";
}

TEST(DpdkUdpMulticast, GroupWarningsNominalConfigEmpty) {
    ::eph::dpdk::MulticastGroup g{};
    g.group_ip   = 0xE1010203u;   // 225.1.2.3 — outside control block
    g.group_port = 30000;
    EXPECT_TRUE(g.warnings().empty());
}
