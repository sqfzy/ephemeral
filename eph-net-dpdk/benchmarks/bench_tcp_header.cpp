/// @file bench_tcp_header.cpp
/// DPDK TCP/UDP/IP header layer benchmarks — checksum, header build/parse.
///
/// Requires DPDK (eph-dpdk).

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include <benchmark/benchmark.h>

#include "eph/dpdk/net_header.hpp"

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
// write_syn_options — SYN option serialization
// ─────────────────────────────────────────────────────────────────────────────

static void BM_WriteSynOptions(benchmark::State& state) {
    uint8_t buf[12];
    for (auto _ : state) {
        auto len = eph::dpdk::net::write_syn_options(buf, 1460);
        benchmark::DoNotOptimize(len);
        benchmark::DoNotOptimize(buf);
    }
}
BENCHMARK(BM_WriteSynOptions);

// ─────────────────────────────────────────────────────────────────────────────
// ConnectionTuple::dump — formatting
// ─────────────────────────────────────────────────────────────────────────────

static void BM_ConnectionTupleDump(benchmark::State& state) {
    eph::dpdk::net::ConnectionTuple t{
        .src_ip = 0x0A000001, .dst_ip = 0x0A000002,
        .src_port = 12345, .dst_port = 443};
    for (auto _ : state) {
        auto s = t.dump();
        benchmark::DoNotOptimize(s.data());
    }
}
BENCHMARK(BM_ConnectionTupleDump);

// ─────────────────────────────────────────────────────────────────────────────
// ParsedPacket::dump — diagnostic output formatting
// ─────────────────────────────────────────────────────────────────────────────

static void BM_ParsedPacketDump(benchmark::State& state) {
    uint16_t total_pkt_len = eph::dpdk::net::kAllHeadersLen + 100;
    std::vector<uint8_t> pkt_buf(total_pkt_len, 0);

    auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt_buf.data());
    eth->ether_type = eph::dpdk::net::hton16(eph::dpdk::net::kEtherTypeIpv4);

    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(
        pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen);
    ip->version_ihl = 0x45;
    ip->total_length = eph::dpdk::net::hton16(
        eph::dpdk::net::kIpv4HeaderLen + eph::dpdk::net::kTcpHeaderLen + 100);
    ip->next_proto_id = eph::dpdk::net::kIpProtoTcp;
    ip->src_addr = eph::dpdk::net::hton32(0x0A000001);
    ip->dst_addr = eph::dpdk::net::hton32(0x0A000002);

    auto* tcp = reinterpret_cast<rte_tcp_hdr*>(
        pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen +
        eph::dpdk::net::kIpv4HeaderLen);
    tcp->data_off = (eph::dpdk::net::kTcpHeaderLen / 4) << 4;
    tcp->tcp_flags = eph::dpdk::net::kTcpAck | eph::dpdk::net::kTcpPsh;
    tcp->src_port = eph::dpdk::net::hton16(12345);
    tcp->dst_port = eph::dpdk::net::hton16(443);
    tcp->sent_seq = eph::dpdk::net::hton32(1000);
    tcp->recv_ack = eph::dpdk::net::hton32(2000);
    tcp->rx_win   = eph::dpdk::net::hton16(65535);

    rte_mbuf mbuf{};
    mbuf.buf_addr = pkt_buf.data();
    mbuf.data_off = 0;
    mbuf.data_len = total_pkt_len;
    mbuf.pkt_len  = total_pkt_len;

    auto parsed = eph::dpdk::net::parse_packet(&mbuf);

    for (auto _ : state) {
        auto s = parsed.dump();
        benchmark::DoNotOptimize(s.data());
    }
}
BENCHMARK(BM_ParsedPacketDump);

// ─────────────────────────────────────────────────────────────────────────────
// PacketTemplate::dump — diagnostic output formatting
// ─────────────────────────────────────────────────────────────────────────────

