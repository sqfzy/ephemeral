// Fuzz harness for UDP packet parser (eph::dpdk::net::parse_udp_packet +
// parse_udp_from_ip).
//
// See fuzzers/README.md for the full workflow (build / seed corpus / run).
//
// Why UDP? DpdkUdpSocket::process_burst_ consumes every RX mbuf via
// parse_udp_packet before any other validation. Upstream packets on
// market-data feeds (MoldUDP64 etc.) are operator-controlled but the
// parser must still tolerate crafted / corrupted frames that slip past
// the NIC checksum layer (covered by Tier 1 #1 review item). The UDP
// length × IP total_length cross-check (packet_parse.hpp:319-326) and
// the payload pointer arithmetic are the two most likely off-by-one
// sources — fuzz input is the best way to exercise them.
//
// Build differs from fuzz_arp_reply in one way: parse_udp_packet lives
// in packet_parse.hpp which pulls in real DPDK mbuf / ether / ip types.
// We build against system libdpdk headers rather than shimming them.
// The fuzzer never calls rte_eal_init — only struct definitions and
// inline accessors are touched.
//
// Quick build (clang ≥ 17, libFuzzer available, system libdpdk installed):
//   clang++ -fsanitize=fuzzer,address,undefined -std=c++23 -O1 -g \
//       -Ieph-net-dpdk/include -Ieph-core/include -Ieph-utils/include \
//       $(pkg-config --cflags libdpdk) \
//       -lspdlog \
//       eph-net-dpdk/fuzzers/fuzz_udp_packet.cpp -o fuzz_udp_packet
//
// Run:
//   mkdir -p /tmp/udp_fuzz_corpus
//   cp eph-net-dpdk/fuzzers/corpus/fuzz_udp_packet/* /tmp/udp_fuzz_corpus/
//   ./fuzz_udp_packet -max_total_time=600 -max_len=1500 /tmp/udp_fuzz_corpus

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <rte_mbuf.h>

#include "eph/dpdk/packet_parse.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 0xFFFFu) size = 0xFFFFu;

    // Single-segment mbuf pointing at the fuzzer's input. See
    // fuzz_icmp_reply.cpp for the rationale on buf_addr / data_off /
    // nb_segs setup.
    rte_mbuf m{};
    m.buf_addr  = const_cast<uint8_t*>(data);
    m.data_off  = 0;
    m.data_len  = static_cast<uint16_t>(size);
    m.pkt_len   = static_cast<uint32_t>(size);
    m.nb_segs   = 1;

    // Primary target: one-shot parse_udp_packet (parse_ip_header +
    // parse_udp_from_ip). Drives the happy-path dispatch that
    // DpdkUdpSocket::process_burst_ actually calls.
    [[maybe_unused]] auto r1 = eph::dpdk::net::parse_udp_packet(&m);

    // Secondary: layered API. `DpdkPoller` splits L3 and L4 parsing in
    // the dispatch fast path, so the layered entries deserve direct
    // exercise in case parse_udp_from_ip evolves independently of the
    // one-shot wrapper.
    if (auto ip = eph::dpdk::net::parse_ip_header(&m)) {
        [[maybe_unused]] auto r2 =
            eph::dpdk::net::parse_udp_from_ip(&m, ip);
        [[maybe_unused]] auto r3 =
            eph::dpdk::net::parse_tcp_from_ip(&m, ip);
    }

    // Tertiary: is_ip_fragment (Tier 2 #3 helper). process_burst_ pairs
    // it with parse_udp_packet to attribute kFragmentRejected vs
    // kPacketsDropped; both must tolerate the same inputs.
    [[maybe_unused]] bool r4 = eph::dpdk::net::is_ip_fragment(&m);

    return 0;
}
