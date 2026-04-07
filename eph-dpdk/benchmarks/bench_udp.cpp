/// @file bench_udp.cpp
/// UDP micro-benchmarks — header fill/build, checksum, layered parse API,
/// Reactor UDP dispatch simulation.
///
/// Establishes UDP performance baselines for comparison with TCP equivalents
/// in bench_tcp_header.cpp. Does NOT require DPDK EAL — uses FakeMbuf.

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include <benchmark/benchmark.h>

#include "eph/dpdk/net_header.hpp"
#include "eph/dpdk/reactor.hpp"

namespace {

using namespace eph::dpdk;
using namespace eph::dpdk::net;

void fill_random(uint8_t* buf, size_t len, uint32_t seed = 42) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint16_t> dist(0, 255);
    for (size_t i = 0; i < len; ++i)
        buf[i] = static_cast<uint8_t>(dist(rng));
}

void UdpPayloadSizeArgs(::benchmark::Benchmark* b) {
    for (int sz : {32, 64, 128, 512, 1472}) b->Arg(sz);
}

/// Fake mbuf for benchmarking without EAL — stack-allocated buffer.
struct BenchMbuf {
    uint8_t buf[2048]{};
    rte_mbuf mbuf{};

    BenchMbuf() {
        std::memset(&mbuf, 0, sizeof(mbuf));
        mbuf.buf_addr = buf;
        mbuf.buf_len = sizeof(buf);
        mbuf.data_off = 0;
        mbuf.data_len = 0;
        mbuf.pkt_len = 0;
    }
    void set_len(uint16_t len) {
        mbuf.data_off = 0;
        mbuf.data_len = len;
        mbuf.pkt_len = len;
    }
};

/// Build a valid Eth+IP+TCP fake packet in buffer. Returns total length.
uint16_t build_tcp_pkt(uint8_t* buf, uint16_t payload_len) {
    uint16_t total = kAllHeadersLen + payload_len;
    std::memset(buf, 0, total);
    auto* eth = reinterpret_cast<rte_ether_hdr*>(buf);
    eth->ether_type = hton16(kEtherTypeIpv4);
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + kEtherHeaderLen);
    ip->version_ihl = 0x45;
    ip->next_proto_id = kIpProtoTcp;
    ip->total_length = hton16(kIpv4HeaderLen + kTcpHeaderLen + payload_len);
    ip->src_addr = hton32(0x0A000001);
    ip->dst_addr = hton32(0x0A000002);
    auto* tcp = reinterpret_cast<rte_tcp_hdr*>(buf + kEtherHeaderLen + kIpv4HeaderLen);
    tcp->src_port = hton16(12345);
    tcp->dst_port = hton16(443);
    tcp->data_off = (kTcpHeaderLen / 4) << 4;
    tcp->sent_seq = hton32(100);
    tcp->recv_ack = hton32(200);
    return total;
}

/// Build a valid Eth+IP+UDP fake packet in buffer. Returns total length.
uint16_t build_udp_pkt(uint8_t* buf, uint16_t payload_len) {
    uint16_t total = kUdpAllHeadersLen + payload_len;
    std::memset(buf, 0, total);
    auto* eth = reinterpret_cast<rte_ether_hdr*>(buf);
    eth->ether_type = hton16(kEtherTypeIpv4);
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + kEtherHeaderLen);
    ip->version_ihl = 0x45;
    ip->next_proto_id = kIpProtoUdp;
    ip->total_length = hton16(kIpv4HeaderLen + kUdpHeaderLen + payload_len);
    ip->src_addr = hton32(0x0A000001);
    ip->dst_addr = hton32(0x0A000002);
    auto* udp = reinterpret_cast<UdpHeader*>(buf + kEtherHeaderLen + kIpv4HeaderLen);
    udp->src_port = hton16(50000);
    udp->dst_port = hton16(8080);
    udp->length = hton16(kUdpHeaderLen + payload_len);
    udp->checksum = 0;
    return total;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// UDP TX hot path: UdpPacketTemplate::fill
// ─────────────────────────────────────────────────────────────────────────────

