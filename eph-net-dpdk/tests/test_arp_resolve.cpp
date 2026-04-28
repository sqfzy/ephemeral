/// @file test_arp_resolve.cpp
/// Unit tests for `eph::dpdk::arp::resolve_with_io` — the synchronous ARP
/// resolver state machine added in T2.13 of the crypto-HFT review meta-plan.
///
/// Test strategy (mirrors `test_dns_async.cpp`):
///   - Boot EAL once with `--no-pci --vdev=net_null0` via `dpdk_test_env.hpp`
///     so we can `rte_pktmbuf_pool_create` real mbufs for both query and
///     reply paths. No physical NIC required.
///   - Substitute `FakeNicIo` for the resolver's NIC TX/RX shim. FakeNicIo
///     records each tx packet in a static vector and serves crafted
///     reply mbufs from a static reply queue. Tests reset both in
///     SetUp() — fine for single-threaded gtest execution.
///   - The synchronous resolver loop in `resolve_with_io` does the
///     send / retry / RX-poll / parse-and-match / timeout state-machine
///     dance internally; we assert on the recorded sends and the
///     returned `expected<rte_ether_addr, std::string>`.
///
/// Coverage map (T2.13 plan):
///   1. HappyPathReply           — send → fake reply → returns gateway MAC
///   2. CacheHit                 — N/A: arp::resolve is stateless. The
///                                 task plan permits SKIP. We document the
///                                 absence with a SKIP test that prints
///                                 the reason and leaves a follow-up note.
///   3. Timeout                  — no reply → returns Err(timeout)
///   4. WrongTargetIpIgnored     — reply with mismatched sender IP dropped
///   5. MalformedReply           — truncated or wrong opcode → no crash;
///                                 eventually times out
///   6. NullMempoolReturnsError  — pre-flight error path
///
/// Note: ARP / DNS / ICMP state-machine tests were previously absent (only
/// fuzzer harnesses existed). This file partially satisfies M11 of the
/// review plan as a side effect.

#include <chrono>
#include <cstring>
#include <deque>
#include <vector>

#include <gtest/gtest.h>

#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#include "dpdk_test_env.hpp"  // IWYU pragma: keep — boots EAL once
#include "eph/dpdk/arp.hpp"
#include "eph/dpdk/net_header.hpp"

using namespace eph::dpdk;
using eph::dpdk::arp::ArpPacket;
using eph::dpdk::arp::kArpHwAddrLen;
using eph::dpdk::arp::kArpHwTypeEthernet;
using eph::dpdk::arp::kArpOpReply;
using eph::dpdk::arp::kArpProtoAddrLen;
using eph::dpdk::arp::kArpProtoIpv4;
using eph::dpdk::arp::kEtherTypeArp;

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// FakeNicIo — substitutes for RealNicIoArp in tests.
// ─────────────────────────────────────────────────────────────────────────────
//
// Static state because the resolve_with_io<Io> template parameter must be a
// type, not an instance — same constraint as test_dns_async. Tests reset in
// SetUp(). Single-threaded; if ever parallelised, swap to thread_local.
struct FakeNicIo {
    struct SentRecord {
        uint16_t  port_id;
        uint16_t  queue_id;
        uint16_t  pkt_len;
    };

    /// Recorded TX bursts.
    static inline std::vector<SentRecord> sent_records{};

    /// Pending RX replies, returned FIFO from rx_burst (one per call).
    /// The resolver's `kArpResolveBurstSize` is 16, so we *could* return
    /// multiple at once — but feeding one-per-call models real PMDs
    /// where replies trickle in and exercises the inner per-reply
    /// scan loop more faithfully.
    static inline std::deque<rte_mbuf*> rx_queue{};

    /// `tx_outcome` controls what TX returns (default = always succeed).
    /// Test can set to 0 to simulate ring-full / NIC-error → resolver
    /// retries on the next interval instead of failing fast.
    static inline uint16_t tx_outcome = 1;

    [[nodiscard]] static uint16_t tx_burst(uint16_t port_id, uint16_t queue_id,
                                            rte_mbuf** pkts, uint16_t n) noexcept {
        for (uint16_t i = 0; i < n; ++i) {
            if (i < tx_outcome) {
                sent_records.push_back(SentRecord{
                    port_id, queue_id,
                    rte_pktmbuf_pkt_len(pkts[i]),
                });
                // Real PMDs consume the mbuf on accept. Resolver does
                // NOT free a packet that tx_burst claimed.
                rte_pktmbuf_free(pkts[i]);
            }
        }
        return tx_outcome > n ? n : tx_outcome;
    }

