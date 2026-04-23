// Fuzz harness for ICMP parser (eph::dpdk::net::parse_icmp).
//
// See fuzzers/README.md for the full workflow (build / seed corpus / run).
//
// Why ICMP? The parser is invoked on remote-originated Type 3 Code 4
// (Fragmentation Needed) messages via `Platform::on_icmp` /
// `TcpSession::on_icmp_frag_needed` — any router along the path can
// inject one, so the input is genuinely adversarial. parse_icmp also
// walks an embedded IP+L4 header at a caller-trusted offset, making
// off-by-one / truncation edge cases worth fuzzing.
//
// Build differs from fuzz_arp_reply in one way: parse_icmp lives in
// packet_parse.hpp which pulls in real DPDK mbuf / ether / ip types.
// We therefore build against system libdpdk headers rather than
// shimming them. The fuzzer still never calls rte_eal_init — only
// struct definitions and inline accessors are exercised.
//
// Quick build (clang ≥ 17, libFuzzer available, system libdpdk installed):
//   clang++ -fsanitize=fuzzer,address,undefined -std=c++23 -O1 -g \
//       -Ieph-net-dpdk/include -Ieph-core/include -Ieph-utils/include \
//       $(pkg-config --cflags libdpdk) \
//       -lspdlog \
//       eph-net-dpdk/fuzzers/fuzz_icmp_reply.cpp -o fuzz_icmp_reply
//
// Run:
//   mkdir -p /tmp/icmp_fuzz_corpus
//   cp eph-net-dpdk/fuzzers/corpus/fuzz_icmp_reply/* /tmp/icmp_fuzz_corpus/
//   ./fuzz_icmp_reply -max_total_time=600 -max_len=1500 /tmp/icmp_fuzz_corpus

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <rte_mbuf.h>

#include "eph/dpdk/packet_parse.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Real mbuf size field is uint16_t; anything larger is protocol-
    // irrelevant and just wastes mutation budget.
    if (size > 0xFFFFu) size = 0xFFFFu;

    // Build a single-segment mbuf that points at the fuzzer's input.
    // rte_pktmbuf_mtod returns (uint8_t*)(buf_addr + data_off), so we
    // set buf_addr to the input and leave data_off at 0. nb_segs=1
    // satisfies the multi-seg reject branch at packet_parse.hpp:73.
    rte_mbuf m{};
    m.buf_addr  = const_cast<uint8_t*>(data);
    m.data_off  = 0;
    m.data_len  = static_cast<uint16_t>(size);
    m.pkt_len   = static_cast<uint32_t>(size);
    m.nb_segs   = 1;

    // Primary target: parse_icmp. Exercises parse_ip_header +
    // Type 3 Code 4 embedded-4-tuple extraction (the only branch
    // TcpSession::on_icmp_frag_needed acts on).
    [[maybe_unused]] auto r1 = eph::dpdk::net::parse_icmp(&m);

    // Secondary target: is_ip_fragment — introduced in Tier 2 #3 of
    // the lucky-giggling-kahan review. Lightweight peek that runs
    // alongside parse_icmp in DpdkUdpSocket::process_burst_. Both
    // paths should tolerate the same adversarial inputs.
    [[maybe_unused]] bool r2 = eph::dpdk::net::is_ip_fragment(&m);

    // Tertiary target: parse_ip_header. Reached by parse_icmp/
    // parse_udp_packet/parse_packet, but also callable directly on
    // Platform's dispatch path; worth driving separately so libFuzzer
    // can isolate ingress-layer crashes without the ICMP-specific
    // embedded-header logic in the frame.
    [[maybe_unused]] auto r3 = eph::dpdk::net::parse_ip_header(&m);

    return 0;
}