static void BM_UdpHeaderFill(benchmark::State& state) {
    auto sz = static_cast<uint16_t>(state.range(0));

    UdpPacketTemplate tmpl;
    tmpl.init({{0x00, 0x11, 0x22, 0x33, 0x44, 0x55}},
              {{0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB}},
              parse_ipv4("10.0.0.1"), parse_ipv4("10.0.0.2"),
              50000, 8080);

    std::vector<uint8_t> payload(sz);
    fill_random(payload.data(), sz, 50);
    BenchMbuf fake;

    for (auto _ : state) {
        auto n = tmpl.fill(&fake.mbuf, payload.data(), sz);
        benchmark::DoNotOptimize(n);
    }
}
BENCHMARK(BM_UdpHeaderFill)->Apply(UdpPayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// UDP checksum (compare with BM_TcpChecksum in bench_tcp_header.cpp)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_UdpChecksum(benchmark::State& state) {
    auto sz = static_cast<size_t>(state.range(0));
    // Build a fake UDP segment: header + payload
    size_t seg_len = kUdpHeaderLen + sz;
    std::vector<uint8_t> seg(seg_len);
    auto* udp = reinterpret_cast<UdpHeader*>(seg.data());
    udp->src_port = hton16(50000);
    udp->dst_port = hton16(8080);
    udp->length = hton16(static_cast<uint16_t>(seg_len));
    udp->checksum = 0;
    fill_random(seg.data() + kUdpHeaderLen, sz, 99);

    uint32_t src_net = hton32(0x0A000001);
    uint32_t dst_net = hton32(0x0A000002);

    for (auto _ : state) {
        auto c = udp_checksum(src_net, dst_net, seg.data(),
                              static_cast<uint16_t>(seg_len));
        benchmark::DoNotOptimize(c);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(seg_len));
}
BENCHMARK(BM_UdpChecksum)->Arg(64)->Arg(128)->Arg(256)->Arg(512)->Arg(1024);

