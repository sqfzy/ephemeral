#pragma once

/// @file packet_core.hpp
/// Network constants, byte-order helpers, checksum algorithms, and ConnectionTuple.
///
/// This is the stable foundation layer — rarely changes. Includes constants for
/// all protocol header lengths, byte-order conversion, Internet/TCP/UDP checksum
/// computation, IPv4 address formatting, and the protocol-agnostic ConnectionTuple.

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
inline constexpr uint8_t  kIpProtoIcmp     = 1;       ///< IP protocol number for ICMP (RFC 792)
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

/// @brief Identifies a network connection by its 4-tuple (source/destination IP and port).
///
/// All fields are stored in HOST byte order. Conversion to/from network byte
/// order happens at the packet construction/parsing boundary (PacketTemplate,
/// parse_packet).
struct ConnectionTuple {
    uint32_t src_ip    = 0;  ///< Source IPv4 address (host byte order)
    uint32_t dst_ip    = 0;  ///< Destination IPv4 address (host byte order)
    uint16_t src_port  = 0;  ///< Source port (host byte order)
    uint16_t dst_port  = 0;  ///< Destination port (host byte order)

    /// @brief Defaulted equality comparison over all four fields.
    [[nodiscard]] bool operator==(const ConnectionTuple&) const = default;

    /// Validate all four fields are non-zero (required for a valid TCP connection).
    /// @return empty string_view on success, error description on failure.
    [[nodiscard]] constexpr std::string_view validate() const noexcept {
        if (src_ip == 0) return "src_ip must not be zero";
        if (dst_ip == 0) return "dst_ip must not be zero";
        if (src_port == 0) return "src_port must be explicit (DPDK has no ephemeral port allocator)";
        if (dst_port == 0) return "dst_port must be explicit (DPDK has no ephemeral port allocator)";
        return {};
    }

    /// Human-readable dump for logging/debugging.
    /// Defined after format_ipv4() (see below).
    [[nodiscard]] inline std::string dump() const;

    /// JSON-formatted tuple for monitoring system integration.
    /// Defined after format_ipv4() (see below).
    [[nodiscard]] inline std::string to_json() const;
};


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