    [[nodiscard]] static uint16_t rx_burst(uint16_t /*port_id*/,
                                            uint16_t /*queue_id*/,
                                            rte_mbuf** pkts, uint16_t n) noexcept {
        uint16_t i = 0;
        while (i < n && !rx_queue.empty()) {
            pkts[i++] = rx_queue.front();
            rx_queue.pop_front();
        }
        return i;
    }

    static void reset() {
        sent_records.clear();
        // Anything left over (e.g. test that injected a reply but the
        // resolver returned early on a different reply) must be freed
        // so the mempool is not leaked across test cases.
        while (!rx_queue.empty()) {
            rte_pktmbuf_free(rx_queue.front());
            rx_queue.pop_front();
        }
        tx_outcome = 1;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Reply builder — produces an Ethernet + ARP reply mbuf addressed to
// `dst_mac` from `sender_mac`. `sender_ip` is the IP the reply *claims*
// to be answering for (host byte order). Caller transfers ownership to
// FakeNicIo::rx_queue.
// ─────────────────────────────────────────────────────────────────────────────

rte_mbuf* build_arp_reply(rte_mempool* pool,
                          const rte_ether_addr& sender_mac,
                          uint32_t sender_ip,
                          const rte_ether_addr& target_mac,
                          uint32_t target_ip) {
    auto* mbuf = rte_pktmbuf_alloc(pool);
    if (!mbuf) return nullptr;

    constexpr uint16_t frame_len = net::kEtherHeaderLen + sizeof(ArpPacket);
    auto* pkt = reinterpret_cast<uint8_t*>(rte_pktmbuf_append(mbuf, frame_len));
    if (!pkt) {
        rte_pktmbuf_free(mbuf);
        return nullptr;
    }

    // Ethernet header — unicast reply back to us (target_mac).
    auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt);
    rte_ether_addr_copy(&target_mac, &eth->dst_addr);
    rte_ether_addr_copy(&sender_mac, &eth->src_addr);
    eth->ether_type = net::hton16(kEtherTypeArp);

    // ARP payload: opcode=2 (reply), sender = gateway, target = us.
    auto* arp = reinterpret_cast<ArpPacket*>(pkt + net::kEtherHeaderLen);
    arp->hw_type        = net::hton16(kArpHwTypeEthernet);
    arp->proto_type     = net::hton16(kArpProtoIpv4);
    arp->hw_addr_len    = kArpHwAddrLen;
    arp->proto_addr_len = kArpProtoAddrLen;
    arp->opcode         = net::hton16(kArpOpReply);
    std::memcpy(arp->sender_mac, sender_mac.addr_bytes, 6);
    arp->sender_ip = net::hton32(sender_ip);
    std::memcpy(arp->target_mac, target_mac.addr_bytes, 6);
    arp->target_ip = net::hton32(target_ip);

    return mbuf;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test fixture — shared mempool + FakeNicIo reset.
// ─────────────────────────────────────────────────────────────────────────────

class ArpResolveTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        pool_ = rte_pktmbuf_pool_create(
            "test_arp_resolve_pool",
            /*n=*/256, /*cache=*/16, /*priv_size=*/0,
            RTE_MBUF_DEFAULT_BUF_SIZE, SOCKET_ID_ANY);
        ASSERT_NE(pool_, nullptr) << "rte_pktmbuf_pool_create failed";
    }

    static void TearDownTestSuite() {
        if (pool_) {
            rte_mempool_free(pool_);
            pool_ = nullptr;
        }
    }

    void SetUp() override {
        FakeNicIo::reset();
    }

    void TearDown() override {
        // Defensive: free anything the resolver left behind (tests that
        // queue extra replies but the resolver returned on the first).
        FakeNicIo::reset();
    }

    /// Standard test addresses — chosen to be distinct and obviously
    /// synthetic (no real-world network would use these together).
    static constexpr uint32_t kSrcIp     = 0x0A000001u;  // 10.0.0.1 (us)
    static constexpr uint32_t kTargetIp  = 0x0A0000FEu;  // 10.0.0.254 (gateway)
    static const rte_ether_addr kSrcMac;
    static const rte_ether_addr kGatewayMac;

    static rte_mempool* pool_;
};

rte_mempool* ArpResolveTest::pool_ = nullptr;
const rte_ether_addr ArpResolveTest::kSrcMac{
    {0x02, 0x00, 0x00, 0x00, 0x00, 0x01}};
const rte_ether_addr ArpResolveTest::kGatewayMac{
    {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE}};

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// 1. Happy path — request goes out, reply parses, MAC returned
// ═══════════════════════════════════════════════════════════════════════

