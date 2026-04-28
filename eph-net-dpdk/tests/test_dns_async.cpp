/// @file test_dns_async.cpp
/// Unit tests for `eph::dpdk::dns::AsyncDnsResolverT` — the DpdkPollable
/// async DNS state machine added in T1.2 of the crypto-HFT review meta-plan.
///
/// Test strategy:
///   - Boot EAL once with `--no-pci --vdev=net_null0` via `dpdk_test_env.hpp`
///     so we can `rte_pktmbuf_pool_create` real mbufs for both query and
///     reply paths. No physical NIC required.
///   - Substitute `FakeNicIo` for the resolver's TX shim. FakeNicIo records
///     each tx packet in a static vector instead of touching real hardware.
///     Recorded packets can be inspected to verify wire format + parameters.
///   - Drive `process_burst_` directly with crafted reply mbufs built via
///     the same `build_dns_packet` helper used by the resolver, so the
///     production parsing path is exercised end-to-end.
///
/// Coverage map:
///   1. Happy path                — start → reply → Ready with correct IP
///   2. Retry on dropped reply    — first tick retries; later reply parses
///   3. Timeout                   — no reply; on_poll_tick advances state
///   4. Bad reply (wrong txn ID)  — packet ignored; eventual TimedOut
///   5. Malformed reply           — truncated DNS section; no crash
///   6. Concurrent resolves       — 4 resolvers driven sequentially complete
///   7. Dotted-decimal fast path  — bypasses NIC entirely
///   8. Pre-flight error paths    — invalid config / null pool / empty hostname
///   9. DpdkPollable conformance  — concept checks via static_assert
///
/// Note: ARP / DNS / ICMP state-machine tests were previously absent (only
/// fuzzer harnesses existed). This file partially satisfies M11 of the
/// review plan as a side effect.

#include <array>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include <rte_mbuf.h>
#include <rte_mempool.h>

#include "dpdk_test_env.hpp"  // IWYU pragma: keep — boots EAL once
#include "eph/dpdk/dns.hpp"
#include "eph/net/dpdk/poller.hpp"
#include "eph/utils/time.hpp"

using namespace eph::dpdk;
using eph::dpdk::dns::AsyncDnsResolverT;
using eph::dpdk::dns::DnsConfig;
using eph::dpdk::dns::ResolveStatus;

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// FakeNicIo — substitutes for RealNicIo in tests; records sent packets.
// ─────────────────────────────────────────────────────────────────────────────
//
// Static state because the AsyncDnsResolverT template parameter must be a
// type, not an instance. Tests reset the state in SetUp(). This is fine for
// single-threaded gtest execution; if we ever need parallel tests, swap to
// thread_local or a per-instance pointer.
struct FakeNicIo {
    struct SentRecord {
        uint16_t  port_id;
        uint16_t  queue_id;
        uint16_t  pkt_len;
    };

    static inline std::vector<SentRecord> sent_records{};
    /// `tx_outcome` controls what TX returns. Default = always succeed (sent=1).
    /// Tests can set to `0` to simulate ring-fullness (causes the resolver to
    /// log + retry).
    static inline uint16_t tx_outcome = 1;

    [[nodiscard]] static uint16_t tx_burst(uint16_t port_id, uint16_t queue_id,
                                            rte_mbuf** pkts, uint16_t n) noexcept {
        for (uint16_t i = 0; i < n; ++i) {
            if (i < tx_outcome) {
                sent_records.push_back(SentRecord{
                    port_id, queue_id,
                    rte_pktmbuf_pkt_len(pkts[i]),
                });
                // Consume the packet (mimics real PMD behaviour) — the
                // resolver does NOT free packets it sent successfully.
                rte_pktmbuf_free(pkts[i]);
            }
        }
        return tx_outcome > n ? n : tx_outcome;
    }

    static void reset() {
        sent_records.clear();
        tx_outcome = 1;
    }
};

using TestResolver = AsyncDnsResolverT<FakeNicIo>;

// Compile-time conformance: TestResolver MUST satisfy DpdkPollable. This
// catches concept drift early — if the concept grows a new requirement,
// the resolver compilation breaks here, not deep inside `Poller::add()`.
//
// Note: the concept declaration uses `DpdkPoller<void>*` for
// notify_attached_; our resolver accepts `void*` (it doesn't need to call
// back into the Poller). DpdkPoller<void>* is implicitly convertible to
// void* so this passes.
static_assert(eph::net::dpdk::DpdkPollable<TestResolver>,
              "AsyncDnsResolverT must satisfy DpdkPollable concept");

