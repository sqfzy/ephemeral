/// @file bench_tcp_header.cpp
/// DPDK TCP/IP header layer benchmarks — checksum, header build/parse.
///
/// Requires DPDK (eph-dpdk).

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include <benchmark/benchmark.h>

#include "eph/dpdk/net_header.hpp"
#include "eph/dpdk/reactor.hpp"

namespace {

void fill_random(uint8_t* buf, size_t len, uint32_t seed = 42) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint16_t> dist(0, 255);
    for (size_t i = 0; i < len; ++i) {
        buf[i] = static_cast<uint8_t>(dist(rng));
    }
}

void PayloadSizeArgs(::benchmark::Benchmark* b) {
    for (int sz : {64, 128, 256, 512, 1024}) b->Arg(sz);
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Internet checksum
// ─────────────────────────────────────────────────────────────────────────────

static void BM_Checksum(benchmark::State& state) {
    auto sz = static_cast<size_t>(state.range(0));
    std::vector<uint8_t> data(sz);
    fill_random(data.data(), sz, 99);

    for (auto _ : state) {
        auto c = eph::dpdk::net::internet_checksum(data.data(), sz);
        benchmark::DoNotOptimize(c);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(sz));
}
BENCHMARK(BM_Checksum)->Apply(PayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// TCP/IP header build (manual, no mbuf)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_TcpHeaderBuild(benchmark::State& state) {
    auto sz = static_cast<uint16_t>(state.range(0));

    eph::dpdk::net::PacketTemplate tmpl{};
    tmpl.src_mac = {{0x00, 0x11, 0x22, 0x33, 0x44, 0x55}};
    tmpl.dst_mac = {{0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB}};
    tmpl.tuple = {
        .src_ip   = eph::dpdk::net::parse_ipv4("10.0.0.1"),
        .dst_ip   = eph::dpdk::net::parse_ipv4("10.0.0.2"),
        .src_port = 12345,
        .dst_port = 443,
    };

    std::vector<uint8_t> payload(sz);
    fill_random(payload.data(), sz, 50);

    uint16_t total_pkt_len = eph::dpdk::net::kAllHeadersLen + sz;
    std::vector<uint8_t> pkt_buf(total_pkt_len);
    uint32_t seq = 1000, ack = 2000;

    for (auto _ : state) {
        auto* pkt = pkt_buf.data();
        auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt);
        rte_ether_addr_copy(&tmpl.dst_mac, &eth->dst_addr);
        rte_ether_addr_copy(&tmpl.src_mac, &eth->src_addr);
        eth->ether_type = eph::dpdk::net::hton16(eph::dpdk::net::kEtherTypeIpv4);

        auto* ip = reinterpret_cast<rte_ipv4_hdr*>(pkt + eph::dpdk::net::kEtherHeaderLen);
        ip->version_ihl = 0x45;
        ip->type_of_service = 0;
        ip->total_length = eph::dpdk::net::hton16(
            eph::dpdk::net::kIpv4HeaderLen + eph::dpdk::net::kTcpHeaderLen + sz);
        ip->packet_id = eph::dpdk::net::hton16(tmpl.ip_id++);
        ip->fragment_offset = eph::dpdk::net::hton16(0x4000);
        ip->time_to_live = 64;
        ip->next_proto_id = eph::dpdk::net::kIpProtoTcp;
        ip->hdr_checksum = 0;
        ip->src_addr = eph::dpdk::net::hton32(tmpl.tuple.src_ip);
        ip->dst_addr = eph::dpdk::net::hton32(tmpl.tuple.dst_ip);
        ip->hdr_checksum = eph::dpdk::net::internet_checksum(ip, eph::dpdk::net::kIpv4HeaderLen);

        auto* tcp = reinterpret_cast<rte_tcp_hdr*>(
            pkt + eph::dpdk::net::kEtherHeaderLen + eph::dpdk::net::kIpv4HeaderLen);
        tcp->src_port = eph::dpdk::net::hton16(tmpl.tuple.src_port);
        tcp->dst_port = eph::dpdk::net::hton16(tmpl.tuple.dst_port);
        tcp->sent_seq = eph::dpdk::net::hton32(seq++);
        tcp->recv_ack = eph::dpdk::net::hton32(ack);
        tcp->data_off = (eph::dpdk::net::kTcpHeaderLen / 4) << 4;
        tcp->tcp_flags = eph::dpdk::net::kTcpAck | eph::dpdk::net::kTcpPsh;
        tcp->rx_win = eph::dpdk::net::hton16(65535);
        tcp->cksum = 0;
        tcp->tcp_urp = 0;
        std::memcpy(pkt + eph::dpdk::net::kAllHeadersLen, payload.data(), sz);
        uint16_t tcp_total = eph::dpdk::net::kTcpHeaderLen + sz;
        tcp->cksum = eph::dpdk::net::tcp_checksum(ip->src_addr, ip->dst_addr, tcp, tcp_total);

        benchmark::DoNotOptimize(pkt_buf.data());
    }
}
BENCHMARK(BM_TcpHeaderBuild)->Apply(PayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// TCP/IP header parse
// ─────────────────────────────────────────────────────────────────────────────

static void BM_TcpHeaderParse(benchmark::State& state) {
    auto sz = static_cast<uint16_t>(state.range(0));
    uint16_t total_pkt_len = eph::dpdk::net::kAllHeadersLen + sz;
    std::vector<uint8_t> pkt_buf(total_pkt_len, 0);

    auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt_buf.data());
    eth->ether_type = eph::dpdk::net::hton16(eph::dpdk::net::kEtherTypeIpv4);

    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(
        pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen);
    ip->version_ihl = 0x45;
    ip->total_length = eph::dpdk::net::hton16(
        eph::dpdk::net::kIpv4HeaderLen + eph::dpdk::net::kTcpHeaderLen + sz);
    ip->next_proto_id = eph::dpdk::net::kIpProtoTcp;

    auto* tcp = reinterpret_cast<rte_tcp_hdr*>(
        pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen +
        eph::dpdk::net::kIpv4HeaderLen);
    tcp->data_off = (eph::dpdk::net::kTcpHeaderLen / 4) << 4;
    tcp->tcp_flags = eph::dpdk::net::kTcpAck;

    for (auto _ : state) {
        auto* e = reinterpret_cast<const rte_ether_hdr*>(pkt_buf.data());
        uint16_t etype = eph::dpdk::net::ntoh16(e->ether_type);
        const rte_tcp_hdr* t = nullptr;
        uint16_t pl_len = 0;

        if (etype == eph::dpdk::net::kEtherTypeIpv4) {
            auto* ipv4 = reinterpret_cast<const rte_ipv4_hdr*>(
                pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen);
            uint8_t ihl = (ipv4->version_ihl & 0x0F) << 2;
            if (ipv4->next_proto_id == eph::dpdk::net::kIpProtoTcp) {
                uint16_t tcp_off = eph::dpdk::net::kEtherHeaderLen + ihl;
                t = reinterpret_cast<const rte_tcp_hdr*>(pkt_buf.data() + tcp_off);
                uint8_t doff = (t->data_off >> 4) << 2;
                uint16_t payload_off = tcp_off + doff;
                if (total_pkt_len > payload_off)
                    pl_len = total_pkt_len - payload_off;
            }
        }
        benchmark::DoNotOptimize(pl_len);
        benchmark::DoNotOptimize(t);
    }
}
BENCHMARK(BM_TcpHeaderParse)->Apply(PayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// parse_packet() — real function (includes all validation guards)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_ParsePacketReal(benchmark::State& state) {
    auto sz = static_cast<uint16_t>(state.range(0));
    uint16_t total_pkt_len = eph::dpdk::net::kAllHeadersLen + sz;
    std::vector<uint8_t> pkt_buf(total_pkt_len, 0);

    auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt_buf.data());
    eth->ether_type = eph::dpdk::net::hton16(eph::dpdk::net::kEtherTypeIpv4);

    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(
        pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen);
    ip->version_ihl = 0x45;
    ip->total_length = eph::dpdk::net::hton16(
        eph::dpdk::net::kIpv4HeaderLen + eph::dpdk::net::kTcpHeaderLen + sz);
    ip->next_proto_id = eph::dpdk::net::kIpProtoTcp;
    ip->src_addr = eph::dpdk::net::hton32(0x0A000001);
    ip->dst_addr = eph::dpdk::net::hton32(0x0A000002);

    auto* tcp = reinterpret_cast<rte_tcp_hdr*>(
        pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen +
        eph::dpdk::net::kIpv4HeaderLen);
    tcp->data_off = (eph::dpdk::net::kTcpHeaderLen / 4) << 4;
    tcp->tcp_flags = eph::dpdk::net::kTcpAck;
    tcp->src_port = eph::dpdk::net::hton16(12345);
    tcp->dst_port = eph::dpdk::net::hton16(443);

    // Simulate mbuf with flat buffer
    rte_mbuf mbuf{};
    mbuf.buf_addr = pkt_buf.data();
    mbuf.data_off = 0;
    mbuf.data_len = total_pkt_len;
    mbuf.pkt_len  = total_pkt_len;

    for (auto _ : state) {
        auto parsed = eph::dpdk::net::parse_packet(&mbuf);
        benchmark::DoNotOptimize(parsed.payload_len);
        benchmark::DoNotOptimize(parsed.tcp);
    }
}
BENCHMARK(BM_ParsePacketReal)->Apply(PayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// TCP checksum (pseudo-header + payload)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_TcpChecksum(benchmark::State& state) {
    auto sz = static_cast<uint16_t>(state.range(0));
    uint16_t tcp_total = eph::dpdk::net::kTcpHeaderLen + sz;
    std::vector<uint8_t> tcp_seg(tcp_total);
    fill_random(tcp_seg.data(), tcp_total, 77);

    // Set up a minimal TCP header
    auto* tcp = reinterpret_cast<rte_tcp_hdr*>(tcp_seg.data());
    tcp->data_off = (eph::dpdk::net::kTcpHeaderLen / 4) << 4;
    tcp->cksum = 0;

    uint32_t src_ip = eph::dpdk::net::hton32(0x0A000001);
    uint32_t dst_ip = eph::dpdk::net::hton32(0x0A000002);

    for (auto _ : state) {
        auto c = eph::dpdk::net::tcp_checksum(src_ip, dst_ip, tcp_seg.data(), tcp_total);
        benchmark::DoNotOptimize(c);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(tcp_total));
}
BENCHMARK(BM_TcpChecksum)->Apply(PayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// IPv4 parse + format roundtrip
// ─────────────────────────────────────────────────────────────────────────────

static void BM_Ipv4ParseFormat(benchmark::State& state) {
    const char* ips[] = {
        "10.0.0.1", "192.168.1.100", "172.16.254.1",
        "8.8.8.8", "255.255.255.255",
    };
    constexpr size_t n = 5;
    size_t idx = 0;

    for (auto _ : state) {
        auto ip = eph::dpdk::net::parse_ipv4(ips[idx % n]);
        auto buf = eph::dpdk::net::format_ipv4(ip);
        benchmark::DoNotOptimize(buf.data());
        idx++;
    }
}
BENCHMARK(BM_Ipv4ParseFormat);

// ─────────────────────────────────────────────────────────────────────────────
// ReactorEntry::hash_tuple — per-packet dispatch pre-filter
// ─────────────────────────────────────────────────────────────────────────────

static void BM_ReactorHashTuple(benchmark::State& state) {
    eph::dpdk::net::ConnectionTuple tuples[] = {
        {.src_ip = 0x0A000001, .dst_ip = 0x0A000002, .src_port = 12345, .dst_port = 443},
        {.src_ip = 0xC0A80101, .dst_ip = 0x08080808, .src_port = 5000, .dst_port = 80},
        {.src_ip = 0xAC100001, .dst_ip = 0xAC100002, .src_port = 8443, .dst_port = 9090},
        {.src_ip = 0x0A0A0A01, .dst_ip = 0x0A0A0A02, .src_port = 55123, .dst_port = 443},
    };
    size_t idx = 0;

    for (auto _ : state) {
        auto h = eph::dpdk::ReactorEntry::hash_tuple(tuples[idx & 3]);
        benchmark::DoNotOptimize(h);
        ++idx;
    }
}
BENCHMARK(BM_ReactorHashTuple);

// ─────────────────────────────────────────────────────────────────────────────
// Reactor dispatch simulation — parse + hash + linear scan match
// ─────────────────────────────────────────────────────────────────────────────

static void BM_ReactorDispatchSim(benchmark::State& state) {
    auto n_conns = static_cast<size_t>(state.range(0));

    // Pre-build connection tuples and hashes
    std::vector<eph::dpdk::net::ConnectionTuple> tuples(n_conns);
    std::vector<uint64_t> hashes(n_conns);
    for (size_t i = 0; i < n_conns; ++i) {
        tuples[i] = {
            .src_ip = 0x0A000001 + static_cast<uint32_t>(i),
            .dst_ip = 0x0A000002,
            .src_port = static_cast<uint16_t>(12345 + i),
            .dst_port = 443,
        };
        hashes[i] = eph::dpdk::ReactorEntry::hash_tuple(tuples[i]);
    }

    // Build a packet tuple that matches the LAST connection (worst case)
    auto pkt_tuple = tuples[n_conns - 1];
    // Swap src/dst as incoming packet would
    std::swap(pkt_tuple.src_ip, pkt_tuple.dst_ip);
    std::swap(pkt_tuple.src_port, pkt_tuple.dst_port);
    uint64_t pkt_hash = eph::dpdk::ReactorEntry::hash_tuple(pkt_tuple);

    for (auto _ : state) {
        bool found = false;
        for (size_t j = 0; j < n_conns; ++j) {
            if (hashes[j] != pkt_hash) continue;
            // Simulate matches() check
            if (tuples[j].src_ip == pkt_tuple.dst_ip &&
                tuples[j].dst_ip == pkt_tuple.src_ip &&
                tuples[j].src_port == pkt_tuple.dst_port &&
                tuples[j].dst_port == pkt_tuple.src_port) {
                found = true;
                break;
            }
        }
        benchmark::DoNotOptimize(found);
    }
}
BENCHMARK(BM_ReactorDispatchSim)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16);

BENCHMARK_MAIN();
