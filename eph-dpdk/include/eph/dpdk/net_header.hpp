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
#include <cstring>
#include <format>
#include <string>

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_tcp.h>

// Logger factory (LoggerName, get_logger) — re-exported for backward compat.
#include "eph/dpdk/detail/logger.hpp"

namespace eph::dpdk::net {

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

/// @name EtherType and IP protocol constants
/// @{
inline constexpr uint16_t kEtherTypeIpv4   = 0x0800;  ///< EtherType for IPv4 (IEEE 802.3)
inline constexpr uint8_t  kIpProtoTcp      = 6;       ///< IP protocol number for TCP (RFC 793)
inline constexpr uint8_t  kIpProtoUdp      = 17;      ///< IP protocol number for UDP (RFC 768)
/// @}

/// @name Header length constants (bytes)
/// @{
inline constexpr uint16_t kIpv4HeaderLen   = 20;  ///< IPv4 header without options
inline constexpr uint16_t kTcpHeaderLen    = 20;  ///< TCP header without options
inline constexpr uint16_t kEtherHeaderLen  = 14;  ///< Ethernet II header (dst + src + type)
inline constexpr uint16_t kUdpHeaderLen    = 8;   ///< UDP header without options (RFC 768)
inline constexpr uint16_t kAllHeadersLen   = kEtherHeaderLen + kIpv4HeaderLen + kTcpHeaderLen;  ///< Combined Eth+IP+TCP header length (54 bytes)
inline constexpr uint16_t kUdpAllHeadersLen = kEtherHeaderLen + kIpv4HeaderLen + kUdpHeaderLen;  ///< Combined Eth+IP+UDP header length (42 bytes)
/// @}

// ─────────────────────────────────────────────────────────────────────────────
// UDP header (packed, wire format)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief UDP header structure matching the wire format (RFC 768).
///
/// All fields are in network byte order. 8 bytes, packed.
struct UdpHeader {
    uint16_t src_port;   ///< Source port (network byte order)
    uint16_t dst_port;   ///< Destination port (network byte order)
    uint16_t length;     ///< UDP header + payload length (network byte order)
    uint16_t checksum;   ///< Checksum (0 = disabled, optional for IPv4)
} __attribute__((packed));

static_assert(sizeof(UdpHeader) == kUdpHeaderLen);

/// @name TCP flag bitmasks (RFC 793 section 3.1)
/// @{
inline constexpr uint8_t kTcpFin = 0x01;  ///< No more data from sender
inline constexpr uint8_t kTcpSyn = 0x02;  ///< Synchronize sequence numbers
inline constexpr uint8_t kTcpRst = 0x04;  ///< Reset the connection
inline constexpr uint8_t kTcpPsh = 0x08;  ///< Push buffered data to application
inline constexpr uint8_t kTcpAck = 0x10;  ///< Acknowledgment field is significant
inline constexpr uint8_t kTcpUrg = 0x20;  ///< Urgent pointer field is significant
/// @}

/// @name IPv4 header defaults
/// @{
inline constexpr uint8_t  kIpv4VersionIhl5 = 0x45;   ///< Version 4, IHL 5 (20 bytes, no options)
inline constexpr uint16_t kIpDontFragment   = 0x4000; ///< Don't Fragment flag in fragment_offset field
inline constexpr uint8_t  kDefaultTtl       = 64;     ///< Default Time-To-Live hop count
/// @}

/// @brief Default TCP Maximum Segment Size for standard Ethernet (MTU 1500 - IP header - TCP header).
inline constexpr uint16_t kDefaultMss = 1460;

/// @brief SYN options total length: MSS(4) + SACK_PERM(2) + NOP(1) + WSCALE(3) + NOP(1) + NOP(1) = 12 bytes.
inline constexpr uint16_t kSynOptionsLen = 12;

/// @brief TCP header length for SYN packets (standard header + SYN options = 32 bytes).
inline constexpr uint16_t kSynTcpHeaderLen = kTcpHeaderLen + kSynOptionsLen;

// ─────────────────────────────────────────────────────────────────────────────
// Byte order helpers
// ─────────────────────────────────────────────────────────────────────────────

/// Host-to-network 16-bit (constexpr-safe).
[[nodiscard]] constexpr uint16_t hton16(uint16_t h) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return static_cast<uint16_t>((h >> 8) | (h << 8));
    } else {
        return h;
    }
}