// ─────────────────────────────────────────────────────────────────────────────
// Layered parse API: parse_ip_header (L2+L3 only)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_ParseIpHeader(benchmark::State& state) {
    auto sz = static_cast<uint16_t>(state.range(0));
    BenchMbuf fake;
    build_udp_pkt(fake.buf, sz);
    fake.set_len(kUdpAllHeadersLen + sz);

    for (auto _ : state) {
        auto ip = parse_ip_header(&fake.mbuf);
        benchmark::DoNotOptimize(ip);
    }
}
BENCHMARK(BM_ParseIpHeader)->Apply(UdpPayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// Layered parse: parse_tcp_from_ip vs parse_udp_from_ip
// ─────────────────────────────────────────────────────────────────────────────

static void BM_ParseTcpFromIp(benchmark::State& state) {
    auto sz = static_cast<uint16_t>(state.range(0));
    BenchMbuf fake;
    build_tcp_pkt(fake.buf, sz);
    fake.set_len(kAllHeadersLen + sz);

    // Parse IP header once (setup cost, not measured)
    auto ip_hdr = parse_ip_header(&fake.mbuf);

    for (auto _ : state) {
        auto parsed = parse_tcp_from_ip(&fake.mbuf, ip_hdr);
        benchmark::DoNotOptimize(parsed);
    }
}
BENCHMARK(BM_ParseTcpFromIp)->Apply(UdpPayloadSizeArgs);

static void BM_ParseUdpFromIp(benchmark::State& state) {
    auto sz = static_cast<uint16_t>(state.range(0));
    BenchMbuf fake;
    build_udp_pkt(fake.buf, sz);
    fake.set_len(kUdpAllHeadersLen + sz);

    auto ip_hdr = parse_ip_header(&fake.mbuf);

    for (auto _ : state) {
        auto parsed = parse_udp_from_ip(&fake.mbuf, ip_hdr);
        benchmark::DoNotOptimize(parsed);
    }
}
BENCHMARK(BM_ParseUdpFromIp)->Apply(UdpPayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// Layered vs direct: parse_ip_header + parse_tcp_from_ip vs parse_packet
// ─────────────────────────────────────────────────────────────────────────────

static void BM_ParsePacketLayered(benchmark::State& state) {
    auto sz = static_cast<uint16_t>(state.range(0));
    BenchMbuf fake;
    build_tcp_pkt(fake.buf, sz);
    fake.set_len(kAllHeadersLen + sz);

    for (auto _ : state) {
        auto ip = parse_ip_header(&fake.mbuf);
        auto parsed = parse_tcp_from_ip(&fake.mbuf, ip);
        benchmark::DoNotOptimize(parsed);
    }
}
BENCHMARK(BM_ParsePacketLayered)->Apply(UdpPayloadSizeArgs);

static void BM_ParsePacketDirect(benchmark::State& state) {
    auto sz = static_cast<uint16_t>(state.range(0));
    BenchMbuf fake;
    build_tcp_pkt(fake.buf, sz);
    fake.set_len(kAllHeadersLen + sz);

    for (auto _ : state) {
        auto parsed = parse_packet(&fake.mbuf);
        benchmark::DoNotOptimize(parsed);
    }
}
BENCHMARK(BM_ParsePacketDirect)->Apply(UdpPayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// Reactor UDP dispatch simulation
// Mirrors BM_ReactorDispatchSim in bench_tcp_header.cpp but for UDP entries.
// ─────────────────────────────────────────────────────────────────────────────

static void BM_ReactorUdpDispatchSim(benchmark::State& state) {
    auto num_entries = static_cast<size_t>(state.range(0));

    // Build UDP entries with distinct tuples
    std::array<ConnectionTuple, 8> tuples{};
    std::array<uint64_t, 8> hashes{};
    for (size_t i = 0; i < num_entries; ++i) {
        tuples[i] = {.src_ip = 0x0A000001, .dst_ip = 0x0A000002,
                     .src_port = static_cast<uint16_t>(50000 + i), .dst_port = 8080};
        hashes[i] = ReactorEntry::hash_tuple(tuples[i]);
    }

    // Build a packet matching the LAST entry (worst-case linear scan)
    BenchMbuf fake;
    auto last = tuples[num_entries - 1];
    // Incoming packets have swapped src/dst
    auto* buf = fake.buf;
    std::memset(buf, 0, kUdpAllHeadersLen + 64);
    auto* eth = reinterpret_cast<rte_ether_hdr*>(buf);
    eth->ether_type = hton16(kEtherTypeIpv4);
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + kEtherHeaderLen);
    ip->version_ihl = 0x45;
    ip->next_proto_id = kIpProtoUdp;
    ip->total_length = hton16(kIpv4HeaderLen + kUdpHeaderLen + 64);
    ip->src_addr = hton32(last.dst_ip);  // swapped
    ip->dst_addr = hton32(last.src_ip);  // swapped
    auto* udp = reinterpret_cast<UdpHeader*>(buf + kEtherHeaderLen + kIpv4HeaderLen);
    udp->src_port = hton16(last.dst_port);  // swapped
    udp->dst_port = hton16(last.src_port);  // swapped
    udp->length = hton16(kUdpHeaderLen + 64);
    fake.set_len(kUdpAllHeadersLen + 64);

    for (auto _ : state) {
        // Simulate Reactor UDP dispatch: parse + hash + linear scan
        auto ip_hdr = parse_ip_header(&fake.mbuf);
        auto parsed = parse_udp_from_ip(&fake.mbuf, ip_hdr);

        ConnectionTuple pkt_tuple{
            .src_ip = parsed.src_ip(), .dst_ip = parsed.dst_ip(),
            .src_port = parsed.src_port(), .dst_port = parsed.dst_port()};
        uint64_t pkt_hash = ReactorEntry::hash_tuple(pkt_tuple);

        bool matched = false;
        for (size_t j = 0; j < num_entries; ++j) {
            if (hashes[j] != pkt_hash) continue;
            // Reverse match (incoming has swapped src/dst)
            if (pkt_tuple.src_ip == tuples[j].dst_ip &&
                pkt_tuple.dst_ip == tuples[j].src_ip &&
                pkt_tuple.src_port == tuples[j].dst_port &&
                pkt_tuple.dst_port == tuples[j].src_port) {
                matched = true;
                break;
            }
        }
        benchmark::DoNotOptimize(matched);
    }
}
BENCHMARK(BM_ReactorUdpDispatchSim)->Arg(1)->Arg(2)->Arg(4)->Arg(8);

BENCHMARK_MAIN();
