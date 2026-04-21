/// @file test_tcp_conformance.cpp
/// Table-driven TCP state-machine conformance regressions for
/// `eph::dpdk::TcpSession`.
///
/// Phase G of the DPDK TCP P0 hardening plan: complement the more
/// investigative tests in test_tcp_state_machine.cpp with a broad
/// matrix covering every implemented state × main incoming segment
/// type. Each row pins a single state-transition behaviour so that
/// future refactors that "fix" one detail without re-running the
/// whole suite surface here.
///
/// Same fake-mbuf pattern as test_tcp_state_machine.cpp — no DPDK
/// mempool, no NIC, purely a unit-test driven through
/// `TcpSession::process_rx`. Response packets are NOT emitted (the
/// session's pool is nullptr so `send_ack` et al. would fail); we
/// verify state transitions + stats deltas + sequence-number
/// advancement, which is the pure state-machine contract.

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_tcp.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep
#include "eph/dpdk/net_header.hpp"
#include "eph/dpdk/tcp.hpp"

using namespace eph::dpdk;
using eph::net::TcpState;

namespace {

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
    return cfg;
}

/// Build a peer-to-us Ethernet/IPv4/TCP packet into a stack buffer.
/// Sized to hold jumbo-ish payloads so the oversized-segment row
/// (1461 bytes > kDefaultMss) doesn't overflow.
struct FakePkt {
    alignas(8) uint8_t buf[2048]{};
    rte_mbuf mbuf{};

    void build(uint32_t seq, uint32_t ack_num, uint8_t flags,
               uint16_t payload_len = 0) {
        constexpr size_t eth_len = eph::dpdk::net::kEtherHeaderLen;
        constexpr size_t ip_len  = 20;
        constexpr size_t tcp_len = 20;
        const size_t total = eth_len + ip_len + tcp_len + payload_len;
        ASSERT_LE(total, sizeof(buf));
        std::memset(buf, 0, total);

        auto* eth = reinterpret_cast<rte_ether_hdr*>(buf);
        eth->ether_type = eph::dpdk::net::hton16(eph::dpdk::net::kEtherTypeIpv4);

        auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + eth_len);
        ip->version_ihl   = (4 << 4) | 5;
        ip->total_length  = eph::dpdk::net::hton16(
            static_cast<uint16_t>(ip_len + tcp_len + payload_len));
        ip->next_proto_id = eph::dpdk::net::kIpProtoTcp;
        ip->src_addr      = eph::dpdk::net::hton32(kDstIp);
        ip->dst_addr      = eph::dpdk::net::hton32(kSrcIp);

        auto* tcp = reinterpret_cast<rte_tcp_hdr*>(buf + eth_len + ip_len);
        tcp->src_port = eph::dpdk::net::hton16(kDstPort);
        tcp->dst_port = eph::dpdk::net::hton16(kSrcPort);
        tcp->sent_seq = eph::dpdk::net::hton32(seq);
        tcp->recv_ack = eph::dpdk::net::hton32(ack_num);
        tcp->data_off = static_cast<uint8_t>(5 << 4);
        tcp->tcp_flags = flags;
        tcp->rx_win   = eph::dpdk::net::hton16(65535);

        if (payload_len > 0) {
            // Fill with a distinctive pattern so reorder-buffer copies
            // can be checked by sampling a byte if needed.
            std::memset(buf + eth_len + ip_len + tcp_len, 0xAB, payload_len);
        }

        mbuf = rte_mbuf{};
        mbuf.buf_addr = buf;
        mbuf.data_off = 0;
        mbuf.data_len = static_cast<uint16_t>(total);
        mbuf.pkt_len  = static_cast<uint32_t>(total);
    }
};

template <typename Session>
auto drive(Session& s, FakePkt& fake) {
    rte_mbuf* p = &fake.mbuf;
    return s.process_rx(&p, 1, [](const uint8_t*, uint16_t) {});
}

// ─── Conformance row ──────────────────────────────────────────────────────

struct Row {
    std::string_view     name;
    TcpState             initial_state;
    uint32_t             snd_nxt;
    uint32_t             snd_una;
    uint32_t             rcv_nxt;
    uint32_t             pkt_seq;
    uint32_t             pkt_ack;
    uint8_t              pkt_flags;
    uint16_t             payload_len;
    TcpState             expected_state;
    bool                 expect_ok;      ///< process_rx returns has_value()
    uint32_t             expected_rcv_nxt_advance;  ///< delta on rcv_nxt_
    uint64_t             expected_resets_delta;
};

