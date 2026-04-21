/// @file test_dpdk_udp_socket.cpp
/// Unit tests for `eph::net::dpdk::DpdkUdpSocket`.
///
///   - concept conformance static_asserts (Pollable + Datagram)
///   - InvalidConfig surface: zero IPs, null pool
///   - send_to before attach returns NotAttached
///
/// Like the TCP stream tests, we do NOT exercise the live RX path here
/// (would require a vfio-pci-bound NIC). The unit tests focus on the
/// type system, configuration plumbing, and pre-attach guards.

#include <cstdint>

#include <gtest/gtest.h>

#include <rte_mbuf.h>
#include <rte_mempool.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep

#include "eph/codec/raw_datagram_codec.hpp"
#include "eph/net/concepts.hpp"
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/dpdk/udp_socket.hpp"
#include "eph/net/socket_addr.hpp"

namespace edpk = eph::net::dpdk;
namespace ec  = eph::codec;

using RawUdpSocket = edpk::DpdkUdpSocket<ec::RawDatagramCodec>;

// ---------------------------------------------------------------------------
// Concept conformance
// ---------------------------------------------------------------------------

static_assert(eph::net::Pollable<RawUdpSocket>,
              "DpdkUdpSocket<RawDatagramCodec> must be Pollable");
static_assert(eph::net::Datagram<RawUdpSocket>,
              "DpdkUdpSocket<RawDatagramCodec> must be Datagram");
static_assert(eph::net::dpdk::DpdkPollable<RawUdpSocket>,
              "DpdkUdpSocket<RawDatagramCodec> must be DpdkPollable");

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(DpdkUdpSocket, ZeroSrcIpFailsInvalidConfig) {
    edpk::UdpConfig cfg{};
    // Default-init: src_ip == 0, dst_ip == 0, pool == nullptr.
    // Validation should reject before any DPDK call.
    auto r = RawUdpSocket::create(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
}

TEST(DpdkUdpSocket, NullPoolFailsInvalidConfig) {
    edpk::UdpConfig cfg{};
    cfg.legacy.src_ip   = 0x0A000001;
    cfg.legacy.dst_ip   = 0x0A000002;
    cfg.legacy.src_port = 12345;
    cfg.legacy.dst_port = 30000;
    cfg.legacy.pool     = nullptr;  // explicit
    auto r = RawUdpSocket::create(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
}

// Compile-time check that the type-aliased Datagram concept arguments
// are correct (catches accidental drift in associated types).
TEST(DpdkUdpSocket, AssociatedTypesPresent) {
    using S = RawUdpSocket;
    static_assert(std::is_same_v<S::CodecType, ec::RawDatagramCodec>);
    static_assert(std::is_same_v<S::PacketView, edpk::detail::MbufView>);
    EXPECT_TRUE((eph::net::Datagram<S>));
}

// ---------------------------------------------------------------------------
// connect_to peer validation — UdpSender is a precomputed-template fixed-
// peer sender, so `connect_to(peer)` must reject any peer that does not
// match `cfg.legacy.dst_*`. Otherwise TX goes to the real server but the
// RX filter drops everything, leaving the socket silently hung.
// ---------------------------------------------------------------------------

class DpdkUdpSocketConnectTo : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        pool_ = rte_pktmbuf_pool_create(
            "test_udp_connect_pool", /*n=*/64, /*cache=*/16,
            /*priv_size=*/0, RTE_MBUF_DEFAULT_BUF_SIZE, SOCKET_ID_ANY);
        ASSERT_NE(pool_, nullptr);
    }
    static void TearDownTestSuite() {
        if (pool_) { rte_mempool_free(pool_); pool_ = nullptr; }
    }

    static edpk::UdpConfig make_cfg() {
        edpk::UdpConfig cfg{};
        cfg.legacy.src_ip   = 0x0A000001;  // 10.0.0.1
        cfg.legacy.dst_ip   = 0x0A000002;  // 10.0.0.2
        cfg.legacy.src_port = 12345;
        cfg.legacy.dst_port = 30000;
        cfg.legacy.pool     = pool_;
        return cfg;
    }

    static rte_mempool* pool_;
};

rte_mempool* DpdkUdpSocketConnectTo::pool_ = nullptr;

TEST_F(DpdkUdpSocketConnectTo, AcceptsPeerMatchingConfiguredDst) {
    auto cfg = make_cfg();
    auto r = RawUdpSocket::create(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().detail;

    const eph::net::SocketAddr matching{
        eph::net::Ipv4Addr::from_be32(cfg.legacy.dst_ip),
        cfg.legacy.dst_port};
    auto ok = (*r)->connect_to(matching);
    EXPECT_TRUE(ok.has_value()) << (ok ? "" : ok.error().detail);
}

TEST_F(DpdkUdpSocketConnectTo, RejectsPeerWithMismatchedIp) {
    auto cfg = make_cfg();
    auto r = RawUdpSocket::create(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().detail;

    const eph::net::SocketAddr wrong_ip{
        eph::net::Ipv4Addr::from_be32(0x0A000003),  // 10.0.0.3 ≠ configured dst
        cfg.legacy.dst_port};
    auto bad = (*r)->connect_to(wrong_ip);
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code, eph::core::Error::InvalidConfig);
}

TEST_F(DpdkUdpSocketConnectTo, RejectsPeerWithMismatchedPort) {
    auto cfg = make_cfg();
    auto r = RawUdpSocket::create(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().detail;

    const eph::net::SocketAddr wrong_port{
        eph::net::Ipv4Addr::from_be32(cfg.legacy.dst_ip),
        static_cast<uint16_t>(cfg.legacy.dst_port + 1)};  // port ≠ configured
    auto bad = (*r)->connect_to(wrong_port);
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code, eph::core::Error::InvalidConfig);
}
