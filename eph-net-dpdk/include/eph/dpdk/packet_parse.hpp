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
/// L4 (TCP/UDP). Used by `DpdkPoller<>` dispatch to determine protocol
/// before committing to a full L4 parse, avoiding redundant IP header parsing.
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
    // Reject multi-segment mbufs. All downstream parsers use
    // `rte_pktmbuf_data_len` (first segment only), so a chained
    // mbuf where the full packet spans segments would let the
    // header sit in segment 0 while payload bytes extend into
    // segment 1 beyond data_len. Bounds checks would then pass
    // on segment-0 alone but report a payload slice that doesn't
    // actually fit in the contiguous buffer. This codebase
    // assumes standard-MTU traffic (single-segment mbuf). NICs
    // configured without RTE_ETH_RX_OFFLOAD_SCATTER never deliver
    // multi-segment RX; the check is defense-in-depth for any
    // topology that does enable scatter + jumbo and ensures the
    // bounds invariants in parse_tcp_from_ip / parse_udp_from_ip
    // hold unconditionally.
    if (mbuf->nb_segs > 1) [[unlikely]] return {};
    const uint16_t pkt_len = rte_pktmbuf_data_len(mbuf);
    if (pkt_len < kEtherHeaderLen + kIpv4HeaderLen) return {};

    const auto* data = rte_pktmbuf_mtod(mbuf, const uint8_t*);

    auto* eth = reinterpret_cast<const rte_ether_hdr*>(data);
    if (ntoh16(eth->ether_type) != kEtherTypeIpv4) return {};

    auto* ip = reinterpret_cast<const rte_ipv4_hdr*>(data + kEtherHeaderLen);
    if ((ip->version_ihl >> 4) != 4) return {};
    uint8_t ihl = (ip->version_ihl & 0x0F) << 2;
    if (ihl < kIpv4HeaderLen) return {};
    // Validate IHL against actual packet length (defense in depth).
    if (kEtherHeaderLen + ihl > pkt_len) return {};
    // Reject IP fragments. Our L4 parsers expect the full TCP/UDP/ICMP
    // header to sit at offset kEtherHeaderLen + ihl; that's only true
    // for the first fragment (MF=1, offset=0) OR an unfragmented packet.
    // A non-first fragment (offset != 0) carries arbitrary payload bytes
    // where a parser would otherwise read src/dst/seq/ack — accepting it
    // means a crafted fragment can impersonate a TCP header with any
    // 4-tuple. We don't do L3 reassembly in this backend (HFT workloads
    // set DF and negotiate MSS; fragments are either hostile or a sign
    // the path MTU is wrong). Drop both fragment variants at L3.
    const uint16_t frag_off = ntoh16(ip->fragment_offset);
    if ((frag_off & (kIpMoreFragments | kIpFragOffsetMask)) != 0) return {};

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
/// Skips L2/L3 parsing (already done by parse_ip_header). Use this in
/// `DpdkPoller<>` dispatch paths where parse_ip_header is called once for
/// protocol detection, then the appropriate L4 parser is invoked.
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
    if (tcp_doff < kTcpHeaderLen || tcp_doff > 60) return {};
    // Validate that the full TCP header (with options) fits in the packet.
    if (tcp_offset + tcp_doff > pkt_len) return {};

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
/// For `DpdkPoller<>` dispatch where protocol detection is done first, prefer
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
/// Skips L2/L3 parsing (already done by parse_ip_header). Use this in
/// `DpdkPoller<>` dispatch paths for zero-redundancy protocol dispatch.
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
    // Cross-check UDP length against IP total_length: the UDP datagram must
    // fit exactly inside the IP payload (ip_total_length - ihl). A mismatch
    // signals a malformed or tampered packet — reject rather than accept the
    // smaller of the two values, because either field being wrong is a red
    // flag worth losing the packet over.
    const uint16_t ip_total = ntoh16(ip_hdr.ip->total_length);
    if (ip_total < ip_hdr.ihl + kUdpHeaderLen) return {};
    if (udp_len != ip_total - ip_hdr.ihl) return {};

    ParsedUdpPacket result;
    result.eth = ip_hdr.eth;
    result.ip  = ip_hdr.ip;
    result.udp = udp;

    uint16_t payload_offset = udp_offset + kUdpHeaderLen;
    result.payload_len = udp_len - kUdpHeaderLen;
    // Always set payload pointer — even for zero-length datagrams (RFC 768).
    // Callers distinguish "no payload" (valid) from "parse failed" (null udp).
    result.payload = data + payload_offset;

    return result;
}