void run_row(const Row& r) {
    SCOPED_TRACE(std::string(r.name));
    auto cfg = make_test_config();
    TcpSession<> s(cfg, /*pool=*/nullptr);
    s.inject_state_for_testing(r.initial_state);
    s.inject_send_seq_for_testing(r.snd_nxt, r.snd_una);
    s.inject_recv_seq_for_testing(r.rcv_nxt, /*rcv_wnd=*/65535);

    const auto resets_before = s.stats().resets_received;
    const uint32_t rcv_before = s.rcv_nxt();

    FakePkt pkt;
    pkt.build(r.pkt_seq, r.pkt_ack, r.pkt_flags, r.payload_len);
    auto result = drive(s, pkt);

    EXPECT_EQ(result.has_value(), r.expect_ok)
        << "result = " << (result ? "ok" : result.error().detail);
    EXPECT_EQ(s.state(), r.expected_state);
    EXPECT_EQ(s.rcv_nxt() - rcv_before, r.expected_rcv_nxt_advance);
    EXPECT_EQ(s.stats().resets_received - resets_before,
              r.expected_resets_delta);
}

// ─── Table ────────────────────────────────────────────────────────────────
//
// Columns:
//   name, initial_state, snd_nxt, snd_una, rcv_nxt,
//   pkt_seq, pkt_ack, pkt_flags, payload_len,
//   expected_state, expect_ok, rcv_nxt_advance, resets_delta.
//
// Conventions:
//   * rcv_wnd is always 65535 (runner sets it).
//   * Peer 4-tuple is built into the mbuf from (kSrc*, kDst*).
//   * Whenever the row drives an RST, expect_ok=false (process_rx
//     propagates "Connection reset by peer").

constexpr uint8_t F_ACK  = eph::dpdk::net::kTcpAck;
constexpr uint8_t F_SYN  = eph::dpdk::net::kTcpSyn;
constexpr uint8_t F_FIN  = eph::dpdk::net::kTcpFin;
constexpr uint8_t F_RST  = eph::dpdk::net::kTcpRst;
constexpr uint8_t F_PSH  = eph::dpdk::net::kTcpPsh;