/// Host-to-network 32-bit (constexpr-safe).
[[nodiscard]] constexpr uint32_t hton32(uint32_t h) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return ((h >> 24) & 0xFF) |
               ((h >> 8)  & 0xFF00) |
               ((h << 8)  & 0xFF0000) |
               ((h << 24) & 0xFF000000);
    } else {
        return h;
    }
}

/// @brief Network-to-host 16-bit (constexpr-safe). Identical to hton16 (symmetric).
[[nodiscard]] constexpr uint16_t ntoh16(uint16_t n) noexcept { return hton16(n); }

/// @brief Network-to-host 32-bit (constexpr-safe). Identical to hton32 (symmetric).
[[nodiscard]] constexpr uint32_t ntoh32(uint32_t n) noexcept { return hton32(n); }

// ─────────────────────────────────────────────────────────────────────────────
// Checksum computation
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Compute Internet checksum (RFC 1071) over typed byte data.
///
/// Sums 16-bit words with end-around carry, then one's-complements the result.
/// Handles odd-length buffers by zero-padding the final byte.
/// constexpr-evaluable: can be used in static_assert for compile-time
/// checksum verification of fixed headers.
///
/// @param data  Pointer to the byte data to checksum
/// @param len   Length of the data in bytes
/// @return One's complement checksum in network byte order
[[nodiscard]] constexpr uint16_t internet_checksum(const uint8_t* data, size_t len) noexcept {
    if (len == 0 || !data) return 0xFFFF;
    uint32_t sum = 0;

    // Sum 16-bit words
    size_t i = 0;
    while (i + 1 < len) {
        // Reconstruct uint16_t from two bytes — constexpr-safe (no memcpy needed).
        uint16_t word = static_cast<uint16_t>(data[i]) |
                        static_cast<uint16_t>(static_cast<uint16_t>(data[i + 1]) << 8);
        sum += word;
        i += 2;
    }

    // Handle odd byte
    if (i < len) {
        sum += data[i];
    }

    // Fold 32-bit sum to 16-bit
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return static_cast<uint16_t>(~sum);
}

/// @brief Overload accepting const void* for backward compatibility with
///        DPDK structs (rte_ipv4_hdr, etc.) passed as opaque pointers.
///
/// Delegates to the constexpr uint8_t* overload at runtime.
/// Not constexpr because void*->uint8_t* reinterpret is not constexpr before C++26.
[[nodiscard]] inline uint16_t internet_checksum(const void* data, size_t len) noexcept {
    return internet_checksum(static_cast<const uint8_t*>(data), len);
}