// ─────────────────────────────────────────────────────────────────────────────
// Reply builder — wraps a DNS A-record reply in an Ethernet/IP/UDP frame
// using the same helper the resolver uses for queries. Returns an mbuf
// owned by the caller (caller responsibility to free or hand to
// process_burst_, which frees it).
// ─────────────────────────────────────────────────────────────────────────────

/// Build the DNS payload for a reply: header + question (echoed) + answer.
/// `tx_id` is host order; written into the header via hton16.
std::vector<uint8_t>
build_dns_reply_payload(uint16_t tx_id_host, uint16_t flags,
                         const std::string& hostname,
                         uint32_t answer_ip_host) {
    std::vector<uint8_t> pkt;
    eph::dpdk::dns::DnsHeader hdr{};
    hdr.id       = eph::dpdk::net::hton16(tx_id_host);
    hdr.flags    = eph::dpdk::net::hton16(flags);
    hdr.qd_count = eph::dpdk::net::hton16(1);
    hdr.an_count = eph::dpdk::net::hton16(1);
    pkt.insert(pkt.end(),
               reinterpret_cast<uint8_t*>(&hdr),
               reinterpret_cast<uint8_t*>(&hdr) + sizeof(hdr));

    // Question: echo our QNAME
    uint8_t qname[256];
    size_t qname_len = eph::dpdk::dns::detail::encode_qname(qname, hostname);
    pkt.insert(pkt.end(), qname, qname + qname_len);
    uint16_t qtype  = eph::dpdk::net::hton16(eph::dpdk::dns::kDnsTypeA);
    uint16_t qclass = eph::dpdk::net::hton16(eph::dpdk::dns::kDnsClassIn);
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&qtype),
               reinterpret_cast<uint8_t*>(&qtype) + 2);
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&qclass),
               reinterpret_cast<uint8_t*>(&qclass) + 2);

    // Answer: A record using QNAME pointer compression to offset 12.
    uint16_t ptr = eph::dpdk::net::hton16(0xC00C);
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&ptr),
               reinterpret_cast<uint8_t*>(&ptr) + 2);
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&qtype),
               reinterpret_cast<uint8_t*>(&qtype) + 2);
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&qclass),
               reinterpret_cast<uint8_t*>(&qclass) + 2);
    uint32_t ttl = eph::dpdk::net::hton32(60);
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&ttl),
               reinterpret_cast<uint8_t*>(&ttl) + 4);
    uint16_t rdlen = eph::dpdk::net::hton16(4);
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&rdlen),
               reinterpret_cast<uint8_t*>(&rdlen) + 2);
    uint32_t ip_net = eph::dpdk::net::hton32(answer_ip_host);
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&ip_net),
               reinterpret_cast<uint8_t*>(&ip_net) + 4);
    return pkt;
}

