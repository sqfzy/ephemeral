#pragma once

/// @file packet_parse.hpp
/// Zero-copy packet parsing for IPv4/TCP/UDP.
///
/// Provides a layered parse API:
///   - parse_ip_header()    — L2+L3 only (protocol dispatch)
///   - parse_tcp_from_ip()  — L4 TCP from pre-parsed IP (zero redundancy)
///   - parse_udp_from_ip()  — L4 UDP from pre-parsed IP (zero redundancy)
///   - parse_packet()       — convenience: L2+L3+TCP in one call
///   - parse_udp_packet()   — convenience: L2+L3+UDP in one call

#include "eph/dpdk/packet_core.hpp"

#include <rte_mbuf.h>
#include <rte_tcp.h>

namespace eph::dpdk::net {

// ─────────────────────────────────────────────────────────────────────────────
// IP header parser — protocol-agnostic L2+L3 parsing
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Minimal L2+L3 parse result for protocol dispatch.
///
/// Extracts Ethernet + IPv4 headers and IP protocol number without parsing
/// L4 (TCP/UDP). Used by RxDispatcher to determine protocol before committing
/// to a full L4 parse, avoiding redundant IP header parsing.
///
/// @note Zero-copy — all pointers reference the original mbuf data buffer.
struct ParsedIpHeader {
    const rte_ether_hdr* eth{nullptr};  ///< Ethernet header
    const rte_ipv4_hdr*  ip{nullptr};   ///< IPv4 header
    uint8_t ihl{0};                     ///< IP header length in bytes (typically 20)
    uint8_t proto{0};                   ///< IP protocol number (kIpProtoTcp=6, kIpProtoUdp=17)

    /// Check if parse succeeded (valid IPv4 packet).
    [[nodiscard]] explicit operator bool() const noexcept { return ip != nullptr; }

    /// Extract source IP (host byte order).
    [[nodiscard]] uint32_t src_ip() const noexcept {
        return ip ? ntoh32(ip->src_addr) : 0;
    }
    /// Extract destination IP (host byte order).
    [[nodiscard]] uint32_t dst_ip() const noexcept {
        return ip ? ntoh32(ip->dst_addr) : 0;
    }
};

/// @brief Parse L2+L3 headers from an mbuf without L4 parsing.
///
/// Validates Ethernet type (IPv4) and IPv4 version/IHL. Does NOT check the
/// L4 protocol — callers use the returned proto field to dispatch to
/// parse_packet() (TCP) or parse_udp_packet() (UDP).
///
/// @param mbuf  Received packet mbuf (must not be null)
/// @return ParsedIpHeader with eth/ip populated on success, all-null on failure
[[nodiscard]] inline ParsedIpHeader parse_ip_header(const rte_mbuf* mbuf) noexcept {
    if (!mbuf) [[unlikely]] return {};
    const uint16_t pkt_len = rte_pktmbuf_data_len(mbuf);
    if (pkt_len < kEtherHeaderLen + kIpv4HeaderLen) return {};

    const auto* data = rte_pktmbuf_mtod(mbuf, const uint8_t*);

    auto* eth = reinterpret_cast<const rte_ether_hdr*>(data);
    if (ntoh16(eth->ether_type) != kEtherTypeIpv4) return {};

    auto* ip = reinterpret_cast<const rte_ipv4_hdr*>(data + kEtherHeaderLen);
    if ((ip->version_ihl >> 4) != 4) return {};
    uint8_t ihl = (ip->version_ihl & 0x0F) << 2;
    if (ihl < kIpv4HeaderLen) return {};

    return {.eth = eth, .ip = ip, .ihl = ihl, .proto = ip->next_proto_id};
}

// ─────────────────────────────────────────────────────────────────────────────
// Packet parser — extract headers from received mbufs
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Zero-copy parsed view of an Ethernet/IPv4/TCP packet.
///
/// All pointers reference memory within the original mbuf data buffer.
/// The view is valid only as long as the underlying mbuf is alive and
/// unmodified. Returned by parse_packet().
///
/// @warning Do not store a ParsedPacket beyond the lifetime of the mbuf
///          it was parsed from. Access after rte_pktmbuf_free is undefined.
struct ParsedPacket {
    const rte_ether_hdr* eth  = nullptr;   ///< Ethernet header (null if parse failed)
    const rte_ipv4_hdr*  ip   = nullptr;   ///< IPv4 header (null if not IPv4)
    const rte_tcp_hdr*   tcp  = nullptr;   ///< TCP header (null if not TCP)
    const uint8_t*       payload = nullptr; ///< TCP payload start (null if no payload)
    uint16_t             payload_len = 0;   ///< TCP payload length in bytes

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

