/// @file test_tcp_close_reset.cpp
/// State-machine + packet-build tests for TcpSession::close() and reset().
///
/// Round 1's test_tcp_state_machine.cpp drove TcpSession::process_rx with
/// crafted mbufs but used pool=nullptr because the RX path doesn't
/// allocate.  close() and reset() build TX packets and call
/// rte_eth_tx_burst, so they need a real mempool — and they need a
/// port to TX into.  net_null is the appropriate vdev: it accepts TX
/// (drops to /dev/null) so close()/reset() can complete without an
/// actual peer.
///
/// What we verify:
///   1. close() in Established → state transitions to FinWait1
///   2. close() in CloseWait → state transitions to LastAck
///   3. close() in any other state → returns error, no state change
///   4. close() advances snd_nxt by 1 (FIN consumes a sequence number)
///   5. close() increments tx_packets stat
///   6. reset() unconditionally moves state to Closed
///
/// Note: tx_burst on net_null reports the packets as sent but they go
/// to a black hole — no peer to validate.  These tests therefore
/// assert on observable client-side state, not received bytes.

#include <cstdint>

#include <gtest/gtest.h>

#include <rte_mbuf.h>
#include <rte_mempool.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep
#include "eph/dpdk/platform.hpp"
#include "eph/dpdk/tcp.hpp"

using namespace eph::dpdk;
using eph::net::TcpState;

namespace {

/// Shared Platform fixture — net_null on port 0 with default mempool.
class TcpCloseResetTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        PlatformConfig pcfg{};
        pcfg.port_id = 0;
        auto plat = Platform::create(pcfg);
        if (!plat.has_value()) {
            // Some test binaries in the same run may have already
            // grabbed the only available net_null port.  Skip rather
            // than fail in that case.
            skip_reason_ = plat.error();
            return;
        }
        platform_ = new Platform(std::move(*plat));
    }

    static void TearDownTestSuite() {
        delete platform_;
        platform_ = nullptr;
    }

    void SetUp() override {
        if (!platform_) {
            GTEST_SKIP() << "Platform unavailable: " << skip_reason_;
        }
    }

    /// Build a TcpConfig pointing at the test port and a unique src_port.
    TcpConfig make_config(uint16_t src_port) {
        TcpConfig cfg;
        cfg.tuple.src_ip   = 0x0A000001;
        cfg.tuple.dst_ip   = 0x0A000002;
        cfg.tuple.src_port = src_port;
        cfg.tuple.dst_port = 443;
        cfg.mss            = 1460;
        cfg.recv_window    = 65535;
        cfg.port_id        = platform_->port_id();
        cfg.tx_queue_id    = 0;
        cfg.rx_queue_id    = 0;
        // src_mac / dst_mac left zero — packet_template still builds,
        // and net_null doesn't care about Ethernet.
        return cfg;
    }

    static Platform* platform_;
    static std::string skip_reason_;
};

Platform* TcpCloseResetTest::platform_ = nullptr;
std::string TcpCloseResetTest::skip_reason_;

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// close() — Established → FinWait1
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TcpCloseResetTest, CloseInEstablishedTransitionsToFinWait1) {
    auto cfg = make_config(45000);
    TcpSession<> s(cfg, platform_->mempool());
    s.inject_state_for_testing(TcpState::Established);
    s.inject_send_seq_for_testing(/*snd_nxt=*/1000, /*snd_una=*/1000);
    s.inject_recv_seq_for_testing(/*rcv_nxt=*/2000, /*rcv_wnd=*/8192);

    auto r = s.close();
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(s.state(), TcpState::FinWait1);
}

TEST_F(TcpCloseResetTest, CloseInEstablishedAdvancesSndNxt) {
    auto cfg = make_config(45001);
    TcpSession<> s(cfg, platform_->mempool());
    s.inject_state_for_testing(TcpState::Established);
    s.inject_send_seq_for_testing(1000, 1000);
    s.inject_recv_seq_for_testing(2000, 8192);

    EXPECT_EQ(s.snd_nxt(), 1000u);
    auto r = s.close();
    ASSERT_TRUE(r.has_value());
    // FIN consumes 1 sequence number.
    EXPECT_EQ(s.snd_nxt(), 1001u);
}

TEST_F(TcpCloseResetTest, CloseIncrementsTxPackets) {
    auto cfg = make_config(45002);
    TcpSession<> s(cfg, platform_->mempool());
    s.inject_state_for_testing(TcpState::Established);
    s.inject_send_seq_for_testing(1, 1);
    s.inject_recv_seq_for_testing(1, 100);

    auto before = s.stats().tx_packets;
    auto r = s.close();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(s.stats().tx_packets, before + 1);
}