/// Build a complete frame (Eth + IP + UDP + DNS reply) addressed to
/// `(dst_ip:dst_port)` from `(src_ip:src_port)`. We invert src/dst from the
/// resolver's perspective: the reply's src is the nameserver, dst is us.
rte_mbuf*
build_reply_mbuf(rte_mempool* pool,
                  uint32_t src_ip_host, uint16_t src_port_host,   // nameserver
                  uint32_t dst_ip_host, uint16_t dst_port_host,   // us (ephemeral)
                  const std::vector<uint8_t>& dns_payload) {
    rte_ether_addr src_mac{{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}};
    rte_ether_addr dst_mac{{0x01, 0x02, 0x03, 0x04, 0x05, 0x06}};
    return eph::dpdk::dns::detail::build_dns_packet(
        pool, src_mac, dst_mac,
        src_ip_host, dst_ip_host,
        src_port_host, dst_port_host,
        dns_payload.data(), dns_payload.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// Test fixture — provides shared mempool + reset of FakeNicIo state.
// ─────────────────────────────────────────────────────────────────────────────

class AsyncDnsTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        pool_ = rte_pktmbuf_pool_create(
            "test_dns_async_pool",
            /*n=*/256, /*cache=*/16, /*priv_size=*/0,
            RTE_MBUF_DEFAULT_BUF_SIZE, SOCKET_ID_ANY);
        ASSERT_NE(pool_, nullptr) << "rte_pktmbuf_pool_create failed";
        // Calibrate TSC so the resolver's TSC::to_cycles() returns real
        // cycle counts instead of falling back to the 3 GHz estimate.
        // Without this, the test's `to_cycles` and the resolver's
        // `to_cycles` would use *different* fallback estimates, breaking
        // the retry timing math.
        eph::utils::TSC::init();
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

    /// Default config — 8.8.8.8 nameserver, port 53, 1s timeout.
    static DnsConfig default_cfg() noexcept {
        DnsConfig c{};
        c.nameserver_ip = 0x08080808;
        c.port          = 53;
        c.timeout       = std::chrono::milliseconds{1000};
        return c;
    }

    /// Construct a resolver with the standard test address layout:
    ///   port=0 queue=0 src_ip=10.0.0.1 dst_mac=zeros (net_null doesn't care).
    static TestResolver make_resolver(DnsConfig cfg = default_cfg()) {
        rte_ether_addr src_mac{};
        rte_ether_addr dst_mac{};
        return TestResolver(/*port=*/0, /*queue=*/0, pool_,
                            src_mac, dst_mac,
                            /*src_ip=*/0x0A000001, cfg);
    }

    /// Helper: drive the resolver's poll-tick with a TSC value chosen so
    /// the deadline / retry math behaves predictably. Returns the resolver's
    /// current status after the tick.
    static ResolveStatus tick(TestResolver& r, uint64_t tsc) noexcept {
        r.on_poll_tick_(tsc);
        return r.status();
    }

    static rte_mempool* pool_;
};

rte_mempool* AsyncDnsTest::pool_ = nullptr;

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// 1. Happy path — query → reply → Ready
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AsyncDnsTest, HappyPath_StartSendsQueryReplyTransitionsToReady) {
    auto r = make_resolver();
    EXPECT_EQ(r.status(), ResolveStatus::Idle);

    auto sr = r.start("example.com");
    ASSERT_TRUE(sr.has_value()) << sr.error().detail;
    EXPECT_EQ(r.status(), ResolveStatus::InProgress);
    EXPECT_EQ(r.attempts_sent(), 1);
    EXPECT_EQ(FakeNicIo::sent_records.size(), 1u);
    EXPECT_EQ(FakeNicIo::sent_records[0].port_id, 0u);
    EXPECT_EQ(FakeNicIo::sent_records[0].queue_id, 0u);

    // Build a matching reply and feed it via process_burst_.
    auto payload = build_dns_reply_payload(
        r.tx_id(),
        eph::dpdk::dns::kDnsFlagQr | eph::dpdk::dns::kDnsFlagRd,
        "example.com",
        /*answer_ip=*/0xC0A80105);  // 192.168.1.5
    auto* reply = build_reply_mbuf(
        pool_,
        /*src=*/0x08080808, 53,
        /*dst=*/0x0A000001, r.src_port(),
        payload);
    ASSERT_NE(reply, nullptr);

    rte_mbuf* burst[1] = {reply};
    r.process_burst_(burst, 1, /*rx_tsc=*/eph::utils::TSC::now());

    EXPECT_EQ(r.status(), ResolveStatus::Ready);
    auto result = r.result();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0xC0A80105u);
    EXPECT_TRUE(r.is_done());
}

