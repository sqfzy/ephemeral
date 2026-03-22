/// @file bench_pipeline.cpp
/// DPDK end-to-end pipeline benchmarks — WS + TLS + TCP combined.
///
/// Measures the full TX/RX pipeline latency when all protocol layers
/// are composed together, capturing cache interaction effects that
/// isolated per-layer benchmarks miss.
///
/// Requires DPDK (eph-dpdk) + TLS (aws-lc).

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include <benchmark/benchmark.h>

#include "eph/dpdk/net_header.hpp"
#include "eph/net/tls_record.hpp"
#include "eph/net/websocket.hpp"

namespace {

void fill_random(uint8_t* buf, size_t len, uint32_t seed = 42) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint16_t> dist(0, 255);
    for (size_t i = 0; i < len; ++i) {
        buf[i] = static_cast<uint8_t>(dist(rng));
    }
}

eph::net::TlsHotState make_roundtrip_state(uint32_t seed = 42) {
    eph::net::TlsHotState state{};
    fill_random(state.write.ki.key, eph::net::tls_const::kAes256KeyLen, seed);
    fill_random(state.write.ki.iv,  eph::net::tls_const::kTls13NonceLen, seed + 1);
    std::memcpy(state.read.ki.key, state.write.ki.key, eph::net::tls_const::kAes256KeyLen);
    std::memcpy(state.read.ki.iv,  state.write.ki.iv,  eph::net::tls_const::kTls13NonceLen);
    state.write.seq = 0;
    state.read.seq  = 0;
    return state;
}

