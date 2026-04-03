/// @file bench_dns_codec.cpp
/// DNS codec benchmarks — QNAME encoding, query building, response parsing.
///
/// Measures the pure codec path (no NIC I/O) to quantify the CPU overhead
/// of DNS packet construction and parsing on the DPDK data plane.

#include <cstdint>
#include <cstring>
#include <vector>

#include <benchmark/benchmark.h>

#include "eph/dpdk/dns.hpp"

using namespace eph::dpdk;
using namespace eph::dpdk::dns;
using namespace eph::dpdk::dns::detail;

// ─────────────────────────────────────────────────────────────────────────────
// QNAME encoding
// ─────────────────────────────────────────────────────────────────────────────

static void BM_EncodeQname(benchmark::State& state) {
    const char* hostnames[] = {
        "example.com",
        "www.example.com",
        "api.exchange.example.com",
        "a.b.c.d.e.f.g.example.com",
    };
    constexpr size_t n = 4;
    size_t idx = 0;
    uint8_t buf[256];

    for (auto _ : state) {
        auto len = encode_qname(buf, hostnames[idx % n]);
        benchmark::DoNotOptimize(len);
        ++idx;
    }
}
BENCHMARK(BM_EncodeQname);

// ─────────────────────────────────────────────────────────────────────────────
// DNS query building (header + question section)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_BuildDnsQuery(benchmark::State& state) {
    uint8_t buf[kMaxDnsPacketLen];
    uint16_t tx_id = net::hton16(0x1234);

    for (auto _ : state) {
        auto len = build_dns_query(buf, tx_id, "stream.binance.com");
        benchmark::DoNotOptimize(len);
    }
}
BENCHMARK(BM_BuildDnsQuery);

// ─────────────────────────────────────────────────────────────────────────────
// DNS response parsing (A record extraction)
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Build a minimal DNS response with one A record for testing.
std::vector<uint8_t> build_bench_response(uint16_t tx_id_net, uint32_t ip_host) {
    // Simplified DNS response: header + question + 1 answer
    uint8_t query_buf[kMaxDnsPacketLen];
    size_t qlen = build_dns_query(query_buf, tx_id_net, "stream.binance.com");

    std::vector<uint8_t> resp(qlen + 16); // room for answer RR
    std::memcpy(resp.data(), query_buf, qlen);

    // Fix header: set QR, set an_count=1
    auto* hdr = reinterpret_cast<DnsHeader*>(resp.data());
    hdr->flags = net::hton16(net::ntoh16(hdr->flags) | kDnsFlagQr);
    hdr->an_count = net::hton16(1);

    // Answer RR: pointer to QNAME + TYPE_A + CLASS_IN + TTL + RDLENGTH=4 + IP
    size_t pos = qlen;
    resp[pos++] = 0xC0; resp[pos++] = 0x0C; // Pointer to QNAME at offset 12
    uint16_t rr_type = net::hton16(kDnsTypeA);
    uint16_t rr_class = net::hton16(kDnsClassIn);
    uint32_t ttl = net::hton32(300);
    uint16_t rdlen = net::hton16(4);
    uint32_t ip_net = net::hton32(ip_host);

    std::memcpy(&resp[pos], &rr_type, 2); pos += 2;
    std::memcpy(&resp[pos], &rr_class, 2); pos += 2;
    std::memcpy(&resp[pos], &ttl, 4); pos += 4;
    std::memcpy(&resp[pos], &rdlen, 2); pos += 2;
    std::memcpy(&resp[pos], &ip_net, 4); pos += 4;

    resp.resize(pos);
    return resp;
}

} // anonymous namespace

static void BM_ParseDnsResponse(benchmark::State& state) {
    uint16_t tx_id_net = net::hton16(0x1234);
    auto resp = build_bench_response(tx_id_net, 0x0A000001);

    for (auto _ : state) {
        auto result = parse_dns_response(resp.data(), resp.size(), tx_id_net);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_ParseDnsResponse);

// ─────────────────────────────────────────────────────────────────────────────
// skip_dns_name — pointer following in response parsing
// ─────────────────────────────────────────────────────────────────────────────

static void BM_SkipDnsName(benchmark::State& state) {
    // Build a DNS message with an inline name (no pointers)
    uint8_t buf[256];
    size_t pos = 0;
    // "stream.binance.com" as QNAME
    buf[pos++] = 6; std::memcpy(&buf[pos], "stream", 6); pos += 6;
    buf[pos++] = 7; std::memcpy(&buf[pos], "binance", 7); pos += 7;
    buf[pos++] = 3; std::memcpy(&buf[pos], "com", 3); pos += 3;
    buf[pos++] = 0; // root

    for (auto _ : state) {
        auto end = skip_dns_name(buf, 0, pos);
        benchmark::DoNotOptimize(end);
    }
}
BENCHMARK(BM_SkipDnsName);

BENCHMARK_MAIN();
