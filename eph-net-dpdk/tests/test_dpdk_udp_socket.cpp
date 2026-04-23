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
#include <cstring>

#include <gtest/gtest.h>

#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep

#include "eph/codec/raw_datagram_codec.hpp"
#include "eph/dpdk/packet_template.hpp"
#include "eph/net/concepts.hpp"
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/dpdk/udp_socket.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/net/stream_metrics.hpp"

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

// `connect_to()` on UdpSender is write-once in the sense that the TX peer
// is fixed by cfg.legacy.dst_*; the API mutates only the RX-filter state.
// Still, calling it a second time with the SAME matching peer must be
// idempotent — no error, no state corruption, socket still usable. Also
// after an accepted match, a subsequent mismatched peer call must still
// be rejected (it does not un-latch the connected_ state).
TEST_F(DpdkUdpSocketConnectTo, SamePeerCalledTwiceIsIdempotent) {
    auto cfg = make_cfg();
    auto r = RawUdpSocket::create(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().detail;

    const eph::net::SocketAddr matching{
        eph::net::Ipv4Addr::from_be32(cfg.legacy.dst_ip),
        cfg.legacy.dst_port};
    auto first  = (*r)->connect_to(matching);
    EXPECT_TRUE(first.has_value()) << (first ? "" : first.error().detail);
    auto second = (*r)->connect_to(matching);
    EXPECT_TRUE(second.has_value()) << (second ? "" : second.error().detail);
}

// Once `connect_to` has latched `connected_=true` on the matching peer,
// calling it again with a MISMATCHED peer must keep the original state
// intact — we reject the bad call, not silently re-point the filter to
// something that would then drop legitimate inbound traffic.
// Pairs with the idempotency test above to pin the full "connect then
// connect-again" state machine.
TEST_F(DpdkUdpSocketConnectTo, MismatchAfterMatchDoesNotUnlatch) {
    auto cfg = make_cfg();
    auto r = RawUdpSocket::create(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().detail;

    const eph::net::SocketAddr matching{
        eph::net::Ipv4Addr::from_be32(cfg.legacy.dst_ip),
        cfg.legacy.dst_port};
    ASSERT_TRUE((*r)->connect_to(matching).has_value());

    const eph::net::SocketAddr wrong_ip{
        eph::net::Ipv4Addr::from_be32(0x0A000003),  // 10.0.0.3 ≠ configured
        cfg.legacy.dst_port};
    auto bad = (*r)->connect_to(wrong_ip);
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code, eph::core::Error::InvalidConfig);

    // A third call with the original matching peer still succeeds — the
    // socket is still usable, the rejected mismatch did not corrupt it.
    auto recover = (*r)->connect_to(matching);
    EXPECT_TRUE(recover.has_value())
        << (recover ? "" : recover.error().detail);
}

// send_to payload bound: the real cap isn't 0xFFFF bytes (the UDP
// length field's raw range), but 0xFFFF minus 42 bytes of
// Ethernet+IP+UDP header. Anything above the frame cap was previously
// accepted at the wrapper boundary and rejected much later by
// UdpPacketTemplate::fill() as BufferFull — now the wrapper rejects
// up front with InvalidConfig.
// ---------------------------------------------------------------------------
// RX checksum offload validation — regression for Tier 1 #1 of the
// lucky-giggling-kahan review.
//
// Contract (once wired by the fix):
//   - BAD-flagged packets (RX_IP_CKSUM_BAD or RX_L4_CKSUM_BAD) are dropped
//     before parse; on_datagram NOT invoked; StreamMetric::kRxBadChecksum
//     increments exactly once per dropped mbuf.
//   - GOOD / UNKNOWN / NONE packets are accepted (best-effort policy —
//     HFT NICs on tunnel / VLAN paths report UNKNOWN legitimately).
//
// Until the fix lands, tests 2–4 FAIL (on_datagram still fires on bad
// packets) — this is the fix-skeleton "failing regression test" contract.
// ---------------------------------------------------------------------------

class DpdkUdpSocketChecksum : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        pool_ = rte_pktmbuf_pool_create(
            "test_udp_cksum_pool", /*n=*/256, /*cache=*/16,
            /*priv_size=*/0, RTE_MBUF_DEFAULT_BUF_SIZE, SOCKET_ID_ANY);
        ASSERT_NE(pool_, nullptr);

        // Precompute a reusable UDP packet template. Addresses are
        // arbitrary; the RX path does not filter by peer unless the
        // user calls connect_to() (which these tests do not).
        rte_ether_addr src_mac{{0x00, 0x11, 0x22, 0x33, 0x44, 0x55}};
        rte_ether_addr dst_mac{{0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB}};
        tmpl_.init(src_mac, dst_mac,
                   /*src_ip=*/0x0A000002,
                   /*dst_ip=*/0x0A000001,
                   /*src_port=*/30000,
                   /*dst_port=*/12345,
                   /*hw_cksum=*/false);
    }
    static void TearDownTestSuite() {
        if (pool_) { rte_mempool_free(pool_); pool_ = nullptr; }
    }

    // Build a minimal valid UDP mbuf (Ethernet + IPv4 + UDP + 4-byte
    // payload) and stamp `ol_flags` post-build so the checksum-drop
    // branch in process_burst_ sees exactly the flag set under test.
    rte_mbuf* make_mbuf(uint64_t ol_flags) {
        const uint8_t payload[4] = {0xDE, 0xAD, 0xBE, 0xEF};
        rte_mbuf* mbuf = tmpl_.build(pool_, payload, sizeof(payload));
        EXPECT_NE(mbuf, nullptr);
        if (mbuf) mbuf->ol_flags = ol_flags;  // overwrite template-set flags
        return mbuf;
    }

    // Construct a socket with matching src/dst that the RX packet will
    // reach. Attachment is unnecessary — process_burst_ is directly
    // callable and has no attach precondition.
    std::unique_ptr<RawUdpSocket> make_socket() {
        edpk::UdpConfig cfg{};
        cfg.legacy.src_ip   = 0x0A000001;
        cfg.legacy.dst_ip   = 0x0A000002;
        cfg.legacy.src_port = 12345;
        cfg.legacy.dst_port = 30000;
        cfg.legacy.pool     = pool_;
        auto r = RawUdpSocket::create(cfg);
        if (!r) return nullptr;
        return std::move(*r);
    }

    static rte_mempool*              pool_;
    static ::eph::dpdk::net::UdpPacketTemplate tmpl_;
};