    /// Check if this parsed view is valid (all required headers present).
    [[nodiscard]] explicit operator bool() const noexcept {
        return tcp != nullptr;
    }

    /// Human-readable one-line summary for diagnostics/logging.
    /// Returns "(invalid)" if the packet was not successfully parsed.
    /// Defined below — deferred to keep struct definition compact.
    [[nodiscard]] inline std::string dump() const;

    /// JSON-formatted packet summary for monitoring/logging.
    /// Returns "{\"valid\":false}" if the packet was not parsed.
    /// Defined below — deferred to keep struct definition compact.
    [[nodiscard]] inline std::string to_json() const;
};

/// @brief Parse TCP L4 headers from a pre-parsed IP header (zero-redundancy).
///
/// Skips L2/L3 parsing (already done by parse_ip_header). Use this in RxDispatcher
/// dispatch paths where parse_ip_header is called once for protocol detection,
/// then the appropriate L4 parser is invoked.
///
/// @param mbuf    Received packet mbuf
/// @param ip_hdr  Pre-parsed IP header from parse_ip_header()
/// @return ParsedPacket with TCP fields on success, all-null if not valid TCP
[[nodiscard]] inline ParsedPacket
parse_tcp_from_ip(const rte_mbuf* mbuf, const ParsedIpHeader& ip_hdr) noexcept {
    if (!mbuf || !ip_hdr) [[unlikely]] return {};
    if (ip_hdr.proto != kIpProtoTcp) return {};

    const uint16_t pkt_len = rte_pktmbuf_data_len(mbuf);
    const auto* data = rte_pktmbuf_mtod(mbuf, const uint8_t*);

    uint16_t tcp_offset = kEtherHeaderLen + ip_hdr.ihl;
    if (pkt_len < tcp_offset + kTcpHeaderLen) return {};

    auto* tcp = reinterpret_cast<const rte_tcp_hdr*>(data + tcp_offset);
    uint8_t tcp_doff = (tcp->data_off >> 4) << 2;
    if (tcp_doff < kTcpHeaderLen) return {};

    ParsedPacket result;
    result.eth = ip_hdr.eth;
    result.ip  = ip_hdr.ip;
    result.tcp = tcp;

    // Use IP total_length (not pkt_len) to compute payload size.
    // Ethernet frames have a 64-byte minimum — NICs pad short frames.
    uint16_t ip_total   = ntoh16(ip_hdr.ip->total_length);
    uint16_t tcp_start  = ip_hdr.ihl;
    uint16_t data_start = tcp_start + tcp_doff;

    if (data_start > ip_total) return {};
    if (kEtherHeaderLen + ip_total > pkt_len) return {};

    if (ip_total > data_start) {
        uint16_t payload_offset = kEtherHeaderLen + data_start;
        result.payload     = data + payload_offset;
        result.payload_len = ip_total - data_start;
    }

    return result;
}

/// @brief Parse an Ethernet/IPv4/TCP packet from an mbuf (zero-copy).
///
/// Convenience wrapper that calls parse_ip_header() + parse_tcp_from_ip().
/// For RxDispatcher dispatch where protocol detection is done first, prefer
/// calling the layered API directly to avoid redundant IP parsing.
///
/// @param mbuf  Received packet mbuf (must not be null)
/// @return ParsedPacket with all fields populated on success, or all-null
///         fields if the packet is not a valid IPv4/TCP packet
[[nodiscard]] inline ParsedPacket parse_packet(const rte_mbuf* mbuf) noexcept {
    auto ip_hdr = parse_ip_header(mbuf);
    if (!ip_hdr || ip_hdr.proto != kIpProtoTcp) return {};
    return parse_tcp_from_ip(mbuf, ip_hdr);
}

// ─────────────────────────────────────────────────────────────────────────────
// UDP packet parser — extract headers from received mbufs
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Zero-copy parsed view of an Ethernet/IPv4/UDP packet.
///
/// All pointers reference memory within the original mbuf data buffer.
/// The view is valid only as long as the underlying mbuf is alive.
/// Returned by parse_udp_packet().
struct ParsedUdpPacket {
    const rte_ether_hdr* eth     = nullptr;
    const rte_ipv4_hdr*  ip      = nullptr;
    const UdpHeader*     udp     = nullptr;
    const uint8_t*       payload = nullptr;
    uint16_t             payload_len = 0;

