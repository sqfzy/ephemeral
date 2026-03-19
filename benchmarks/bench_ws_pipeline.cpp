/// @file bench_ws_pipeline.cpp
/// Benchmark for the DPDK WebSocket pipeline — measures per-stage latency.
///
/// Stages measured (in hot-path order):
///   1. WebSocket frame encoding (binary, with client masking)
///   2. TLS record encryption (AES-256-GCM via EVP API)
///   3. TLS record decryption (AES-256-GCM)
///   4. WebSocket frame decoding
///   5. TCP/IP header construction (PacketTemplate::fill_packet on mbuf-like buffer)
///   6. TCP/IP header parsing
///   7. Internet checksum
///   8. End-to-end TX pipeline: WS encode → TLS encrypt → TCP header build
///   9. End-to-end RX pipeline: TCP parse → TLS decrypt → WS decode
///
/// Uses eph::utils::Recorder for HdrHistogram-based latency distribution.
/// Results are printed to stdout and exported as JSON+CSV to outputs/.

#include <cstdint>
#include <cstring>
#include <print>
#include <random>
#include <vector>

#include "eph/utils/record.hpp"
#include "eph/utils/time.hpp"

#include "eph/dpdk/net_header.hpp"
#include "eph/dpdk/tls_record.hpp"
#include "eph/dpdk/websocket.hpp"

using eph::utils::Recorder;
using eph::utils::SystemStats;
using eph::utils::TSC;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Fill a buffer with deterministic pseudo-random bytes.
void fill_random(uint8_t* buf, size_t len, uint32_t seed = 42) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint16_t> dist(0, 255);
    for (size_t i = 0; i < len; ++i) {
        buf[i] = static_cast<uint8_t>(dist(rng));
    }
}

/// Prevent compiler from optimizing away a buffer.
void do_not_optimize(const void* p, size_t len) {
    const volatile auto* vp = static_cast<const volatile uint8_t*>(p);
    auto v = vp[0];
    if (len > 1) v = vp[len - 1];
    (void)v;
}

// Output directory: EPH_PROJECT_ROOT is injected by xmake as an absolute path.
#ifndef EPH_PROJECT_ROOT
#define EPH_PROJECT_ROOT "."
#endif
inline std::string get_output_dir() {
    return std::string(EPH_PROJECT_ROOT) + "/outputs";
}
#define kOutputDir get_output_dir()

// Number of iterations per benchmark
constexpr int kIterations = 100'000;

// Warmup iterations (not recorded)
constexpr int kWarmupIterations = 1'000;

// Payload sizes to benchmark
constexpr uint16_t kPayloadSizes[] = {64, 128, 256, 512, 1024};

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Stage 1: WebSocket frame encode
// ─────────────────────────────────────────────────────────────────────────────