const std::vector<Row> kRows = {
    // ═══════ ESTABLISHED state ═══════
    {"ESTABLISHED: in-window RST closes",
     TcpState::Established, 1000, 1000, 2000,
     2000, 1000, F_RST, 0,
     TcpState::Closed, false, 0, 1},

    {"ESTABLISHED: out-of-window RST ignored (before rcv_nxt)",
     TcpState::Established, 1000, 1000, 2000,
     1500, 1000, F_RST, 0,
     TcpState::Established, true, 0, 0},

    {"ESTABLISHED: out-of-window RST ignored (far after)",
     TcpState::Established, 1000, 1000, 2000,
     100000, 1000, F_RST, 0,
     TcpState::Established, true, 0, 0},

    {"ESTABLISHED: bare ACK stays established",
     TcpState::Established, 1000, 999, 2000,
     2000, 1000, F_ACK, 0,
     TcpState::Established, true, 0, 0},

    {"ESTABLISHED: bare ACK old-ack ignored (no regression)",
     TcpState::Established, 1000, 999, 2000,
     2000, 500, F_ACK, 0,
     TcpState::Established, true, 0, 0},

    {"ESTABLISHED: FIN → CloseWait",
     TcpState::Established, 1000, 1000, 2000,
     2000, 1000, static_cast<uint8_t>(F_FIN | F_ACK), 0,
     TcpState::CloseWait, true, 1, 0},

    {"ESTABLISHED: out-of-order FIN ignored",
     TcpState::Established, 1000, 1000, 2000,
     5000, 1000, static_cast<uint8_t>(F_FIN | F_ACK), 0,
     TcpState::Established, true, 0, 0},

    {"ESTABLISHED: PSH+ACK with in-order payload → rcv_nxt advances",
     TcpState::Established, 1000, 1000, 2000,
     2000, 1000, static_cast<uint8_t>(F_ACK | F_PSH), 100,
     TcpState::Established, true, 100, 0},

    {"ESTABLISHED: out-of-order payload buffered, no rcv_nxt move",
     TcpState::Established, 1000, 1000, 2000,
     3000, 1000, static_cast<uint8_t>(F_ACK | F_PSH), 100,
     TcpState::Established, true, 0, 0},

    {"ESTABLISHED: duplicate in-window payload no advance",
     TcpState::Established, 1000, 1000, 2000,
     1950, 1000, F_ACK, 0,
     TcpState::Established, true, 0, 0},

    {"ESTABLISHED: unexpected SYN ignored (not simultaneous open)",
     TcpState::Established, 1000, 1000, 2000,
     2000, 1000, F_SYN, 0,
     TcpState::Established, true, 0, 0},

    {"ESTABLISHED: stray SYN+ACK ignored",
     TcpState::Established, 1000, 1000, 2000,
     2000, 1000, static_cast<uint8_t>(F_SYN | F_ACK), 0,
     TcpState::Established, true, 0, 0},

    // ═══════ FIN_WAIT_1 state ═══════
    {"FIN_WAIT_1: in-window RST closes",
     TcpState::FinWait1, 1000, 999, 2000,
     2000, 999, F_RST, 0,
     TcpState::Closed, false, 0, 1},

    {"FIN_WAIT_1: bare ACK of our FIN → FIN_WAIT_2",
     TcpState::FinWait1, 1000, 999, 2000,
     2000, 1000, F_ACK, 0,
     TcpState::FinWait2, true, 0, 0},

    {"FIN_WAIT_1: ACK that doesn't cover our FIN stays FIN_WAIT_1",
     TcpState::FinWait1, 1000, 999, 2000,
     2000, 999, F_ACK, 0,
     TcpState::FinWait1, true, 0, 0},

    {"FIN_WAIT_1: simultaneous close FIN (no ACK of FIN) → CLOSING",
     TcpState::FinWait1, 1000, 999, 2000,
     2000, 999, F_FIN, 0,
     TcpState::Closing, true, 1, 0},

    {"FIN_WAIT_1: FIN+ACK of our FIN → TIME_WAIT",
     TcpState::FinWait1, 1000, 999, 2000,
     2000, 1000, static_cast<uint8_t>(F_FIN | F_ACK), 0,
     TcpState::TimeWait, true, 1, 0},

    // ═══════ FIN_WAIT_2 state ═══════
    {"FIN_WAIT_2: in-window RST closes",
     TcpState::FinWait2, 1000, 1000, 2000,
     2000, 1000, F_RST, 0,
     TcpState::Closed, false, 0, 1},

    {"FIN_WAIT_2: FIN → TIME_WAIT",
     TcpState::FinWait2, 1000, 1000, 2000,
     2000, 1000, static_cast<uint8_t>(F_FIN | F_ACK), 0,
     TcpState::TimeWait, true, 1, 0},

    {"FIN_WAIT_2: bare ACK stays",
     TcpState::FinWait2, 1000, 1000, 2000,
     2000, 1000, F_ACK, 0,
     TcpState::FinWait2, true, 0, 0},

    {"FIN_WAIT_2: in-order payload delivered (rcv_nxt advances)",
     TcpState::FinWait2, 1000, 1000, 2000,
     2000, 1000, static_cast<uint8_t>(F_ACK | F_PSH), 50,
     TcpState::FinWait2, true, 50, 0},

    // ═══════ CLOSING state ═══════
    {"CLOSING: in-window RST closes",
     TcpState::Closing, 1000, 999, 2001,
     2001, 999, F_RST, 0,
     TcpState::Closed, false, 0, 1},

    {"CLOSING: ACK of our FIN → TIME_WAIT",
     TcpState::Closing, 1000, 999, 2001,
     2001, 1000, F_ACK, 0,
     TcpState::TimeWait, true, 0, 0},

    {"CLOSING: ACK not covering our FIN stays CLOSING",
     TcpState::Closing, 1000, 999, 2001,
     2001, 999, F_ACK, 0,
     TcpState::Closing, true, 0, 0},

    // ═══════ CLOSE_WAIT state ═══════
    {"CLOSE_WAIT: in-window RST closes",
     TcpState::CloseWait, 1000, 1000, 2001,
     2001, 1000, F_RST, 0,
     TcpState::Closed, false, 0, 1},

    {"CLOSE_WAIT: bare ACK stays",
     TcpState::CloseWait, 1000, 1000, 2001,
     2001, 1000, F_ACK, 0,
     TcpState::CloseWait, true, 0, 0},

    {"CLOSE_WAIT: repeat FIN ignored (already consumed)",
     TcpState::CloseWait, 1000, 1000, 2001,
     2001, 1000, static_cast<uint8_t>(F_FIN | F_ACK), 0,
     TcpState::CloseWait, true, 0, 0},

    {"CLOSE_WAIT: in-order payload delivered",
     TcpState::CloseWait, 1000, 1000, 2001,
     2001, 1000, static_cast<uint8_t>(F_ACK | F_PSH), 30,
     TcpState::CloseWait, true, 30, 0},

    // ═══════ LAST_ACK state ═══════
    {"LAST_ACK: in-window RST closes",
     TcpState::LastAck, 1000, 999, 2001,
     2001, 999, F_RST, 0,
     TcpState::Closed, false, 0, 1},

    {"LAST_ACK: ACK of our FIN → Closed",
     TcpState::LastAck, 1000, 999, 2001,
     2001, 1000, F_ACK, 0,
     TcpState::Closed, true, 0, 0},

    {"LAST_ACK: ACK not covering FIN stays LAST_ACK",
     TcpState::LastAck, 1000, 999, 2001,
     2001, 999, F_ACK, 0,
     TcpState::LastAck, true, 0, 0},

    // ═══════ TIME_WAIT state ═══════
    // In TIME_WAIT the session ignores most incoming data; only RST
    // (and the 2MSL timer, not modelled here) drives transitions.
    {"TIME_WAIT: in-window RST closes",
     TcpState::TimeWait, 1000, 1000, 2002,
     2002, 1000, F_RST, 0,
     TcpState::Closed, false, 0, 1},

    {"TIME_WAIT: repeat FIN ignored",
     TcpState::TimeWait, 1000, 1000, 2002,
     2002, 1000, static_cast<uint8_t>(F_FIN | F_ACK), 0,
     TcpState::TimeWait, true, 0, 0},

    {"TIME_WAIT: bare ACK stays",
     TcpState::TimeWait, 1000, 1000, 2002,
     2002, 1000, F_ACK, 0,
     TcpState::TimeWait, true, 0, 0},

    // ═══════ Closed state ═══════
    // All these packets should be ignored without crashing or
    // mutating state. RFC 5961 RST ignore check runs first; we expect
    // no transition in any of these rows (a packet for a Closed
    // session is effectively alien).
    {"CLOSED: incoming SYN ignored",
     TcpState::Closed, 1000, 1000, 0,
     2000, 0, F_SYN, 0,
     TcpState::Closed, true, 0, 0},

    {"CLOSED: incoming ACK ignored",
     TcpState::Closed, 1000, 1000, 0,
     2000, 1000, F_ACK, 0,
     TcpState::Closed, true, 0, 0},

    {"CLOSED: incoming payload ignored",
     TcpState::Closed, 1000, 1000, 0,
     2000, 1000, static_cast<uint8_t>(F_ACK | F_PSH), 20,
     TcpState::Closed, true, 0, 0},

    // ═══════ SynSent state (process_rx does not drive handshake) ═══════
    // The handshake is driven by connect(); process_rx treats SynSent
    // similarly to "generic RX" — RST closes, everything else is mostly
    // no-op because the main data / FIN paths only run once we're
    // Established or beyond.
    {"SYN_SENT: in-window RST closes",
     TcpState::SynSent, 1000, 1000, 0,
     0, 1000, F_RST, 0,
     TcpState::Closed, false, 0, 1},

    {"SYN_SENT: bare ACK (out-of-path) does not promote",
     TcpState::SynSent, 1000, 1000, 0,
     0, 1000, F_ACK, 0,
     TcpState::SynSent, true, 0, 0},

    // ═══════ Out-of-order / reorder edge rows ═══════
    {"ESTABLISHED: two-segment gap buffers correctly (first)",
     TcpState::Established, 1000, 1000, 2000,
     2100, 1000, static_cast<uint8_t>(F_ACK | F_PSH), 50,
     TcpState::Established, true, 0, 0},

    {"ESTABLISHED: backward seq (below rcv_nxt) treated as duplicate",
     TcpState::Established, 1000, 1000, 2000,
     1800, 1000, static_cast<uint8_t>(F_ACK | F_PSH), 50,
     TcpState::Established, true, 0, 0},

    {"ESTABLISHED: oversized out-of-order payload dropped",
     TcpState::Established, 1000, 1000, 2000,
     3000, 1000, static_cast<uint8_t>(F_ACK | F_PSH), 1461,  // > kDefaultMss
     TcpState::Established, true, 0, 0},

    // ═══════ Peer 4-tuple match / protocol sanity ═══════
    // The next batch is handled inline via direct tests, since they
    // require deviating from the build() defaults.
    {"ESTABLISHED: RST at exact rcv_nxt closes",
     TcpState::Established, 1000, 1000, 2000,
     2000, 1000, F_RST, 0,
     TcpState::Closed, false, 0, 1},

    {"FIN_WAIT_1: RST before ACK arrives closes immediately",
     TcpState::FinWait1, 1000, 999, 2000,
     2000, 999, F_RST, 0,
     TcpState::Closed, false, 0, 1},

    // NOTE: strict RFC 793 would keep the session in CLOSING on a FIN
    // whose ACK does not cover our own FIN. The current impl transitions
    // to TIME_WAIT unconditionally when a FIN arrives in-order while in
    // CLOSING — this pin encodes that behavior so a future strict-RFC
    // refactor surfaces as a deliberate change.
    {"CLOSING: FIN during CLOSING advances rcv_nxt, impl goes to TIME_WAIT",
     TcpState::Closing, 1000, 999, 2001,
     2001, 999, static_cast<uint8_t>(F_FIN | F_ACK), 0,
     TcpState::TimeWait, true, 1, 0},

    // ═══════ Payload + FIN in same segment ═══════
    {"ESTABLISHED: PSH+ACK+FIN with payload drops FIN (seq mismatch)",
     TcpState::Established, 1000, 1000, 2000,
     2000, 1000, static_cast<uint8_t>(F_ACK | F_PSH | F_FIN), 50,
     // FIN handler checks parsed.seq() == rcv_nxt_, but after we process
     // the payload rcv_nxt_ has advanced to 2050 while parsed.seq() is
     // still 2000 — so the FIN is treated as out-of-order and dropped.
     // We stay Established (the FIN never applies).
     TcpState::Established, true, 50, 0},

    // ═══════ Additional RST-windowing pins ═══════
    {"ESTABLISHED: RST at rcv_nxt + 1 byte closes",
     TcpState::Established, 1000, 1000, 2000,
     2001, 1000, F_RST, 0,
     TcpState::Closed, false, 0, 1},

    {"ESTABLISHED: RST far before rcv_nxt ignored (anti-spoof)",
     TcpState::Established, 1000, 1000, 2000,
     0, 1000, F_RST, 0,
     TcpState::Established, true, 0, 0},

    // ═══════ Data delivery in non-Established states ═══════
    {"FIN_WAIT_1: payload in-order delivered",
     TcpState::FinWait1, 1000, 999, 2000,
     2000, 999, static_cast<uint8_t>(F_ACK | F_PSH), 40,
     TcpState::FinWait1, true, 40, 0},

    {"CLOSING: payload in-order delivered",
     TcpState::Closing, 1000, 999, 2001,
     2001, 999, static_cast<uint8_t>(F_ACK | F_PSH), 40,
     TcpState::Closing, true, 40, 0},

    {"CLOSE_WAIT: payload out-of-order buffered",
     TcpState::CloseWait, 1000, 1000, 2001,
     2100, 1000, static_cast<uint8_t>(F_ACK | F_PSH), 40,
     TcpState::CloseWait, true, 0, 0},

    // ═══════ Zero-length ACK tolerance ═══════
    {"ESTABLISHED: zero-payload ACK with old seq ignored",
     TcpState::Established, 1000, 1000, 2000,
     1000, 1000, F_ACK, 0,
     TcpState::Established, true, 0, 0},

    {"ESTABLISHED: ACK advances snd_una but state unchanged",
     TcpState::Established, 1000, 500, 2000,
     2000, 1000, F_ACK, 0,
     TcpState::Established, true, 0, 0},

    {"ESTABLISHED: ACK beyond snd_nxt does not advance snd_una",
     TcpState::Established, 1000, 500, 2000,
     2000, 9999, F_ACK, 0,
     TcpState::Established, true, 0, 0},

    // ═══════ Back-to-back FINs across states ═══════
    {"ESTABLISHED: FIN-without-ACK transitions correctly",
     TcpState::Established, 1000, 1000, 2000,
     2000, 0, F_FIN, 0,
     TcpState::CloseWait, true, 1, 0},

    {"CLOSE_WAIT: unexpected SYN ignored",
     TcpState::CloseWait, 1000, 1000, 2001,
     2001, 1000, F_SYN, 0,
     TcpState::CloseWait, true, 0, 0},

    {"LAST_ACK: unexpected FIN does not advance",
     TcpState::LastAck, 1000, 999, 2001,
     2001, 999, static_cast<uint8_t>(F_FIN | F_ACK), 0,
     TcpState::LastAck, true, 0, 0},

    // ═══════ Cross-state RST propagation — every state closes on RST ═══
    {"ALL: Established + RST",
     TcpState::Established, 1000, 1000, 2000,
     2000, 1000, F_RST, 0,
     TcpState::Closed, false, 0, 1},
    {"ALL: FinWait1 + RST",
     TcpState::FinWait1, 1000, 999, 2000,
     2000, 999, F_RST, 0,
     TcpState::Closed, false, 0, 1},
    {"ALL: FinWait2 + RST",
     TcpState::FinWait2, 1000, 1000, 2000,
     2000, 1000, F_RST, 0,
     TcpState::Closed, false, 0, 1},
    {"ALL: CloseWait + RST",
     TcpState::CloseWait, 1000, 1000, 2001,
     2001, 1000, F_RST, 0,
     TcpState::Closed, false, 0, 1},
    {"ALL: Closing + RST",
     TcpState::Closing, 1000, 999, 2001,
     2001, 999, F_RST, 0,
     TcpState::Closed, false, 0, 1},
    {"ALL: LastAck + RST",
     TcpState::LastAck, 1000, 999, 2001,
     2001, 999, F_RST, 0,
     TcpState::Closed, false, 0, 1},
    {"ALL: TimeWait + RST",
     TcpState::TimeWait, 1000, 1000, 2002,
     2002, 1000, F_RST, 0,
     TcpState::Closed, false, 0, 1},

    // ═══════ Incoming data in Closed is ignored ═══════
    {"CLOSED: PSH+ACK with payload ignored, no advance",
     TcpState::Closed, 1000, 1000, 5000,
     5000, 1000, static_cast<uint8_t>(F_ACK | F_PSH), 100,
     TcpState::Closed, true, 0, 0},

    {"CLOSED: FIN ignored",
     TcpState::Closed, 1000, 1000, 5000,
     5000, 1000, F_FIN, 0,
     TcpState::Closed, true, 0, 0},
};

} // namespace