static void BM_PacketTemplateDump(benchmark::State& state) {
    eph::dpdk::net::PacketTemplate tmpl{};
    tmpl.src_mac = {{0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x23}};
    tmpl.dst_mac = {{0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB}};
    tmpl.tuple = {
        .src_ip = 0x0A000001, .dst_ip = 0x0A000002,
        .src_port = 12345, .dst_port = 443};
    tmpl.mss = 1460;
    tmpl.hw_cksum = true;

    for (auto _ : state) {
        auto s = tmpl.dump();
        benchmark::DoNotOptimize(s.data());
    }
}
BENCHMARK(BM_PacketTemplateDump);

// ───────────────────────────────────────────────────────��─────────────────────
// PacketTemplate::validate — compile-time validation
// ─────────────────────────────────────────────────────────────────────────────

static void BM_PacketTemplateValidate(benchmark::State& state) {
    eph::dpdk::net::PacketTemplate tmpl{};
    tmpl.tuple = {
        .src_ip = 0x0A000001, .dst_ip = 0x0A000002,
        .src_port = 12345, .dst_port = 443};

    for (auto _ : state) {
        auto v = tmpl.validate();
        benchmark::DoNotOptimize(v.data());
    }
}
BENCHMARK(BM_PacketTemplateValidate);

// ─────────────────────────────────────────────────────────────────────────────
// PacketTemplate::to_json — JSON serialization
// ─────────────────────────────────────────────────────────────────────────────

static void BM_PacketTemplateToJson(benchmark::State& state) {
    eph::dpdk::net::PacketTemplate tmpl{};
    tmpl.src_mac = {{0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x23}};
    tmpl.dst_mac = {{0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB}};
    tmpl.tuple = {
        .src_ip = 0x0A000001, .dst_ip = 0x0A000002,
        .src_port = 12345, .dst_port = 443};
    tmpl.mss = 1460;
    tmpl.hw_cksum = true;

    for (auto _ : state) {
        auto s = tmpl.to_json();
        benchmark::DoNotOptimize(s.data());
    }
}
BENCHMARK(BM_PacketTemplateToJson);

// ─────────────────────────────────────────────────────────────────────────────
// ParsedPacket::to_json
// ─────────────────────────────────────────────────────────────────────────────

static void BM_ParsedPacketToJson(benchmark::State& state) {
    uint16_t total_pkt_len = eph::dpdk::net::kAllHeadersLen + 100;
    std::vector<uint8_t> pkt_buf(total_pkt_len, 0);

    auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt_buf.data());
    eth->ether_type = eph::dpdk::net::hton16(eph::dpdk::net::kEtherTypeIpv4);

    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(
        pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen);
    ip->version_ihl = 0x45;
    ip->total_length = eph::dpdk::net::hton16(
        eph::dpdk::net::kIpv4HeaderLen + eph::dpdk::net::kTcpHeaderLen + 100);
    ip->next_proto_id = eph::dpdk::net::kIpProtoTcp;
    ip->src_addr = eph::dpdk::net::hton32(0x0A000001);
    ip->dst_addr = eph::dpdk::net::hton32(0x0A000002);

    auto* tcp = reinterpret_cast<rte_tcp_hdr*>(
        pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen +
        eph::dpdk::net::kIpv4HeaderLen);
    tcp->data_off = (eph::dpdk::net::kTcpHeaderLen / 4) << 4;
    tcp->tcp_flags = eph::dpdk::net::kTcpAck | eph::dpdk::net::kTcpPsh;
    tcp->src_port = eph::dpdk::net::hton16(12345);
    tcp->dst_port = eph::dpdk::net::hton16(443);
    tcp->sent_seq = eph::dpdk::net::hton32(1000);
    tcp->recv_ack = eph::dpdk::net::hton32(2000);
    tcp->rx_win   = eph::dpdk::net::hton16(65535);

    rte_mbuf mbuf{};
    mbuf.buf_addr = pkt_buf.data();
    mbuf.data_off = 0;
    mbuf.data_len = total_pkt_len;
    mbuf.pkt_len  = total_pkt_len;

    auto parsed = eph::dpdk::net::parse_packet(&mbuf);

    for (auto _ : state) {
        auto s = parsed.to_json();
        benchmark::DoNotOptimize(s.data());
    }
}
BENCHMARK(BM_ParsedPacketToJson);

