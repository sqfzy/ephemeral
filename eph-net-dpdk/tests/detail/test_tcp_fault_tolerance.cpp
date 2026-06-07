/// @file test_tcp_fault_tolerance.cpp
/// Fault-tolerance regression tests for DPDK TCP and packet infrastructure.
///
/// Tests cover edge cases that were hardened:
///   1. FIN in unexpected TCP states must NOT advance rcv_nxt_.
///   2. Null-payload guard in the data callback path.
///   3. PacketTemplate overflow guard for large payload_len.
///   4. MbufView::trim_front on default-constructed (null) views.
///   5. EAL double-init protection.

#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_tcp.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep
#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/net_header.hpp"
#include "eph/dpdk/packet_template.hpp"
#include "eph/dpdk/tcp.hpp"
#include "eph/net/dpdk/detail/mbuf_view.hpp"

using namespace eph::dpdk;
using eph::net::TcpState;

namespace {

// ─────────────────────────────────────────────────────────────────────────
// Test helpers (same convention as test_tcp_state_machine.cpp)
// ─────────────────────────────────────────────────────────────────────────

constexpr uint32_t kSrcIp   = 0x0A000001;
constexpr uint32_t kDstIp   = 0x0A000002;
constexpr uint16_t kSrcPort = 55000;
constexpr uint16_t kDstPort = 443;

TcpConfig make_test_config() {
    TcpConfig cfg;
    cfg.tuple.src_ip   = kSrcIp;
    cfg.tuple.dst_ip   = kDstIp;
    cfg.tuple.src_port = kSrcPort;
    cfg.tuple.dst_port = kDstPort;
    cfg.mss            = 1460;
    cfg.recv_window    = 65535;
    cfg.port_id        = 0;
    cfg.tx_queue_id    = 0;
    cfg.rx_queue_id    = 0;
    return cfg;
}

struct FakeTcpMbuf {
    alignas(8) uint8_t buf[256];
    rte_mbuf mbuf{};

    void build(uint32_t seq, uint32_t ack_num, uint8_t flags,
               uint16_t window = 65535, const uint8_t* payload = nullptr,
               size_t payload_len = 0) {
        constexpr size_t eth_len = net::kEtherHeaderLen;
        constexpr size_t ip_len  = 20;
        constexpr size_t tcp_len = 20;
        size_t total = eth_len + ip_len + tcp_len + payload_len;
        ASSERT_LE(total, sizeof(buf));

        std::memset(buf, 0, total);

        auto* eth = reinterpret_cast<rte_ether_hdr*>(buf);
        eth->ether_type = net::hton16(net::kEtherTypeIpv4);

        auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + eth_len);
        ip->version_ihl  = (4 << 4) | 5;
        ip->total_length  = net::hton16(
            static_cast<uint16_t>(ip_len + tcp_len + payload_len));
        ip->next_proto_id = net::kIpProtoTcp;
        ip->src_addr      = net::hton32(kDstIp);
        ip->dst_addr      = net::hton32(kSrcIp);

        auto* tcp = reinterpret_cast<rte_tcp_hdr*>(buf + eth_len + ip_len);
        tcp->src_port  = net::hton16(kDstPort);
        tcp->dst_port  = net::hton16(kSrcPort);
        tcp->sent_seq  = net::hton32(seq);
        tcp->recv_ack  = net::hton32(ack_num);
        tcp->data_off  = static_cast<uint8_t>(5 << 4);
        tcp->tcp_flags = flags;
        tcp->rx_win    = net::hton16(window);

        if (payload && payload_len > 0) {
            std::memcpy(buf + eth_len + ip_len + tcp_len, payload, payload_len);
        }

        mbuf = rte_mbuf{};
        mbuf.buf_addr = buf;
        mbuf.data_off = 0;
        mbuf.data_len = static_cast<uint16_t>(total);
        mbuf.pkt_len  = static_cast<uint32_t>(total);
    }
};