// ═══════════════════════════════════════════════════════════════════════
// 2. Retry — first reply dropped, on_poll_tick re-sends, eventually Ready
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AsyncDnsTest, Retry_OnPollTickResendsAndAcceptsLaterReply) {
    auto r = make_resolver();
    auto sr = r.start("retry.example.com");
    ASSERT_TRUE(sr.has_value());
    ASSERT_EQ(r.attempts_sent(), 1);
    ASSERT_EQ(FakeNicIo::sent_records.size(), 1u);

    // Tick at a TSC far enough in the future to trigger a retry but
    // well inside the 1s deadline. Use a healthy 500ms window — even
    // on a 1GHz fallback that's 500 million cycles, comfortably past
    // the retry interval (≥ 100ms).
    auto cycles_500ms = eph::utils::TSC::to_cycles(500'000'000.0).value_or(
        500'000'000ULL);  // 1 GHz fallback estimate
    uint64_t now = eph::utils::TSC::now() + cycles_500ms;
    auto st = tick(r, now);
    EXPECT_EQ(st, ResolveStatus::InProgress);
    // Second send happened on the tick.
    EXPECT_EQ(r.attempts_sent(), 2);
    EXPECT_EQ(FakeNicIo::sent_records.size(), 2u);

    // Now feed a valid reply matching the (still unchanged) tx_id.
    auto payload = build_dns_reply_payload(
        r.tx_id(),
        eph::dpdk::dns::kDnsFlagQr | eph::dpdk::dns::kDnsFlagRd,
        "retry.example.com", 0x0A0A0A0Au);  // 10.10.10.10
    auto* reply = build_reply_mbuf(
        pool_, 0x08080808, 53, 0x0A000001, r.src_port(), payload);
    ASSERT_NE(reply, nullptr);
    rte_mbuf* burst[1] = {reply};
    r.process_burst_(burst, 1, eph::utils::TSC::now());

    EXPECT_EQ(r.status(), ResolveStatus::Ready);
    EXPECT_EQ(*r.result(), 0x0A0A0A0Au);
}

// ═══════════════════════════════════════════════════════════════════════
// 3. Timeout — no reply ever; status advances to TimedOut on tick
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AsyncDnsTest, Timeout_NoReplyEverElapsesDeadline) {
    DnsConfig cfg = default_cfg();
    // Use a small but non-zero timeout. The resolver's retry interval is
    // max(timeout/3, 100ms) = 100ms with this config — so no extra retries
    // queue inside the 200ms window past the deadline.
    cfg.timeout = std::chrono::milliseconds{200};
    auto r = make_resolver(cfg);
    auto sr = r.start("timeout.example");
    ASSERT_TRUE(sr.has_value());
    ASSERT_EQ(r.status(), ResolveStatus::InProgress);

    // Advance well past the 200ms deadline. Use 5s of headroom — the
    // TSC fallback estimate (3 GHz) and the calibrated path both put
    // 5s far past 200ms.
    auto cycles_5s = eph::utils::TSC::to_cycles(5'000'000'000.0).value_or(
        5ULL * 1'000'000'000ULL);
    uint64_t now = eph::utils::TSC::now() + cycles_5s;
    auto st = tick(r, now);
    EXPECT_EQ(st, ResolveStatus::TimedOut);
    EXPECT_TRUE(r.is_done());
    auto res = r.result();
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code, eph::core::Error::Timeout);
}

// ═══════════════════════════════════════════════════════════════════════
// 4. Bad reply — wrong txn ID is silently ignored, eventually times out
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AsyncDnsTest, BadReply_WrongTxIdIgnored) {
    auto r = make_resolver(default_cfg());
    ASSERT_TRUE(r.start("badtxn.example").has_value());
    uint16_t actual_txid = r.tx_id();
    // Pick a tx_id guaranteed different.
    uint16_t wrong_txid = static_cast<uint16_t>(actual_txid ^ 0xFFFF);

    auto payload = build_dns_reply_payload(
        wrong_txid,
        eph::dpdk::dns::kDnsFlagQr | eph::dpdk::dns::kDnsFlagRd,
        "badtxn.example", 0xDEADBEEFu);
    auto* reply = build_reply_mbuf(
        pool_, 0x08080808, 53, 0x0A000001, r.src_port(), payload);
    ASSERT_NE(reply, nullptr);
    rte_mbuf* burst[1] = {reply};
    r.process_burst_(burst, 1, eph::utils::TSC::now());

    // Reply was rejected — still InProgress.
    EXPECT_EQ(r.status(), ResolveStatus::InProgress);
}

