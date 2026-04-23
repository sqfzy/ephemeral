/// @file bench_rx_hot_path.cpp
/// DPDK RX hot-path per-packet parse cost microbench.
///
/// Tier 2 #7 of the lucky-giggling-kahan review: establish a baseline
/// for the parse functions in `packet_parse.hpp` that every RX mbuf
/// traverses before reaching codec dispatch. Phase 9 added several
/// defense-in-depth checks (multi-segment reject, fragment reject,
/// UDP length cross-check, IHL / total_length validation) — the cost
/// of each check was not measured as it landed. This bench captures
/// the current baseline so a future change that adds ≥ 5 ns / packet
/// to any of these paths can be caught by a simple numerical diff
/// against the pinned baseline.
///
/// Scope:
///   - parse_ip_header   (ingress L3 dispatch; every RX mbuf)
///   - parse_udp_packet  (one-shot, used by DpdkUdpSocket)
///   - parse_udp_from_ip (layered; used by DpdkPoller dispatch)
///   - parse_tcp_from_ip (layered; reference for TCP RX path)
///   - parse_icmp        (Type 3 Code 4 + embedded header extraction)
///   - is_ip_fragment    (cheap fragment detection helper)
///
/// Methodology:
///   Each bench builds a valid mbuf template of the right shape once,
///   then drives the parser in a tight loop with DoNotOptimize on the
///   result. No mempool allocation or NIC bring-up. The `PayloadSizeArgs`
///   (64 / 128 / 256 / 512 / 1024 bytes) matches `bench_tcp_header.cpp`
///   so the two files can be read side-by-side.
///
/// Baseline capture protocol:
///   xmake f --cxx=/usr/bin/gcc14-g++ --ld=/usr/bin/gcc14-g++ \
///           --sh=/usr/bin/gcc14-g++ -m release
///   xmake build bench_rx_hot_path
///   xmake run bench_rx_hot_path --benchmark_format=console \
///       > .artifacts/bench-rx-hot-path-<yyyymmdd>.txt

#include <cstdint>
#include <cstring>
#include <vector>

#include <benchmark/benchmark.h>

#include "eph/dpdk/net_header.hpp"
#include "eph/dpdk/packet_parse.hpp"

namespace {

void PayloadSizeArgs(::benchmark::Benchmark* b) {
    for (int sz : {64, 128, 256, 512, 1024}) b->Arg(sz);
}

/// Build a flat buffer holding a valid Ethernet + IPv4 + UDP packet.
/// Returns the total packet length. `pkt_buf` must be sized to at least
/// kUdpAllHeadersLen + payload_len.
uint16_t build_udp_packet(std::vector<uint8_t>& pkt_buf, uint16_t payload_len) {
    const uint16_t total =
        static_cast<uint16_t>(eph::dpdk::net::kUdpAllHeadersLen + payload_len);
    pkt_buf.assign(total, 0);

    auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt_buf.data());
    eth->ether_type = eph::dpdk::net::hton16(eph::dpdk::net::kEtherTypeIpv4);

    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(
        pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen);
    ip->version_ihl     = 0x45;  // version 4, IHL 5 (20 bytes)
    ip->total_length    = eph::dpdk::net::hton16(
        static_cast<uint16_t>(eph::dpdk::net::kIpv4HeaderLen +
                              eph::dpdk::net::kUdpHeaderLen + payload_len));
    ip->fragment_offset = eph::dpdk::net::hton16(eph::dpdk::net::kIpDontFragment);
    ip->next_proto_id   = eph::dpdk::net::kIpProtoUdp;
    ip->src_addr        = eph::dpdk::net::hton32(0x0A000002);
    ip->dst_addr        = eph::dpdk::net::hton32(0x0A000001);

    auto* udp = reinterpret_cast<eph::dpdk::net::UdpHeader*>(
        pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen +
        eph::dpdk::net::kIpv4HeaderLen);
    udp->src_port = eph::dpdk::net::hton16(30000);
    udp->dst_port = eph::dpdk::net::hton16(12345);
    udp->length   = eph::dpdk::net::hton16(
        static_cast<uint16_t>(eph::dpdk::net::kUdpHeaderLen + payload_len));
    udp->checksum = 0;
    return total;
}

