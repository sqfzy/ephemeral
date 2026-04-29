/// @file test_icmp_dispatch.cpp
/// Unit tests for the ICMP Frag Needed (Type 3 Code 4) dispatch path
/// added in T2.13 of the crypto-HFT review meta-plan.
///
/// Test strategy (mirrors `test_dns_async.cpp`):
///   - Boot EAL once with `--no-pci --vdev=net_null0` via `dpdk_test_env.hpp`
///     so we can `rte_pktmbuf_pool_create` real mbufs. No physical NIC
///     required.
///   - Drive the dispatch path directly through `IcmpRegistry` (the
///     concrete state-machine that lives behind `Platform::register_icmp_target`).
///     Platform itself is a thin facade that requires a real port for
///     init; bypassing it yields the same exact dispatch logic with no
///     EAL port dependency.
///   - For each case build a crafted ICMP Type 3 Code 4 mbuf, run it
///     through `parse_icmp`, and feed the resulting `ParsedIcmp` into
///     `IcmpRegistry::dispatch`. Assert on (a) whether the registered
///     callback fired, (b) the MTU value forwarded, and (c) the
///     `dispatched()` counter.
///
/// Coverage map (T2.13 plan):
///   1. FragNeededDispatchedToOwnerStream   — registered tuple receives MTU
///   2. FragNeededForUnknownTupleDropped    — unrelated tuple → no callback
///   3. MtuFloorClamped                     — TcpSession's MSS-floor clamp
///                                            handles MTU < min headers
///   4. CodeNot4Ignored                     — type=3 code in {0,1,3} → no
///                                            callback (the registry only
///                                            dispatches embedded_valid
///                                            from is_frag_needed())
///   5. NonTcpProtoDoesNotMatchTcpTarget    — embedded UDP must not match
///                                            a TCP-registered tuple
///   6. UnregisterStopsDispatch             — handle drop → next dispatch no-op
///   7. RegistryFullReturnsOom              — kMaxTargets cap
///
/// Note: the existing `test_packet_parse_adversarial.cpp` covers the
/// `parse_icmp` parser in isolation; this file focuses on the
/// register/dispatch state machine that connects parse output to the
/// owning stream's `on_icmp_frag_needed` callback.

#include <atomic>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#include "dpdk_test_env.hpp"  // IWYU pragma: keep — boots EAL once
#include "eph/dpdk/detail/icmp_registry.hpp"
#include "eph/dpdk/net_header.hpp"
#include "eph/dpdk/packet_core.hpp"
#include "eph/dpdk/packet_parse.hpp"

using namespace eph::dpdk::net;
using eph::dpdk::detail::IcmpRegistry;

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// FakeStream — stand-in for a `DpdkTcpStream` callback target. Records
// the MTU values passed to its on_icmp_frag_needed_for_test method and
// also exercises a TcpSession-equivalent MSS-floor clamp so we can
// assert on the same invariants the real session enforces (MTU below
// IP+TCP header overhead must not produce UB / wrap-around).
// ─────────────────────────────────────────────────────────────────────────────

struct FakeStream {
    uint16_t effective_mss = 1460;            ///< current MSS; only ever shrinks
    uint16_t initial_mss   = 1460;            ///< starting MSS — for floor assertion
    std::vector<uint16_t> received_mtus{};    ///< all MTU values received
    int      callback_invocations = 0;        ///< count of callback hits

    /// Same algorithm as `TcpSession::on_icmp_frag_needed` (tcp.hpp:1469).
    /// Replicated here so we don't have to instantiate a full TcpSession
    /// (that needs a TcpConfig + state machine state which is irrelevant
    /// to the dispatch test).
    void on_icmp_frag_needed(uint16_t next_hop_mtu) noexcept {
        ++callback_invocations;
        received_mtus.push_back(next_hop_mtu);
        constexpr uint16_t kMinHeaders =
            kIpv4HeaderLen + kTcpHeaderLen;  // 20 + 20 = 40
        if (next_hop_mtu <= kMinHeaders) {
            // RFC 1191 floor clamp — MTU < 40 cannot fit any TCP. Ignore.
            return;
        }
        const uint16_t new_mss =
            static_cast<uint16_t>(next_hop_mtu - kMinHeaders);
        effective_mss = std::min(effective_mss, new_mss);
    }
};