// ═══════════════════════════════════════════════════════════════════════
// 5. Malformed reply — truncated DNS section must not crash
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AsyncDnsTest, MalformedReply_TruncatedDnsDoesNotCrash) {
    auto r = make_resolver(default_cfg());
    ASSERT_TRUE(r.start("trunc.example").has_value());

    // Build a normal payload then chop the answer section so the parser
    // sees an inconsistent an_count vs actual bytes. Same fault model
    // the fuzz harness hits.
    auto payload = build_dns_reply_payload(
        r.tx_id(),
        eph::dpdk::dns::kDnsFlagQr | eph::dpdk::dns::kDnsFlagRd,
        "trunc.example", 0xC0A80101u);
    ASSERT_GT(payload.size(), 6u);
    payload.resize(payload.size() - 6);  // drop tail; truncates RDATA

    auto* reply = build_reply_mbuf(
        pool_, 0x08080808, 53, 0x0A000001, r.src_port(), payload);
    ASSERT_NE(reply, nullptr);
    rte_mbuf* burst[1] = {reply};
    // Must not crash — truncated parse path returns error which we silently
    // ignore (defense-in-depth — never trust adversarial replies).
    r.process_burst_(burst, 1, eph::utils::TSC::now());
    EXPECT_EQ(r.status(), ResolveStatus::InProgress);
}

// ═══════════════════════════════════════════════════════════════════════
// 6. Concurrent resolves — 4 resolvers complete independently on shared pool
// ═══════════════════════════════════════════════════════════════════════
//
// This is the *raison d'etre* of the async API: parallelism. We don't
// wire them through a real DpdkPoller (that would require a NIC); we
// drive each one individually but interleave the cycles to prove they
// hold independent state.

TEST_F(AsyncDnsTest, ConcurrentResolves_FourIndependentResolversAllReady) {
    constexpr int N = 4;
    std::array<TestResolver, N> resolvers = {
        make_resolver(), make_resolver(), make_resolver(), make_resolver(),
    };
    const std::array<std::string, N> hostnames = {
        "venue0.example", "venue1.example",
        "venue2.example", "venue3.example",
    };
    const std::array<uint32_t, N> answers = {
        0x01010101u, 0x02020202u, 0x03030303u, 0x04040404u,
    };

    // Start all four. Each picks its own ephemeral src_port + tx_id.
    for (int i = 0; i < N; ++i) {
        auto sr = resolvers[i].start(hostnames[i]);
        ASSERT_TRUE(sr.has_value()) << "resolver #" << i << ": "
                                     << sr.error().detail;
        EXPECT_EQ(resolvers[i].status(), ResolveStatus::InProgress);
    }
    EXPECT_EQ(FakeNicIo::sent_records.size(), static_cast<size_t>(N));

    // Build replies and feed them out-of-order — resolver 2 first, then 0,
    // then 3, then 1 — to verify each only matches its own tx_id.
    const int order[N] = {2, 0, 3, 1};
    for (int idx : order) {
        auto payload = build_dns_reply_payload(
            resolvers[idx].tx_id(),
            eph::dpdk::dns::kDnsFlagQr | eph::dpdk::dns::kDnsFlagRd,
            hostnames[idx], answers[idx]);
        auto* reply = build_reply_mbuf(
            pool_, 0x08080808, 53, 0x0A000001,
            resolvers[idx].src_port(), payload);
        ASSERT_NE(reply, nullptr);
        rte_mbuf* burst[1] = {reply};
        resolvers[idx].process_burst_(burst, 1, eph::utils::TSC::now());
    }

    // All four should be Ready with their distinct answers.
    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(resolvers[i].status(), ResolveStatus::Ready)
            << "resolver " << i << " failed to reach Ready";
        auto res = resolvers[i].result();
        ASSERT_TRUE(res.has_value());
        EXPECT_EQ(*res, answers[i]) << "resolver " << i << " wrong IP";
    }
}

// ═══════════════════════════════════════════════════════════════════════
// 7. Dotted-decimal fast path — bypasses NIC entirely
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AsyncDnsTest, DottedDecimal_FastPath_SkipsTxBurst) {
    auto r = make_resolver();
    auto sr = r.start("203.0.113.42");
    ASSERT_TRUE(sr.has_value());
    EXPECT_EQ(r.status(), ResolveStatus::Ready);
    EXPECT_EQ(*r.result(), 0xCB00712Au);  // 203.0.113.42
    // No TX should have happened.
    EXPECT_EQ(FakeNicIo::sent_records.size(), 0u);
    EXPECT_EQ(r.attempts_sent(), 0);
}

// ═══════════════════════════════════════════════════════════════════════
// 8. Pre-flight error paths — empty hostname / null pool / invalid config
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AsyncDnsTest, EmptyHostnameReturnsError) {
    auto r = make_resolver();
    auto sr = r.start("");
    ASSERT_FALSE(sr.has_value());
    EXPECT_EQ(sr.error().code, eph::core::Error::InvalidConfig);
    EXPECT_EQ(r.status(), ResolveStatus::Error);
}