/// @brief Parse a UDP/IPv4/Ethernet packet from an mbuf (zero-copy).
///
/// Convenience wrapper that calls parse_ip_header() + parse_udp_from_ip().
/// For `DpdkPoller<>` dispatch, prefer the layered API directly.
///
/// @param mbuf  Received packet mbuf (must not be null)
/// @return ParsedUdpPacket with all fields on success, all-null if not valid UDP
[[nodiscard]] inline ParsedUdpPacket parse_udp_packet(const rte_mbuf* mbuf) noexcept {
    auto ip_hdr = parse_ip_header(mbuf);
    if (!ip_hdr || ip_hdr.proto != kIpProtoUdp) return {};
    return parse_udp_from_ip(mbuf, ip_hdr);
}

// ─────────────────────────────────────────────────────────────────────────────
// ICMP parser — RFC 792. We only care about Type 3 Code 4 (Fragmentation
// Needed and DF Set) for path-MTU feedback; other types are surfaced
// verbatim for diagnostic callers but their embedded-4-tuple fields are
// only populated for Type 3 Code 4 where the layout is well-defined.
// ─────────────────────────────────────────────────────────────────────────────

struct ParsedIcmp {
    const rte_ether_hdr* eth = nullptr;
    const rte_ipv4_hdr*  ip  = nullptr;
    uint8_t  type = 0;
    uint8_t  code = 0;

    // Populated only for Type 3 Code 4 when the embedded headers are
    // present and well-formed. Values are in host byte order.
    uint16_t next_hop_mtu     = 0;
    uint32_t embedded_src_ip  = 0;
    uint32_t embedded_dst_ip  = 0;
    uint16_t embedded_src_port = 0;
    uint16_t embedded_dst_port = 0;
    uint8_t  embedded_proto   = 0;
    bool     embedded_valid   = false;

    [[nodiscard]] bool is_frag_needed() const noexcept {
        return type == 3 && code == 4;
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return ip != nullptr;
    }
};