void PayloadSizeArgs(::benchmark::Benchmark* b) {
    for (int sz : {64, 128, 256, 512, 1024}) b->Arg(sz);
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// End-to-end TX pipeline (WS encode → TLS encrypt → TCP header build)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_E2E_TX(benchmark::State& state) {
    auto sz = static_cast<uint16_t>(state.range(0));
    auto hot = make_roundtrip_state();
    auto crypto = eph::net::TlsRecordCrypto::create(hot);
    if (!crypto) { state.SkipWithError(crypto.error()); return; }

    eph::dpdk::net::PacketTemplate pkt_tmpl{};
    pkt_tmpl.src_mac = {{0x00, 0x11, 0x22, 0x33, 0x44, 0x55}};
    pkt_tmpl.dst_mac = {{0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB}};
    pkt_tmpl.tuple = {
        .src_ip   = eph::dpdk::net::parse_ipv4("10.0.0.1"),
        .dst_ip   = eph::dpdk::net::parse_ipv4("10.0.0.2"),
        .src_port = 12345, .dst_port = 443,
    };

    std::vector<uint8_t> payload(sz);
    fill_random(payload.data(), sz, 10);

    // +1 for TLS content type byte
    size_t ws_max = eph::net::ws::kMaxFrameHeaderLen + sz + 1;
    std::vector<uint8_t> ws_buf(ws_max);
    uint16_t tls_max = eph::net::TlsRecordCrypto::encrypted_size(
        static_cast<uint16_t>(ws_max));
    std::vector<uint8_t> tls_buf(tls_max);
    uint16_t total_pkt_max = eph::dpdk::net::kAllHeadersLen + tls_max;
    std::vector<uint8_t> pkt_buf(total_pkt_max);

    auto ws_tmpl = eph::net::ws::FrameTemplate::for_binary();
    uint32_t seq = 1000, ack = 2000;

    for (auto _ : state) {
        // 1. WS encode
        size_t ws_len = ws_tmpl.encode(ws_buf.data(), payload.data(), sz);
        // 2. TLS encrypt
        uint16_t tls_len = crypto->encrypt(
            ws_buf.data(), static_cast<uint16_t>(ws_len), tls_buf.data());
        // 3. TCP/IP header
        auto* pkt = pkt_buf.data();
        auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt);
        rte_ether_addr_copy(&pkt_tmpl.dst_mac, &eth->dst_addr);
        rte_ether_addr_copy(&pkt_tmpl.src_mac, &eth->src_addr);
        eth->ether_type = eph::dpdk::net::hton16(eph::dpdk::net::kEtherTypeIpv4);
        auto* ip = reinterpret_cast<rte_ipv4_hdr*>(pkt + eph::dpdk::net::kEtherHeaderLen);
        ip->version_ihl = 0x45; ip->type_of_service = 0;
        ip->total_length = eph::dpdk::net::hton16(
            eph::dpdk::net::kIpv4HeaderLen + eph::dpdk::net::kTcpHeaderLen + tls_len);
        ip->packet_id = eph::dpdk::net::hton16(pkt_tmpl.ip_id++);
        ip->fragment_offset = eph::dpdk::net::hton16(0x4000);
        ip->time_to_live = 64;
        ip->next_proto_id = eph::dpdk::net::kIpProtoTcp;
        ip->hdr_checksum = 0;
        ip->src_addr = eph::dpdk::net::hton32(pkt_tmpl.tuple.src_ip);
        ip->dst_addr = eph::dpdk::net::hton32(pkt_tmpl.tuple.dst_ip);
        ip->hdr_checksum = eph::dpdk::net::internet_checksum(ip, eph::dpdk::net::kIpv4HeaderLen);
        auto* tcp = reinterpret_cast<rte_tcp_hdr*>(
            pkt + eph::dpdk::net::kEtherHeaderLen + eph::dpdk::net::kIpv4HeaderLen);
        tcp->src_port = eph::dpdk::net::hton16(pkt_tmpl.tuple.src_port);
        tcp->dst_port = eph::dpdk::net::hton16(pkt_tmpl.tuple.dst_port);
        tcp->sent_seq = eph::dpdk::net::hton32(seq++);
        tcp->recv_ack = eph::dpdk::net::hton32(ack);
        tcp->data_off = (eph::dpdk::net::kTcpHeaderLen / 4) << 4;
        tcp->tcp_flags = eph::dpdk::net::kTcpAck | eph::dpdk::net::kTcpPsh;
        tcp->rx_win = eph::dpdk::net::hton16(65535);
        tcp->cksum = 0; tcp->tcp_urp = 0;
        std::memcpy(pkt + eph::dpdk::net::kAllHeadersLen, tls_buf.data(), tls_len);
        uint16_t tcp_total = eph::dpdk::net::kTcpHeaderLen + tls_len;
        tcp->cksum = eph::dpdk::net::tcp_checksum(ip->src_addr, ip->dst_addr, tcp, tcp_total);

        benchmark::DoNotOptimize(pkt_buf.data());
    }
    state.SetBytesProcessed(state.iterations() * sz);
}
BENCHMARK(BM_E2E_TX)->Apply(PayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// End-to-end RX pipeline (TLS decrypt → WS decode)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_E2E_RX(benchmark::State& state) {
    auto sz = static_cast<uint16_t>(state.range(0));
    auto hot = make_roundtrip_state();

    auto enc = eph::net::TlsRecordCrypto::create(hot);
    if (!enc) { state.SkipWithError(enc.error()); return; }

    // Build unmasked WS frame (server → client)
    std::vector<uint8_t> payload(sz);
    fill_random(payload.data(), sz, 10);

    size_t ws_max = 2 + 8 + sz + 1; // +1 for TLS content type
    std::vector<uint8_t> ws_buf(ws_max);
    size_t ws_pos = 0;
    ws_buf[ws_pos++] = eph::net::ws::kFinBit | eph::net::ws::opcode::kBinary;
    if (sz < 126) {
        ws_buf[ws_pos++] = static_cast<uint8_t>(sz);
    } else {
        ws_buf[ws_pos++] = 126;
        ws_buf[ws_pos++] = static_cast<uint8_t>(sz >> 8);
        ws_buf[ws_pos++] = static_cast<uint8_t>(sz & 0xFF);
    }
    std::memcpy(ws_buf.data() + ws_pos, payload.data(), sz);
    size_t ws_frame_len = ws_pos + sz;

    // Pre-encrypt batch of records
    uint16_t record_size = eph::net::TlsRecordCrypto::encrypted_size(
        static_cast<uint16_t>(ws_frame_len));
    constexpr int64_t kBatch = 500'000;
    std::vector<std::vector<uint8_t>> records(kBatch, std::vector<uint8_t>(record_size));
    for (int64_t i = 0; i < kBatch; ++i) {
        enc->encrypt(ws_buf.data(), static_cast<uint16_t>(ws_frame_len),
                     records[i].data());
    }

    // Decryptor
    eph::net::TlsHotState dec_hot{};
    std::memcpy(dec_hot.read.ki.key, hot.write.ki.key, eph::net::tls_const::kAes256KeyLen);
    std::memcpy(dec_hot.read.ki.iv,  hot.write.ki.iv,  eph::net::tls_const::kTls13NonceLen);
    auto dec = eph::net::TlsRecordCrypto::create(dec_hot);
    if (!dec) { state.SkipWithError(dec.error()); return; }

    std::vector<uint8_t> decrypted(ws_frame_len + 16);
    int64_t idx = 0;

    for (auto _ : state) {
        if (idx > 0 && idx % kBatch == 0) {
            dec = eph::net::TlsRecordCrypto::create(dec_hot);
        }
        uint16_t dec_len;
        dec->decrypt(records[idx % kBatch].data(), record_size,
                     decrypted.data(), dec_len);
        auto frame = eph::net::ws::decode_frame(decrypted.data(), dec_len);
        benchmark::DoNotOptimize(frame);
        idx++;
    }
    state.SetBytesProcessed(state.iterations() * sz);
}
BENCHMARK(BM_E2E_RX)->Apply(PayloadSizeArgs);

BENCHMARK_MAIN();
