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
#include "eph/dpdk/tcp.hpp"

using namespace eph::dpdk;
using eph::net::TcpState;

namespace {

/// Shared mempool fixture — net_null on port 0.
///
/// Daemon-reshape (post-b4fc8969): the previous fixture wrapped a
/// `Platform::create(PlatformConfig{ .port_id = 0 })` that worked
/// purely in-process. The new daemon-led `Platform::create` requires
/// a running `eph-nicd` primary, which the unit-test EAL bring-up
/// (`--no-pci --vdev=net_null0` in `dpdk_test_env.hpp`) cannot
/// provide. We therefore bypass `Platform` here and create a pktmbuf
/// pool directly: this test only ever uses Platform for `mempool()`
/// and `port_id()`, both of which are trivially provided by the
/// already-initialized EAL + the well-known net_null port id.
class TcpCloseResetTest : public ::testing::Test {
protected:
    static constexpr uint16_t kPortId = 0;     // net_null0 from DpdkTestEnv
    static rte_mempool*       pool_;
    static std::string        skip_reason_;

    static void SetUpTestSuite() {
        // Single shared pool for every test in this binary. Cache name
        // includes pid to avoid clashes across re-runs in the same
        // hugepage-less process group.
        char name[64];
        std::snprintf(name, sizeof(name),
                      "tcp_close_reset_pool_%d",
                      static_cast<int>(::getpid()));
        pool_ = rte_pktmbuf_pool_create(
            name,
            /*n=*/1023,
            /*cache_size=*/32,
            /*priv_size=*/0,
            /*data_room_size=*/RTE_MBUF_DEFAULT_BUF_SIZE,
            /*socket_id=*/SOCKET_ID_ANY);
        if (pool_ == nullptr) {
            skip_reason_ = "rte_pktmbuf_pool_create failed (rte_errno=" +
                           std::to_string(rte_errno) + ")";
        }
    }

    static void TearDownTestSuite() {
        if (pool_ != nullptr) {
            rte_mempool_free(pool_);
            pool_ = nullptr;
        }
    }

    void SetUp() override {
        if (pool_ == nullptr) {
            GTEST_SKIP() << "mempool unavailable: " << skip_reason_;
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
        cfg.port_id        = kPortId;
        cfg.tx_queue_id    = 0;
        cfg.rx_queue_id    = 0;
        // src_mac / dst_mac left zero — packet_template still builds,
        // and net_null doesn't care about Ethernet.
        return cfg;
    }
};

rte_mempool* TcpCloseResetTest::pool_      = nullptr;
std::string  TcpCloseResetTest::skip_reason_;

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// close() — Established → FinWait1
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TcpCloseResetTest, CloseInEstablishedTransitionsToFinWait1) {
    auto cfg = make_config(45000);
    TcpSession<> s(cfg, pool_);
    s.inject_state_for_testing(TcpState::Established);
    s.inject_send_seq_for_testing(/*snd_nxt=*/1000, /*snd_una=*/1000);
    s.inject_recv_seq_for_testing(/*rcv_nxt=*/2000, /*rcv_wnd=*/8192);

    auto r = s.close();
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(s.state(), TcpState::FinWait1);
}

TEST_F(TcpCloseResetTest, CloseInEstablishedAdvancesSndNxt) {
    auto cfg = make_config(45001);
    TcpSession<> s(cfg, pool_);
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
    TcpSession<> s(cfg, pool_);
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
    TcpSession<> s(cfg, pool_);
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
    TcpSession<> s(cfg, pool_);
    // State is Closed by default.
    EXPECT_EQ(s.state(), TcpState::Closed);

    auto r = s.close();
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(s.state(), TcpState::Closed);
}

TEST_F(TcpCloseResetTest, CloseInSynSentRejected) {
    auto cfg = make_config(45005);
    TcpSession<> s(cfg, pool_);
    s.inject_state_for_testing(TcpState::SynSent);

    auto r = s.close();
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(s.state(), TcpState::SynSent);
}

TEST_F(TcpCloseResetTest, CloseInFinWait1Rejected) {
    // Already closing — second close() must be rejected, no double-FIN.
    auto cfg = make_config(45006);
    TcpSession<> s(cfg, pool_);
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
    TcpSession<> s(cfg, pool_);
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
    TcpSession<> s(cfg, pool_);
    s.inject_state_for_testing(TcpState::Established);
    s.inject_send_seq_for_testing(1000, 1000);
    s.inject_recv_seq_for_testing(2000, 8192);

    s.reset();
    EXPECT_EQ(s.state(), TcpState::Closed);
}

TEST_F(TcpCloseResetTest, ResetFromFinWait1GoesToClosed) {
    auto cfg = make_config(45011);
    TcpSession<> s(cfg, pool_);
    s.inject_state_for_testing(TcpState::FinWait1);

    s.reset();
    EXPECT_EQ(s.state(), TcpState::Closed);
}

TEST_F(TcpCloseResetTest, ResetFromAlreadyClosedIsNoOp) {
    auto cfg = make_config(45012);
    TcpSession<> s(cfg, pool_);
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
    TcpSession<> s(cfg, pool_);
    s.inject_state_for_testing(TcpState::Established);
    s.inject_send_seq_for_testing(1, 1);
    s.inject_recv_seq_for_testing(1, 100);

    auto before = s.stats().tx_packets;
    s.reset();
    // net_null accepts every TX, so the RST always bursts successfully.
    EXPECT_EQ(s.stats().tx_packets, before + 1);
    EXPECT_EQ(s.state(), TcpState::Closed);
}

// ═══════════════════════════════════════════════════════════════════════
// Keepalive probe exhaustion — Tier 2 #5 of the lucky-giggling-kahan
// review. The existing test_tcp_state_machine.cpp keepalive tests use
// pool=nullptr, so probe alloc always fails and `keepalive_misses_` never
// advances — the dead-connection transition (state_=Closed after
// `keepalive_probes` unanswered probes) is untested there. With a real
// net_null-backed mempool probes fire successfully, and we can drive the
// full exhaust sequence:
//
//   tick(t₀)           → anchor baseline, no probe
//   tick(t₀+interval)  → probe #1, misses=1, probes_sent=1
//   tick(t₀+2·interval)→ probe #2, misses=2, probes_sent=2
//   tick(t₀+3·interval)→ probe #3, misses=3, probes_sent=3
//   tick(t₀+4·interval)→ misses >= probes → state=Closed, no more TX
//
// Asserts:
//   * session state flips Established→Closed exactly on the (N+1)-th tick
//   * `stats().keepalive_probes_sent == keepalive_probes` (not more —
//     dead-close tick does NOT emit another probe)
//   * tx_packets grows by exactly `keepalive_probes` (one TX per probe)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(TcpCloseResetTest, KeepaliveProbeExhaustionTransitionsToClosed) {
    auto cfg = make_config(45014);
    // 10 ms interval: with the 1 GHz fallback this is 10 M cycles. We
    // advance by 20 M cycles between ticks so both the "inter-tick >
    // interval" and "rate-limit" guards in tick_keepalive are crossed.
    cfg.keepalive_interval = std::chrono::milliseconds{10};
    cfg.keepalive_probes   = 3;