TEST_F(ArpResolveTest, HappyPathReply_ReturnsGatewayMac) {
    // Pre-queue the reply so the very first rx_burst poll finds it.
    auto* reply = build_arp_reply(pool_, kGatewayMac, kTargetIp,
                                   kSrcMac, kSrcIp);
    ASSERT_NE(reply, nullptr);
    FakeNicIo::rx_queue.push_back(reply);

    auto r = arp::resolve_with_io<FakeNicIo>(
        /*port=*/0, /*queue=*/0, pool_,
        kSrcMac, kSrcIp, kTargetIp,
        std::chrono::milliseconds{500});

    ASSERT_TRUE(r.has_value()) << "resolve failed: " << r.error();
    EXPECT_EQ(0, std::memcmp(r->addr_bytes, kGatewayMac.addr_bytes, 6))
        << "resolved MAC mismatch";

    // Exactly one ARP request frame should have been sent — frame size
    // is Eth (14) + ARP (28) = 42 bytes.
    ASSERT_EQ(FakeNicIo::sent_records.size(), 1u);
    EXPECT_EQ(FakeNicIo::sent_records[0].port_id,  0u);
    EXPECT_EQ(FakeNicIo::sent_records[0].queue_id, 0u);
    EXPECT_EQ(FakeNicIo::sent_records[0].pkt_len,
              net::kEtherHeaderLen + sizeof(ArpPacket));

    // No leftovers in rx_queue — the resolver consumed the reply.
    EXPECT_TRUE(FakeNicIo::rx_queue.empty());
}

// ═══════════════════════════════════════════════════════════════════════
// 2. Cache hit — N/A for stateless arp::resolve
// ═══════════════════════════════════════════════════════════════════════
//
// The plan asks for a CacheHit case "if the API exposes a cache; if not,
// document and SKIP". `arp::resolve` is documented as stateless: "No ARP
// cache, no background threads, no state between calls." (arp.hpp:11).
// Caching is a deliberate non-goal — HFT colo deployments either
// resolve once at startup or pre-configure the gateway MAC. This test
// records the deviation explicitly so the absence is intentional, not
// an oversight.

TEST_F(ArpResolveTest, CacheHit_NotApplicable_StatelessResolverByDesign) {
    GTEST_SKIP()
        << "arp::resolve is stateless by design (see arp.hpp module doc). "
        << "There is no cache, so a cache-hit case has no behaviour to "
        << "verify. HFT deployments resolve once at startup or skip ARP "
        << "entirely by pre-configuring the gateway MAC.";
}

// ═══════════════════════════════════════════════════════════════════════
// 3. Timeout — no reply ever comes back; resolver returns error
// ═══════════════════════════════════════════════════════════════════════

TEST_F(ArpResolveTest, Timeout_NoReplyReturnsError) {
    // No mbufs queued — rx_burst always returns 0.
    auto t0 = std::chrono::steady_clock::now();
    auto r = arp::resolve_with_io<FakeNicIo>(
        /*port=*/0, /*queue=*/0, pool_,
        kSrcMac, kSrcIp, kTargetIp,
        std::chrono::milliseconds{150}); // short timeout for fast test

    auto elapsed = std::chrono::steady_clock::now() - t0;

    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("timeout"), std::string::npos)
        << "expected 'timeout' in error message, got: " << r.error();

    // Sanity: actually waited ~150ms (allow generous slack for CI hosts).
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed),
              std::chrono::milliseconds{100});
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed),
              std::chrono::milliseconds{2000})
        << "resolver took unexpectedly long to time out";

    // At least one ARP request should have been sent (the immediate
    // initial send). The retry interval has a 50ms floor and the
    // timeout/3 expression yields 50ms here, so we may see 2-3 retries
    // depending on scheduling — assert >=1.
    EXPECT_GE(FakeNicIo::sent_records.size(), 1u);
}

// ═══════════════════════════════════════════════════════════════════════
// 4. Wrong target IP in reply — silently ignored, eventually times out
// ═══════════════════════════════════════════════════════════════════════
//
// `parse_arp_reply` rejects replies whose sender_ip != target_ip.
// Verifying this defensively: the resolver must NOT confuse a reply for
// a different host as a hit on `kTargetIp`.

