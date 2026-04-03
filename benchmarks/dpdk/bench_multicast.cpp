/// @file bench_multicast.cpp
/// DPDK multicast packet parsing benchmarks — UDP parse, MAC computation.
///
/// Measures hot-path operations for the multicast receiver:
///   - parse_udp_packet() throughput at various payload sizes
///   - multicast_mac_from_ip() computation cost
///   - is_multicast_ip() classification cost

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include <benchmark/benchmark.h>

#include "eph/dpdk/multicast.hpp"

namespace {

void fill_random(uint8_t* buf, size_t len, uint32_t seed = 42) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint16_t> dist(0, 255);
    for (size_t i = 0; i < len; ++i) {
        buf[i] = static_cast<uint8_t>(dist(rng));
    }
}

/// Build a valid Ethernet/IPv4/UDP packet in a flat buffer.
/// Returns total packet length.
size_t build_udp_packet(uint8_t* buf, size_t buf_size,
                        uint32_t src_ip, uint16_t src_port,
                        uint32_t dst_ip, uint16_t dst_port,
                        const uint8_t* payload, uint16_t payload_len) {
    using namespace eph::dpdk;
    constexpr uint16_t eth_len = net::kEtherHeaderLen;
    constexpr uint16_t ip_len  = net::kIpv4HeaderLen;
    constexpr uint16_t udp_hdr = 8;
    uint16_t total = eth_len + ip_len + udp_hdr + payload_len;
    if (buf_size < total) return 0;
    std::memset(buf, 0, total);

    // Ethernet
    auto* eth = reinterpret_cast<rte_ether_hdr*>(buf);
    auto mcast_mac = multicast_mac_from_ip(dst_ip);
    rte_ether_addr_copy(&mcast_mac, &eth->dst_addr);
    eth->ether_type = net::hton16(net::kEtherTypeIpv4);

    // IPv4
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + eth_len);
    ip->version_ihl    = net::kIpv4VersionIhl5;
    ip->total_length   = net::hton16(ip_len + udp_hdr + payload_len);
    ip->time_to_live   = net::kDefaultTtl;
    ip->next_proto_id  = net::kIpProtoUdp;
    ip->src_addr       = net::hton32(src_ip);
    ip->dst_addr       = net::hton32(dst_ip);
    ip->hdr_checksum   = net::internet_checksum(ip, ip_len);

    // UDP
    auto* udp = reinterpret_cast<UdpHeader*>(buf + eth_len + ip_len);
    udp->src_port = net::hton16(src_port);
    udp->dst_port = net::hton16(dst_port);
    udp->length   = net::hton16(udp_hdr + payload_len);
    udp->checksum = 0;

    // Payload
    if (payload && payload_len > 0) {
        std::memcpy(buf + eth_len + ip_len + udp_hdr, payload, payload_len);
    }
    return total;
}

/// Fake rte_mbuf with inline data buffer.
struct FakeMbuf {
    uint8_t  buf[2048]{};
    rte_mbuf mbuf{};

    FakeMbuf() {
        mbuf.buf_addr = buf;
        mbuf.data_off = 0;
        mbuf.data_len = 0;
        mbuf.pkt_len  = 0;
    }

    void set_len(uint16_t len) {
        mbuf.data_len = len;
        mbuf.pkt_len  = len;
    }
};

void PayloadSizeArgs(::benchmark::Benchmark* b) {
    for (int sz : {0, 64, 256, 512, 1024, 1460}) b->Arg(sz);
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// parse_udp_packet — the hot path for multicast RX
// ─────────────────────────────────────────────────────────────────────────────

static void BM_ParseUdpPacket(benchmark::State& state) {
    auto payload_sz = static_cast<uint16_t>(state.range(0));

    std::vector<uint8_t> payload(payload_sz);
    fill_random(payload.data(), payload_sz, 99);

    FakeMbuf fake;
    size_t pkt_len = build_udp_packet(
        fake.buf, sizeof(fake.buf),
        eph::dpdk::net::parse_ipv4("10.1.2.3"), 5000,
        eph::dpdk::net::parse_ipv4("233.54.12.111"), 26477,
        payload.data(), payload_sz);
    fake.set_len(static_cast<uint16_t>(pkt_len));

    for (auto _ : state) {
        auto parsed = eph::dpdk::parse_udp_packet(&fake.mbuf);
        benchmark::DoNotOptimize(parsed);
    }
    state.SetBytesProcessed(
        state.iterations() * static_cast<int64_t>(pkt_len));
}
BENCHMARK(BM_ParseUdpPacket)->Apply(PayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// multicast_mac_from_ip — called once per group join (control path)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_MulticastMacFromIp(benchmark::State& state) {
    // Cycle through different IPs to avoid trivial optimization
    constexpr uint32_t kBaseIp = 0xE9360C00; // 233.54.12.0
    uint32_t ip = kBaseIp;

    for (auto _ : state) {
        auto mac = eph::dpdk::multicast_mac_from_ip(ip);
        benchmark::DoNotOptimize(mac);
        ip = kBaseIp + ((ip - kBaseIp + 1) & 0xFF);
    }
}
BENCHMARK(BM_MulticastMacFromIp);

// ─────────────────────────────────────────────────────────────────────────────
// is_multicast_ip — fast classification on RX path
// ─────────────────────────────────────────────────────────────────────────────

static void BM_IsMulticastIp(benchmark::State& state) {
    // Mix of multicast and non-multicast IPs
    constexpr uint32_t kIps[] = {
        0xE9360C6F, // 233.54.12.111 (multicast)
        0x0A000001, // 10.0.0.1 (not multicast)
        0xE0000001, // 224.0.0.1 (multicast)
        0xC0A80101, // 192.168.1.1 (not multicast)
    };
    size_t idx = 0;

    for (auto _ : state) {
        auto result = eph::dpdk::is_multicast_ip(kIps[idx & 3]);
        benchmark::DoNotOptimize(result);
        ++idx;
    }
}
BENCHMARK(BM_IsMulticastIp);

// ─────────────────────────────────────────────────────────────────────────────
// parse_udp_packet — rejection path (non-UDP protocol)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_ParseUdpPacketRejectTcp(benchmark::State& state) {
    // Build a TCP packet that parse_udp_packet should reject quickly
    FakeMbuf fake;
    constexpr uint16_t eth_len = eph::dpdk::net::kEtherHeaderLen;
    constexpr uint16_t ip_len  = eph::dpdk::net::kIpv4HeaderLen;

    auto* eth = reinterpret_cast<rte_ether_hdr*>(fake.buf);
    eth->ether_type = eph::dpdk::net::hton16(eph::dpdk::net::kEtherTypeIpv4);

    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(fake.buf + eth_len);
    ip->version_ihl    = eph::dpdk::net::kIpv4VersionIhl5;
    ip->next_proto_id  = eph::dpdk::net::kIpProtoTcp; // TCP, not UDP
    ip->total_length   = eph::dpdk::net::hton16(ip_len + 20);
    fake.set_len(eth_len + ip_len + 20);

    for (auto _ : state) {
        auto parsed = eph::dpdk::parse_udp_packet(&fake.mbuf);
        benchmark::DoNotOptimize(parsed);
    }
}
BENCHMARK(BM_ParseUdpPacketRejectTcp);