template <typename Session>
auto drive(Session& s, FakeTcpMbuf& fake) {
    rte_mbuf* p = &fake.mbuf;
    return s.process_rx(&p, 1, [](const uint8_t*, uint16_t) {});
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// FIN in unexpected states must NOT advance rcv_nxt_
// ═══════════════════════════════════════════════════════════════════════

TEST(FaultTolerance, FinInCloseWaitDoesNotAdvanceRcvNxt) {
    // CloseWait means peer's FIN was already consumed and rcv_nxt_
    // includes it. A duplicate/retransmitted FIN must be dropped
    // without advancing the sequence counter.
    auto cfg = make_test_config();
    TcpSession<> s(cfg, nullptr);
    s.inject_state_for_testing(TcpState::CloseWait);
    s.inject_send_seq_for_testing(1000, 1000);
    s.inject_recv_seq_for_testing(2001, 8192); // 2001 = already past FIN

    // Craft a FIN at exactly rcv_nxt_ — this should NOT be processed
    // because we're already in CloseWait.
    FakeTcpMbuf fake;
    fake.build(/*seq=*/2001, /*ack=*/1000,
               static_cast<uint8_t>(net::kTcpFin | net::kTcpAck));

    auto r = drive(s, fake);
    ASSERT_TRUE(r.has_value());
    // rcv_nxt_ must stay at 2001, not advance to 2002.
    EXPECT_EQ(s.state(), TcpState::CloseWait) << "State must not change";
}

TEST(FaultTolerance, FinInLastAckDoesNotAdvanceRcvNxt) {
    auto cfg = make_test_config();
    TcpSession<> s(cfg, nullptr);
    s.inject_state_for_testing(TcpState::LastAck);
    s.inject_send_seq_for_testing(1000, 999);
    s.inject_recv_seq_for_testing(2001, 8192);

    FakeTcpMbuf fake;
    fake.build(/*seq=*/2001, /*ack=*/999,
               static_cast<uint8_t>(net::kTcpFin | net::kTcpAck));

    auto r = drive(s, fake);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(s.state(), TcpState::LastAck) << "State must not change";
}

TEST(FaultTolerance, FinInTimeWaitDoesNotAdvanceRcvNxt) {
    auto cfg = make_test_config();
    TcpSession<> s(cfg, nullptr);
    s.inject_state_for_testing(TcpState::TimeWait);
    s.inject_send_seq_for_testing(1000, 1000);
    s.inject_recv_seq_for_testing(2001, 8192);

    FakeTcpMbuf fake;
    fake.build(/*seq=*/2001, /*ack=*/1000,
               static_cast<uint8_t>(net::kTcpFin | net::kTcpAck));

    auto r = drive(s, fake);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(s.state(), TcpState::TimeWait) << "State must not change";
}

// ═══════════════════════════════════════════════════════════════════════
// FIN in valid states still works correctly
// ═══════════════════════════════════════════════════════════════════════

TEST(FaultTolerance, FinInEstablishedStillTransitions) {
    // Verify our fix didn't break normal FIN handling.
    auto cfg = make_test_config();
    TcpSession<> s(cfg, nullptr);
    s.inject_state_for_testing(TcpState::Established);
    s.inject_send_seq_for_testing(1000, 1000);
    s.inject_recv_seq_for_testing(2000, 8192);

    FakeTcpMbuf fake;
    fake.build(/*seq=*/2000, /*ack=*/1000,
               static_cast<uint8_t>(net::kTcpFin | net::kTcpAck));

    auto r = drive(s, fake);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(s.state(), TcpState::CloseWait);
}

TEST(FaultTolerance, FinInFinWait2TransitionsToTimeWait) {
    auto cfg = make_test_config();
    TcpSession<> s(cfg, nullptr);
    s.inject_state_for_testing(TcpState::FinWait2);
    s.inject_send_seq_for_testing(1000, 1000);
    s.inject_recv_seq_for_testing(2000, 8192);

    FakeTcpMbuf fake;
    fake.build(/*seq=*/2000, /*ack=*/1000,
               static_cast<uint8_t>(net::kTcpFin | net::kTcpAck));

    auto r = drive(s, fake);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(s.state(), TcpState::TimeWait);
}

TEST(FaultTolerance, SimultaneousCloseFinWait1ToClosing) {
    auto cfg = make_test_config();
    TcpSession<> s(cfg, nullptr);
    s.inject_state_for_testing(TcpState::FinWait1);
    s.inject_send_seq_for_testing(1000, 999);
    s.inject_recv_seq_for_testing(2000, 8192);

    // FIN without ACK of our FIN → simultaneous close → CLOSING
    FakeTcpMbuf fake;
    fake.build(/*seq=*/2000, /*ack=*/999,
               static_cast<uint8_t>(net::kTcpFin));

    auto r = drive(s, fake);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(s.state(), TcpState::Closing);
}

// RFC 793 §3.5: a segment may carry FIN together with data. The FIN
// occupies one sequence number AT (seg_seq + payload_len), not at
// seg_seq. Confirm process_rx delivers the data, transitions to
// CloseWait, and advances rcv_nxt_ past the FIN — the regression
// (pre-fix) dropped the FIN as "out-of-order" because the legacy
// in-order check compared parsed.seq() against the post-payload
// rcv_nxt_, which they never equal when payload_len > 0.
TEST(FaultTolerance, FinWithDataInEstablishedConsumesBoth) {
    auto cfg = make_test_config();
    TcpSession<> s(cfg, nullptr);
    s.inject_state_for_testing(TcpState::Established);
    s.inject_send_seq_for_testing(1000, 1000);
    s.inject_recv_seq_for_testing(2000, 8192);

    constexpr uint8_t payload[] = {0xAB, 0xCD, 0xEF, 0x12, 0x34};
    constexpr size_t  payload_len = sizeof(payload);

    FakeTcpMbuf fake;
    fake.build(/*seq=*/2000, /*ack=*/1000,
               static_cast<uint8_t>(net::kTcpFin | net::kTcpAck),
               /*window=*/65535, payload, payload_len);

    bool data_seen = false;
    uint16_t data_len_seen = 0;
    rte_mbuf* p = &fake.mbuf;
    auto r = s.process_rx(&p, 1,
        [&](const uint8_t* d, uint16_t len) {
            data_seen = true;
            data_len_seen = len;
            (void)d;
        });
    ASSERT_TRUE(r.has_value());

    // Data callback must fire even though the same segment carries FIN.
    EXPECT_TRUE(data_seen);
    EXPECT_EQ(data_len_seen, payload_len);
    // FIN consumes one byte AFTER the payload.
    // rcv_nxt_ must end at 2000 + payload_len + 1 (FIN).
    EXPECT_EQ(s.rcv_nxt(), 2000u + payload_len + 1u);
    // Established + FIN(+ACK) → CloseWait per RFC 793 §3.5.
    EXPECT_EQ(s.state(), TcpState::CloseWait);
}

// Companion case: data + FIN arriving in FinWait2 must complete the
// half-close → TimeWait transition.
TEST(FaultTolerance, FinWithDataInFinWait2ToTimeWait) {
    auto cfg = make_test_config();
    TcpSession<> s(cfg, nullptr);
    s.inject_state_for_testing(TcpState::FinWait2);
    s.inject_send_seq_for_testing(1000, 1000);
    s.inject_recv_seq_for_testing(2000, 8192);

    constexpr uint8_t payload[] = {0x01, 0x02, 0x03};
    constexpr size_t  payload_len = sizeof(payload);

    FakeTcpMbuf fake;
    fake.build(/*seq=*/2000, /*ack=*/1000,
               static_cast<uint8_t>(net::kTcpFin | net::kTcpAck),
               /*window=*/65535, payload, payload_len);

    rte_mbuf* p = &fake.mbuf;
    auto r = s.process_rx(&p, 1, [](const uint8_t*, uint16_t) {});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(s.rcv_nxt(), 2000u + payload_len + 1u);
    EXPECT_EQ(s.state(), TcpState::TimeWait);
}

// ═══════════════════════════════════════════════════════════════════════
// PacketTemplate overflow guard
// ═══════════════════════════════════════════════════════════════════════

class PacketTemplateOverflowTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        pool_ = rte_pktmbuf_pool_create(
            "test_pkt_overflow_pool",
            /*n=*/64,
            /*cache_size=*/0,
            /*priv_size=*/0,
            /*data_room_size=*/RTE_MBUF_DEFAULT_BUF_SIZE,
            SOCKET_ID_ANY);
        ASSERT_NE(pool_, nullptr);
    }

    static void TearDownTestSuite() {
        if (pool_) {
            rte_mempool_free(pool_);
            pool_ = nullptr;
        }
    }

    net::PacketTemplate make_template() const {
        net::PacketTemplate t;
        for (int i = 0; i < 6; ++i) t.src_mac.addr_bytes[i] = static_cast<uint8_t>(0xAA + i);
        for (int i = 0; i < 6; ++i) t.dst_mac.addr_bytes[i] = static_cast<uint8_t>(0x11 + i);
        t.tuple.src_ip   = 0x0A000001;
        t.tuple.dst_ip   = 0x0A000002;
        t.tuple.src_port = 12345;
        t.tuple.dst_port = 443;
        t.mss            = 1460;
        t.hw_cksum       = false;
        return t;
    }

    static rte_mempool* pool_;
};