/// Build a flat buffer holding a valid Ethernet + IPv4 + ICMP Type 3
/// Code 4 (Fragmentation Needed) packet with embedded IP + TCP
/// fragments. Returns total packet length.
uint16_t build_icmp_frag_needed(std::vector<uint8_t>& pkt_buf) {
    // ICMP Type 3 Code 4: 8-byte ICMP header + 20-byte embedded IP +
    // 8 bytes of L4 (we emit 4 for tightness; parse_icmp reads ports
    // from the first 4).
    constexpr uint16_t kIcmpPayload = 8 + 20 + 8;
    const uint16_t total =
        eph::dpdk::net::kEtherHeaderLen +
        eph::dpdk::net::kIpv4HeaderLen +
        kIcmpPayload;
    pkt_buf.assign(total, 0);

    auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt_buf.data());
    eth->ether_type = eph::dpdk::net::hton16(eph::dpdk::net::kEtherTypeIpv4);

    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(
        pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen);
    ip->version_ihl     = 0x45;
    ip->total_length    = eph::dpdk::net::hton16(
        static_cast<uint16_t>(eph::dpdk::net::kIpv4HeaderLen + kIcmpPayload));
    ip->fragment_offset = eph::dpdk::net::hton16(eph::dpdk::net::kIpDontFragment);
    ip->next_proto_id   = eph::dpdk::net::kIpProtoIcmp;
    ip->src_addr        = eph::dpdk::net::hton32(0x0A000002);
    ip->dst_addr        = eph::dpdk::net::hton32(0x0A000001);

    uint8_t* icmp = pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen +
                    eph::dpdk::net::kIpv4HeaderLen;
    icmp[0] = 3;     // Type = Destination Unreachable
    icmp[1] = 4;     // Code = Fragmentation Needed
    // next_hop_mtu at [6..7]
    uint16_t mtu_net = eph::dpdk::net::hton16(1492);
    std::memcpy(icmp + 6, &mtu_net, 2);

    // Embedded IP header at [8..27]
    auto* eip = reinterpret_cast<rte_ipv4_hdr*>(icmp + 8);
    eip->version_ihl   = 0x45;
    eip->next_proto_id = eph::dpdk::net::kIpProtoTcp;
    eip->src_addr      = eph::dpdk::net::hton32(0x0A000001);
    eip->dst_addr      = eph::dpdk::net::hton32(0x0A000002);

    // Embedded src_port (2) + dst_port (2) at [28..31]
    uint16_t sp_net = eph::dpdk::net::hton16(12345);
    uint16_t dp_net = eph::dpdk::net::hton16(443);
    std::memcpy(icmp + 8 + 20, &sp_net, 2);
    std::memcpy(icmp + 8 + 20 + 2, &dp_net, 2);
    return total;
}

