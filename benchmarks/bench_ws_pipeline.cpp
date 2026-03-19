/// @file bench_ws_pipeline.cpp
/// Benchmark for the DPDK WebSocket pipeline — measures per-stage latency
/// using Google Benchmark with payload size as a parameter.

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include <benchmark/benchmark.h>

#include "eph/dpdk/net_header.hpp"
#include "eph/dpdk/tls_record.hpp"
#include "eph/dpdk/websocket.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

void fill_random(uint8_t* buf, size_t len, uint32_t seed = 42) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint16_t> dist(0, 255);
    for (size_t i = 0; i < len; ++i) {
        buf[i] = static_cast<uint8_t>(dist(rng));
    }
}

eph::dpdk::TlsHotState make_roundtrip_state(uint32_t seed = 42) {
    eph::dpdk::TlsHotState state{};
    fill_random(state.write.key, eph::dpdk::tls_const::kAes256KeyLen, seed);
    fill_random(state.write.iv,  eph::dpdk::tls_const::kTls13NonceLen, seed + 1);
    std::memcpy(state.read.key, state.write.key, eph::dpdk::tls_const::kAes256KeyLen);
    std::memcpy(state.read.iv,  state.write.iv,  eph::dpdk::tls_const::kTls13NonceLen);
    state.write.seq = 0;
    state.read.seq  = 0;
    return state;
}