TEST_F(AsyncDnsTest, NullPoolReturnsError) {
    rte_ether_addr src_mac{};
    rte_ether_addr dst_mac{};
    TestResolver r(0, 0, /*pool=*/nullptr, src_mac, dst_mac,
                    0x0A000001u, default_cfg());
    auto sr = r.start("foo.example");
    ASSERT_FALSE(sr.has_value());
    EXPECT_EQ(sr.error().code, eph::core::Error::InvalidConfig);
}

TEST_F(AsyncDnsTest, InvalidConfigZeroNameserverReturnsError) {
    DnsConfig cfg = default_cfg();
    cfg.nameserver_ip = 0;  // fails validate()
    auto r = make_resolver(cfg);
    auto sr = r.start("foo.example");
    ASSERT_FALSE(sr.has_value());
    EXPECT_EQ(sr.error().code, eph::core::Error::InvalidConfig);
    EXPECT_EQ(r.status(), ResolveStatus::Error);
}

TEST_F(AsyncDnsTest, RepeatedStartFromNonIdleStateRejected) {
    auto r = make_resolver();
    ASSERT_TRUE(r.start("once.example").has_value());
    auto sr2 = r.start("twice.example");
    ASSERT_FALSE(sr2.has_value());
    EXPECT_EQ(sr2.error().code, eph::core::Error::InvalidConfig);
}

// ═══════════════════════════════════════════════════════════════════════
// 9. Tuple — verifies the 5-tuple supplied to the Poller after start()
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AsyncDnsTest, TupleForPoller_AdvertisesCorrectFields) {
    auto r = make_resolver();
    ASSERT_TRUE(r.start("tuple.example").has_value());

    uint32_t src_ip = 0, dst_ip = 0;
    uint16_t src_port = 0, dst_port = 0;
    uint8_t  proto = 0;
    r.tuple_for_poller_(&src_ip, &dst_ip, &src_port, &dst_port, &proto);
    EXPECT_EQ(src_ip,   0x0A000001u);    // resolver's local IP
    EXPECT_EQ(dst_ip,   0x08080808u);    // configured nameserver
    EXPECT_EQ(src_port, r.src_port());    // ephemeral
    EXPECT_EQ(dst_port, 53u);             // standard DNS port
    EXPECT_EQ(proto,    eph::dpdk::net::kIpProtoUdp);
}

// ═══════════════════════════════════════════════════════════════════════
// 10. Late-arriving reply after Ready is silently dropped (mbuf freed)
// ═══════════════════════════════════════════════════════════════════════
//
// Defensive: in retry scenarios the resolver may have sent N queries
// and N replies arrive — only the first must matter. Subsequent mbufs
// must be freed without state churn.

TEST_F(AsyncDnsTest, LateArrivingReplyAfterReadyIsIgnored) {
    auto r = make_resolver();
    ASSERT_TRUE(r.start("late.example").has_value());

    auto payload = build_dns_reply_payload(
        r.tx_id(),
        eph::dpdk::dns::kDnsFlagQr | eph::dpdk::dns::kDnsFlagRd,
        "late.example", 0x11223344u);
    auto* reply1 = build_reply_mbuf(
        pool_, 0x08080808, 53, 0x0A000001, r.src_port(), payload);
    rte_mbuf* burst[1] = {reply1};
    r.process_burst_(burst, 1, eph::utils::TSC::now());
    ASSERT_EQ(r.status(), ResolveStatus::Ready);

    // Build a second reply with a different IP. After Ready, we must
    // ignore it (no overwrite) and free the mbuf.
    auto payload2 = build_dns_reply_payload(
        r.tx_id(),
        eph::dpdk::dns::kDnsFlagQr | eph::dpdk::dns::kDnsFlagRd,
        "late.example", 0x55667788u);
    auto* reply2 = build_reply_mbuf(
        pool_, 0x08080808, 53, 0x0A000001, r.src_port(), payload2);
    rte_mbuf* burst2[1] = {reply2};
    r.process_burst_(burst2, 1, eph::utils::TSC::now());
    EXPECT_EQ(r.status(), ResolveStatus::Ready);
    EXPECT_EQ(*r.result(), 0x11223344u);  // first answer wins
}
