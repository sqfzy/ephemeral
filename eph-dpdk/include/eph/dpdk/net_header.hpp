#pragma once

/// @file net_header.hpp
/// Network header structures for Ethernet/IPv4/TCP.
///
/// All headers use packed layout matching wire format (network byte order).
/// Provides constexpr checksum computation and header template building
/// for zero-copy packet construction on mbufs.

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_tcp.h>

namespace eph::dpdk::net {

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

inline constexpr uint16_t kEtherTypeIpv4   = 0x0800;
inline constexpr uint8_t  kIpProtoTcp      = 6;
inline constexpr uint8_t  kIpProtoUdp      = 17;
inline constexpr uint16_t kIpv4HeaderLen   = 20; // No options
inline constexpr uint16_t kTcpHeaderLen    = 20; // No options
inline constexpr uint16_t kEtherHeaderLen  = 14;
inline constexpr uint16_t kAllHeadersLen   = kEtherHeaderLen + kIpv4HeaderLen + kTcpHeaderLen;

// TCP flags
inline constexpr uint8_t kTcpFin = 0x01;
inline constexpr uint8_t kTcpSyn = 0x02;
inline constexpr uint8_t kTcpRst = 0x04;
inline constexpr uint8_t kTcpPsh = 0x08;
inline constexpr uint8_t kTcpAck = 0x10;
inline constexpr uint8_t kTcpUrg = 0x20;

// Default TCP MSS for Ethernet (MTU 1500 - IP header - TCP header)
inline constexpr uint16_t kDefaultMss = 1460;

// SYN options: MSS(4) + SACK_PERM(2) + NOP(1) + WSCALE(3) + NOP(1) + NOP(1) = 12 bytes
inline constexpr uint16_t kSynOptionsLen = 12;
inline constexpr uint16_t kSynTcpHeaderLen = kTcpHeaderLen + kSynOptionsLen; // 32 bytes

// ─────────────────────────────────────────────────────────────────────────────
// Byte order helpers
// ─────────────────────────────────────────────────────────────────────────────

/// Host-to-network 16-bit (constexpr-safe).
constexpr uint16_t hton16(uint16_t h) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return static_cast<uint16_t>((h >> 8) | (h << 8));
    } else {
        return h;
    }
}

/// Host-to-network 32-bit (constexpr-safe).
constexpr uint32_t hton32(uint32_t h) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return ((h >> 24) & 0xFF) |
               ((h >> 8)  & 0xFF00) |
               ((h << 8)  & 0xFF0000) |
               ((h << 24) & 0xFF000000);
    } else {
        return h;
    }
}

constexpr uint16_t ntoh16(uint16_t n) noexcept { return hton16(n); }
constexpr uint32_t ntoh32(uint32_t n) noexcept { return hton32(n); }

// ─────────────────────────────────────────────────────────────────────────────
// Checksum computation
// ─────────────────────────────────────────────────────────────────────────────

/// Internet checksum (RFC 1071) over arbitrary data.
/// Operates on host byte order internally, returns network byte order result.
inline uint16_t internet_checksum(const void* data, size_t len) noexcept {
    uint32_t sum = 0;
    auto ptr = static_cast<const uint8_t*>(data);

    // Sum 16-bit words
    while (len > 1) {
        uint16_t word;
        std::memcpy(&word, ptr, 2);
        sum += word;
        ptr += 2;
        len -= 2;
    }

    // Handle odd byte
    if (len == 1) {
        uint16_t word = 0;
        std::memcpy(&word, ptr, 1);
        sum += word;
    }

    // Fold 32-bit sum to 16-bit
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return static_cast<uint16_t>(~sum);
}

/// TCP/UDP pseudo-header checksum contribution.
/// Returns partial sum (NOT complemented) in network byte order format.
inline uint32_t pseudo_header_sum(uint32_t src_ip_net, uint32_t dst_ip_net,
                                   uint8_t protocol, uint16_t tcp_len_host) noexcept {
    uint32_t sum = 0;
    // Source IP (already network order, sum as two 16-bit words)
    sum += (src_ip_net & 0xFFFF);
    sum += (src_ip_net >> 16);
    // Dest IP
    sum += (dst_ip_net & 0xFFFF);
    sum += (dst_ip_net >> 16);
    // Protocol (zero-padded to 16 bits)
    sum += hton16(static_cast<uint16_t>(protocol));
    // TCP length
    sum += hton16(tcp_len_host);
    return sum;
}