/// @brief Parse an Ethernet/IPv4/ICMP packet from an mbuf. Only fully
///        populates the embedded 4-tuple + next_hop_mtu for Type 3
///        Code 4 (Fragmentation Needed and DF Set), which is the only
///        ICMP variant TcpSession::on_icmp_frag_needed acts on.
///
/// Non-ICMP packets and malformed ICMP packets return a ParsedIcmp
/// whose `operator bool()` is false.
[[nodiscard]] inline ParsedIcmp parse_icmp(const rte_mbuf* mbuf) noexcept {
    auto ip_hdr = parse_ip_header(mbuf);
    if (!ip_hdr) return {};
    if (ip_hdr.proto != kIpProtoIcmp) return {};

    const uint16_t pkt_len = rte_pktmbuf_data_len(mbuf);
    const auto* data = rte_pktmbuf_mtod(mbuf, const uint8_t*);
    const uint16_t icmp_offset = kEtherHeaderLen + ip_hdr.ihl;
    // ICMP minimum header is 8 bytes; Type 3 extends to include the
    // next-hop MTU at offset 6..7.
    if (static_cast<uint32_t>(icmp_offset) + 8u > pkt_len) return {};

    ParsedIcmp out{};
    out.eth  = ip_hdr.eth;
    out.ip   = ip_hdr.ip;
    out.type = data[icmp_offset];
    out.code = data[icmp_offset + 1];

    if (out.is_frag_needed()) {
        // ICMP Type 3 Code 4 layout (RFC 1191 for the MTU field):
        //   0      type=3
        //   1      code=4
        //   2..3   checksum
        //   4..5   unused
        //   6..7   next-hop MTU (network order)
        //   8..    embedded original IP header + first 8 bytes of L4
        uint16_t mtu_net = 0;
        std::memcpy(&mtu_net, data + icmp_offset + 6, 2);
        out.next_hop_mtu = ntoh16(mtu_net);

        const uint16_t emb_ip_off = icmp_offset + 8;
        if (static_cast<uint32_t>(emb_ip_off) + kIpv4HeaderLen > pkt_len) return out;
        const auto* e_ip = reinterpret_cast<const rte_ipv4_hdr*>(data + emb_ip_off);
        if ((e_ip->version_ihl >> 4) != 4) return out;
        const uint8_t e_ihl = (e_ip->version_ihl & 0x0F) << 2;
        if (e_ihl < kIpv4HeaderLen) return out;
        if (static_cast<uint32_t>(emb_ip_off) + e_ihl + 4u > pkt_len) return out;

        out.embedded_src_ip = ntoh32(e_ip->src_addr);
        out.embedded_dst_ip = ntoh32(e_ip->dst_addr);
        out.embedded_proto  = e_ip->next_proto_id;

        // First 4 bytes of the embedded L4 are src_port + dst_port for
        // both TCP and UDP — sufficient to match against a registered
        // Pollable's 4-tuple. RFC 792 requires exactly the first 8 bytes
        // of the original L4 be included, so doing this for ICMP from
        // modern implementations is safe.
        if (out.embedded_proto == kIpProtoTcp ||
            out.embedded_proto == kIpProtoUdp) {
            uint16_t sp_net = 0, dp_net = 0;
            std::memcpy(&sp_net, data + emb_ip_off + e_ihl, 2);
            std::memcpy(&dp_net, data + emb_ip_off + e_ihl + 2, 2);
            out.embedded_src_port = ntoh16(sp_net);
            out.embedded_dst_port = ntoh16(dp_net);
            out.embedded_valid = true;
        }
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// TCP options parser — MSS / WSCALE / SACK_PERM from SYN / SYN-ACK
// ─────────────────────────────────────────────────────────────────────────────

/// @brief TCP options extracted from the options area of a SYN / SYN-ACK
///        TCP header (RFC 793 + RFC 1323).
///
/// Absent options leave their respective field at its default — callers
/// must inspect the matching `has_*` flag before using the value. Unknown
/// option kinds are silently skipped; malformed options stop the parse at
/// the first offending byte and return whatever was successfully parsed.
struct TcpOptions {
    uint16_t mss           = 0;      ///< MSS option value (host order); valid iff has_mss
    uint8_t  wscale        = 0;      ///< Window scale shift count; valid iff has_wscale
    bool     has_mss       = false;
    bool     has_wscale    = false;
    bool     has_sack_perm = false;
};

/// @brief Parse the TCP options area of a parsed packet.
///
/// The options area lives between the end of the fixed 20-byte TCP header
/// and the start of the payload, inclusive of up to 40 bytes (doff - 5
/// words). Handles kind=0 (EOL) and kind=1 (NOP) by the special-case
/// rules in RFC 793; every other option is length-delimited.
///
/// Only SYN / SYN-ACK packets typically carry meaningful options in this
/// codebase (we do not use Timestamps or SACK on the data path); a non-SYN
/// packet with no options area returns an all-default result.
///
/// @param parsed  A ParsedPacket from parse_packet() / parse_tcp_from_ip.
/// @return TcpOptions populated with whichever options were parsed.
[[nodiscard]] inline TcpOptions parse_tcp_options(const ParsedPacket& parsed) noexcept {
    TcpOptions out{};
    if (!parsed.tcp) return out;
    const uint8_t tcp_doff_bytes = (parsed.tcp->data_off >> 4) << 2;
    if (tcp_doff_bytes <= kTcpHeaderLen) return out;
    const uint16_t opt_len = tcp_doff_bytes - kTcpHeaderLen;

    // The options area begins immediately after the 20-byte fixed TCP
    // header. Deriving its pointer from `parsed.tcp` keeps the parser
    // layering-aware: the mbuf's backing buffer has already been bounds-
    // checked by parse_tcp_from_ip (which enforced doff <= 60 and
    // eth_hdr + ip_hdr + doff <= pkt_len).
    const uint8_t* opts =
        reinterpret_cast<const uint8_t*>(parsed.tcp) + kTcpHeaderLen;

    uint16_t i = 0;
    while (i < opt_len) {
        const uint8_t kind = opts[i];
        if (kind == 0) break;                  // End of Option List
        if (kind == 1) { ++i; continue; }      // NOP — single byte
        if (i + 1 >= opt_len) break;           // missing length byte
        const uint8_t len = opts[i + 1];
        if (len < 2) break;                    // malformed
        if (static_cast<uint16_t>(i + len) > opt_len) break;

        switch (kind) {
            case 2:  // Maximum Segment Size
                if (len == 4) {
                    uint16_t mss_net;
                    std::memcpy(&mss_net, &opts[i + 2], 2);
                    out.mss = ntoh16(mss_net);
                    out.has_mss = true;
                }
                break;
            case 3:  // Window Scale (RFC 1323)
                if (len == 3) {
                    out.wscale = opts[i + 2];
                    out.has_wscale = true;
                }
                break;
            case 4:  // SACK Permitted
                if (len == 2) {
                    out.has_sack_perm = true;
                }
                break;
            default:
                break;                         // unknown — skip
        }
        i += len;
    }
    return out;
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