/// @brief Compute TCP/UDP pseudo-header checksum contribution (RFC 793 section 3.1).
///
/// Returns the partial sum (NOT one's-complemented) for inclusion in the full
/// TCP or UDP checksum calculation.
///
/// @param src_ip_net    Source IP address in network byte order
/// @param dst_ip_net    Destination IP address in network byte order
/// @param protocol      IP protocol number (e.g., kIpProtoTcp = 6)
/// @param tcp_len_host  TCP/UDP segment length (header + payload) in host byte order
/// @return Partial 32-bit sum in network byte order format
[[nodiscard]] inline uint32_t pseudo_header_sum(uint32_t src_ip_net, uint32_t dst_ip_net,
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

/// @brief Compute the full TCP checksum including the pseudo-header.
///
/// Combines the pseudo-header sum with the TCP segment checksum per RFC 793.
///
/// @param src_ip_net     Source IP in network byte order
/// @param dst_ip_net     Destination IP in network byte order
/// @param tcp_seg        Pointer to the TCP header (followed by payload)
/// @param total_tcp_len  Total length of TCP header + payload in bytes
/// @return TCP checksum in network byte order, ready to store in tcp->cksum
[[nodiscard]] inline uint16_t tcp_checksum(uint32_t src_ip_net, uint32_t dst_ip_net,
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

/// @brief Compute the full UDP checksum including the pseudo-header.
///
/// Combines the pseudo-header sum with the UDP segment checksum per RFC 768.
/// Primarily useful for test verification of NIC checksum offload correctness.
///
/// @param src_ip_net     Source IP in network byte order
/// @param dst_ip_net     Destination IP in network byte order
/// @param udp_seg        Pointer to the UDP header (followed by payload)
/// @param total_udp_len  Total length of UDP header + payload in bytes
/// @return UDP checksum in network byte order, ready to store in udp->checksum
[[nodiscard]] inline uint16_t udp_checksum(uint32_t src_ip_net, uint32_t dst_ip_net,
                              const void* udp_seg, uint16_t total_udp_len) noexcept {
    uint32_t sum = pseudo_header_sum(src_ip_net, dst_ip_net, kIpProtoUdp, total_udp_len);

    auto ptr = static_cast<const uint8_t*>(udp_seg);
    size_t len = total_udp_len;

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

    // RFC 768: if computed checksum is zero, transmit as 0xFFFF
    auto result = static_cast<uint16_t>(~sum);
    return result == 0 ? static_cast<uint16_t>(0xFFFF) : result;
}

// ─────────────────────────────────────────────────────────────────────────────
// TCP SYN options
// ─────────────────────────────────────────────────────────────────────────────

/// Write standard TCP SYN options into buffer.
/// Layout: MSS(4) + SACK_PERM(2) + NOP(1) + WSCALE(3) + NOP(1) + NOP(1) = 12 bytes
/// @param buf  Pointer to option area (right after 20-byte TCP header)
/// @param mss  MSS value in host byte order
/// @return Number of bytes written (always kSynOptionsLen = 12)
[[nodiscard]] inline uint16_t write_syn_options(uint8_t* buf, uint16_t mss) noexcept {
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

/// @brief Identifies a TCP connection by its 4-tuple (source/destination IP and port).
///
/// All fields are stored in HOST byte order. Conversion to/from network byte
/// order happens at the packet construction/parsing boundary (PacketTemplate,
/// parse_packet).
struct ConnectionTuple {
    uint32_t src_ip    = 0;  ///< Source IPv4 address (host byte order)
    uint32_t dst_ip    = 0;  ///< Destination IPv4 address (host byte order)
    uint16_t src_port  = 0;  ///< Source TCP port (host byte order)
    uint16_t dst_port  = 0;  ///< Destination TCP port (host byte order)

    /// @brief Defaulted equality comparison over all four fields.
    [[nodiscard]] bool operator==(const ConnectionTuple&) const = default;

    /// Validate all four fields are non-zero (required for a valid TCP connection).
    /// @return empty string_view on success, error description on failure.
    [[nodiscard]] constexpr std::string_view validate() const noexcept {
        if (src_ip == 0) return "src_ip must not be zero";
        if (dst_ip == 0) return "dst_ip must not be zero";
        if (src_port == 0) return "src_port must not be zero";
        if (dst_port == 0) return "dst_port must not be zero";
        return {};
    }

    /// Human-readable dump for logging/debugging.
    /// Defined after format_ipv4() (see below).
    [[nodiscard]] inline std::string dump() const;

    /// JSON-formatted tuple for monitoring system integration.
    /// Defined after format_ipv4() (see below).
    [[nodiscard]] inline std::string to_json() const;
};

// ─────────────────────────────────────────────────────────────────────────────
// Packet builder — constructs Ethernet/IP/TCP headers on an mbuf
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Ethernet + IPv4 + TCP header template for fast packet construction.
///
/// Pre-fill static fields (MAC addresses, IP/port tuple) once at connection
/// setup, then update only dynamic fields (seq, ack, flags, payload length)
/// per packet on the hot path. Provides both allocating (build_packet) and
/// zero-alloc (fill_packet) construction methods.
///
/// @note Not thread-safe. Each TX thread or session must use its own
///       PacketTemplate instance. The ip_id counter is incremented per packet.
struct PacketTemplate {
    rte_ether_addr src_mac{};
    rte_ether_addr dst_mac{};
    ConnectionTuple tuple{};
    /// IP identification field, incremented per packet.
    /// @warning Not thread-safe — PacketTemplate must be used from a single
    ///          TX thread per session. Use separate PacketTemplate instances
    ///          for multi-threaded TX.
    uint16_t ip_id = 0;
    uint16_t mss = kDefaultMss; // MSS advertised in SYN options

    /// [P2] Enable NIC TX checksum offload (IP + TCP).
    /// When true, sets ol_flags and computes pseudo-header checksum only.
    /// When false, falls back to software checksum (default).
    /// NOTE: Callers must verify NIC TX checksum offload capability via
    /// Platform::dev_info (RTE_ETH_TX_OFFLOAD_IPV4_CKSUM / TCP_CKSUM)
    /// before enabling this flag; setting it on unsupported NICs produces
    /// silently corrupt packets.
    bool hw_cksum = false;

    /// Validate template fields. Returns empty string_view if valid.
    [[nodiscard]] constexpr std::string_view validate() const noexcept {
        if (tuple.src_ip == 0) return "src_ip must not be zero";
        if (tuple.dst_ip == 0) return "dst_ip must not be zero";
        if (tuple.src_port == 0) return "src_port must not be zero";
        if (tuple.dst_port == 0) return "dst_port must not be zero";
        if (mss == 0) return "mss must not be zero";
        if (mss > 9000) return "mss exceeds jumbo frame limit (9000)";
        return {};
    }

    /// Human-readable dump for logging/diagnostics.
    /// Defined after format_ipv4() (see below).
    [[nodiscard]] inline std::string dump() const;

    /// JSON-formatted template for monitoring system integration.
    /// Defined after format_ipv4() (see below).
    [[nodiscard]] inline std::string to_json() const;

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
        if (!pool) [[unlikely]] return nullptr;
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
        ip->version_ihl     = kIpv4VersionIhl5;
        ip->type_of_service  = 0;
        ip->total_length     = hton16(kIpv4HeaderLen + tcp_hdr_len + payload_len);
        ip->packet_id        = hton16(ip_id++);
        ip->fragment_offset  = hton16(kIpDontFragment);
        ip->time_to_live     = kDefaultTtl;
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
            [[maybe_unused]] auto syn_opt_len = write_syn_options(opt_ptr, mss);
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
    ///
    /// @note The header construction mirrors build_packet() but operates on a
    ///       pre-allocated mbuf without SYN option handling. Changes to header
    ///       fields must be synchronized across both methods.
    uint16_t fill_packet(rte_mbuf* mbuf,
                         uint32_t seq, uint32_t ack,
                         uint8_t flags, uint16_t window,
                         const void* payload = nullptr,
                         uint16_t payload_len = 0) noexcept {
        if (!mbuf) [[unlikely]] return 0;

        // SYN requires TCP options (MSS, SACK, WScale) — use build_packet() instead.
        if (flags & kTcpSyn) [[unlikely]] {
            // Programming error: use build_packet() for SYN segments
            return 0;
        }

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
        ip->version_ihl     = kIpv4VersionIhl5;
        ip->type_of_service  = 0;
        ip->total_length     = hton16(kIpv4HeaderLen + kTcpHeaderLen + payload_len);
        ip->packet_id        = hton16(ip_id++);
        ip->fragment_offset  = hton16(kIpDontFragment);
        ip->time_to_live     = kDefaultTtl;
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
// UDP packet template — precomputed Eth+IP+UDP header for fast TX
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Precomputed Ethernet + IPv4 + UDP header template for fast packet construction.
///
/// All static fields (MAC addresses, IP addresses, ports) are pre-converted to
/// network byte order and stored in a 42-byte header array at init() time.
/// Each send only updates 3 dynamic fields: ip_total_length, ip_id, udp_length.
///
/// TX hot path (fill): 1x memcpy(42B) + 3 stores + 1x memcpy(payload) + checksum.
///
/// @note Not thread-safe. Each TX thread must use its own UdpPacketTemplate
///       instance (ip_id_ is incremented per packet without synchronization).
struct UdpPacketTemplate {
    /// Precomputed 42-byte header: Ethernet(14) + IPv4(20) + UDP(8).
    /// Cache-line aligned for optimal memcpy performance on the TX hot path.
    alignas(64) uint8_t header_[kUdpAllHeadersLen]{};

    /// IP identification field, incremented per packet.
    uint16_t ip_id_{0};

    /// Enable NIC TX checksum offload (IP + UDP).
    /// When true: sets ol_flags, IP checksum = 0 (NIC fills), UDP checksum = pseudo-header.
    /// When false: IP checksum = software, UDP checksum = 0 (optional for IPv4, RFC 768).
    bool hw_cksum_{false};

    /// Initialize the precomputed header template from connection parameters.
    ///
    /// Pre-fills all static fields in network byte order. Dynamic fields
    /// (ip_total_length, ip_id, udp_length) are set to placeholder values
    /// and updated per-packet in fill()/build().
    ///
    /// @param src_mac   Source Ethernet address
    /// @param dst_mac   Destination Ethernet address (usually gateway MAC)
    /// @param src_ip    Source IPv4 address (host byte order)
    /// @param dst_ip    Destination IPv4 address (host byte order)
    /// @param src_port  Source UDP port (host byte order)
    /// @param dst_port  Destination UDP port (host byte order)
    /// @param hw_cksum  Enable NIC checksum offload
    void init(const rte_ether_addr& src_mac, const rte_ether_addr& dst_mac,
              uint32_t src_ip, uint32_t dst_ip,
              uint16_t src_port, uint16_t dst_port,
              bool hw_cksum = false) noexcept {
        hw_cksum_ = hw_cksum;

        auto* pkt = header_;

        // ── Ethernet header (14 bytes) ──
        auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt);
        rte_ether_addr_copy(&dst_mac, &eth->dst_addr);
        rte_ether_addr_copy(&src_mac, &eth->src_addr);
        eth->ether_type = hton16(kEtherTypeIpv4);

        // ── IPv4 header (20 bytes) ──
        auto* ip = reinterpret_cast<rte_ipv4_hdr*>(pkt + kEtherHeaderLen);
        std::memset(ip, 0, kIpv4HeaderLen);
        ip->version_ihl     = kIpv4VersionIhl5;
        ip->type_of_service  = 0;
        ip->total_length     = 0;  // Updated per packet in fill()
        ip->packet_id        = 0;  // Updated per packet in fill()
        ip->fragment_offset  = hton16(kIpDontFragment);
        ip->time_to_live     = kDefaultTtl;
        ip->next_proto_id    = kIpProtoUdp;
        ip->hdr_checksum     = 0;  // Computed per packet in fill()
        ip->src_addr         = hton32(src_ip);
        ip->dst_addr         = hton32(dst_ip);

        // ── UDP header (8 bytes) ──
        auto* udp = reinterpret_cast<UdpHeader*>(pkt + kEtherHeaderLen + kIpv4HeaderLen);
        udp->src_port = hton16(src_port);
        udp->dst_port = hton16(dst_port);
        udp->length   = 0;  // Updated per packet in fill()
        udp->checksum = 0;
    }

    /// Fill a pre-allocated mbuf with a UDP packet (hot path, zero-alloc).
    ///
    /// The mbuf must have at least kUdpAllHeadersLen + payload_len bytes
    /// of available space. The mbuf is reset before filling.
    ///
    /// @param mbuf         Pre-allocated mbuf (must not be null)
    /// @param payload      Payload data to copy after UDP header
    /// @param payload_len  Payload length in bytes
    /// @return Total bytes written (42 + payload_len), or 0 on failure
    uint16_t fill(rte_mbuf* mbuf,
                  const void* payload, uint16_t payload_len) noexcept {
        if (!mbuf) [[unlikely]] return 0;

        const uint16_t total_len = kUdpAllHeadersLen + payload_len;

        rte_pktmbuf_reset(mbuf);
        auto* pkt = reinterpret_cast<uint8_t*>(rte_pktmbuf_append(mbuf, total_len));
        if (!pkt) [[unlikely]] return 0;

        // Copy precomputed 42-byte header template
        std::memcpy(pkt, header_, kUdpAllHeadersLen);

        // Update dynamic fields
        auto* ip = reinterpret_cast<rte_ipv4_hdr*>(pkt + kEtherHeaderLen);
        ip->total_length = hton16(kIpv4HeaderLen + kUdpHeaderLen + payload_len);
        ip->packet_id    = hton16(ip_id_++);

        auto* udp = reinterpret_cast<UdpHeader*>(pkt + kEtherHeaderLen + kIpv4HeaderLen);
        udp->length = hton16(kUdpHeaderLen + payload_len);

        // Copy payload
        if (payload && payload_len > 0) {
            std::memcpy(pkt + kUdpAllHeadersLen, payload, payload_len);
        }

        // Checksum handling
        if (hw_cksum_) {
            // NIC computes both checksums — set offload metadata
            mbuf->ol_flags = RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_UDP_CKSUM;
            mbuf->l2_len = kEtherHeaderLen;
            mbuf->l3_len = kIpv4HeaderLen;
            mbuf->l4_len = kUdpHeaderLen;
            ip->hdr_checksum = 0;
            udp->checksum = rte_ipv4_phdr_cksum(ip, mbuf->ol_flags);
        } else {
            mbuf->ol_flags = 0;
            // IPv4 UDP checksum is optional (RFC 768) — set to 0 for lowest latency.
            // IP header checksum computed in software.
            udp->checksum = 0;
            ip->hdr_checksum = 0;
            ip->hdr_checksum = internet_checksum(ip, kIpv4HeaderLen);
        }

        return total_len;
    }

    /// Allocate an mbuf from the pool and fill it with a UDP packet.
    ///
    /// Convenience wrapper around fill() for callers who don't pre-allocate mbufs.
    ///
    /// @param pool         Mempool to allocate from (must not be null)
    /// @param payload      Payload data
    /// @param payload_len  Payload length in bytes
    /// @return Filled mbuf on success, nullptr on allocation failure
    rte_mbuf* build(rte_mempool* pool,
                    const void* payload, uint16_t payload_len) noexcept {
        if (!pool) [[unlikely]] return nullptr;
        rte_mbuf* mbuf = rte_pktmbuf_alloc(pool);
        if (!mbuf) [[unlikely]] return nullptr;

        if (fill(mbuf, payload, payload_len) == 0) {
            rte_pktmbuf_free(mbuf);
            return nullptr;
        }
        return mbuf;
    }

    /// Human-readable dump for logging/diagnostics.
    /// Defined after format_ipv4() (see below).
    [[nodiscard]] inline std::string dump() const;
};

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
    /// Defined after format_ipv4() (see below).
    [[nodiscard]] inline std::string dump() const;

    /// JSON-formatted packet summary for monitoring/logging.
    /// Returns "{\"valid\":false}" if the packet was not parsed.
    /// Defined after format_ipv4() (see below).
    [[nodiscard]] inline std::string to_json() const;
};

/// @brief Parse an Ethernet/IPv4/TCP packet from an mbuf (zero-copy).
///
/// Validates Ethernet type, IPv4 version/IHL, and TCP data offset before
/// returning pointers into the mbuf data buffer. Handles variable-length
/// IP headers (IHL > 5) and uses IP total_length (not mbuf pkt_len) to
/// compute payload size, avoiding NIC-padded bytes in short frames.
///
/// @param mbuf  Received packet mbuf (must not be null)
/// @return ParsedPacket with all fields populated on success, or all-null
///         fields if the packet is not a valid IPv4/TCP packet
///
/// @note Three safety guards prevent buffer over-read and integer underflow:
///       1. TCP data offset must not exceed IP total length
///       2. IP total length must not exceed mbuf data length
///       3. Payload pointer is only set when actual bytes exist beyond TCP header
[[nodiscard]] inline ParsedPacket parse_packet(const rte_mbuf* mbuf) noexcept {
    if (!mbuf) [[unlikely]] return {};
    const uint16_t pkt_len = rte_pktmbuf_data_len(mbuf);
    if (pkt_len < kAllHeadersLen) return {};

    const auto* data = rte_pktmbuf_mtod(mbuf, const uint8_t*);

    // Validate all headers before populating result (fewer branch deps)
    auto* eth = reinterpret_cast<const rte_ether_hdr*>(data);
    if (ntoh16(eth->ether_type) != kEtherTypeIpv4) return {};

    auto* ip = reinterpret_cast<const rte_ipv4_hdr*>(data + kEtherHeaderLen);
    if ((ip->version_ihl >> 4) != 4) return {};  // Not IPv4
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

    // Guard 1: Prevents uint16_t underflow — if TCP data offset exceeds IP
    // total length, (ip_total - data_start) would wrap to a huge value.
    if (data_start > ip_total) return {};

    // Guard 2: Prevents buffer over-read — rejects packets where the IP
    // header claims more data than the mbuf actually contains (corrupt or
    // malicious ip_total).
    if (kEtherHeaderLen + ip_total > pkt_len) return {};

    // Guard 3: Only assign payload when there are actual bytes beyond the
    // TCP header. Combined with guards 1 and 2, this guarantees
    // payload_len > 0 and payload points within [data, data + pkt_len).
    if (ip_total > data_start) {
        uint16_t payload_offset = kEtherHeaderLen + data_start;
        result.payload     = data + payload_offset;
        result.payload_len = ip_total - data_start;
    }

    return result;
}

/// @brief Parse a dotted-decimal IPv4 address string "a.b.c.d" to host-order uint32_t.
///
/// Validates each octet is in [0, 255] and rejects trailing characters.
/// Does NOT accept leading zeros, whitespace, or port suffixes.
///
/// @param str  Null-terminated IPv4 string (e.g., "10.0.0.1")
/// @return Host-order uint32_t on success, 0 on invalid input
///
/// @note Returns 0 for both invalid input and the valid address "0.0.0.0".
///       Callers that need to distinguish these cases should check the input
///       string directly.
[[nodiscard]] inline uint32_t parse_ipv4(const char* str) noexcept {
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

/// @brief Format a host-order IPv4 address as "a.b.c.d" into a fixed-size array.
///
/// Returns a stack-allocated null-terminated char array. Use `.data()` to
/// get a `const char*`. The array is valid for the lifetime of the returned
/// object; when used as a temporary in a logging call, the expression's
/// lifetime guarantees safety.
///
/// @param ip  IPv4 address in host byte order
/// @return Null-terminated char array containing the formatted address
[[nodiscard]] inline std::array<char, 16> format_ipv4(uint32_t ip) noexcept {
    std::array<char, 16> buf{};
    // std::format_to for type-safety; result is always <= 15 chars ("255.255.255.255").
    auto [end, _] = std::format_to_n(buf.data(), buf.size() - 1, "{}.{}.{}.{}",
        (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
        (ip >> 8) & 0xFF, ip & 0xFF);
    *end = '\0';
    return buf;
}

/// @brief Format a MAC address as "xx:xx:xx:xx:xx:xx" into a fixed-size array.
///
/// @param mac  Ethernet MAC address (rte_ether_addr)
/// @return Null-terminated char array containing the formatted MAC
[[nodiscard]] inline std::array<char, 18> format_mac(const rte_ether_addr& mac) noexcept {
    std::array<char, 18> buf{};
    // std::format_to for type-safety; result is always exactly 17 chars ("xx:xx:xx:xx:xx:xx").
    auto [end, _] = std::format_to_n(buf.data(), buf.size() - 1,
        "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
        mac.addr_bytes[0], mac.addr_bytes[1], mac.addr_bytes[2],
        mac.addr_bytes[3], mac.addr_bytes[4], mac.addr_bytes[5]);
    *end = '\0';
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
// ConnectionTuple method definitions (deferred because format_ipv4 is above)
// ─────────────────────────────────────────────────────────────────────────────

inline std::string ConnectionTuple::dump() const {
    return std::format("{}:{} -> {}:{}",
        format_ipv4(src_ip).data(), src_port,
        format_ipv4(dst_ip).data(), dst_port);
}

inline std::string ConnectionTuple::to_json() const {
    return std::format(
        "{{\"src_ip\":\"{}\",\"src_port\":{},\"dst_ip\":\"{}\",\"dst_port\":{}}}",
        format_ipv4(src_ip).data(), src_port,
        format_ipv4(dst_ip).data(), dst_port);
}

inline std::string PacketTemplate::dump() const {
    return std::format("PacketTemplate({} -> {}, {}:{} -> {}:{}, mss={}, ip_id={}, hw_cksum={})",
        format_mac(src_mac).data(), format_mac(dst_mac).data(),
        format_ipv4(tuple.src_ip).data(), tuple.src_port,
        format_ipv4(tuple.dst_ip).data(), tuple.dst_port,
        mss, ip_id, hw_cksum);
}

inline std::string PacketTemplate::to_json() const {
    return std::format(
        "{{\"src_mac\":\"{}\",\"dst_mac\":\"{}\","
        "\"src_ip\":\"{}\",\"dst_ip\":\"{}\","
        "\"src_port\":{},\"dst_port\":{},"
        "\"mss\":{},\"ip_id\":{},\"hw_cksum\":{}}}",
        format_mac(src_mac).data(), format_mac(dst_mac).data(),
        format_ipv4(tuple.src_ip).data(), format_ipv4(tuple.dst_ip).data(),
        tuple.src_port, tuple.dst_port,
        mss, ip_id, hw_cksum ? "true" : "false");
}

inline std::string UdpPacketTemplate::dump() const {
    auto* ip = reinterpret_cast<const rte_ipv4_hdr*>(header_ + kEtherHeaderLen);
    auto* udp = reinterpret_cast<const UdpHeader*>(header_ + kEtherHeaderLen + kIpv4HeaderLen);
    return std::format("UdpPacketTemplate({}:{} -> {}:{}, hw_cksum={})",
                       format_ipv4(ntoh32(ip->src_addr)).data(),
                       ntoh16(udp->src_port),
                       format_ipv4(ntoh32(ip->dst_addr)).data(),
                       ntoh16(udp->dst_port),
                       hw_cksum_);
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
// Ephemeral port generation
// ─────────────────────────────────────────────────────────────────────────────

/// @name IANA ephemeral port range (RFC 6335 section 6)
/// @{
inline constexpr uint16_t kEphemeralPortMin   = 49152;
inline constexpr uint16_t kEphemeralPortRange = 16384; // 65536 - 49152, must be 2^n
/// @}

} // namespace eph::dpdk::net

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specialization for ConnectionTuple
// ─────────────────────────────────────────────────────────────────────────────

template <>
struct std::formatter<eph::dpdk::net::ConnectionTuple> : std::formatter<std::string> {
    auto format(const eph::dpdk::net::ConnectionTuple& t, auto& ctx) const {
        return std::formatter<std::string>::format(t.dump(), ctx);
    }
};

/// @brief std::formatter specialization for ParsedPacket.
///
/// Formats as the one-line dump string showing IPs, ports, flags, and payload.
template <>
struct std::formatter<eph::dpdk::net::ParsedPacket> : std::formatter<std::string> {
    auto format(const eph::dpdk::net::ParsedPacket& p, auto& ctx) const {
        return std::formatter<std::string>::format(p.dump(), ctx);
    }
};

/// @brief std::formatter specialization for PacketTemplate.
///
/// Formats as a compact one-line summary with MACs, IPs, ports, and MSS.
template <>
struct std::formatter<eph::dpdk::net::PacketTemplate> : std::formatter<std::string> {
    auto format(const eph::dpdk::net::PacketTemplate& t, auto& ctx) const {
        return std::formatter<std::string>::format(t.dump(), ctx);
    }
};