    TcpSession<> s(cfg, pool_);
    s.inject_state_for_testing(TcpState::Established);
    // Seqs picked out of the way of other tests sharing the fixture's
    // platform_ — any value works since net_null drops the wire bytes.
    s.inject_send_seq_for_testing(/*snd_nxt=*/5000, /*snd_una=*/5000);
    s.inject_recv_seq_for_testing(/*rcv_nxt=*/6000, /*rcv_wnd=*/65535);

    const uint64_t tx_before = s.stats().tx_packets;
    ASSERT_EQ(s.stats().keepalive_probes_sent, 0u);

    // tick 0: anchor the RX baseline. The guard at tick_keepalive:1417
    // (`last_rx_tsc_ == 0`) means the first tick never probes.
    constexpr uint64_t kStep = 20'000'000ull;  // 20 M cycles ≈ 20 ms @ 1 GHz
    s.tick_keepalive(kStep);
    EXPECT_EQ(s.state(), TcpState::Established);
    EXPECT_EQ(s.stats().keepalive_probes_sent, 0u)
        << "first tick must anchor, not probe";

    // ticks 1..N: each fires exactly one probe.
    for (uint8_t i = 1; i <= cfg.keepalive_probes; ++i) {
        s.tick_keepalive(kStep * (uint64_t{i} + 1));
        EXPECT_EQ(s.state(), TcpState::Established)
            << "still alive mid-exhaust at probe #" << static_cast<int>(i);
        EXPECT_EQ(s.stats().keepalive_probes_sent, uint64_t{i})
            << "probe count mismatch at probe #" << static_cast<int>(i);
    }

    // final tick: misses_ now equals keepalive_probes; the next tick must
    // take the dead-close branch (tick_keepalive:1427) and flip state to
    // Closed WITHOUT emitting a probe.
    const uint64_t final_tsc = kStep * (uint64_t{cfg.keepalive_probes} + 2);
    s.tick_keepalive(final_tsc);
    EXPECT_EQ(s.state(), TcpState::Closed)
        << "exhaust must declare connection dead";
    EXPECT_EQ(s.stats().keepalive_probes_sent,
              uint64_t{cfg.keepalive_probes})
        << "dead-close branch must NOT emit an additional probe";

    // tx_packets should have grown by exactly keepalive_probes — one TX
    // per successful probe, none for the dead-close tick.
    EXPECT_EQ(s.stats().tx_packets - tx_before,
              uint64_t{cfg.keepalive_probes})
        << "each probe emits one TX; dead-close does not";

    // Subsequent ticks after Closed must be a no-op (guard at 1416).
    s.tick_keepalive(final_tsc + kStep);
    EXPECT_EQ(s.state(), TcpState::Closed);
    EXPECT_EQ(s.stats().keepalive_probes_sent,
              uint64_t{cfg.keepalive_probes});
}

TEST_F(TcpCloseResetTest, KeepaliveWithSingleProbeExhaustsOnTwoTicks) {
    // Boundary case: `keepalive_probes = 1` should declare the connection
    // dead after exactly ONE unanswered probe. Pins the "≥" comparison at
    // tick_keepalive:1427 so a future refactor to ">" wouldn't silently
    // regress the min-probe semantic.
    auto cfg = make_config(45015);
    cfg.keepalive_interval = std::chrono::milliseconds{10};
    cfg.keepalive_probes   = 1;

    TcpSession<> s(cfg, pool_);
    s.inject_state_for_testing(TcpState::Established);
    s.inject_send_seq_for_testing(5100, 5100);
    s.inject_recv_seq_for_testing(6100, 65535);

    constexpr uint64_t kStep = 20'000'000ull;
    s.tick_keepalive(kStep);               // anchor
    s.tick_keepalive(kStep * 2);           // probe #1
    EXPECT_EQ(s.state(), TcpState::Established);
    EXPECT_EQ(s.stats().keepalive_probes_sent, 1u);

    s.tick_keepalive(kStep * 3);           // declare dead
    EXPECT_EQ(s.state(), TcpState::Closed);
    EXPECT_EQ(s.stats().keepalive_probes_sent, 1u)
        << "dead-close must not bump probes_sent";
}