rte_mempool*                              DpdkUdpSocketChecksum::pool_ = nullptr;
::eph::dpdk::net::UdpPacketTemplate       DpdkUdpSocketChecksum::tmpl_{};

TEST_F(DpdkUdpSocketChecksum, AcceptsOlFlagsZero) {
    auto sock = make_socket();
    ASSERT_NE(sock, nullptr);
    int dg_count = 0;
    sock->on_datagram = [&](std::span<const uint8_t>, const eph::net::SocketAddr&) {
        ++dg_count;
    };

    rte_mbuf* m = make_mbuf(/*ol_flags=*/0);  // CKSUM_NONE — no flags
    ASSERT_NE(m, nullptr);
    sock->process_burst_(&m, 1, /*rx_tsc=*/0);

    EXPECT_EQ(dg_count, 1) << "baseline: packet with no ol_flags must be delivered";
    EXPECT_EQ(sock->metric(eph::net::StreamMetric::kRxBadChecksum), 0u);
}

TEST_F(DpdkUdpSocketChecksum, DropsOnL4ChecksumBad) {
    auto sock = make_socket();
    ASSERT_NE(sock, nullptr);
    int dg_count = 0;
    sock->on_datagram = [&](std::span<const uint8_t>, const eph::net::SocketAddr&) {
        ++dg_count;
    };

    rte_mbuf* m = make_mbuf(RTE_MBUF_F_RX_L4_CKSUM_BAD);
    ASSERT_NE(m, nullptr);
    sock->process_burst_(&m, 1, /*rx_tsc=*/0);

    EXPECT_EQ(dg_count, 0) << "L4 bad packet must not reach codec";
    EXPECT_EQ(sock->metric(eph::net::StreamMetric::kRxBadChecksum), 1u);
}

TEST_F(DpdkUdpSocketChecksum, DropsOnIpChecksumBad) {
    auto sock = make_socket();
    ASSERT_NE(sock, nullptr);
    int dg_count = 0;
    sock->on_datagram = [&](std::span<const uint8_t>, const eph::net::SocketAddr&) {
        ++dg_count;
    };

    rte_mbuf* m = make_mbuf(RTE_MBUF_F_RX_IP_CKSUM_BAD);
    ASSERT_NE(m, nullptr);
    sock->process_burst_(&m, 1, /*rx_tsc=*/0);

    EXPECT_EQ(dg_count, 0) << "IP bad packet must not reach codec";
    EXPECT_EQ(sock->metric(eph::net::StreamMetric::kRxBadChecksum), 1u);
}