// ═══════════════════════════════════════════════════════════════════════
// close() — CloseWait → LastAck
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TcpCloseResetTest, CloseInCloseWaitTransitionsToLastAck) {
    auto cfg = make_config(45003);
    TcpSession<> s(cfg, platform_->mempool());
    s.inject_state_for_testing(TcpState::CloseWait);
    s.inject_send_seq_for_testing(5000, 5000);
    s.inject_recv_seq_for_testing(6000, 8192);

    auto r = s.close();
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(s.state(), TcpState::LastAck);
    EXPECT_EQ(s.snd_nxt(), 5001u);
}

// ═══════════════════════════════════════════════════════════════════════
// close() — invalid states reject without state change
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TcpCloseResetTest, CloseInClosedRejected) {
    auto cfg = make_config(45004);
    TcpSession<> s(cfg, platform_->mempool());
    // State is Closed by default.
    EXPECT_EQ(s.state(), TcpState::Closed);

    auto r = s.close();
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(s.state(), TcpState::Closed);
}

TEST_F(TcpCloseResetTest, CloseInSynSentRejected) {
    auto cfg = make_config(45005);
    TcpSession<> s(cfg, platform_->mempool());
    s.inject_state_for_testing(TcpState::SynSent);

    auto r = s.close();
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(s.state(), TcpState::SynSent);
}

TEST_F(TcpCloseResetTest, CloseInFinWait1Rejected) {
    // Already closing — second close() must be rejected, no double-FIN.
    auto cfg = make_config(45006);
    TcpSession<> s(cfg, platform_->mempool());
    s.inject_state_for_testing(TcpState::FinWait1);
    s.inject_send_seq_for_testing(1000, 999);

    auto r = s.close();
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(s.state(), TcpState::FinWait1);
    // snd_nxt MUST NOT be incremented on rejected close.
    EXPECT_EQ(s.snd_nxt(), 1000u);
}

TEST_F(TcpCloseResetTest, CloseInTimeWaitRejected) {
    auto cfg = make_config(45007);
    TcpSession<> s(cfg, platform_->mempool());
    s.inject_state_for_testing(TcpState::TimeWait);

    auto r = s.close();
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(s.state(), TcpState::TimeWait);
}

// ═══════════════════════════════════════════════════════════════════════
// reset() — unconditional transition to Closed
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TcpCloseResetTest, ResetFromEstablishedGoesToClosed) {
    auto cfg = make_config(45010);
    TcpSession<> s(cfg, platform_->mempool());
    s.inject_state_for_testing(TcpState::Established);
    s.inject_send_seq_for_testing(1000, 1000);
    s.inject_recv_seq_for_testing(2000, 8192);

    s.reset();
    EXPECT_EQ(s.state(), TcpState::Closed);
}

TEST_F(TcpCloseResetTest, ResetFromFinWait1GoesToClosed) {
    auto cfg = make_config(45011);
    TcpSession<> s(cfg, platform_->mempool());
    s.inject_state_for_testing(TcpState::FinWait1);

    s.reset();
    EXPECT_EQ(s.state(), TcpState::Closed);
}

TEST_F(TcpCloseResetTest, ResetFromAlreadyClosedIsNoOp) {
    auto cfg = make_config(45012);
    TcpSession<> s(cfg, platform_->mempool());
    EXPECT_EQ(s.state(), TcpState::Closed);

    // reset() on Closed must be safe — no double-RST or crash.
    s.reset();
    EXPECT_EQ(s.state(), TcpState::Closed);
}

TEST_F(TcpCloseResetTest, ResetIncrementsTxPacketsOnSuccess) {
    // Parallel to the close() counterpart above: a successfully-bursted
    // RST must be counted in tx_packets so operators tracking TX volume
    // don't see reset-heavy workloads silently under-report throughput.
    auto cfg = make_config(45013);
    TcpSession<> s(cfg, platform_->mempool());
    s.inject_state_for_testing(TcpState::Established);
    s.inject_send_seq_for_testing(1, 1);
    s.inject_recv_seq_for_testing(1, 100);

    auto before = s.stats().tx_packets;
    s.reset();
    // net_null accepts every TX, so the RST always bursts successfully.
    EXPECT_EQ(s.stats().tx_packets, before + 1);
    EXPECT_EQ(s.state(), TcpState::Closed);
}