/// Trampoline matching `IcmpRegistry::MtuCallback` signature. Forwards
/// the call into `FakeStream::on_icmp_frag_needed`. The registry
/// stores the void* as the registered "stream" — we cast back here.
void fake_stream_mtu_cb(void* stream, uint16_t mtu) noexcept {
    static_cast<FakeStream*>(stream)->on_icmp_frag_needed(mtu);
}

// ─────────────────────────────────────────────────────────────────────────────
// ICMP Type 3 Code 4 builder. Layout:
//   Ether (14) | outer IP (20) | ICMP hdr (8) | embedded IP (20) | embedded L4 first 8 bytes
// Total = 70 bytes.
//
// Returns an mbuf owned by the caller — the test must `rte_pktmbuf_free`
// it, OR pass it through `parse_icmp` (which doesn't free) and then
// free.
// ─────────────────────────────────────────────────────────────────────────────

struct IcmpBuilderArgs {
    uint16_t next_hop_mtu  = 1400;
    uint8_t  type          = 3;        ///< Destination Unreachable
    uint8_t  code          = 4;        ///< Fragmentation Needed
    uint8_t  embedded_proto = kIpProtoTcp;
    uint32_t embedded_src_ip   = 0x0A000001u;
    uint32_t embedded_dst_ip   = 0x0A000002u;
    uint16_t embedded_src_port = 12345;
    uint16_t embedded_dst_port = 443;
};

