// Fuzz harness for ARP reply parser (eph::dpdk::arp::parse_arp_reply).
//
// See fuzzers/README.md for the full workflow (build / seed corpus / run).
//
// Quick build (clang ≥ 17, libFuzzer available):
//   clang++ -fsanitize=fuzzer,address,undefined -std=c++23 \
//       -Ieph-net-dpdk/include -Ieph-core/include -Ieph-utils/include \
//       -lspdlog -lssl -lcrypto \
//       eph-net-dpdk/fuzzers/fuzz_arp_reply.cpp -o fuzz_arp_reply
//
// Run:
//   mkdir -p /tmp/arp_fuzz_corpus
//   cp eph-net-dpdk/fuzzers/corpus/fuzz_arp_reply/* /tmp/arp_fuzz_corpus/
//   ./fuzz_arp_reply -max_total_time=600 /tmp/arp_fuzz_corpus
//
// Note: parse_arp_reply() reads from the mbuf via rte_pktmbuf_mtod() — we
// implement that as `m->data` here (a pointer set up before the call) so the
// parser sees the fuzzer's input byte-for-byte. No DPDK runtime is required.

#include <cstddef>
#include <cstdint>
#include <cstring>

// Minimal DPDK shims: enough to make eph/dpdk/arp.hpp compile and exercise
// the pure parsing paths.  parse_arp_reply only touches:
//   - mbuf->data_len  (length check)
//   - rte_pktmbuf_mtod(m, T*)  (pointer to L2 header)
// Everything else (alloc / append / ether copy / tx_burst / rx_burst) is
// only referenced by the build/resolve helpers we do NOT call from the fuzz
// entry point; declaring them as inline no-ops keeps the TU linkable.
struct rte_mempool;
struct rte_mbuf {
    uint16_t data_len;
    uint16_t nb_segs;     // scatter-rejection check; initialised to 1 in LLVMFuzzerTestOneInput
    const uint8_t* data;  // shim-only: parsed payload pointer
};
#define rte_pktmbuf_data_len(m) ((m)->data_len)
struct rte_ether_addr { uint8_t addr_bytes[6]; };
struct rte_ether_hdr { rte_ether_addr dst_addr; rte_ether_addr src_addr; uint16_t ether_type; };
struct rte_ipv4_hdr {
    uint8_t version_ihl; uint8_t type_of_service; uint16_t total_length;
    uint16_t packet_id; uint16_t fragment_offset; uint8_t time_to_live;
    uint8_t next_proto_id; uint16_t hdr_checksum; uint32_t src_addr; uint32_t dst_addr;
};
inline rte_mbuf* rte_pktmbuf_alloc(rte_mempool*) { return nullptr; }
inline void rte_pktmbuf_free(rte_mbuf*) {}
inline uint8_t* rte_pktmbuf_append(rte_mbuf*, uint16_t) { return nullptr; }
inline void rte_ether_addr_copy(const rte_ether_addr* src, rte_ether_addr* dst) {
    if (src && dst) std::memcpy(dst->addr_bytes, src->addr_bytes, 6);
}
inline uint32_t rte_mempool_avail_count(const rte_mempool*) { return 0; }
// rte_pktmbuf_mtod: in real DPDK this returns mbuf->buf_addr + data_off.
// For the fuzzer we redirect it to our shim `data` pointer.
#define rte_pktmbuf_mtod(m, t) reinterpret_cast<t>(const_cast<uint8_t*>((m)->data))
inline uint16_t rte_eth_tx_burst(uint16_t, uint16_t, rte_mbuf**, uint16_t) { return 0; }
inline uint16_t rte_eth_rx_burst(uint16_t, uint16_t, rte_mbuf**, uint16_t) { return 0; }

#include "eph/dpdk/arp.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // parse_arp_reply gates on mbuf->data_len < (eth header + ARP packet);
    // values that overflow uint16_t are protocol-irrelevant.
    if (size > 0xFFFFu) size = 0xFFFFu;

    rte_mbuf m{};
    m.data_len = static_cast<uint16_t>(size);
    m.nb_segs  = 1;              // scatter-rejection guard is reachable with nb_segs=1
    m.data     = data;

    // Drive several adversarial parameter combinations so libFuzzer can
    // explore each branch in parse_arp_reply: the target_ip filter, the
    // optional anti-spoof MAC, and the size guard.
    // 1. Permissive: target_ip=0, no expected MAC — the most generic path.
    [[maybe_unused]] auto r1 = eph::dpdk::arp::parse_arp_reply(&m, 0u, std::nullopt);

    // 2. Target IP derived from the input prefix — probes the target_ip
    //    matching branch when the fuzzer explores well-formed-looking
    //    sender_ip fields.
    if (size >= 4) {
        uint32_t want_target = 0;
        std::memcpy(&want_target, data, sizeof(want_target));
        [[maybe_unused]] auto r2 =
            eph::dpdk::arp::parse_arp_reply(&m, want_target, std::nullopt);
    }

    // 3. With expected_mac set — exercises the anti-spoof comparison and
    //    the all-zero / I/G-bit rejection logic.
    rte_ether_addr expected{};
    expected.addr_bytes[0] = 0x02; expected.addr_bytes[1] = 0xAA;
    expected.addr_bytes[2] = 0xBB; expected.addr_bytes[3] = 0xCC;
    expected.addr_bytes[4] = 0xDD; expected.addr_bytes[5] = 0xEE;
    [[maybe_unused]] auto r3 = eph::dpdk::arp::parse_arp_reply(&m, 0u, expected);

    return 0;
}