rte_mempool* PacketTemplateOverflowTest::pool_ = nullptr;

TEST_F(PacketTemplateOverflowTest, LargePayloadLenReturnsNull) {
    // payload_len close to UINT16_MAX would overflow total_len if computed
    // in uint16_t. The fix widens to uint32_t and rejects. The actual
    // rte_pktmbuf_append would also fail for such sizes, but the overflow
    // check should fire first.
    auto t = make_template();
    uint8_t dummy = 0xCC;
    // total_len = 14 + 20 + 20 + 65535 = 65589 > UINT16_MAX
    auto* mbuf = t.build_packet(pool_, 1, 1, net::kTcpAck, 8192,
                                 &dummy, /*payload_len=*/65535);
    EXPECT_EQ(mbuf, nullptr) << "Oversized payload_len must return null";
}

TEST_F(PacketTemplateOverflowTest, NormalPayloadStillWorks) {
    auto t = make_template();
    uint8_t payload[100];
    std::memset(payload, 0xAB, sizeof(payload));
    auto* mbuf = t.build_packet(pool_, 1000, 2000, net::kTcpAck, 8192,
                                 payload, 100);
    ASSERT_NE(mbuf, nullptr);
    rte_pktmbuf_free(mbuf);
}

// ═══════════════════════════════════════════════════════════════════════
// MbufView::trim_front on default-constructed (null) view
// ═══════════════════════════════════════════════════════════════════════