rte_mbuf* build_icmp_mbuf(rte_mempool* pool, const IcmpBuilderArgs& a) {
    auto* mbuf = rte_pktmbuf_alloc(pool);
    if (!mbuf) return nullptr;
    constexpr size_t eth_len    = kEtherHeaderLen;
    constexpr size_t outer_ip   = kIpv4HeaderLen;
    constexpr size_t icmp_hdr   = 8;
    constexpr size_t emb_ip     = kIpv4HeaderLen;
    constexpr size_t emb_l4     = 8;
    constexpr size_t total      = eth_len + outer_ip + icmp_hdr + emb_ip + emb_l4;
    auto* p = reinterpret_cast<uint8_t*>(rte_pktmbuf_append(mbuf, total));
    if (!p) { rte_pktmbuf_free(mbuf); return nullptr; }
    std::memset(p, 0, total);

    // Ethernet — set EtherType IPv4. Addresses don't matter for parse_icmp.
    auto* eth = reinterpret_cast<rte_ether_hdr*>(p);
    eth->ether_type = hton16(kEtherTypeIpv4);

    // Outer IP — protocol = ICMP. src/dst are router/us; not inspected
    // for embedded-tuple matching but populated for completeness.
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(p + eth_len);
    ip->version_ihl   = static_cast<uint8_t>((4u << 4) | 5u);
    ip->total_length  = hton16(static_cast<uint16_t>(
        outer_ip + icmp_hdr + emb_ip + emb_l4));
    ip->next_proto_id = kIpProtoIcmp;
    ip->src_addr      = hton32(0x0A0000FE);  // router
    ip->dst_addr      = hton32(a.embedded_src_ip);

    uint8_t* icmp = p + eth_len + outer_ip;
    icmp[0] = a.type;
    icmp[1] = a.code;
    // checksum (icmp[2..3]) and unused (icmp[4..5]) left zero
    uint16_t mtu_net = hton16(a.next_hop_mtu);
    std::memcpy(icmp + 6, &mtu_net, 2);

    auto* e_ip = reinterpret_cast<rte_ipv4_hdr*>(icmp + icmp_hdr);
    e_ip->version_ihl   = static_cast<uint8_t>((4u << 4) | 5u);
    e_ip->next_proto_id = a.embedded_proto;
    e_ip->src_addr      = hton32(a.embedded_src_ip);
    e_ip->dst_addr      = hton32(a.embedded_dst_ip);

    uint8_t* e_l4 = reinterpret_cast<uint8_t*>(e_ip) + emb_ip;
    uint16_t sp_n = hton16(a.embedded_src_port);
    uint16_t dp_n = hton16(a.embedded_dst_port);
    std::memcpy(e_l4,     &sp_n, 2);
    std::memcpy(e_l4 + 2, &dp_n, 2);
    return mbuf;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test fixture
// ─────────────────────────────────────────────────────────────────────────────

class IcmpDispatchTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        pool_ = rte_pktmbuf_pool_create(
            "test_icmp_dispatch_pool",
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

    /// The standard 4-tuple our FakeStream registers under. Matches the
    /// build_icmp_mbuf default `embedded_*` values so the registry
    /// dispatch lookup hits cleanly when no override is given.
    static constexpr uint32_t kStreamSrcIp   = 0x0A000001u;
    static constexpr uint32_t kStreamDstIp   = 0x0A000002u;
    static constexpr uint16_t kStreamSrcPort = 12345;
    static constexpr uint16_t kStreamDstPort = 443;

    static ConnectionTuple stream_tuple() noexcept {
        return ConnectionTuple{kStreamSrcIp, kStreamDstIp,
                               kStreamSrcPort, kStreamDstPort};
    }

    /// Helper: parse + dispatch a crafted ICMP mbuf. Frees the mbuf on
    /// the way out — gtest assertions stay focused on the dispatch
    /// result, not bookkeeping.
    static void parse_and_dispatch(IcmpRegistry& reg, rte_mbuf* mbuf) {
        ASSERT_NE(mbuf, nullptr);
        auto parsed = parse_icmp(mbuf);
        // Forward `parsed` to the registry. Note: dispatch() is a no-op
        // when embedded_valid==false (e.g. truncated or non-TCP/UDP).
        reg.dispatch(parsed);
        rte_pktmbuf_free(mbuf);
    }

    static rte_mempool* pool_;
};
rte_mempool* IcmpDispatchTest::pool_ = nullptr;

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// 1. Frag Needed dispatched to the owner stream
// ═══════════════════════════════════════════════════════════════════════

TEST_F(IcmpDispatchTest, FragNeededDispatchedToOwnerStream) {
    auto reg = std::make_shared<IcmpRegistry>();
    FakeStream stream;

    auto h = reg->register_target(stream_tuple(), kIpProtoTcp,
                                   &stream, &fake_stream_mtu_cb);
    ASSERT_TRUE(h.has_value()) << h.error().detail;
    EXPECT_EQ(reg->size(), 1u);

    auto* mbuf = build_icmp_mbuf(pool_, IcmpBuilderArgs{
        .next_hop_mtu = 1400,
        .embedded_proto    = kIpProtoTcp,
        .embedded_src_ip   = kStreamSrcIp,
        .embedded_dst_ip   = kStreamDstIp,
        .embedded_src_port = kStreamSrcPort,
        .embedded_dst_port = kStreamDstPort,
    });
    parse_and_dispatch(*reg, mbuf);

    EXPECT_EQ(stream.callback_invocations, 1);
    ASSERT_EQ(stream.received_mtus.size(), 1u);
    EXPECT_EQ(stream.received_mtus[0], 1400u);
    // Effective MSS was 1460 → MTU 1400 - 40 = 1360 → clamps down.
    EXPECT_EQ(stream.effective_mss, 1360u);
    EXPECT_EQ(reg->dispatched(), 1u);
}

// ═══════════════════════════════════════════════════════════════════════
// 2. Frag Needed for an unknown 4-tuple — no callback fires
// ═══════════════════════════════════════════════════════════════════════

TEST_F(IcmpDispatchTest, FragNeededForUnknownTupleDropped) {
    auto reg = std::make_shared<IcmpRegistry>();
    FakeStream stream;

    auto h = reg->register_target(stream_tuple(), kIpProtoTcp,
                                   &stream, &fake_stream_mtu_cb);
    ASSERT_TRUE(h.has_value());

    // Build an ICMP packet whose embedded 4-tuple references a
    // *different* connection (different src_port). The registry must
    // not match it — no callback, no counter bump.
    auto* mbuf = build_icmp_mbuf(pool_, IcmpBuilderArgs{
        .next_hop_mtu = 1400,
        .embedded_proto    = kIpProtoTcp,
        .embedded_src_ip   = kStreamSrcIp,
        .embedded_dst_ip   = kStreamDstIp,
        .embedded_src_port = static_cast<uint16_t>(kStreamSrcPort + 1),  // wrong
        .embedded_dst_port = kStreamDstPort,
    });
    parse_and_dispatch(*reg, mbuf);

    EXPECT_EQ(stream.callback_invocations, 0);
    EXPECT_EQ(stream.effective_mss, stream.initial_mss);  // unchanged
    EXPECT_EQ(reg->dispatched(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════
// 3. MTU floor clamp — MTU below IPv4 minimum (296) is dispatched but
//    the receiver MUST not produce a wrap-around or UB. The TcpSession
//    semantics: ignore the MSS update entirely when next_hop_mtu <= 40
//    (kIpv4HeaderLen + kTcpHeaderLen). Counter still bumps so operators
//    can see "we got hostile/malformed Frag Needed messages".
// ═══════════════════════════════════════════════════════════════════════

TEST_F(IcmpDispatchTest, MtuFloorClamped_BelowIp4MinimumNoUb) {
    auto reg = std::make_shared<IcmpRegistry>();
    FakeStream stream;

    auto h = reg->register_target(stream_tuple(), kIpProtoTcp,
                                   &stream, &fake_stream_mtu_cb);
    ASSERT_TRUE(h.has_value());

    // RFC 791 mandates an absolute IPv4 minimum of 68 bytes. We test
    // even smaller — and an attacker's "Frag Needed mtu=0" — to
    // confirm the floor clamp in TcpSession protects us.
    constexpr uint16_t kBelowMinimum[] = {0, 1, 20, 39, 40};
    for (uint16_t mtu : kBelowMinimum) {
        auto* mbuf = build_icmp_mbuf(pool_, IcmpBuilderArgs{
            .next_hop_mtu      = mtu,
            .embedded_proto    = kIpProtoTcp,
            .embedded_src_ip   = kStreamSrcIp,
            .embedded_dst_ip   = kStreamDstIp,
            .embedded_src_port = kStreamSrcPort,
            .embedded_dst_port = kStreamDstPort,
        });
        parse_and_dispatch(*reg, mbuf);
    }

    // The callback fired N times (registry forwards every embedded-valid
    // hit) but effective_mss never changed — TcpSession's <=40 floor
    // clamp swallowed each MTU.
    EXPECT_EQ(stream.callback_invocations,
              static_cast<int>(std::size(kBelowMinimum)));
    EXPECT_EQ(stream.effective_mss, stream.initial_mss)
        << "MSS must not shrink below the IPv4-min derivation";
    EXPECT_EQ(reg->dispatched(), std::size(kBelowMinimum));
}

// ═══════════════════════════════════════════════════════════════════════
// 4. Non-Frag-Needed ICMP types/codes are NOT dispatched
// ═══════════════════════════════════════════════════════════════════════
//
// Codes 0/1/2/3 of Type 3 (Net unreachable, Host unreachable, Protocol
// unreachable, Port unreachable) carry no MTU info; the parser leaves
// `embedded_valid=false` for them; dispatch() bails out early.

TEST_F(IcmpDispatchTest, CodeNot4Ignored) {
    auto reg = std::make_shared<IcmpRegistry>();
    FakeStream stream;

    auto h = reg->register_target(stream_tuple(), kIpProtoTcp,
                                   &stream, &fake_stream_mtu_cb);
    ASSERT_TRUE(h.has_value());

    for (uint8_t code : {uint8_t{0}, uint8_t{1}, uint8_t{2}, uint8_t{3}}) {
        auto* mbuf = build_icmp_mbuf(pool_, IcmpBuilderArgs{
            .next_hop_mtu = 1400,
            .type = 3,
            .code = code,
            .embedded_proto    = kIpProtoTcp,
            .embedded_src_ip   = kStreamSrcIp,
            .embedded_dst_ip   = kStreamDstIp,
            .embedded_src_port = kStreamSrcPort,
            .embedded_dst_port = kStreamDstPort,
        });
        parse_and_dispatch(*reg, mbuf);
    }

    EXPECT_EQ(stream.callback_invocations, 0)
        << "Code != 4 must not invoke the Frag-Needed callback";
    EXPECT_EQ(reg->dispatched(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════
// 4b. Type != 3 (e.g. Echo Request type=8) — also not dispatched
// ═══════════════════════════════════════════════════════════════════════

TEST_F(IcmpDispatchTest, TypeNot3Ignored) {
    auto reg = std::make_shared<IcmpRegistry>();
    FakeStream stream;

    auto h = reg->register_target(stream_tuple(), kIpProtoTcp,
                                   &stream, &fake_stream_mtu_cb);
    ASSERT_TRUE(h.has_value());

    auto* mbuf = build_icmp_mbuf(pool_, IcmpBuilderArgs{
        .next_hop_mtu = 1400,
        .type = 8,  // Echo Request — not Frag Needed
        .code = 0,
        .embedded_proto    = kIpProtoTcp,
        .embedded_src_ip   = kStreamSrcIp,
        .embedded_dst_ip   = kStreamDstIp,
        .embedded_src_port = kStreamSrcPort,
        .embedded_dst_port = kStreamDstPort,
    });
    parse_and_dispatch(*reg, mbuf);

    EXPECT_EQ(stream.callback_invocations, 0);
    EXPECT_EQ(reg->dispatched(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════
// 5. Embedded UDP must not match a TCP-registered tuple
// ═══════════════════════════════════════════════════════════════════════

TEST_F(IcmpDispatchTest, NonTcpProtoDoesNotMatchTcpTarget) {
    auto reg = std::make_shared<IcmpRegistry>();
    FakeStream stream;

    auto h = reg->register_target(stream_tuple(), kIpProtoTcp,
                                   &stream, &fake_stream_mtu_cb);
    ASSERT_TRUE(h.has_value());

    auto* mbuf = build_icmp_mbuf(pool_, IcmpBuilderArgs{
        .next_hop_mtu = 1400,
        .embedded_proto    = kIpProtoUdp,  // wrong protocol for our target
        .embedded_src_ip   = kStreamSrcIp,
        .embedded_dst_ip   = kStreamDstIp,
        .embedded_src_port = kStreamSrcPort,
        .embedded_dst_port = kStreamDstPort,
    });
    parse_and_dispatch(*reg, mbuf);

    EXPECT_EQ(stream.callback_invocations, 0);
    EXPECT_EQ(reg->dispatched(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════
// 6. Handle drop unregisters cleanly — next dispatch is a no-op
// ═══════════════════════════════════════════════════════════════════════

TEST_F(IcmpDispatchTest, UnregisterStopsDispatch) {
    auto reg = std::make_shared<IcmpRegistry>();
    FakeStream stream;
    {
        auto h = reg->register_target(stream_tuple(), kIpProtoTcp,
                                       &stream, &fake_stream_mtu_cb);
        ASSERT_TRUE(h.has_value());
        EXPECT_EQ(reg->size(), 1u);

        auto* m1 = build_icmp_mbuf(pool_, IcmpBuilderArgs{
            .next_hop_mtu = 1400,
            .embedded_proto    = kIpProtoTcp,
            .embedded_src_ip   = kStreamSrcIp,
            .embedded_dst_ip   = kStreamDstIp,
            .embedded_src_port = kStreamSrcPort,
            .embedded_dst_port = kStreamDstPort,
        });
        parse_and_dispatch(*reg, m1);
        EXPECT_EQ(stream.callback_invocations, 1);
    }
    // h destructed → registry now empty.
    EXPECT_EQ(reg->size(), 0u);

    auto* m2 = build_icmp_mbuf(pool_, IcmpBuilderArgs{
        .next_hop_mtu = 800,
        .embedded_proto    = kIpProtoTcp,
        .embedded_src_ip   = kStreamSrcIp,
        .embedded_dst_ip   = kStreamDstIp,
        .embedded_src_port = kStreamSrcPort,
        .embedded_dst_port = kStreamDstPort,
    });
    parse_and_dispatch(*reg, m2);
    EXPECT_EQ(stream.callback_invocations, 1)  // unchanged
        << "callback fired after handle dropped — registry leaked the entry";
    EXPECT_EQ(reg->dispatched(), 1u);
}

// ═══════════════════════════════════════════════════════════════════════
// 7. Registry full — kMaxTargets cap returns OutOfMemory
// ═══════════════════════════════════════════════════════════════════════

TEST_F(IcmpDispatchTest, RegistryFullReturnsOom) {
    auto reg = std::make_shared<IcmpRegistry>();
    // Hold all the handles in a vector so they don't release until
    // after the test body — otherwise registration N+1 would refill
    // the freed slot and we'd never hit kMaxTargets.
    std::vector<IcmpRegistry::Handle> handles;
    handles.reserve(IcmpRegistry::kMaxTargets);

    // Use a sentinel non-null void* so register_target's null check
    // doesn't fire. We never dispatch into these slots.
    int dummy = 0;
    void* sentinel_stream = &dummy;

    for (std::size_t i = 0; i < IcmpRegistry::kMaxTargets; ++i) {
        ConnectionTuple t{kStreamSrcIp, kStreamDstIp,
                          static_cast<uint16_t>(20000 + i),  // unique src_port
                          kStreamDstPort};
        auto h = reg->register_target(t, kIpProtoTcp,
                                       sentinel_stream, &fake_stream_mtu_cb);
        ASSERT_TRUE(h.has_value()) << "registration #" << i
                                    << " failed: " << h.error().detail;
        handles.emplace_back(std::move(*h));
    }
    EXPECT_EQ(reg->size(), IcmpRegistry::kMaxTargets);

    // The kMaxTargets+1'th must fail with OutOfMemory.
    ConnectionTuple overflow_tuple{kStreamSrcIp, kStreamDstIp, 30001, kStreamDstPort};
    auto fail = reg->register_target(overflow_tuple, kIpProtoTcp,
                                      sentinel_stream, &fake_stream_mtu_cb);
    ASSERT_FALSE(fail.has_value());
    EXPECT_EQ(fail.error().code, ::eph::core::Error::OutOfMemory);
}

// ═══════════════════════════════════════════════════════════════════════
// 7b. 100+ targets (regression for kMaxTargets bump 64 → 256)
// ═══════════════════════════════════════════════════════════════════════
//
// The rss_scaling_ws bench's conn_count=100 cell hit the original 64-entry
// cap because every DpdkTcpStream registers an ICMP target on
// create_and_attach. After the bump to 256, registering 100 distinct
// tuples must succeed.
TEST_F(IcmpDispatchTest, HundredTargetsRegisterSuccessfully) {
    auto reg = std::make_shared<IcmpRegistry>();
    std::vector<IcmpRegistry::Handle> handles;
    constexpr std::size_t kCount = 100;
    handles.reserve(kCount);

    int dummy = 0;
    void* sentinel_stream = &dummy;

    for (std::size_t i = 0; i < kCount; ++i) {
        ConnectionTuple t{kStreamSrcIp, kStreamDstIp,
                          static_cast<uint16_t>(40000 + i),
                          kStreamDstPort};
        auto h = reg->register_target(t, kIpProtoTcp,
                                       sentinel_stream, &fake_stream_mtu_cb);
        ASSERT_TRUE(h.has_value())
            << "registration #" << i << " failed at kMaxTargets="
            << IcmpRegistry::kMaxTargets << ": " << h.error().detail;
        handles.emplace_back(std::move(*h));
    }
    EXPECT_EQ(reg->size(), kCount);
    static_assert(IcmpRegistry::kMaxTargets >= 256,
                  "kMaxTargets must be at least 256 to support the "
                  "rss_scaling_ws conn_count=100 sweep + headroom");
}

// ═══════════════════════════════════════════════════════════════════════
// 8. Duplicate tuple registration is rejected
// ═══════════════════════════════════════════════════════════════════════

TEST_F(IcmpDispatchTest, DuplicateRegistrationRejected) {
    auto reg = std::make_shared<IcmpRegistry>();
    FakeStream stream;
    auto h1 = reg->register_target(stream_tuple(), kIpProtoTcp,
                                    &stream, &fake_stream_mtu_cb);
    ASSERT_TRUE(h1.has_value());

    auto h2 = reg->register_target(stream_tuple(), kIpProtoTcp,
                                    &stream, &fake_stream_mtu_cb);
    ASSERT_FALSE(h2.has_value());
    EXPECT_EQ(h2.error().code, ::eph::core::Error::InvalidConfig);
    EXPECT_EQ(reg->size(), 1u);
}