// ─────────────────────────────────────────────────────────────────────────────
// ConnectionTuple::to_json
// ─────────────────────────────────────────────────────────────────────────────

static void BM_ConnectionTupleToJson(benchmark::State& state) {
    eph::dpdk::net::ConnectionTuple t{
        .src_ip = 0x0A000001, .dst_ip = 0x0A000002,
        .src_port = 12345, .dst_port = 443};
    for (auto _ : state) {
        auto s = t.to_json();
        benchmark::DoNotOptimize(s.data());
    }
}
BENCHMARK(BM_ConnectionTupleToJson);

// ─────────────────────────────────────────────────────────────────────────────
// TcpConfig::dump / to_json
// ─────────────────────────────────────────────────────────────────────────────

#include "eph/dpdk/tcp.hpp"

static void BM_TcpConfigDump(benchmark::State& state) {
    eph::dpdk::TcpConfig cfg{};
    cfg.tuple = {
        .src_ip = 0x0A000001, .dst_ip = 0x0A000002,
        .src_port = 12345, .dst_port = 443};
    cfg.mss = 1460;
    cfg.recv_window = 65535;
    cfg.src_mac = {{0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x23}};
    cfg.dst_mac = {{0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB}};

    for (auto _ : state) {
        auto s = cfg.dump();
        benchmark::DoNotOptimize(s.data());
    }
}
BENCHMARK(BM_TcpConfigDump);

static void BM_TcpConfigToJson(benchmark::State& state) {
    eph::dpdk::TcpConfig cfg{};
    cfg.tuple = {
        .src_ip = 0x0A000001, .dst_ip = 0x0A000002,
        .src_port = 12345, .dst_port = 443};
    cfg.mss = 1460;
    cfg.recv_window = 65535;
    cfg.src_mac = {{0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x23}};
    cfg.dst_mac = {{0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB}};

    for (auto _ : state) {
        auto s = cfg.to_json();
        benchmark::DoNotOptimize(s.data());
    }
}
BENCHMARK(BM_TcpConfigToJson);

// ─────────────────────────────────────────────────────────────────────────────
// TcpStats::dump / to_json
// ─────────────────────────────────────────────────────────────────────────────

static void BM_TcpStatsDump(benchmark::State& state) {
    eph::dpdk::TcpSession<>::Stats s{};
    s.tx_packets = 10000;
    s.rx_packets = 9500;
    s.tx_bytes = 5000000;
    s.rx_bytes = 4750000;
    s.reorder_hits = 42;
    s.gap_histogram[10] = 5;
    s.gap_histogram[11] = 2;

    for (auto _ : state) {
        auto d = s.dump();
        benchmark::DoNotOptimize(d.data());
    }
}
BENCHMARK(BM_TcpStatsDump);

static void BM_TcpStatsToJson(benchmark::State& state) {
    eph::dpdk::TcpSession<>::Stats s{};
    s.tx_packets = 10000;
    s.rx_packets = 9500;
    s.tx_bytes = 5000000;
    s.rx_bytes = 4750000;
    s.reorder_hits = 42;
    s.gap_histogram[10] = 5;
    s.gap_histogram[11] = 2;

    for (auto _ : state) {
        auto j = s.to_json();
        benchmark::DoNotOptimize(j.data());
    }
}
BENCHMARK(BM_TcpStatsToJson);

// ─────────────────────────────────────────────────────────────────────────────
// FlowRule::dump / to_json
// ─────────────────────────────────────────────────────────────────────────────

#include "eph/net/dpdk/flow_steering.hpp"

static void BM_FlowRuleDump(benchmark::State& state) {
    eph::net::dpdk::FlowRule rule;
    rule.port_id = 1;
    rule.queue_id = 3;
    // Simulate active rule with fake pointer (LocalFlowHandle variant arm).
    rule.handle = eph::net::dpdk::LocalFlowHandle{
        reinterpret_cast<rte_flow*>(0xDEAD)};

    for (auto _ : state) {
        auto s = rule.dump();
        benchmark::DoNotOptimize(s.data());
    }
    // Reset to monostate so the dtor does not try rte_flow_destroy on the
    // synthetic 0xDEAD pointer.
    rule.handle = std::monostate{};
}
BENCHMARK(BM_FlowRuleDump);