TEST(MbufViewFaultTolerance, TrimFrontOnDefaultConstructedView) {
    // Default-constructed MbufView has data_=nullptr, length_=0.
    // trim_front must not trigger nullptr arithmetic UB.
    eph::net::dpdk::detail::MbufView view;
    EXPECT_EQ(view.data(), nullptr);
    EXPECT_EQ(view.length(), 0u);

    // Should not crash or trigger UB.
    view.trim_front(0);
    EXPECT_EQ(view.data(), nullptr);
    EXPECT_EQ(view.length(), 0u);

    view.trim_front(10);
    EXPECT_EQ(view.length(), 0u);
}

TEST(MbufViewFaultTolerance, TrimBackOnDefaultConstructedView) {
    eph::net::dpdk::detail::MbufView view;
    view.trim_back(0);
    EXPECT_EQ(view.length(), 0u);

    view.trim_back(10);
    EXPECT_EQ(view.length(), 0u);
}

TEST(MbufViewFaultTolerance, TrimFrontOnValidViewWorks) {
    // Verify trim_front still works correctly on valid views.
    uint8_t buf[32] = {};
    eph::net::dpdk::detail::MbufView view(buf, 32);
    EXPECT_EQ(view.data(), buf);
    EXPECT_EQ(view.length(), 32u);

    view.trim_front(10);
    EXPECT_EQ(view.data(), buf + 10);
    EXPECT_EQ(view.length(), 22u);

    // Trim more than remaining — should clamp to empty.
    view.trim_front(100);
    EXPECT_EQ(view.length(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════
// EAL double-init protection
// ═══════════════════════════════════════════════════════════════════════

TEST(EalFaultTolerance, DoubleInitFlagPreventsSecondCall) {
    // The test environment sets the process-wide EAL init flag after
    // calling rte_eal_init(). A second eal_init() call must be rejected
    // without invoking rte_eal_init (which would be DPDK UB).
    auto& flag = ::eph::dpdk::eal_initialized_flag();
    ASSERT_TRUE(flag.load()) << "Test env must set the EAL init flag";

    char arg0[] = "test";
    char* argv[] = {arg0, nullptr};
    int argc = 1;

    auto result = ::eph::dpdk::eal_init(argc, argv);
    EXPECT_FALSE(result.has_value())
        << "eal_init must reject double-init";
    if (!result.has_value()) {
        EXPECT_NE(result.error().find("already initialized"), std::string::npos)
            << "Error message: " << result.error();
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Sequence number wraparound — RST window at UINT32_MAX boundary
// ═══════════════════════════════════════════════════════════════════════

TEST(FaultTolerance, RstWindowWraparoundAtUint32Max) {
    // rcv_nxt near UINT32_MAX, window wraps past zero. An RST just past
    // the wrap point must be accepted as in-window.
    auto cfg = make_test_config();
    TcpSession<> s(cfg, nullptr);
    s.inject_state_for_testing(TcpState::Established);
    s.inject_send_seq_for_testing(1000, 1000);
    s.inject_recv_seq_for_testing(/*rcv_nxt=*/0xFFFFFFFE, /*rcv_wnd=*/8192);

    FakeTcpMbuf fake;
    // Window wraps: [0xFFFFFFFE, 0xFFFFFFFE+8192) = [0xFFFFFFFE, 0x00001FFE)
    // RST at 0x00000010 is within the wrapped window.
    fake.build(/*seq=*/0x00000010, /*ack=*/1000, net::kTcpRst);

    auto r = drive(s, fake);
    EXPECT_FALSE(r.has_value()) << "RST in wrapped window must close";
    EXPECT_EQ(s.state(), TcpState::Closed);
}

TEST(FaultTolerance, RstOutsideWrappedWindowIgnored) {
    auto cfg = make_test_config();
    TcpSession<> s(cfg, nullptr);
    s.inject_state_for_testing(TcpState::Established);
    s.inject_send_seq_for_testing(1000, 1000);
    s.inject_recv_seq_for_testing(/*rcv_nxt=*/0xFFFFFFFE, /*rcv_wnd=*/8192);

    FakeTcpMbuf fake;
    // 0x80000000 is well outside the wrapped window — must be ignored.
    fake.build(/*seq=*/0x80000000, /*ack=*/1000, net::kTcpRst);

    auto r = drive(s, fake);
    EXPECT_TRUE(r.has_value()) << "RST outside wrapped window must be ignored";
    EXPECT_EQ(s.state(), TcpState::Established);
}

// ═══════════════════════════════════════════════════════════════════════
// Adversarial packet parse — malformed inputs must not crash
// ═══════════════════════════════════════════════════════════════════════

TEST(FaultTolerance, ParsePacketWithZeroLengthMbuf) {
    // A zero-length mbuf must be rejected without crashing.
    alignas(8) uint8_t buf[1] = {0};
    rte_mbuf mbuf{};
    mbuf.buf_addr = buf;
    mbuf.data_off = 0;
    mbuf.data_len = 0;
    mbuf.pkt_len = 0;

    auto parsed = net::parse_packet(&mbuf);
    EXPECT_EQ(parsed.tcp, nullptr) << "Zero-length mbuf must fail parse";
}

TEST(FaultTolerance, ParsePacketWithTruncatedIpHeader) {
    // Packet just long enough for Ethernet but not IP.
    alignas(8) uint8_t buf[net::kEtherHeaderLen + 10] = {};
    auto* eth = reinterpret_cast<rte_ether_hdr*>(buf);
    eth->ether_type = net::hton16(net::kEtherTypeIpv4);

    rte_mbuf mbuf{};
    mbuf.buf_addr = buf;
    mbuf.data_off = 0;
    mbuf.data_len = net::kEtherHeaderLen + 10; // < 20 byte IP header
    mbuf.pkt_len = mbuf.data_len;

    auto parsed = net::parse_packet(&mbuf);
    EXPECT_EQ(parsed.tcp, nullptr) << "Truncated IP header must fail parse";
}

TEST(FaultTolerance, ParsePacketWithIpTotalLengthZero) {
    // Valid Ethernet + IP headers, but ip->total_length = 0.
    constexpr size_t hdr_len = net::kEtherHeaderLen + 20 + 20;
    alignas(8) uint8_t buf[hdr_len] = {};
    auto* eth = reinterpret_cast<rte_ether_hdr*>(buf);
    eth->ether_type = net::hton16(net::kEtherTypeIpv4);

    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + net::kEtherHeaderLen);
    ip->version_ihl = (4 << 4) | 5;
    ip->total_length = 0; // Malformed
    ip->next_proto_id = net::kIpProtoTcp;

    rte_mbuf mbuf{};
    mbuf.buf_addr = buf;
    mbuf.data_off = 0;
    mbuf.data_len = hdr_len;
    mbuf.pkt_len = hdr_len;

    auto parsed = net::parse_packet(&mbuf);
    // ip_total=0, data_start=40, data_start > ip_total → rejected.
    EXPECT_EQ(parsed.payload, nullptr) << "ip_total=0 must produce no payload";
}

TEST(EalFaultTolerance, FlagIsSetByTestEnv) {
    // Verify that the test environment correctly synchronizes the
    // process-wide EAL init flag (dpdk_test_env.hpp sets it after
    // raw rte_eal_init succeeds).
    auto& flag = ::eph::dpdk::eal_initialized_flag();
    EXPECT_TRUE(flag.load())
        << "Test env must set EAL init flag for guard consistency";
}
