/// @file test_dpdk_udp_multicast.cpp
/// Phase 5: surface tests for `DpdkUdpSocket::join_multicast`,
/// `leave_multicast`, and `connect_to` (the Phase 4 stubs were no-ops).
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
              "DpdkUdpSocket must still satisfy Datagram concept post-Phase 5");

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