    [[nodiscard]] uint32_t src_ip() const noexcept {
        return ip ? ntoh32(ip->src_addr) : 0;
    }
    [[nodiscard]] uint32_t dst_ip() const noexcept {
        return ip ? ntoh32(ip->dst_addr) : 0;
    }
    [[nodiscard]] uint16_t src_port() const noexcept {
        return udp ? ntoh16(udp->src_port) : 0;
    }
    [[nodiscard]] uint16_t dst_port() const noexcept {
        return udp ? ntoh16(udp->dst_port) : 0;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return eth != nullptr && ip != nullptr && udp != nullptr;
    }

    /// Human-readable one-line summary.
    /// Defined below — deferred to keep struct definition compact.
    [[nodiscard]] inline std::string dump() const;

    /// JSON-formatted packet summary.
    /// Defined below — deferred to keep struct definition compact.
    [[nodiscard]] inline std::string to_json() const;
};

/// @brief Parse UDP L4 headers from a pre-parsed IP header (zero-redundancy).
///
/// Skips L2/L3 parsing (already done by parse_ip_header). Use this in RxDispatcher
/// dispatch paths for zero-redundancy protocol dispatch.
///
/// @param mbuf    Received packet mbuf
/// @param ip_hdr  Pre-parsed IP header from parse_ip_header()
/// @return ParsedUdpPacket with UDP fields on success, all-null if not valid UDP
[[nodiscard]] inline ParsedUdpPacket
parse_udp_from_ip(const rte_mbuf* mbuf, const ParsedIpHeader& ip_hdr) noexcept {
    if (!mbuf || !ip_hdr) [[unlikely]] return {};
    if (ip_hdr.proto != kIpProtoUdp) return {};

    const uint16_t pkt_len = rte_pktmbuf_data_len(mbuf);
    const auto* data = rte_pktmbuf_mtod(mbuf, const uint8_t*);

    uint16_t udp_offset = kEtherHeaderLen + ip_hdr.ihl;
    if (udp_offset + kUdpHeaderLen > pkt_len) return {};

    auto* udp = reinterpret_cast<const UdpHeader*>(data + udp_offset);
    uint16_t udp_len = ntoh16(udp->length);
    if (udp_len < kUdpHeaderLen) return {};
    if (udp_offset + udp_len > pkt_len) return {};

    ParsedUdpPacket result;
    result.eth = ip_hdr.eth;
    result.ip  = ip_hdr.ip;
    result.udp = udp;

    uint16_t payload_offset = udp_offset + kUdpHeaderLen;
    result.payload_len = udp_len - kUdpHeaderLen;
    if (result.payload_len > 0) {
        result.payload = data + payload_offset;
    }

    return result;
}

/// @brief Parse a UDP/IPv4/Ethernet packet from an mbuf (zero-copy).
///
/// Convenience wrapper that calls parse_ip_header() + parse_udp_from_ip().
/// For RxDispatcher dispatch, prefer the layered API directly.
///
/// @param mbuf  Received packet mbuf (must not be null)
/// @return ParsedUdpPacket with all fields on success, all-null if not valid UDP
[[nodiscard]] inline ParsedUdpPacket parse_udp_packet(const rte_mbuf* mbuf) noexcept {
    auto ip_hdr = parse_ip_header(mbuf);
    if (!ip_hdr || ip_hdr.proto != kIpProtoUdp) return {};
    return parse_udp_from_ip(mbuf, ip_hdr);
}

inline std::string ParsedPacket::dump() const {
    if (!tcp) return "(invalid)";
    auto flags = tcp_flags();
    std::string flag_str;
    if (flags & kTcpSyn) flag_str += "SYN ";
    if (flags & kTcpAck) flag_str += "ACK ";
    if (flags & kTcpFin) flag_str += "FIN ";
    if (flags & kTcpRst) flag_str += "RST ";
    if (flags & kTcpPsh) flag_str += "PSH ";
    if (flags & kTcpUrg) flag_str += "URG ";
    if (!flag_str.empty()) flag_str.pop_back(); // trailing space
    return std::format("{}:{} -> {}:{} [{}] seq={} ack={} win={} payload={}B",
        format_ipv4(src_ip()).data(), src_port(),
        format_ipv4(dst_ip()).data(), dst_port(),
        flag_str, seq(), ack(), window(), payload_len);
}

inline std::string ParsedPacket::to_json() const {
    if (!tcp) return "{\"valid\":false}";
    return std::format(
        "{{\"src_ip\":\"{}\",\"src_port\":{},\"dst_ip\":\"{}\",\"dst_port\":{},"
        "\"seq\":{},\"ack\":{},\"window\":{},\"flags\":{},\"payload_len\":{}}}",
        format_ipv4(src_ip()).data(), src_port(),
        format_ipv4(dst_ip()).data(), dst_port(),
        seq(), ack(), window(), tcp_flags(), payload_len);
}

// ─────────────────────────────────────────────────────────────────────────────
// ParsedUdpPacket method definitions (deferred because format_ipv4 is above)
// ─────────────────────────────────────────────────────────────────────────────

inline std::string ParsedUdpPacket::dump() const {
    if (!udp) return "(invalid)";
    return std::format("UDP {}:{} -> {}:{} payload={}B",
        format_ipv4(src_ip()).data(), src_port(),
        format_ipv4(dst_ip()).data(), dst_port(),
        payload_len);
}

inline std::string ParsedUdpPacket::to_json() const {
    if (!udp) return "{\"valid\":false}";
    return std::format(
        "{{\"src_ip\":\"{}\",\"src_port\":{},\"dst_ip\":\"{}\",\"dst_port\":{},"
        "\"payload_len\":{}}}",
        format_ipv4(src_ip()).data(), src_port(),
        format_ipv4(dst_ip()).data(), dst_port(),
        payload_len);
}

} // namespace eph::dpdk::net

/// @brief std::formatter specialization for ParsedPacket.
///
/// Formats as the one-line dump string showing IPs, ports, flags, and payload.
template <>
struct std::formatter<eph::dpdk::net::ParsedPacket> : std::formatter<std::string> {
    auto format(const eph::dpdk::net::ParsedPacket& p, auto& ctx) const {
        return std::formatter<std::string>::format(p.dump(), ctx);
    }
};

/// @brief std::formatter specialization for ParsedUdpPacket.
template <>
struct std::formatter<eph::dpdk::net::ParsedUdpPacket> : std::formatter<std::string> {
    auto format(const eph::dpdk::net::ParsedUdpPacket& p, auto& ctx) const {
        return std::formatter<std::string>::format(p.dump(), ctx);
    }
};