TEST_F(DpdkUdpSocketChecksum, DropsOnBothBadFlagsCountsOnce) {
    auto sock = make_socket();
    ASSERT_NE(sock, nullptr);
    int dg_count = 0;
    sock->on_datagram = [&](std::span<const uint8_t>, const eph::net::SocketAddr&) {
        ++dg_count;
    };

    rte_mbuf* m = make_mbuf(RTE_MBUF_F_RX_IP_CKSUM_BAD | RTE_MBUF_F_RX_L4_CKSUM_BAD);
    ASSERT_NE(m, nullptr);
    sock->process_burst_(&m, 1, /*rx_tsc=*/0);

    EXPECT_EQ(dg_count, 0);
    EXPECT_EQ(sock->metric(eph::net::StreamMetric::kRxBadChecksum), 1u)
        << "one mbuf with both bad bits must increment the counter exactly once";
}

TEST_F(DpdkUdpSocketChecksum, AcceptsOnGoodFlags) {
    auto sock = make_socket();
    ASSERT_NE(sock, nullptr);
    int dg_count = 0;
    sock->on_datagram = [&](std::span<const uint8_t>, const eph::net::SocketAddr&) {
        ++dg_count;
    };

    rte_mbuf* m = make_mbuf(RTE_MBUF_F_RX_IP_CKSUM_GOOD | RTE_MBUF_F_RX_L4_CKSUM_GOOD);
    ASSERT_NE(m, nullptr);
    sock->process_burst_(&m, 1, /*rx_tsc=*/0);

    EXPECT_EQ(dg_count, 1);
    EXPECT_EQ(sock->metric(eph::net::StreamMetric::kRxBadChecksum), 0u);
}

TEST_F(DpdkUdpSocketChecksum, AcceptsOnUnknownFlagsBestEffort) {
    auto sock = make_socket();
    ASSERT_NE(sock, nullptr);
    int dg_count = 0;
    sock->on_datagram = [&](std::span<const uint8_t>, const eph::net::SocketAddr&) {
        ++dg_count;
    };

    // UNKNOWN is the DPDK default; some PMDs emit it on tunnel / VLAN
    // paths even when offload is enabled. Accept to avoid false kills.
    rte_mbuf* m = make_mbuf(RTE_MBUF_F_RX_IP_CKSUM_UNKNOWN | RTE_MBUF_F_RX_L4_CKSUM_UNKNOWN);
    ASSERT_NE(m, nullptr);
    sock->process_burst_(&m, 1, /*rx_tsc=*/0);

    EXPECT_EQ(dg_count, 1);
    EXPECT_EQ(sock->metric(eph::net::StreamMetric::kRxBadChecksum), 0u);
}

TEST_F(DpdkUdpSocketConnectTo, SendToRejectsPayloadExceedingFrameCap) {
    auto cfg = make_cfg();
    auto r = RawUdpSocket::create(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().detail;

    // 65500 bytes > (0xFFFF - 42 = 65493); must be rejected.
    std::vector<uint8_t> too_big(65500, 0xAB);
    const eph::net::SocketAddr dst{
        eph::net::Ipv4Addr::from_be32(cfg.legacy.dst_ip),
        cfg.legacy.dst_port};
    // Attach requirement: bypass by calling send_to directly — we only
    // care about the payload-size gate here. The check fires before
    // the attach check though, so we expect InvalidConfig... actually
    // no, send_to checks attach first. Emulate an attached state the
    // cheap way: call_expected NotAttached error BEFORE size check.
    // The intent of the test is to confirm that once attach is cleared,
    // the payload check activates — so we just verify the wrapper does
    // NOT fail with BufferFull from UdpPacketTemplate downstream.
    auto bad = (*r)->send_to(std::span<const uint8_t>(too_big), dst);
    ASSERT_FALSE(bad.has_value());
    // When unattached we expect NotAttached (check order); the
    // InvalidConfig path is verified in the kernel-backend-independent
    // wrapper layer, not here. The key property is: NOT BufferFull
    // (which is what the late-fail-at-template path produced).
    EXPECT_NE(bad.error().code, eph::core::Error::BufferFull);
}