/// Wrap a pre-filled flat buffer in an rte_mbuf simulacrum that
/// parse_ip_header / parse_udp_packet / parse_icmp accept.
rte_mbuf make_mbuf(std::vector<uint8_t>& buf, uint16_t total_len) {
    rte_mbuf m{};
    m.buf_addr = buf.data();
    m.data_off = 0;
    m.data_len = total_len;
    m.pkt_len  = total_len;
    m.nb_segs  = 1;
    return m;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// parse_ip_header — L2 + L3 only, protocol-agnostic
// ─────────────────────────────────────────────────────────────────────────────

static void BM_ParseIpHeader(benchmark::State& state) {
    auto sz = static_cast<uint16_t>(state.range(0));
    std::vector<uint8_t> pkt_buf;
    const uint16_t total = build_udp_packet(pkt_buf, sz);
    rte_mbuf m = make_mbuf(pkt_buf, total);

    for (auto _ : state) {
        auto r = eph::dpdk::net::parse_ip_header(&m);
        benchmark::DoNotOptimize(r.ip);
        benchmark::DoNotOptimize(r.proto);
    }
    state.SetBytesProcessed(state.iterations() * total);
}
BENCHMARK(BM_ParseIpHeader)->Apply(PayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// parse_udp_packet — one-shot L2+L3+L4, production path for DpdkUdpSocket
// ─────────────────────────────────────────────────────────────────────────────

static void BM_ParseUdpPacket(benchmark::State& state) {
    auto sz = static_cast<uint16_t>(state.range(0));
    std::vector<uint8_t> pkt_buf;
    const uint16_t total = build_udp_packet(pkt_buf, sz);
    rte_mbuf m = make_mbuf(pkt_buf, total);

    for (auto _ : state) {
        auto parsed = eph::dpdk::net::parse_udp_packet(&m);
        benchmark::DoNotOptimize(parsed.udp);
        benchmark::DoNotOptimize(parsed.payload_len);
    }
    state.SetBytesProcessed(state.iterations() * total);
}
BENCHMARK(BM_ParseUdpPacket)->Apply(PayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// parse_udp_from_ip — layered, drops the second L3 parse (zero-redundancy)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_ParseUdpFromIp(benchmark::State& state) {
    auto sz = static_cast<uint16_t>(state.range(0));
    std::vector<uint8_t> pkt_buf;
    const uint16_t total = build_udp_packet(pkt_buf, sz);
    rte_mbuf m = make_mbuf(pkt_buf, total);
    auto ip_hdr = eph::dpdk::net::parse_ip_header(&m);

    for (auto _ : state) {
        auto parsed = eph::dpdk::net::parse_udp_from_ip(&m, ip_hdr);
        benchmark::DoNotOptimize(parsed.udp);
        benchmark::DoNotOptimize(parsed.payload_len);
    }
    state.SetBytesProcessed(state.iterations() * total);
}
BENCHMARK(BM_ParseUdpFromIp)->Apply(PayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// parse_tcp_from_ip — layered TCP parse, reference for DpdkTcpStream RX
// ─────────────────────────────────────────────────────────────────────────────

static void BM_ParseTcpFromIp(benchmark::State& state) {
    auto sz = static_cast<uint16_t>(state.range(0));
    const uint16_t total = static_cast<uint16_t>(
        eph::dpdk::net::kAllHeadersLen + sz);
    std::vector<uint8_t> pkt_buf(total, 0);

    auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt_buf.data());
    eth->ether_type = eph::dpdk::net::hton16(eph::dpdk::net::kEtherTypeIpv4);

    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(
        pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen);
    ip->version_ihl     = 0x45;
    ip->total_length    = eph::dpdk::net::hton16(
        static_cast<uint16_t>(eph::dpdk::net::kIpv4HeaderLen +
                              eph::dpdk::net::kTcpHeaderLen + sz));
    ip->fragment_offset = eph::dpdk::net::hton16(eph::dpdk::net::kIpDontFragment);
    ip->next_proto_id   = eph::dpdk::net::kIpProtoTcp;
    ip->src_addr        = eph::dpdk::net::hton32(0x0A000002);
    ip->dst_addr        = eph::dpdk::net::hton32(0x0A000001);

    auto* tcp = reinterpret_cast<rte_tcp_hdr*>(
        pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen +
        eph::dpdk::net::kIpv4HeaderLen);
    tcp->data_off  = (eph::dpdk::net::kTcpHeaderLen / 4) << 4;
    tcp->tcp_flags = eph::dpdk::net::kTcpAck;

    rte_mbuf m = make_mbuf(pkt_buf, total);
    auto ip_hdr = eph::dpdk::net::parse_ip_header(&m);

    for (auto _ : state) {
        auto parsed = eph::dpdk::net::parse_tcp_from_ip(&m, ip_hdr);
        benchmark::DoNotOptimize(parsed.tcp);
        benchmark::DoNotOptimize(parsed.payload_len);
    }
    state.SetBytesProcessed(state.iterations() * total);
}
BENCHMARK(BM_ParseTcpFromIp)->Apply(PayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// parse_icmp — Type 3 Code 4 with embedded IP + L4 port extraction.
// No Args() sweep — ICMP Type 3 Code 4 always carries a fixed embedded
// header; payload size is not a meaningful variable here.
// ─────────────────────────────────────────────────────────────────────────────

static void BM_ParseIcmp(benchmark::State& state) {
    std::vector<uint8_t> pkt_buf;
    const uint16_t total = build_icmp_frag_needed(pkt_buf);
    rte_mbuf m = make_mbuf(pkt_buf, total);

    for (auto _ : state) {
        auto parsed = eph::dpdk::net::parse_icmp(&m);
        benchmark::DoNotOptimize(parsed.ip);
        benchmark::DoNotOptimize(parsed.next_hop_mtu);
        benchmark::DoNotOptimize(parsed.embedded_valid);
    }
    state.SetBytesProcessed(state.iterations() * total);
}
BENCHMARK(BM_ParseIcmp);

// ─────────────────────────────────────────────────────────────────────────────
// is_ip_fragment — lightweight peek. Run on TWO shapes:
//   1. an unfragmented UDP packet (the common case — DF set, short-circuits
//      on the frag_off == 0 check after ≤ 20 bytes of inspection)
//   2. a fragmented packet (MF=1) to capture the takes-the-branch cost
// ─────────────────────────────────────────────────────────────────────────────

static void BM_IsIpFragment_NonFragment(benchmark::State& state) {
    std::vector<uint8_t> pkt_buf;
    const uint16_t total = build_udp_packet(pkt_buf, /*payload=*/64);
    rte_mbuf m = make_mbuf(pkt_buf, total);

    for (auto _ : state) {
        bool r = eph::dpdk::net::is_ip_fragment(&m);
        benchmark::DoNotOptimize(r);
    }
    state.SetBytesProcessed(state.iterations() * total);
}
BENCHMARK(BM_IsIpFragment_NonFragment);

static void BM_IsIpFragment_Fragment(benchmark::State& state) {
    std::vector<uint8_t> pkt_buf;
    const uint16_t total = build_udp_packet(pkt_buf, /*payload=*/64);
    // Override fragment_offset to MF=1, offset=0 (first fragment).
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(
        pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen);
    ip->fragment_offset = eph::dpdk::net::hton16(eph::dpdk::net::kIpMoreFragments);
    rte_mbuf m = make_mbuf(pkt_buf, total);

    for (auto _ : state) {
        bool r = eph::dpdk::net::is_ip_fragment(&m);
        benchmark::DoNotOptimize(r);
    }
    state.SetBytesProcessed(state.iterations() * total);
}
BENCHMARK(BM_IsIpFragment_Fragment);

BENCHMARK_MAIN();