// Common payload sizes to parametrize
void PayloadSizeArgs(benchmark::internal::Benchmark* b) {
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
// WebSocket masking (XOR)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_WsMasking(benchmark::State& state) {
    auto sz = static_cast<size_t>(state.range(0));
    std::vector<uint8_t> data(sz, 0xAA);
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};

    for (auto _ : state) {
        eph::dpdk::ws::apply_mask(data.data(), sz, mask);
        benchmark::DoNotOptimize(data.data());
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(sz));
}
BENCHMARK(BM_WsMasking)->Apply(PayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket frame encode
// ─────────────────────────────────────────────────────────────────────────────

static void BM_WsEncode(benchmark::State& state) {
    auto sz = static_cast<size_t>(state.range(0));
    std::vector<uint8_t> payload(sz);
    fill_random(payload.data(), sz);
    std::vector<uint8_t> out(eph::dpdk::ws::kMaxFrameHeaderLen + sz);
    auto tmpl = eph::dpdk::ws::FrameTemplate::for_binary();

    for (auto _ : state) {
        auto n = tmpl.encode(out.data(), payload.data(), sz);
        benchmark::DoNotOptimize(n);
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(sz));
}
BENCHMARK(BM_WsEncode)->Apply(PayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket frame decode
// ─────────────────────────────────────────────────────────────────────────────

static void BM_WsDecode(benchmark::State& state) {
    auto sz = static_cast<size_t>(state.range(0));
    std::vector<uint8_t> payload(sz);
    fill_random(payload.data(), sz);
    std::vector<uint8_t> frame_buf(eph::dpdk::ws::kMaxFrameHeaderLen + sz);
    size_t frame_len = eph::dpdk::ws::encode_frame(
        frame_buf.data(), eph::dpdk::ws::opcode::kBinary,
        payload.data(), sz);

    for (auto _ : state) {
        auto r = eph::dpdk::ws::decode_frame(frame_buf.data(), frame_len);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_WsDecode)->Apply(PayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// TLS record encrypt (AES-256-GCM)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_TlsEncrypt(benchmark::State& state) {
    auto sz = static_cast<uint16_t>(state.range(0));
    auto hot = make_roundtrip_state();
    auto crypto = eph::dpdk::TlsRecordCrypto::create(hot);
    if (!crypto) { state.SkipWithError(crypto.error()); return; }

    // +1 byte for encrypt()'s temporary content-type append
    std::vector<uint8_t> plaintext(sz + 1);
    fill_random(plaintext.data(), sz, 10);
    uint16_t out_size = eph::dpdk::TlsRecordCrypto::encrypted_size(sz);
    std::vector<uint8_t> ciphertext(out_size);

    for (auto _ : state) {
        auto n = crypto->encrypt(plaintext.data(), sz, ciphertext.data());
        benchmark::DoNotOptimize(n);
    }
    state.SetBytesProcessed(state.iterations() * sz);
}
BENCHMARK(BM_TlsEncrypt)->Apply(PayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────
// TLS record decrypt (AES-256-GCM)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_TlsDecrypt(benchmark::State& state) {
    auto sz = static_cast<uint16_t>(state.range(0));
    auto hot = make_roundtrip_state();

    // Encrypt once to produce a valid record for decrypting
    auto enc = eph::dpdk::TlsRecordCrypto::create(hot);
    if (!enc) { state.SkipWithError(enc.error()); return; }

    std::vector<uint8_t> plaintext(sz + 1);
    fill_random(plaintext.data(), sz, 10);
    uint16_t record_size = eph::dpdk::TlsRecordCrypto::encrypted_size(sz);

    // Pre-encrypt enough records for all iterations (each uses a unique seq)
    // We re-create the decryptor to keep seq in sync, so pre-encrypt a batch.
    int64_t batch = state.max_iterations > 0 ? static_cast<int64_t>(state.max_iterations) : 10'000'000;
    // Limit batch to avoid OOM
    if (batch > 2'000'000) batch = 2'000'000;

    std::vector<std::vector<uint8_t>> records(batch, std::vector<uint8_t>(record_size));
    for (int64_t i = 0; i < batch; ++i) {
        enc->encrypt(plaintext.data(), sz, records[i].data());
    }

    // Create decryptor with matching read keys
    eph::dpdk::TlsHotState dec_hot{};
    std::memcpy(dec_hot.read.key, hot.write.key, eph::dpdk::tls_const::kAes256KeyLen);
    std::memcpy(dec_hot.read.iv,  hot.write.iv,  eph::dpdk::tls_const::kTls13NonceLen);
    auto dec = eph::dpdk::TlsRecordCrypto::create(dec_hot);
    if (!dec) { state.SkipWithError(dec.error()); return; }

    std::vector<uint8_t> decrypted(sz + 16);
    int64_t idx = 0;

    for (auto _ : state) {
        uint16_t dec_len;
        auto ok = dec->decrypt(records[idx % batch].data(), record_size,
                                decrypted.data(), dec_len);
        benchmark::DoNotOptimize(ok);
        idx++;
    }
    state.SetBytesProcessed(state.iterations() * sz);
}
BENCHMARK(BM_TlsDecrypt)->Apply(PayloadSizeArgs);

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
        // Inline parse (same logic as parse_packet, no mbuf needed)
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
// End-to-end TX pipeline (WS encode → TLS encrypt → TCP header build)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_E2E_TX(benchmark::State& state) {
    auto sz = static_cast<uint16_t>(state.range(0));
    auto hot = make_roundtrip_state();
    auto crypto = eph::dpdk::TlsRecordCrypto::create(hot);
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
    size_t ws_max = eph::dpdk::ws::kMaxFrameHeaderLen + sz + 1;
    std::vector<uint8_t> ws_buf(ws_max);
    uint16_t tls_max = eph::dpdk::TlsRecordCrypto::encrypted_size(
        static_cast<uint16_t>(ws_max));
    std::vector<uint8_t> tls_buf(tls_max);
    uint16_t total_pkt_max = eph::dpdk::net::kAllHeadersLen + tls_max;
    std::vector<uint8_t> pkt_buf(total_pkt_max);

    auto ws_tmpl = eph::dpdk::ws::FrameTemplate::for_binary();
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

    auto enc = eph::dpdk::TlsRecordCrypto::create(hot);
    if (!enc) { state.SkipWithError(enc.error()); return; }

    // Build unmasked WS frame (server → client)
    std::vector<uint8_t> payload(sz);
    fill_random(payload.data(), sz, 10);

    size_t ws_max = 2 + 8 + sz + 1; // +1 for TLS content type
    std::vector<uint8_t> ws_buf(ws_max);
    size_t ws_pos = 0;
    ws_buf[ws_pos++] = eph::dpdk::ws::kFinBit | eph::dpdk::ws::opcode::kBinary;
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
    uint16_t record_size = eph::dpdk::TlsRecordCrypto::encrypted_size(
        static_cast<uint16_t>(ws_frame_len));
    constexpr int64_t kBatch = 500'000;
    std::vector<std::vector<uint8_t>> records(kBatch, std::vector<uint8_t>(record_size));
    for (int64_t i = 0; i < kBatch; ++i) {
        enc->encrypt(ws_buf.data(), static_cast<uint16_t>(ws_frame_len),
                     records[i].data());
    }

    // Decryptor
    eph::dpdk::TlsHotState dec_hot{};
    std::memcpy(dec_hot.read.key, hot.write.key, eph::dpdk::tls_const::kAes256KeyLen);
    std::memcpy(dec_hot.read.iv,  hot.write.iv,  eph::dpdk::tls_const::kTls13NonceLen);
    auto dec = eph::dpdk::TlsRecordCrypto::create(dec_hot);
    if (!dec) { state.SkipWithError(dec.error()); return; }

    std::vector<uint8_t> decrypted(ws_frame_len + 16);
    int64_t idx = 0;

    for (auto _ : state) {
        uint16_t dec_len;
        dec->decrypt(records[idx % kBatch].data(), record_size,
                     decrypted.data(), dec_len);
        auto frame = eph::dpdk::ws::decode_frame(decrypted.data(), dec_len);
        benchmark::DoNotOptimize(frame);
        idx++;
    }
    state.SetBytesProcessed(state.iterations() * sz);
}
BENCHMARK(BM_E2E_RX)->Apply(PayloadSizeArgs);

// ─────────────────────────────────────────────────────────────────────────────

BENCHMARK_MAIN();