void bench_ws_encode(uint16_t payload_size) {
    std::string name = std::format("WS_Encode_{}B", payload_size);
    Recorder rec(name);

    std::vector<uint8_t> payload(payload_size);
    fill_random(payload.data(), payload_size);

    // Output buffer: max WS frame header (14) + payload
    std::vector<uint8_t> out(eph::dpdk::ws::kMaxFrameHeaderLen + payload_size);

    auto tmpl = eph::dpdk::ws::FrameTemplate::for_binary();

    // Warmup
    for (int i = 0; i < kWarmupIterations; ++i) {
        size_t n = tmpl.encode(out.data(), payload.data(), payload_size);
        do_not_optimize(out.data(), n);
    }

    // Measured
    for (int i = 0; i < kIterations; ++i) {
        uint64_t start = TSC::now();
        size_t n = tmpl.encode(out.data(), payload.data(), payload_size);
        uint64_t end = TSC::now();
        do_not_optimize(out.data(), n);
        rec.record(end - start);
    }

    rec.print_report();
    rec.export_all(kOutputDir);
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 2: WebSocket frame decode
// ─────────────────────────────────────────────────────────────────────────────

void bench_ws_decode(uint16_t payload_size) {
    std::string name = std::format("WS_Decode_{}B", payload_size);
    Recorder rec(name);

    // Prepare an encoded frame to decode
    std::vector<uint8_t> payload(payload_size);
    fill_random(payload.data(), payload_size);

    std::vector<uint8_t> frame_buf(eph::dpdk::ws::kMaxFrameHeaderLen + payload_size);
    size_t frame_len = eph::dpdk::ws::encode_frame(
        frame_buf.data(), eph::dpdk::ws::opcode::kBinary,
        payload.data(), payload_size);

    // Warmup
    for (int i = 0; i < kWarmupIterations; ++i) {
        auto r = eph::dpdk::ws::decode_frame(frame_buf.data(), frame_len);
        do_not_optimize(&r, sizeof(r));
    }

    // Measured
    for (int i = 0; i < kIterations; ++i) {
        uint64_t start = TSC::now();
        auto r = eph::dpdk::ws::decode_frame(frame_buf.data(), frame_len);
        uint64_t end = TSC::now();
        do_not_optimize(&r, sizeof(r));
        rec.record(end - start);
    }

    rec.print_report();
    rec.export_all(kOutputDir);
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 3: TLS record encrypt (AES-256-GCM)
// ─────────────────────────────────────────────────────────────────────────────

void bench_tls_encrypt(uint16_t payload_size) {
    std::string name = std::format("TLS_Encrypt_{}B", payload_size);
    Recorder rec(name);

    // Create a TlsHotState with random keys
    eph::dpdk::TlsHotState hot{};
    fill_random(hot.write.key, eph::dpdk::tls_const::kAes256KeyLen, 1);
    fill_random(hot.write.iv, eph::dpdk::tls_const::kTls13NonceLen, 2);
    fill_random(hot.read.key, eph::dpdk::tls_const::kAes256KeyLen, 3);
    fill_random(hot.read.iv, eph::dpdk::tls_const::kTls13NonceLen, 4);
    hot.write.seq = 0;
    hot.read.seq = 0;

    auto crypto_result = eph::dpdk::TlsRecordCrypto::create(hot);
    if (!crypto_result) {
        std::println(stderr, "Failed to create TlsRecordCrypto: {}",
                     crypto_result.error());
        return;
    }
    auto& crypto = *crypto_result;

    // +1 byte: encrypt() temporarily appends content type past payload_size
    std::vector<uint8_t> plaintext(payload_size + 1);
    fill_random(plaintext.data(), payload_size, 10);

    uint16_t out_size = eph::dpdk::TlsRecordCrypto::encrypted_size(payload_size);
    std::vector<uint8_t> ciphertext(out_size);

    // Warmup
    for (int i = 0; i < kWarmupIterations; ++i) {
        uint16_t n = crypto.encrypt(plaintext.data(), payload_size,
                                     ciphertext.data());
        do_not_optimize(ciphertext.data(), n);
    }

    // Measured
    for (int i = 0; i < kIterations; ++i) {
        uint64_t start = TSC::now();
        uint16_t n = crypto.encrypt(plaintext.data(), payload_size,
                                     ciphertext.data());
        uint64_t end = TSC::now();
        do_not_optimize(ciphertext.data(), n);
        rec.record(end - start);
    }

    rec.print_report();
    rec.export_all(kOutputDir);
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 4: TLS record decrypt (AES-256-GCM)
// ─────────────────────────────────────────────────────────────────────────────

void bench_tls_decrypt(uint16_t payload_size) {
    std::string name = std::format("TLS_Decrypt_{}B", payload_size);
    Recorder rec(name);

    // Create crypto context
    eph::dpdk::TlsHotState hot{};
    fill_random(hot.write.key, eph::dpdk::tls_const::kAes256KeyLen, 1);
    fill_random(hot.write.iv, eph::dpdk::tls_const::kTls13NonceLen, 2);
    // For decrypt test: read key/iv must match write key/iv
    // (simulating decrypting what we just encrypted)
    std::memcpy(hot.read.key, hot.write.key, eph::dpdk::tls_const::kAes256KeyLen);
    std::memcpy(hot.read.iv, hot.write.iv, eph::dpdk::tls_const::kTls13NonceLen);
    hot.write.seq = 0;
    hot.read.seq = 0;

    auto enc_result = eph::dpdk::TlsRecordCrypto::create(hot);
    if (!enc_result) {
        std::println(stderr, "Failed to create encryptor: {}", enc_result.error());
        return;
    }

    // We need a separate decryptor to keep sequence numbers in sync
    // Reset sequences: enc uses write_seq, dec uses read_seq
    eph::dpdk::TlsHotState dec_hot{};
    std::memcpy(dec_hot.read.key, hot.write.key, eph::dpdk::tls_const::kAes256KeyLen);
    std::memcpy(dec_hot.read.iv, hot.write.iv, eph::dpdk::tls_const::kTls13NonceLen);

    // Pre-encrypt all records so decrypt benchmark is pure
    std::vector<uint8_t> plaintext(payload_size);
    fill_random(plaintext.data(), payload_size, 10);

    uint16_t record_size = eph::dpdk::TlsRecordCrypto::encrypted_size(payload_size);

    // Encrypt one record per iteration + warmup
    int total = kIterations + kWarmupIterations;
    std::vector<std::vector<uint8_t>> records(total, std::vector<uint8_t>(record_size));

    for (int i = 0; i < total; ++i) {
        uint16_t n = enc_result->encrypt(plaintext.data(), payload_size,
                                          records[i].data());
        if (n == 0) {
            std::println(stderr, "Encrypt failed at iteration {}", i);
            return;
        }
    }

    // Create fresh decryptor
    auto dec_result = eph::dpdk::TlsRecordCrypto::create(dec_hot);
    if (!dec_result) {
        std::println(stderr, "Failed to create decryptor: {}", dec_result.error());
        return;
    }
    auto& dec = *dec_result;

    std::vector<uint8_t> decrypted(payload_size + 16);

    // Warmup
    for (int i = 0; i < kWarmupIterations; ++i) {
        uint16_t dec_len;
        bool ok = dec.decrypt(records[i].data(), record_size,
                               decrypted.data(), dec_len);
        do_not_optimize(decrypted.data(), dec_len);
        (void)ok;
    }

    // Measured
    for (int i = 0; i < kIterations; ++i) {
        int idx = kWarmupIterations + i;
        uint16_t dec_len;
        uint64_t start = TSC::now();
        bool ok = dec.decrypt(records[idx].data(), record_size,
                               decrypted.data(), dec_len);
        uint64_t end = TSC::now();
        do_not_optimize(decrypted.data(), dec_len);
        (void)ok;
        rec.record(end - start);
    }

    rec.print_report();
    rec.export_all(kOutputDir);
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 5: TCP/IP header build (PacketTemplate)
// ─────────────────────────────────────────────────────────────────────────────

void bench_tcp_header_build(uint16_t payload_size) {
    std::string name = std::format("TCP_HeaderBuild_{}B", payload_size);
    Recorder rec(name);

    eph::dpdk::net::PacketTemplate tmpl{};
    tmpl.src_mac = {{0x00, 0x11, 0x22, 0x33, 0x44, 0x55}};
    tmpl.dst_mac = {{0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB}};
    tmpl.tuple = {
        .src_ip   = eph::dpdk::net::parse_ipv4("10.0.0.1"),
        .dst_ip   = eph::dpdk::net::parse_ipv4("10.0.0.2"),
        .src_port = 12345,
        .dst_port = 443,
    };

    std::vector<uint8_t> payload(payload_size);
    fill_random(payload.data(), payload_size, 50);

    // Simulate mbuf with a raw buffer (no actual DPDK needed)
    uint16_t total_pkt_len = eph::dpdk::net::kAllHeadersLen + payload_size;
    std::vector<uint8_t> pkt_buf(total_pkt_len);

    uint32_t seq = 1000;
    uint32_t ack = 2000;

    // Warmup — use direct header construction (no mbuf alloc)
    for (int i = 0; i < kWarmupIterations; ++i) {
        // Build headers manually to benchmark just header construction
        auto* pkt = pkt_buf.data();

        // Ethernet
        auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt);
        rte_ether_addr_copy(&tmpl.dst_mac, &eth->dst_addr);
        rte_ether_addr_copy(&tmpl.src_mac, &eth->src_addr);
        eth->ether_type = eph::dpdk::net::hton16(eph::dpdk::net::kEtherTypeIpv4);

        // IPv4
        auto* ip = reinterpret_cast<rte_ipv4_hdr*>(pkt + eph::dpdk::net::kEtherHeaderLen);
        ip->version_ihl = 0x45;
        ip->total_length = eph::dpdk::net::hton16(
            eph::dpdk::net::kIpv4HeaderLen + eph::dpdk::net::kTcpHeaderLen + payload_size);
        ip->src_addr = eph::dpdk::net::hton32(tmpl.tuple.src_ip);
        ip->dst_addr = eph::dpdk::net::hton32(tmpl.tuple.dst_ip);
        ip->next_proto_id = eph::dpdk::net::kIpProtoTcp;
        ip->time_to_live = 64;
        ip->hdr_checksum = 0;
        ip->hdr_checksum = eph::dpdk::net::internet_checksum(
            ip, eph::dpdk::net::kIpv4HeaderLen);

        // TCP
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
        std::memcpy(pkt + eph::dpdk::net::kAllHeadersLen, payload.data(), payload_size);
        uint16_t tcp_total = eph::dpdk::net::kTcpHeaderLen + payload_size;
        tcp->cksum = eph::dpdk::net::tcp_checksum(
            ip->src_addr, ip->dst_addr, tcp, tcp_total);

        do_not_optimize(pkt_buf.data(), total_pkt_len);
    }

    // Measured
    for (int i = 0; i < kIterations; ++i) {
        auto* pkt = pkt_buf.data();

        uint64_t start = TSC::now();

        auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt);
        rte_ether_addr_copy(&tmpl.dst_mac, &eth->dst_addr);
        rte_ether_addr_copy(&tmpl.src_mac, &eth->src_addr);
        eth->ether_type = eph::dpdk::net::hton16(eph::dpdk::net::kEtherTypeIpv4);

        auto* ip = reinterpret_cast<rte_ipv4_hdr*>(pkt + eph::dpdk::net::kEtherHeaderLen);
        ip->version_ihl = 0x45;
        ip->type_of_service = 0;
        ip->total_length = eph::dpdk::net::hton16(
            eph::dpdk::net::kIpv4HeaderLen + eph::dpdk::net::kTcpHeaderLen + payload_size);
        ip->packet_id = eph::dpdk::net::hton16(static_cast<uint16_t>(i));
        ip->fragment_offset = eph::dpdk::net::hton16(0x4000);
        ip->time_to_live = 64;
        ip->next_proto_id = eph::dpdk::net::kIpProtoTcp;
        ip->hdr_checksum = 0;
        ip->src_addr = eph::dpdk::net::hton32(tmpl.tuple.src_ip);
        ip->dst_addr = eph::dpdk::net::hton32(tmpl.tuple.dst_ip);
        ip->hdr_checksum = eph::dpdk::net::internet_checksum(
            ip, eph::dpdk::net::kIpv4HeaderLen);

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
        std::memcpy(pkt + eph::dpdk::net::kAllHeadersLen, payload.data(), payload_size);
        uint16_t tcp_total = eph::dpdk::net::kTcpHeaderLen + payload_size;
        tcp->cksum = eph::dpdk::net::tcp_checksum(
            ip->src_addr, ip->dst_addr, tcp, tcp_total);

        uint64_t end = TSC::now();
        do_not_optimize(pkt_buf.data(), total_pkt_len);
        rec.record(end - start);
    }

    rec.print_report();
    rec.export_all(kOutputDir);
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 6: TCP/IP header parse
// ─────────────────────────────────────────────────────────────────────────────

void bench_tcp_header_parse(uint16_t payload_size) {
    std::string name = std::format("TCP_HeaderParse_{}B", payload_size);
    Recorder rec(name);

    // Build a valid packet to parse
    uint16_t total_pkt_len = eph::dpdk::net::kAllHeadersLen + payload_size;
    std::vector<uint8_t> pkt_buf(total_pkt_len, 0);

    auto* pkt = pkt_buf.data();
    auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt);
    eth->ether_type = eph::dpdk::net::hton16(eph::dpdk::net::kEtherTypeIpv4);

    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(pkt + eph::dpdk::net::kEtherHeaderLen);
    ip->version_ihl = 0x45;
    ip->total_length = eph::dpdk::net::hton16(
        eph::dpdk::net::kIpv4HeaderLen + eph::dpdk::net::kTcpHeaderLen + payload_size);
    ip->next_proto_id = eph::dpdk::net::kIpProtoTcp;
    ip->src_addr = eph::dpdk::net::hton32(eph::dpdk::net::parse_ipv4("10.0.0.2"));
    ip->dst_addr = eph::dpdk::net::hton32(eph::dpdk::net::parse_ipv4("10.0.0.1"));

    auto* tcp = reinterpret_cast<rte_tcp_hdr*>(
        pkt + eph::dpdk::net::kEtherHeaderLen + eph::dpdk::net::kIpv4HeaderLen);
    tcp->src_port = eph::dpdk::net::hton16(443);
    tcp->dst_port = eph::dpdk::net::hton16(12345);
    tcp->sent_seq = eph::dpdk::net::hton32(5000);
    tcp->recv_ack = eph::dpdk::net::hton32(1001);
    tcp->data_off = (eph::dpdk::net::kTcpHeaderLen / 4) << 4;
    tcp->tcp_flags = eph::dpdk::net::kTcpAck | eph::dpdk::net::kTcpPsh;
    tcp->rx_win = eph::dpdk::net::hton16(65535);

    // Simulate an mbuf via a simple struct matching parse_packet expectations
    // parse_packet uses rte_pktmbuf_data_len and rte_pktmbuf_mtod — we need
    // an actual mbuf or a mock. Since we don't have DPDK runtime, manually
    // parse the raw buffer using the header structures.

    // Warmup
    for (int i = 0; i < kWarmupIterations; ++i) {
        // Manual parse (same logic as parse_packet but on raw buffer)
        auto* e = reinterpret_cast<const rte_ether_hdr*>(pkt_buf.data());
        auto* ipv4 = reinterpret_cast<const rte_ipv4_hdr*>(
            pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen);
        auto* t = reinterpret_cast<const rte_tcp_hdr*>(
            pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen +
            eph::dpdk::net::kIpv4HeaderLen);
        uint32_t s = eph::dpdk::net::ntoh32(t->sent_seq);
        do_not_optimize(&s, sizeof(s));
        (void)e;
        (void)ipv4;
    }

    // Measured
    for (int i = 0; i < kIterations; ++i) {
        uint64_t start = TSC::now();

        auto* e = reinterpret_cast<const rte_ether_hdr*>(pkt_buf.data());
        uint16_t etype = eph::dpdk::net::ntoh16(e->ether_type);

        const rte_ipv4_hdr* ipv4 = nullptr;
        const rte_tcp_hdr* t = nullptr;
        const uint8_t* pl = nullptr;
        uint16_t pl_len = 0;

        if (etype == eph::dpdk::net::kEtherTypeIpv4) {
            ipv4 = reinterpret_cast<const rte_ipv4_hdr*>(
                pkt_buf.data() + eph::dpdk::net::kEtherHeaderLen);
            uint8_t ihl = (ipv4->version_ihl & 0x0F) * 4;

            if (ipv4->next_proto_id == eph::dpdk::net::kIpProtoTcp) {
                uint16_t tcp_off = eph::dpdk::net::kEtherHeaderLen + ihl;
                t = reinterpret_cast<const rte_tcp_hdr*>(pkt_buf.data() + tcp_off);
                uint8_t doff = (t->data_off >> 4) * 4;
                uint16_t payload_off = tcp_off + doff;
                if (total_pkt_len > payload_off) {
                    pl = pkt_buf.data() + payload_off;
                    pl_len = total_pkt_len - payload_off;
                }
            }
        }

        uint64_t end = TSC::now();

        do_not_optimize(&pl_len, sizeof(pl_len));
        (void)pl;
        rec.record(end - start);
    }

    rec.print_report();
    rec.export_all(kOutputDir);
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 7: Internet checksum
// ─────────────────────────────────────────────────────────────────────────────

void bench_checksum(uint16_t data_size) {
    std::string name = std::format("Checksum_{}B", data_size);
    Recorder rec(name);

    std::vector<uint8_t> data(data_size);
    fill_random(data.data(), data_size, 99);

    // Warmup
    for (int i = 0; i < kWarmupIterations; ++i) {
        uint16_t c = eph::dpdk::net::internet_checksum(data.data(), data_size);
        do_not_optimize(&c, sizeof(c));
    }

    // Measured
    for (int i = 0; i < kIterations; ++i) {
        uint64_t start = TSC::now();
        uint16_t c = eph::dpdk::net::internet_checksum(data.data(), data_size);
        uint64_t end = TSC::now();
        do_not_optimize(&c, sizeof(c));
        rec.record(end - start);
    }

    rec.print_report();
    rec.export_all(kOutputDir);
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 8: End-to-end TX pipeline (WS encode → TLS encrypt → TCP header)
// ─────────────────────────────────────────────────────────────────────────────

void bench_e2e_tx(uint16_t payload_size) {
    std::string name = std::format("E2E_TX_{}B", payload_size);
    Recorder rec(name);

    // Setup TLS crypto
    eph::dpdk::TlsHotState hot{};
    fill_random(hot.write.key, eph::dpdk::tls_const::kAes256KeyLen, 1);
    fill_random(hot.write.iv, eph::dpdk::tls_const::kTls13NonceLen, 2);
    hot.write.seq = 0;

    auto crypto_result = eph::dpdk::TlsRecordCrypto::create(hot);
    if (!crypto_result) {
        std::println(stderr, "Failed to create TlsRecordCrypto: {}",
                     crypto_result.error());
        return;
    }
    auto& crypto = *crypto_result;

    // Setup packet template
    eph::dpdk::net::PacketTemplate pkt_tmpl{};
    pkt_tmpl.src_mac = {{0x00, 0x11, 0x22, 0x33, 0x44, 0x55}};
    pkt_tmpl.dst_mac = {{0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB}};
    pkt_tmpl.tuple = {
        .src_ip   = eph::dpdk::net::parse_ipv4("10.0.0.1"),
        .dst_ip   = eph::dpdk::net::parse_ipv4("10.0.0.2"),
        .src_port = 12345,
        .dst_port = 443,
    };

    std::vector<uint8_t> payload(payload_size);
    fill_random(payload.data(), payload_size, 10);

    // Workspace buffers
    size_t ws_max = eph::dpdk::ws::kMaxFrameHeaderLen + payload_size;
    std::vector<uint8_t> ws_buf(ws_max);

    uint16_t tls_max = eph::dpdk::TlsRecordCrypto::encrypted_size(
        static_cast<uint16_t>(ws_max));
    std::vector<uint8_t> tls_buf(tls_max);

    uint16_t total_pkt_max = eph::dpdk::net::kAllHeadersLen + tls_max;
    std::vector<uint8_t> pkt_buf(total_pkt_max);

    auto ws_tmpl = eph::dpdk::ws::FrameTemplate::for_binary();
    uint32_t seq = 1000;
    uint32_t ack = 2000;

    // Warmup
    for (int i = 0; i < kWarmupIterations; ++i) {
        size_t ws_len = ws_tmpl.encode(ws_buf.data(), payload.data(), payload_size);
        uint16_t tls_len = crypto.encrypt(
            ws_buf.data(), static_cast<uint16_t>(ws_len), tls_buf.data());
        // Build TCP/IP headers around TLS record
        auto* pkt = pkt_buf.data();
        auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt);
        rte_ether_addr_copy(&pkt_tmpl.dst_mac, &eth->dst_addr);
        rte_ether_addr_copy(&pkt_tmpl.src_mac, &eth->src_addr);
        eth->ether_type = eph::dpdk::net::hton16(eph::dpdk::net::kEtherTypeIpv4);
        auto* ip = reinterpret_cast<rte_ipv4_hdr*>(pkt + eph::dpdk::net::kEtherHeaderLen);
        ip->version_ihl = 0x45;
        ip->total_length = eph::dpdk::net::hton16(
            eph::dpdk::net::kIpv4HeaderLen + eph::dpdk::net::kTcpHeaderLen + tls_len);
        ip->next_proto_id = eph::dpdk::net::kIpProtoTcp;
        ip->time_to_live = 64;
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
        tcp->cksum = 0;
        std::memcpy(pkt + eph::dpdk::net::kAllHeadersLen, tls_buf.data(), tls_len);
        uint16_t tcp_total = eph::dpdk::net::kTcpHeaderLen + tls_len;
        tcp->cksum = eph::dpdk::net::tcp_checksum(ip->src_addr, ip->dst_addr, tcp, tcp_total);

        do_not_optimize(pkt_buf.data(), eph::dpdk::net::kAllHeadersLen + tls_len);
    }

    // Measured
    for (int i = 0; i < kIterations; ++i) {
        uint64_t start = TSC::now();

        // 1. WebSocket frame encode
        size_t ws_len = ws_tmpl.encode(ws_buf.data(), payload.data(), payload_size);

        // 2. TLS encrypt
        uint16_t tls_len = crypto.encrypt(
            ws_buf.data(), static_cast<uint16_t>(ws_len), tls_buf.data());

        // 3. TCP/IP header construction
        auto* pkt = pkt_buf.data();
        auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt);
        rte_ether_addr_copy(&pkt_tmpl.dst_mac, &eth->dst_addr);
        rte_ether_addr_copy(&pkt_tmpl.src_mac, &eth->src_addr);
        eth->ether_type = eph::dpdk::net::hton16(eph::dpdk::net::kEtherTypeIpv4);

        auto* ip = reinterpret_cast<rte_ipv4_hdr*>(pkt + eph::dpdk::net::kEtherHeaderLen);
        ip->version_ihl = 0x45;
        ip->type_of_service = 0;
        ip->total_length = eph::dpdk::net::hton16(
            eph::dpdk::net::kIpv4HeaderLen + eph::dpdk::net::kTcpHeaderLen + tls_len);
        ip->packet_id = eph::dpdk::net::hton16(static_cast<uint16_t>(i));
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
        tcp->cksum = 0;
        tcp->tcp_urp = 0;
        std::memcpy(pkt + eph::dpdk::net::kAllHeadersLen, tls_buf.data(), tls_len);
        uint16_t tcp_total = eph::dpdk::net::kTcpHeaderLen + tls_len;
        tcp->cksum = eph::dpdk::net::tcp_checksum(ip->src_addr, ip->dst_addr, tcp, tcp_total);

        uint64_t end = TSC::now();
        do_not_optimize(pkt_buf.data(), eph::dpdk::net::kAllHeadersLen + tls_len);
        rec.record(end - start);
    }

    rec.print_report();
    rec.export_all(kOutputDir);
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 9: End-to-end RX pipeline (TCP parse → TLS decrypt → WS decode)
// ─────────────────────────────────────────────────────────────────────────────

void bench_e2e_rx(uint16_t payload_size) {
    std::string name = std::format("E2E_RX_{}B", payload_size);
    Recorder rec(name);

    // Setup: build encrypted WS frames for decrypting
    eph::dpdk::TlsHotState enc_hot{};
    fill_random(enc_hot.write.key, eph::dpdk::tls_const::kAes256KeyLen, 1);
    fill_random(enc_hot.write.iv, eph::dpdk::tls_const::kTls13NonceLen, 2);
    enc_hot.write.seq = 0;

    auto enc_result = eph::dpdk::TlsRecordCrypto::create(enc_hot);
    if (!enc_result) {
        std::println(stderr, "Failed to create encryptor: {}", enc_result.error());
        return;
    }
    auto& enc = *enc_result;

    // Create matching decryptor
    eph::dpdk::TlsHotState dec_hot{};
    std::memcpy(dec_hot.read.key, enc_hot.write.key, eph::dpdk::tls_const::kAes256KeyLen);
    std::memcpy(dec_hot.read.iv, enc_hot.write.iv, eph::dpdk::tls_const::kTls13NonceLen);
    dec_hot.read.seq = 0;

    // Prepare payload
    std::vector<uint8_t> payload(payload_size);
    fill_random(payload.data(), payload_size, 10);

    // Build WS frame (as server would send: unmasked binary frame)
    // For RX benchmark, server frames are NOT masked
    size_t ws_max = 2 + 8 + payload_size; // No mask for server frames
    std::vector<uint8_t> ws_buf(ws_max);
    // Build unmasked frame: FIN + binary, payload length, payload
    size_t ws_pos = 0;
    ws_buf[ws_pos++] = eph::dpdk::ws::kFinBit | eph::dpdk::ws::opcode::kBinary;
    if (payload_size < 126) {
        ws_buf[ws_pos++] = static_cast<uint8_t>(payload_size); // No mask bit
    } else if (payload_size <= 65535) {
        ws_buf[ws_pos++] = 126;
        ws_buf[ws_pos++] = static_cast<uint8_t>(payload_size >> 8);
        ws_buf[ws_pos++] = static_cast<uint8_t>(payload_size & 0xFF);
    }
    std::memcpy(ws_buf.data() + ws_pos, payload.data(), payload_size);
    size_t ws_frame_len = ws_pos + payload_size;

    // Encrypt each WS frame into TLS records
    int total = kIterations + kWarmupIterations;
    uint16_t tls_record_size = eph::dpdk::TlsRecordCrypto::encrypted_size(
        static_cast<uint16_t>(ws_frame_len));
    std::vector<std::vector<uint8_t>> tls_records(
        total, std::vector<uint8_t>(tls_record_size));

    for (int i = 0; i < total; ++i) {
        uint16_t n = enc.encrypt(ws_buf.data(),
                                  static_cast<uint16_t>(ws_frame_len),
                                  tls_records[i].data());
        if (n == 0) {
            std::println(stderr, "Encrypt failed at {}", i);
            return;
        }
    }

    auto dec_result = eph::dpdk::TlsRecordCrypto::create(dec_hot);
    if (!dec_result) {
        std::println(stderr, "Failed to create decryptor: {}", dec_result.error());
        return;
    }
    auto& dec = *dec_result;

    std::vector<uint8_t> decrypted(ws_frame_len + 16);

    // Warmup
    for (int i = 0; i < kWarmupIterations; ++i) {
        uint16_t dec_len;
        dec.decrypt(tls_records[i].data(), tls_record_size,
                    decrypted.data(), dec_len);
        auto frame = eph::dpdk::ws::decode_frame(decrypted.data(), dec_len);
        do_not_optimize(&frame, sizeof(frame));
    }

    // Measured
    for (int i = 0; i < kIterations; ++i) {
        int idx = kWarmupIterations + i;

        uint64_t start = TSC::now();

        // 1. TLS decrypt
        uint16_t dec_len;
        dec.decrypt(tls_records[idx].data(), tls_record_size,
                    decrypted.data(), dec_len);

        // 2. WS decode
        auto frame = eph::dpdk::ws::decode_frame(decrypted.data(), dec_len);

        uint64_t end = TSC::now();

        do_not_optimize(&frame, sizeof(frame));
        rec.record(end - start);
    }

    rec.print_report();
    rec.export_all(kOutputDir);
}

// ─────────────────────────────────────────────────────────────────────────────
// WS masking benchmark (XOR performance)
// ─────────────────────────────────────────────────────────────────────────────

void bench_ws_masking(uint16_t payload_size) {
    std::string name = std::format("WS_Masking_{}B", payload_size);
    Recorder rec(name);

    std::vector<uint8_t> data(payload_size);
    fill_random(data.data(), payload_size, 77);

    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};

    // Warmup
    for (int i = 0; i < kWarmupIterations; ++i) {
        eph::dpdk::ws::apply_mask(data.data(), payload_size, mask);
        do_not_optimize(data.data(), payload_size);
    }

    // Measured
    for (int i = 0; i < kIterations; ++i) {
        uint64_t start = TSC::now();
        eph::dpdk::ws::apply_mask(data.data(), payload_size, mask);
        uint64_t end = TSC::now();
        do_not_optimize(data.data(), payload_size);
        rec.record(end - start);
    }

    rec.print_report();
    rec.export_all(kOutputDir);
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    // TSC calibration
    if (!TSC::init()) {
        std::println(stderr, "TSC initialization failed");
        return 1;
    }

    SystemStats sys_stats;

    std::println("\n{:=^80}", " DPDK WebSocket Pipeline Benchmark ");
    std::println("  Iterations: {}", kIterations);
    std::println("  Warmup:     {}", kWarmupIterations);
    std::println("  Output dir: {}", kOutputDir);
    std::println("{:=^80}\n", "");

    // ── Per-stage benchmarks ──

    std::println("{:─^80}", " Stage: Internet Checksum ");
    for (uint16_t sz : kPayloadSizes) bench_checksum(sz);

    std::println("{:─^80}", " Stage: WebSocket Masking (XOR) ");
    for (uint16_t sz : kPayloadSizes) bench_ws_masking(sz);

    std::println("{:─^80}", " Stage: WebSocket Frame Encode ");
    for (uint16_t sz : kPayloadSizes) bench_ws_encode(sz);

    std::println("{:─^80}", " Stage: WebSocket Frame Decode ");
    for (uint16_t sz : kPayloadSizes) bench_ws_decode(sz);

    std::println("{:─^80}", " Stage: TLS Record Encrypt (AES-256-GCM) ");
    for (uint16_t sz : kPayloadSizes) bench_tls_encrypt(sz);

    std::println("{:─^80}", " Stage: TLS Record Decrypt (AES-256-GCM) ");
    for (uint16_t sz : kPayloadSizes) bench_tls_decrypt(sz);

    std::println("{:─^80}", " Stage: TCP/IP Header Build ");
    for (uint16_t sz : kPayloadSizes) bench_tcp_header_build(sz);

    std::println("{:─^80}", " Stage: TCP/IP Header Parse ");
    for (uint16_t sz : kPayloadSizes) bench_tcp_header_parse(sz);

    // ── End-to-end pipeline ──

    std::println("{:─^80}", " End-to-End: TX Pipeline (WS+TLS+TCP) ");
    for (uint16_t sz : kPayloadSizes) bench_e2e_tx(sz);

    std::println("{:─^80}", " End-to-End: RX Pipeline (TLS+WS) ");
    for (uint16_t sz : kPayloadSizes) bench_e2e_rx(sz);

    // ── System resource report ──
    sys_stats.print_report();

    std::println("\n{:=^80}", " Benchmark Complete ");
    std::println("  Results exported to: {}/", kOutputDir);

    return 0;
}