/// Compute TCP checksum including pseudo-header.
/// All IP addresses in network byte order. tcp_seg points to the TCP header +
/// payload, total_tcp_len is header + payload length in bytes.
inline uint16_t tcp_checksum(uint32_t src_ip_net, uint32_t dst_ip_net,
                              const void* tcp_seg, uint16_t total_tcp_len) noexcept {
    uint32_t sum = pseudo_header_sum(src_ip_net, dst_ip_net, kIpProtoTcp, total_tcp_len);

    auto ptr = static_cast<const uint8_t*>(tcp_seg);
    size_t len = total_tcp_len;

    while (len > 1) {
        uint16_t word;
        std::memcpy(&word, ptr, 2);
        sum += word;
        ptr += 2;
        len -= 2;
    }
    if (len == 1) {
        uint16_t word = 0;
        std::memcpy(&word, ptr, 1);
        sum += word;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return static_cast<uint16_t>(~sum);
}

// ─────────────────────────────────────────────────────────────────────────────
// TCP SYN options
// ─────────────────────────────────────────────────────────────────────────────

/// Write standard TCP SYN options into buffer.
/// Layout: MSS(4) + SACK_PERM(2) + NOP(1) + WSCALE(3) + NOP(1) + NOP(1) = 12 bytes
/// @param buf  Pointer to option area (right after 20-byte TCP header)
/// @param mss  MSS value in host byte order
/// @return Number of bytes written (always kSynOptionsLen = 12)
inline uint16_t write_syn_options(uint8_t* buf, uint16_t mss) noexcept {
    buf[0] = 2;                            // Kind: MSS
    buf[1] = 4;                            // Length
    uint16_t mss_net = hton16(mss);
    std::memcpy(&buf[2], &mss_net, 2);

    buf[4] = 4;                            // Kind: SACK Permitted
    buf[5] = 2;                            // Length

    buf[6] = 1;                            // Kind: NOP (padding)

    buf[7] = 3;                            // Kind: Window Scale
    buf[8] = 3;                            // Length
    buf[9] = 0;                            // Shift count (2^0 = 1, no scaling)

    buf[10] = 1;                           // Kind: NOP
    buf[11] = 1;                           // Kind: NOP

    return kSynOptionsLen;
}

// ─────────────────────────────────────────────────────────────────────────────
// Connection tuple
// ─────────────────────────────────────────────────────────────────────────────

/// Identifies a TCP connection. All fields in HOST byte order.
struct ConnectionTuple {
    uint32_t src_ip    = 0;
    uint32_t dst_ip    = 0;
    uint16_t src_port  = 0;
    uint16_t dst_port  = 0;

    bool operator==(const ConnectionTuple&) const = default;
};

// ─────────────────────────────────────────────────────────────────────────────
// Packet builder — constructs Ethernet/IP/TCP headers on an mbuf
// ─────────────────────────────────────────────────────────────────────────────

/// Ethernet + IPv4 + TCP header template for fast packet construction.
/// Pre-fill static fields once, then update dynamic fields (seq, ack, flags,
/// payload length) per packet on the hot path.
struct PacketTemplate {
    rte_ether_addr src_mac{};
    rte_ether_addr dst_mac{};
    ConnectionTuple tuple{};
    uint16_t ip_id = 0; // Incremented per packet
    uint16_t mss = kDefaultMss; // MSS advertised in SYN options

    /// [P2] Enable NIC TX checksum offload (IP + TCP).
    /// When true, sets ol_flags and computes pseudo-header checksum only.
    /// When false, falls back to software checksum (default).
    bool hw_cksum = false;

    /// Build a complete TCP packet in an mbuf.
    /// @param pool     Mempool to allocate from
    /// @param seq      TCP sequence number (host order)
    /// @param ack      TCP acknowledgment number (host order)
    /// @param flags    TCP flags (SYN, ACK, FIN, RST, PSH, etc.)
    /// @param window   TCP window size (host order)
    /// @param payload  Optional payload data
    /// @param payload_len  Payload length
    /// @return Allocated and filled mbuf, or nullptr on allocation failure
    rte_mbuf* build_packet(rte_mempool* pool,
                           uint32_t seq, uint32_t ack,
                           uint8_t flags, uint16_t window,
                           const void* payload = nullptr,
                           uint16_t payload_len = 0) noexcept {
        rte_mbuf* mbuf = rte_pktmbuf_alloc(pool);
        if (!mbuf) return nullptr;

        // SYN packets include TCP options (MSS, SACK, WinScale) to avoid
        // being dropped by middleboxes that expect standard TCP options.
        const bool is_syn = (flags & kTcpSyn) != 0;
        const uint16_t tcp_hdr_len = is_syn ? kSynTcpHeaderLen : kTcpHeaderLen;
        const uint16_t total_len = kEtherHeaderLen + kIpv4HeaderLen + tcp_hdr_len + payload_len;

        auto* pkt = reinterpret_cast<uint8_t*>(
            rte_pktmbuf_append(mbuf, total_len));
        if (!pkt) {
            rte_pktmbuf_free(mbuf);
            return nullptr;
        }

        // ── Ethernet header ──
        auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt);
        rte_ether_addr_copy(&dst_mac, &eth->dst_addr);
        rte_ether_addr_copy(&src_mac, &eth->src_addr);
        eth->ether_type = hton16(kEtherTypeIpv4);

        // ── IPv4 header ──
        auto* ip = reinterpret_cast<rte_ipv4_hdr*>(pkt + kEtherHeaderLen);
        ip->version_ihl     = 0x45; // Version 4, IHL 5 (20 bytes)
        ip->type_of_service  = 0;
        ip->total_length     = hton16(kIpv4HeaderLen + tcp_hdr_len + payload_len);
        ip->packet_id        = hton16(ip_id++);
        ip->fragment_offset  = hton16(0x4000); // Don't Fragment
        ip->time_to_live     = 64;
        ip->next_proto_id    = kIpProtoTcp;
        ip->hdr_checksum     = 0;
        ip->src_addr         = hton32(tuple.src_ip);
        ip->dst_addr         = hton32(tuple.dst_ip);
        // IP checksum: computed here for software path, overridden for hw offload below
        ip->hdr_checksum     = hw_cksum ? 0 : internet_checksum(ip, kIpv4HeaderLen);

        // ── TCP header ──
        auto* tcp = reinterpret_cast<rte_tcp_hdr*>(pkt + kEtherHeaderLen + kIpv4HeaderLen);
        tcp->src_port  = hton16(tuple.src_port);
        tcp->dst_port  = hton16(tuple.dst_port);
        tcp->sent_seq  = hton32(seq);
        tcp->recv_ack  = hton32(ack);
        tcp->data_off   = (tcp_hdr_len / 4) << 4;
        tcp->tcp_flags  = flags;
        tcp->rx_win     = hton16(window);
        tcp->cksum      = 0;
        tcp->tcp_urp    = 0;

        // Write SYN options (MSS, SACK Permitted, Window Scale)
        if (is_syn) {
            auto* opt_ptr = pkt + kEtherHeaderLen + kIpv4HeaderLen + kTcpHeaderLen;
            write_syn_options(opt_ptr, mss);
        }

        // Copy payload if present
        if (payload && payload_len > 0) {
            std::memcpy(pkt + kEtherHeaderLen + kIpv4HeaderLen + tcp_hdr_len,
                        payload, payload_len);
        }

        // [P2] Checksum: hardware offload or software fallback
        if (hw_cksum) {
            // Let NIC compute checksums — set offload metadata
            mbuf->ol_flags = RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_TCP_CKSUM;
            mbuf->l2_len = kEtherHeaderLen;
            mbuf->l3_len = kIpv4HeaderLen;
            mbuf->l4_len = tcp_hdr_len;
            // IP checksum field must be 0 for HW offload
            ip->hdr_checksum = 0;
            // TCP checksum field must contain pseudo-header checksum
            tcp->cksum = rte_ipv4_phdr_cksum(ip, mbuf->ol_flags);
        } else {
            mbuf->ol_flags = 0; // Clear any stale offload flags
            uint16_t tcp_total = tcp_hdr_len + payload_len;
            tcp->cksum = tcp_checksum(ip->src_addr, ip->dst_addr, tcp, tcp_total);
        }

        return mbuf;
    }

    /// Build a packet directly into an existing mbuf (for hot path, avoids alloc).
    /// The mbuf must have at least kAllHeadersLen + payload_len bytes of space.
    /// Returns number of bytes written, or 0 on failure.
    uint16_t fill_packet(rte_mbuf* mbuf,
                         uint32_t seq, uint32_t ack,
                         uint8_t flags, uint16_t window,
                         const void* payload = nullptr,
                         uint16_t payload_len = 0) noexcept {
        const uint16_t total_len = kAllHeadersLen + payload_len;

        // Reset mbuf data
        rte_pktmbuf_reset(mbuf);
        auto* pkt = reinterpret_cast<uint8_t*>(
            rte_pktmbuf_append(mbuf, total_len));
        if (!pkt) return 0;

        // Ethernet
        auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt);
        rte_ether_addr_copy(&dst_mac, &eth->dst_addr);
        rte_ether_addr_copy(&src_mac, &eth->src_addr);
        eth->ether_type = hton16(kEtherTypeIpv4);

        // IPv4
        auto* ip = reinterpret_cast<rte_ipv4_hdr*>(pkt + kEtherHeaderLen);
        ip->version_ihl     = 0x45;
        ip->type_of_service  = 0;
        ip->total_length     = hton16(kIpv4HeaderLen + kTcpHeaderLen + payload_len);
        ip->packet_id        = hton16(ip_id++);
        ip->fragment_offset  = hton16(0x4000);
        ip->time_to_live     = 64;
        ip->next_proto_id    = kIpProtoTcp;
        ip->hdr_checksum     = 0;
        ip->src_addr         = hton32(tuple.src_ip);
        ip->dst_addr         = hton32(tuple.dst_ip);
        ip->hdr_checksum     = hw_cksum ? 0 : internet_checksum(ip, kIpv4HeaderLen);

        // TCP
        auto* tcp = reinterpret_cast<rte_tcp_hdr*>(pkt + kEtherHeaderLen + kIpv4HeaderLen);
        tcp->src_port  = hton16(tuple.src_port);
        tcp->dst_port  = hton16(tuple.dst_port);
        tcp->sent_seq  = hton32(seq);
        tcp->recv_ack  = hton32(ack);
        tcp->data_off   = (kTcpHeaderLen / 4) << 4;
        tcp->tcp_flags  = flags;
        tcp->rx_win     = hton16(window);
        tcp->cksum      = 0;
        tcp->tcp_urp    = 0;

        if (payload && payload_len > 0) {
            std::memcpy(pkt + kAllHeadersLen, payload, payload_len);
        }

        // [P2] Checksum: hardware offload or software fallback
        if (hw_cksum) {
            mbuf->ol_flags = RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_TCP_CKSUM;
            mbuf->l2_len = kEtherHeaderLen;
            mbuf->l3_len = kIpv4HeaderLen;
            mbuf->l4_len = kTcpHeaderLen;
            tcp->cksum = rte_ipv4_phdr_cksum(ip, mbuf->ol_flags);
        } else {
            mbuf->ol_flags = 0; // Clear any stale offload flags
            uint16_t tcp_total = kTcpHeaderLen + payload_len;
            tcp->cksum = tcp_checksum(ip->src_addr, ip->dst_addr, tcp, tcp_total);
        }

        return total_len;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Packet parser — extract headers from received mbufs
// ─────────────────────────────────────────────────────────────────────────────

/// Parsed packet view — pointers into the original mbuf data (zero-copy).
struct ParsedPacket {
    const rte_ether_hdr* eth  = nullptr;
    const rte_ipv4_hdr*  ip   = nullptr;
    const rte_tcp_hdr*   tcp  = nullptr;
    const uint8_t*       payload = nullptr;
    uint16_t             payload_len = 0;

    /// Extract TCP flags from the parsed packet.
    [[nodiscard]] uint8_t tcp_flags() const noexcept {
        return tcp ? tcp->tcp_flags : 0;
    }

    /// Extract TCP sequence number (host order).
    [[nodiscard]] uint32_t seq() const noexcept {
        return tcp ? ntoh32(tcp->sent_seq) : 0;
    }

    /// Extract TCP acknowledgment number (host order).
    [[nodiscard]] uint32_t ack() const noexcept {
        return tcp ? ntoh32(tcp->recv_ack) : 0;
    }

    /// Extract TCP window size (host order).
    [[nodiscard]] uint16_t window() const noexcept {
        return tcp ? ntoh16(tcp->rx_win) : 0;
    }

    /// Extract source port (host order).
    [[nodiscard]] uint16_t src_port() const noexcept {
        return tcp ? ntoh16(tcp->src_port) : 0;
    }

    /// Extract destination port (host order).
    [[nodiscard]] uint16_t dst_port() const noexcept {
        return tcp ? ntoh16(tcp->dst_port) : 0;
    }

    /// Extract source IP (host order).
    [[nodiscard]] uint32_t src_ip() const noexcept {
        return ip ? ntoh32(ip->src_addr) : 0;
    }

    /// Extract destination IP (host order).
    [[nodiscard]] uint32_t dst_ip() const noexcept {
        return ip ? ntoh32(ip->dst_addr) : 0;
    }

    /// Check if this packet belongs to the given connection tuple.
    [[nodiscard]] bool matches(const ConnectionTuple& t) const noexcept {
        // Incoming packets have swapped src/dst relative to our tuple
        return src_ip() == t.dst_ip && dst_ip() == t.src_ip &&
               src_port() == t.dst_port && dst_port() == t.src_port;
    }

    /// Check if the packet has a specific flag set.
    [[nodiscard]] bool has_flag(uint8_t flag) const noexcept {
        return (tcp_flags() & flag) != 0;
    }
};

/// Parse an Ethernet/IPv4/TCP packet from an mbuf.
/// Returns empty ParsedPacket (all nullptrs) if the packet is not a valid
/// IPv4/TCP packet or is too short.
inline ParsedPacket parse_packet(const rte_mbuf* mbuf) noexcept {
    const uint16_t pkt_len = rte_pktmbuf_data_len(mbuf);
    if (pkt_len < kAllHeadersLen) return {};

    const auto* data = rte_pktmbuf_mtod(mbuf, const uint8_t*);

    // Validate all headers before populating result (fewer branch deps)
    auto* eth = reinterpret_cast<const rte_ether_hdr*>(data);
    if (ntoh16(eth->ether_type) != kEtherTypeIpv4) return {};

    auto* ip = reinterpret_cast<const rte_ipv4_hdr*>(data + kEtherHeaderLen);
    uint8_t ihl = (ip->version_ihl & 0x0F) << 2; // * 4 via shift
    if (ihl < kIpv4HeaderLen || ip->next_proto_id != kIpProtoTcp) return {};

    uint16_t tcp_offset = kEtherHeaderLen + ihl;
    if (pkt_len < tcp_offset + kTcpHeaderLen) return {};

    auto* tcp = reinterpret_cast<const rte_tcp_hdr*>(data + tcp_offset);
    uint8_t tcp_doff = (tcp->data_off >> 4) << 2; // * 4 via shift
    if (tcp_doff < kTcpHeaderLen) return {};

    // All valid — populate result in one shot
    ParsedPacket result;
    result.eth = eth;
    result.ip  = ip;
    result.tcp = tcp;

    // Use IP total_length (not pkt_len) to compute payload size.
    // Ethernet frames have a 64-byte minimum — NICs pad short frames.
    // Using pkt_len would include those padding bytes as TCP payload,
    // corrupting upper-layer protocols (TLS record reassembly).
    uint16_t ip_total  = ntoh16(ip->total_length);    // IP header + TCP header + payload
    uint16_t tcp_start = ihl;                          // offset from IP header to TCP
    uint16_t data_start = tcp_start + tcp_doff;        // offset from IP header to payload

    // Reject if IP advertises more data than the mbuf actually contains.
    // A corrupted IP header could point payload past the end of the buffer.
    if (kEtherHeaderLen + ip_total > pkt_len) return {};

    if (ip_total > data_start) {
        uint16_t payload_offset = kEtherHeaderLen + data_start;
        result.payload     = data + payload_offset;
        result.payload_len = ip_total - data_start;
    }

    return result;
}

/// Parse an IPv4 address string "a.b.c.d" to host-order uint32_t.
/// Returns 0 on invalid input.
inline uint32_t parse_ipv4(const char* str) noexcept {
    if (!str) return 0;

    uint32_t octets[4]{};
    const char* p = str;

    for (int i = 0; i < 4; ++i) {
        if (i > 0) {
            if (*p != '.') return 0;
            ++p;
        }
        // Parse up to 3 digits
        if (*p < '0' || *p > '9') return 0;
        uint32_t val = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 3) {
            val = val * 10 + static_cast<uint32_t>(*p - '0');
            ++p;
            ++digits;
        }
        if (val > 255) return 0;
        octets[i] = val;
    }

    // Must be at end of string (no trailing characters)
    if (*p != '\0') return 0;

    return (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
}

/// Format an IPv4 address from host-order uint32_t to "a.b.c.d".
inline std::array<char, 16> format_ipv4(uint32_t ip) noexcept {
    std::array<char, 16> buf{};
    snprintf(buf.data(), buf.size(), "%u.%u.%u.%u",
                  (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                  (ip >> 8) & 0xFF, ip & 0xFF);
    return buf;
}

} // namespace eph::dpdk::net