TEST_F(ArpResolveTest, WrongTargetIpIgnored_ReplyDropped) {
    constexpr uint32_t kBogusIp = 0x0A0000AAu;  // some other host
    rte_ether_addr bogus_mac{{0x02, 0xDE, 0xAD, 0xBE, 0xEF, 0x00}};

    // Queue ONE reply with the wrong sender IP. `parse_arp_reply` will
    // reject it, and (since we provide nothing else) the resolver will
    // burn through its retries and time out.
    auto* bad = build_arp_reply(pool_, bogus_mac, kBogusIp,
                                 kSrcMac, kSrcIp);
    ASSERT_NE(bad, nullptr);
    FakeNicIo::rx_queue.push_back(bad);

    auto r = arp::resolve_with_io<FakeNicIo>(
        /*port=*/0, /*queue=*/0, pool_,
        kSrcMac, kSrcIp, kTargetIp,
        std::chrono::milliseconds{120});

    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("timeout"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════
// 5. Malformed reply — wrong opcode → ignored; resolver does not crash
// ═══════════════════════════════════════════════════════════════════════

TEST_F(ArpResolveTest, MalformedReply_WrongOpcodeIgnored) {
    // Build a "reply" but stamp the opcode as request (1) instead of
    // reply (2). parse_arp_reply gates on opcode==2 and rejects.
    auto* mbuf = build_arp_reply(pool_, kGatewayMac, kTargetIp,
                                 kSrcMac, kSrcIp);
    ASSERT_NE(mbuf, nullptr);
    auto* pkt = rte_pktmbuf_mtod(mbuf, uint8_t*);
    auto* arp_pkt = reinterpret_cast<ArpPacket*>(pkt + net::kEtherHeaderLen);
    arp_pkt->opcode = net::hton16(1);  // request, not reply

    FakeNicIo::rx_queue.push_back(mbuf);

    auto r = arp::resolve_with_io<FakeNicIo>(
        /*port=*/0, /*queue=*/0, pool_,
        kSrcMac, kSrcIp, kTargetIp,
        std::chrono::milliseconds{120});

    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("timeout"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════
// 5b. Truncated reply — frame too short for an ARP packet → no crash
// ═══════════════════════════════════════════════════════════════════════

TEST_F(ArpResolveTest, MalformedReply_TruncatedDoesNotCrash) {
    auto* mbuf = build_arp_reply(pool_, kGatewayMac, kTargetIp,
                                 kSrcMac, kSrcIp);
    ASSERT_NE(mbuf, nullptr);
    // Trim 6 bytes off the tail so the ARP packet is shorter than its
    // 28-byte minimum. parse_arp_reply gates on data_len >= 42 and
    // rejects without dereferencing past the buffer end.
    rte_pktmbuf_trim(mbuf, 6);
    FakeNicIo::rx_queue.push_back(mbuf);

    auto r = arp::resolve_with_io<FakeNicIo>(
        /*port=*/0, /*queue=*/0, pool_,
        kSrcMac, kSrcIp, kTargetIp,
        std::chrono::milliseconds{120});

    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("timeout"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════
// 6. Pre-flight error — null mempool returns error, no NIC interaction
// ═══════════════════════════════════════════════════════════════════════

TEST_F(ArpResolveTest, NullMempoolReturnsError) {
    auto r = arp::resolve_with_io<FakeNicIo>(
        /*port=*/0, /*queue=*/0, /*pool=*/nullptr,
        kSrcMac, kSrcIp, kTargetIp,
        std::chrono::milliseconds{50});

    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("null"), std::string::npos)
        << "expected 'null' in error message, got: " << r.error();
    EXPECT_TRUE(FakeNicIo::sent_records.empty());
}

// ═══════════════════════════════════════════════════════════════════════
// 7. Anti-spoof — expected_mac mismatch causes the reply to be rejected
// ═══════════════════════════════════════════════════════════════════════
//
// Bonus coverage: the `expected_mac` parameter is the documented
// anti-spoof knob. A reply with the correct sender_ip but a
// non-matching MAC must be rejected.

TEST_F(ArpResolveTest, AntiSpoof_ExpectedMacMismatchRejected) {
    rte_ether_addr attacker_mac{{0x02, 0xBA, 0xDB, 0xAD, 0xBA, 0xDD}};

    auto* reply = build_arp_reply(pool_, attacker_mac, kTargetIp,
                                   kSrcMac, kSrcIp);
    ASSERT_NE(reply, nullptr);
    FakeNicIo::rx_queue.push_back(reply);

    auto r = arp::resolve_with_io<FakeNicIo>(
        /*port=*/0, /*queue=*/0, pool_,
        kSrcMac, kSrcIp, kTargetIp,
        std::chrono::milliseconds{120},
        /*expected_mac=*/kGatewayMac);

    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("timeout"), std::string::npos);
}
