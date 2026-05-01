// Fuzz harness for DNS response parser (eph::dpdk::dns::detail::parse_dns_response).
//
// See fuzzers/README.md for the full workflow (build / seed corpus / run).
//
// Quick build (clang ≥ 17, libFuzzer available):
//   clang++ -fsanitize=fuzzer,address,undefined -std=c++23 \
//       -Ieph-net-dpdk/include -Ieph-core/include -Ieph-utils/include \
//       -lspdlog \
//       eph-net-dpdk/fuzzers/fuzz_dns_reply.cpp -o fuzz_dns_reply
//
// Run:
//   mkdir -p /tmp/dns_fuzz_corpus
//   cp eph-net-dpdk/fuzzers/corpus/fuzz_dns_reply/* /tmp/dns_fuzz_corpus/
//   ./fuzz_dns_reply -max_total_time=600 /tmp/dns_fuzz_corpus
//
// Note: This harness targets the pure parsing function parse_dns_response()
// and the name-skipping helper skip_dns_name(), which operate on raw byte
// buffers without requiring DPDK runtime (EAL, mbufs, NIC ports).

#include <cstddef>
#include <cstdint>
#include <cstring>

// We only need the DNS parsing helpers, not the full DPDK packet builder.
// Forward-declare the minimal DPDK types so the header compiles without
// linking librte_*.  The functions we fuzz (parse_dns_response, skip_dns_name)
// never touch DPDK types.
struct rte_mempool;
// nb_segs: the dns header's `try_parse_dns_packet` rejects scattered
// mbufs defense-in-depth; keep it as uint16_t=1 by default so its own
// early-out check is reachable. Also define `rte_pktmbuf_data_len` since
// the parser now accesses `data_len` through that macro for consistency
// with the rest of the code.
struct rte_mbuf { uint16_t data_len; uint16_t nb_segs; };
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
inline void rte_ether_addr_copy(const rte_ether_addr*, rte_ether_addr*) {}
#define rte_pktmbuf_mtod(m, t) reinterpret_cast<t>(nullptr)
inline uint16_t rte_eth_tx_burst(uint16_t, uint16_t, rte_mbuf**, uint16_t) { return 0; }
inline uint16_t rte_eth_rx_burst(uint16_t, uint16_t, rte_mbuf**, uint16_t) { return 0; }

#include "eph/dpdk/dns.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Exercise parse_dns_response with a wildcard transaction ID (0x0000)
    // so we do not reject every packet on ID mismatch. We set up the first
    // two bytes of the input as the tx_id to match.
    if (size < 2) return 0;

    uint16_t tx_id;
    std::memcpy(&tx_id, data, sizeof(tx_id));

    // Parse with matching tx_id so the parser proceeds past the ID check.
    auto result = eph::dpdk::dns::detail::parse_dns_response(data, size, tx_id);
    // Result is std::expected<uint32_t, std::string>; we only care about crashes / UB.
    (void)result;

    // Also exercise skip_dns_name independently with various offsets.
    // Start after the 12-byte DNS header if the buffer is large enough.
    if (size > 12) {
        [[maybe_unused]] auto off = eph::dpdk::dns::detail::skip_dns_name(data, 12, size);
    }

    return 0;
}