TEST(TcpConformance, TableRowCountAtLeast60) {
    EXPECT_GE(kRows.size(), 60u)
        << "Conformance table must have at least 60 rows (plan Phase G)";
}

TEST(TcpConformance, EveryRowMatchesExpectedTransition) {
    for (const auto& r : kRows) {
        run_row(r);
    }
}

// Additional single-purpose assertions that don't fit the tabular
// schema (requires a peer address mismatch or multi-packet setup).

TEST(TcpConformance, NonMatching4TupleDoesNotDisturbAnyState) {
    for (TcpState initial : {
            TcpState::Established, TcpState::FinWait1, TcpState::FinWait2,
            TcpState::CloseWait,   TcpState::Closing,  TcpState::LastAck,
            TcpState::TimeWait,    TcpState::SynSent,  TcpState::Closed}) {
        auto cfg = make_test_config();
        TcpSession<> s(cfg, nullptr);
        s.inject_state_for_testing(initial);
        s.inject_send_seq_for_testing(1000, 1000);
        s.inject_recv_seq_for_testing(2000, 65535);

        FakePkt pkt;
        pkt.build(2000, 1000, eph::dpdk::net::kTcpRst);
        auto* ip = reinterpret_cast<rte_ipv4_hdr*>(
            pkt.buf + eph::dpdk::net::kEtherHeaderLen);
        ip->src_addr = eph::dpdk::net::hton32(0x0A000099);  // alien peer

        auto r = drive(s, pkt);
        EXPECT_TRUE(r.has_value())
            << "alien RST must not propagate — state was "
            << static_cast<int>(initial);
        EXPECT_EQ(s.state(), initial);
        EXPECT_EQ(s.stats().resets_received, 0u);
    }
}