static void BM_FlowRuleToJson(benchmark::State& state) {
    eph::net::dpdk::FlowRule rule;
    rule.port_id = 1;
    rule.queue_id = 3;
    rule.handle = eph::net::dpdk::LocalFlowHandle{
        reinterpret_cast<rte_flow*>(0xDEAD)};

    for (auto _ : state) {
        auto s = rule.to_json();
        benchmark::DoNotOptimize(s.data());
    }
    rule.handle = std::monostate{};
}
BENCHMARK(BM_FlowRuleToJson);

// ─────────────────────────────────────────────────────────────────────────────
// UDP checksum
// ─────────────────────────────────────────────────────────────────────────────

static void BM_UdpChecksum(benchmark::State& state) {
    auto sz = static_cast<size_t>(state.range(0));
    // Build a fake UDP segment: 8-byte header + payload
    uint16_t total_udp_len = eph::dpdk::net::kUdpHeaderLen + static_cast<uint16_t>(sz);
    std::vector<uint8_t> udp_seg(total_udp_len);
    auto* udp = reinterpret_cast<eph::dpdk::net::UdpHeader*>(udp_seg.data());
    udp->src_port = eph::dpdk::net::hton16(12345);
    udp->dst_port = eph::dpdk::net::hton16(5000);
    udp->length   = eph::dpdk::net::hton16(total_udp_len);
    udp->checksum = 0;
    fill_random(udp_seg.data() + eph::dpdk::net::kUdpHeaderLen, sz, 77);

    uint32_t src_ip_net = eph::dpdk::net::hton32(
        eph::dpdk::net::parse_ipv4("10.0.0.1"));
    uint32_t dst_ip_net = eph::dpdk::net::hton32(
        eph::dpdk::net::parse_ipv4("10.0.0.2"));

    for (auto _ : state) {
        auto c = eph::dpdk::net::udp_checksum(
            src_ip_net, dst_ip_net, udp_seg.data(), total_udp_len);
        benchmark::DoNotOptimize(c);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(total_udp_len));
}
BENCHMARK(BM_UdpChecksum)->Apply(PayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// UDP packet parse — layered API (parse_ip_header → parse_udp_from_ip)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_ParseUdpPacket(benchmark::State& state) {
    auto payload_sz = static_cast<uint16_t>(state.range(0));
    uint16_t total_pkt_len = eph::dpdk::net::kEtherHeaderLen +
                             eph::dpdk::net::kIpv4HeaderLen +
                             eph::dpdk::net::kUdpHeaderLen + payload_sz;
    std::vector<uint8_t> pkt_buf(total_pkt_len, 0);

    auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt_buf.data());
    eth->ether_type = eph::dpdk::net::hton16(eph::dpdk::net::kEtherTypeIpv4);

    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(
        pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen);
    ip->version_ihl = 0x45;
    ip->total_length = eph::dpdk::net::hton16(
        eph::dpdk::net::kIpv4HeaderLen +
        eph::dpdk::net::kUdpHeaderLen + payload_sz);
    ip->next_proto_id = eph::dpdk::net::kIpProtoUdp;
    ip->src_addr = eph::dpdk::net::hton32(0x0A000001);
    ip->dst_addr = eph::dpdk::net::hton32(0x0A000002);

    auto* udp = reinterpret_cast<eph::dpdk::net::UdpHeader*>(
        pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen +
        eph::dpdk::net::kIpv4HeaderLen);
    udp->src_port = eph::dpdk::net::hton16(12345);
    udp->dst_port = eph::dpdk::net::hton16(5000);
    udp->length   = eph::dpdk::net::hton16(
        eph::dpdk::net::kUdpHeaderLen + payload_sz);

    fill_random(pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen +
                eph::dpdk::net::kIpv4HeaderLen +
                eph::dpdk::net::kUdpHeaderLen,
                payload_sz, 88);

    rte_mbuf mbuf{};
    mbuf.buf_addr = pkt_buf.data();
    mbuf.data_off = 0;
    mbuf.data_len = total_pkt_len;
    mbuf.pkt_len  = total_pkt_len;

    for (auto _ : state) {
        auto parsed = eph::dpdk::net::parse_udp_packet(&mbuf);
        benchmark::DoNotOptimize(parsed);
    }
    state.SetBytesProcessed(state.iterations() * total_pkt_len);
}
BENCHMARK(BM_ParseUdpPacket)->Apply(PayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// UDP layered parse — parse_ip_header + parse_udp_from_ip (two-step)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_ParseUdpLayered(benchmark::State& state) {
    auto payload_sz = static_cast<uint16_t>(state.range(0));
    uint16_t total_pkt_len = eph::dpdk::net::kEtherHeaderLen +
                             eph::dpdk::net::kIpv4HeaderLen +
                             eph::dpdk::net::kUdpHeaderLen + payload_sz;
    std::vector<uint8_t> pkt_buf(total_pkt_len, 0);

    auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt_buf.data());
    eth->ether_type = eph::dpdk::net::hton16(eph::dpdk::net::kEtherTypeIpv4);

    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(
        pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen);
    ip->version_ihl = 0x45;
    ip->total_length = eph::dpdk::net::hton16(
        eph::dpdk::net::kIpv4HeaderLen +
        eph::dpdk::net::kUdpHeaderLen + payload_sz);
    ip->next_proto_id = eph::dpdk::net::kIpProtoUdp;
    ip->src_addr = eph::dpdk::net::hton32(0x0A000001);
    ip->dst_addr = eph::dpdk::net::hton32(0x0A000002);

    auto* udp = reinterpret_cast<eph::dpdk::net::UdpHeader*>(
        pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen +
        eph::dpdk::net::kIpv4HeaderLen);
    udp->src_port = eph::dpdk::net::hton16(12345);
    udp->dst_port = eph::dpdk::net::hton16(5000);
    udp->length   = eph::dpdk::net::hton16(
        eph::dpdk::net::kUdpHeaderLen + payload_sz);

    rte_mbuf mbuf{};
    mbuf.buf_addr = pkt_buf.data();
    mbuf.data_off = 0;
    mbuf.data_len = total_pkt_len;
    mbuf.pkt_len  = total_pkt_len;

    for (auto _ : state) {
        auto ip_hdr = eph::dpdk::net::parse_ip_header(&mbuf);
        auto parsed = eph::dpdk::net::parse_udp_from_ip(&mbuf, ip_hdr);
        benchmark::DoNotOptimize(parsed);
    }
    state.SetBytesProcessed(state.iterations() * total_pkt_len);
}
BENCHMARK(BM_ParseUdpLayered)->Apply(PayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// UdpPacketTemplate fill — hot-path template stamping
// ─────────────────────────────────────────────────────────────────────────────

static void BM_UdpTemplateFill(benchmark::State& state) {
    auto payload_sz = static_cast<uint16_t>(state.range(0));

    eph::dpdk::net::UdpPacketTemplate tmpl;
    tmpl.init(
        rte_ether_addr{{0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x23}},
        rte_ether_addr{{0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB}},
        eph::dpdk::net::parse_ipv4("10.0.0.1"),
        eph::dpdk::net::parse_ipv4("10.0.0.2"),
        12345, 5000, false);

    uint16_t total_len = eph::dpdk::net::kEtherHeaderLen +
                         eph::dpdk::net::kIpv4HeaderLen +
                         eph::dpdk::net::kUdpHeaderLen + payload_sz;
    std::vector<uint8_t> pkt_buf(total_len, 0);
    std::vector<uint8_t> payload(payload_sz);
    fill_random(payload.data(), payload_sz, 33);

    rte_mbuf mbuf{};
    mbuf.buf_addr = pkt_buf.data();
    mbuf.data_off = 0;
    mbuf.buf_len  = static_cast<uint16_t>(pkt_buf.size());

    for (auto _ : state) {
        auto len = tmpl.fill(&mbuf, payload.data(), payload_sz);
        benchmark::DoNotOptimize(len);
    }
    state.SetBytesProcessed(state.iterations() * total_len);
}
BENCHMARK(BM_UdpTemplateFill)->Apply(PayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// ParsedUdpPacket dump/to_json
// ─────────────────────────────────────────────────────────────────────────────

static void BM_ParsedUdpPacketDump(benchmark::State& state) {
    uint16_t total_pkt_len = eph::dpdk::net::kEtherHeaderLen +
                             eph::dpdk::net::kIpv4HeaderLen +
                             eph::dpdk::net::kUdpHeaderLen + 100;
    std::vector<uint8_t> pkt_buf(total_pkt_len, 0);

    auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt_buf.data());
    eth->ether_type = eph::dpdk::net::hton16(eph::dpdk::net::kEtherTypeIpv4);

    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(
        pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen);
    ip->version_ihl = 0x45;
    ip->total_length = eph::dpdk::net::hton16(
        eph::dpdk::net::kIpv4HeaderLen + eph::dpdk::net::kUdpHeaderLen + 100);
    ip->next_proto_id = eph::dpdk::net::kIpProtoUdp;
    ip->src_addr = eph::dpdk::net::hton32(0x0A000001);
    ip->dst_addr = eph::dpdk::net::hton32(0x0A000002);

    auto* udp = reinterpret_cast<eph::dpdk::net::UdpHeader*>(
        pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen +
        eph::dpdk::net::kIpv4HeaderLen);
    udp->src_port = eph::dpdk::net::hton16(12345);
    udp->dst_port = eph::dpdk::net::hton16(5000);
    udp->length   = eph::dpdk::net::hton16(
        eph::dpdk::net::kUdpHeaderLen + 100);

    rte_mbuf mbuf{};
    mbuf.buf_addr = pkt_buf.data();
    mbuf.data_off = 0;
    mbuf.data_len = total_pkt_len;
    mbuf.pkt_len  = total_pkt_len;

    auto parsed = eph::dpdk::net::parse_udp_packet(&mbuf);

    for (auto _ : state) {
        auto s = parsed.dump();
        benchmark::DoNotOptimize(s.data());
    }
}
BENCHMARK(BM_ParsedUdpPacketDump);

static void BM_ParsedUdpPacketToJson(benchmark::State& state) {
    uint16_t total_pkt_len = eph::dpdk::net::kEtherHeaderLen +
                             eph::dpdk::net::kIpv4HeaderLen +
                             eph::dpdk::net::kUdpHeaderLen + 100;
    std::vector<uint8_t> pkt_buf(total_pkt_len, 0);

    auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt_buf.data());
    eth->ether_type = eph::dpdk::net::hton16(eph::dpdk::net::kEtherTypeIpv4);

    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(
        pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen);
    ip->version_ihl = 0x45;
    ip->total_length = eph::dpdk::net::hton16(
        eph::dpdk::net::kIpv4HeaderLen + eph::dpdk::net::kUdpHeaderLen + 100);
    ip->next_proto_id = eph::dpdk::net::kIpProtoUdp;
    ip->src_addr = eph::dpdk::net::hton32(0x0A000001);
    ip->dst_addr = eph::dpdk::net::hton32(0x0A000002);

    auto* udp = reinterpret_cast<eph::dpdk::net::UdpHeader*>(
        pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen +
        eph::dpdk::net::kIpv4HeaderLen);
    udp->src_port = eph::dpdk::net::hton16(12345);
    udp->dst_port = eph::dpdk::net::hton16(5000);
    udp->length   = eph::dpdk::net::hton16(
        eph::dpdk::net::kUdpHeaderLen + 100);

    rte_mbuf mbuf{};
    mbuf.buf_addr = pkt_buf.data();
    mbuf.data_off = 0;
    mbuf.data_len = total_pkt_len;
    mbuf.pkt_len  = total_pkt_len;

    auto parsed = eph::dpdk::net::parse_udp_packet(&mbuf);

    for (auto _ : state) {
        auto s = parsed.to_json();
        benchmark::DoNotOptimize(s.data());
    }
}
BENCHMARK(BM_ParsedUdpPacketToJson);

BENCHMARK_MAIN();
