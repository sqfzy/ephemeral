#pragma once

/// @file dns.hpp
/// User-space DNS resolution over DPDK data plane.
///
/// Sends DNS A-record queries as UDP packets directly through the NIC,
/// bypassing the kernel network stack. This is essential for exclusive-mode
/// PMDs where getaddrinfo() is unavailable after EAL initialization.
///
/// Design follows the same blocking send-poll-retry pattern as arp.hpp.
///
/// Usage:
///   // After ARP resolution (need gateway MAC for Ethernet framing)
///   dns::DnsConfig dns_cfg{.nameserver_ip = 0x08080808};  // 8.8.8.8
///   auto ip = dns::resolve(port_id, queue_id, pool,
///                          src_mac, gateway_mac, src_ip,
///                          "example.com", dns_cfg);

#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <openssl/rand.h>

#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_mbuf.h>

#include "eph/dpdk/net_header.hpp"

namespace eph::dpdk::dns {

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

/// @name Protocol constants
/// @{
inline constexpr uint16_t kUdpHeaderLen    = 8;    ///< UDP header length (RFC 768)
inline constexpr uint16_t kDnsHeaderLen    = 12;   ///< DNS header length (RFC 1035)
inline constexpr uint16_t kDnsPort         = 53;   ///< Standard DNS port
inline constexpr uint16_t kMaxDnsPacketLen = 512;  ///< RFC 1035 max UDP DNS message length
/// @}

/// @name DNS flag bitmasks (RFC 1035 section 4.1.1)
/// @{
inline constexpr uint16_t kDnsFlagQr       = 0x8000;  ///< Query/Response flag (1 = response)
inline constexpr uint16_t kDnsFlagRd       = 0x0100;  ///< Recursion Desired
inline constexpr uint16_t kDnsRcodeMask    = 0x000F;   ///< Response code mask (bits 0-3)
/// @}

/// @name DNS record type and class constants
/// @{
inline constexpr uint16_t kDnsTypeA        = 1;   ///< A record (IPv4 address)
inline constexpr uint16_t kDnsClassIn      = 1;   ///< Internet class
/// @}

// ─────────────────────────────────────────────────────────────────────────────
// Wire structures
// ─────────────────────────────────────────────────────────────────────────────

/// @brief UDP header structure matching the wire format (RFC 768).
///
/// All fields are in network byte order. 8 bytes, packed.
struct UdpHeader {
    uint16_t src_port;   ///< Source port (network byte order)
    uint16_t dst_port;   ///< Destination port (network byte order)
    uint16_t length;     ///< UDP header + payload length (network byte order)
    uint16_t checksum;   ///< Checksum (0 = disabled, optional for IPv4 UDP)
} __attribute__((packed));

static_assert(sizeof(UdpHeader) == kUdpHeaderLen);

/// @brief DNS message header (RFC 1035 section 4.1.1).
///
/// All fields are in network byte order. 12 bytes, packed.
struct DnsHeader {
    uint16_t id;         ///< Transaction ID (echoed in response)
    uint16_t flags;      ///< QR, Opcode, AA, TC, RD, RA, Z, RCODE
    uint16_t qd_count;   ///< Number of questions
    uint16_t an_count;   ///< Number of answer resource records
    uint16_t ns_count;   ///< Number of authority resource records
    uint16_t ar_count;   ///< Number of additional resource records
} __attribute__((packed));

static_assert(sizeof(DnsHeader) == kDnsHeaderLen);

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

/// DNS resolver configuration.
struct DnsConfig {
    uint32_t nameserver_ip = 0x08080808;  ///< DNS server (host order), default 8.8.8.8
    uint16_t port          = kDnsPort;
    std::chrono::milliseconds timeout{3000};

    [[nodiscard]] friend bool operator==(const DnsConfig&,
                                          const DnsConfig&) = default;

    /// Multi-line formatted dump for logging/debugging.
    [[nodiscard]] std::string dump() const {
        return std::format(
            "DnsConfig:\n"
            "  nameserver: {}:{}\n"
            "  timeout: {}ms",
            net::format_ipv4(nameserver_ip).data(), port, timeout.count());
    }

    /// Validate configuration, returning an error description or empty string on success.
    [[nodiscard]] constexpr std::string_view validate() const noexcept {
        if (nameserver_ip == 0)
            return "nameserver_ip must not be 0";
        if (port == 0)
            return "port must not be 0";
        if (timeout.count() <= 0)
            return "timeout must be positive";
        return {};
    }

    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"nameserver_ip\":\"{}\",\"port\":{},\"timeout_ms\":{}}}",
            net::format_ipv4(nameserver_ip).data(), port, timeout.count());
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Logger
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

inline spdlog::logger* dns_logger() {
    static auto l = [] {
        auto lg = spdlog::get("dpdk.dns");
        if (!lg) lg = spdlog::stdout_color_mt("dpdk.dns");
        return lg;
    }();
    return l.get();
}

// ─────────────────────────────────────────────────────────────────────────────
// Ephemeral port helper
// ─────────────────────────────────────────────────────────────────────────────

/// Generate a random ephemeral port in [49152, 65535].
/// Returns 0 on CSPRNG failure.
///
/// Range is a power of 2 so the modulo distributes uniformly — no low-value
/// bias from the truncation.
inline uint16_t random_ephemeral_port() noexcept {
    static constexpr uint16_t kMin   = 49152;
    static constexpr uint16_t kRange = 16384;  // 65536 - 49152, must be 2^n
    static_assert((kRange & (kRange - 1)) == 0,
                  "kRange must be a power of 2 for unbiased modulo");
    uint16_t rnd;
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&rnd), sizeof(rnd)) != 1)
        return 0;
    return kMin + (rnd % kRange);
}

// ─────────────────────────────────────────────────────────────────────────────
// DNS query encoding
// ─────────────────────────────────────────────────────────────────────────────

/// Encode a hostname into DNS wire format (QNAME).
/// "example.com" → "\x07example\x03com\x00"
/// @param out    Output buffer (must have at least hostname.size() + 2 bytes)
/// @param hostname  The hostname to encode
/// @return Number of bytes written, or 0 on error (label > 63 chars, total > 253)
inline size_t encode_qname(uint8_t* out, const std::string& hostname) noexcept {
    if (hostname.empty() || hostname.size() > 253) return 0;

    size_t pos = 0;
    size_t label_start = 0;

    for (size_t i = 0; i <= hostname.size(); ++i) {
        if (i == hostname.size() || hostname[i] == '.') {
            size_t label_len = i - label_start;
            if (label_len == 0 || label_len > 63) return 0;

            out[pos++] = static_cast<uint8_t>(label_len);
            std::memcpy(&out[pos], &hostname[label_start], label_len);
            pos += label_len;
            label_start = i + 1;
        }
    }

    out[pos++] = 0;  // Root label terminator
    return pos;
}

/// Build the complete DNS query payload (header + question section).
/// @param out       Output buffer (must have at least kMaxDnsPacketLen bytes)
/// @param tx_id     Transaction ID (network byte order)
/// @param hostname  Hostname to query
/// @return Total bytes written, or 0 on error
inline size_t build_dns_query(uint8_t* out, uint16_t tx_id,
                               const std::string& hostname) noexcept {
    // DNS header
    auto* hdr = reinterpret_cast<DnsHeader*>(out);
    hdr->id       = tx_id;
    hdr->flags    = net::hton16(kDnsFlagRd);  // Standard query, recursion desired
    hdr->qd_count = net::hton16(1);            // One question
    hdr->an_count = 0;
    hdr->ns_count = 0;
    hdr->ar_count = 0;

    size_t pos = kDnsHeaderLen;

    // Question section: QNAME + QTYPE + QCLASS
    size_t qname_len = encode_qname(&out[pos], hostname);
    if (qname_len == 0) return 0;
    pos += qname_len;

    // QTYPE = A (1)
    uint16_t qtype = net::hton16(kDnsTypeA);
    std::memcpy(&out[pos], &qtype, 2);
    pos += 2;

    // QCLASS = IN (1)
    uint16_t qclass = net::hton16(kDnsClassIn);
    std::memcpy(&out[pos], &qclass, 2);
    pos += 2;

    return pos;
}

// ─────────────────────────────────────────────────────────────────────────────
// DNS response parsing
// ─────────────────────────────────────────────────────────────────────────────

/// Skip a DNS name (handles label pointers and inline labels).
/// @param data   Start of DNS message
/// @param offset Current offset into data
/// @param len    Total length of DNS message
/// @return New offset after the name, or 0 on error
inline size_t skip_dns_name(const uint8_t* data, size_t offset, size_t len) noexcept {
    bool followed_pointer = false;
    size_t end_offset = 0;

    // Max iterations guards against pointer loops in malicious packets.
    // A valid DNS name has at most 127 labels (253 chars / 2 chars min per label),
    // so 128 iterations is generous.
    constexpr int kMaxIterations = 128;
    int iterations = 0;

    while (offset < len && iterations++ < kMaxIterations) {
        uint8_t label_len = data[offset];

        if (label_len == 0) {
            if (!followed_pointer) end_offset = offset + 1;
            return end_offset ? end_offset : offset + 1;
        }

        if ((label_len & 0xC0) == 0xC0) {
            if (offset + 1 >= len) return 0;
            if (!followed_pointer) end_offset = offset + 2;
            uint16_t ptr = static_cast<uint16_t>(
                ((label_len & 0x3F) << 8) | data[offset + 1]);
            if (ptr >= len) return 0;
            offset = ptr;
            followed_pointer = true;
            continue;
        }

        if (label_len > 63) return 0;
        // Bounds-check before advancing: a truncated packet must not
        // cause us to read or report an offset beyond the message.
        if (offset + 1 + label_len > len) return 0;
        offset += 1 + label_len;
    }

    return 0;  // Ran off the end or hit iteration limit
}

/// Parse a DNS response and extract the first A record IP address.
/// @param dns_data  Pointer to DNS message (starts at DnsHeader)
/// @param dns_len   Length of DNS message
/// @param tx_id     Expected transaction ID (network byte order)
/// @return IPv4 address in host byte order, or error string
[[nodiscard]] inline std::expected<uint32_t, std::string>
parse_dns_response(const uint8_t* dns_data, size_t dns_len,
                   uint16_t tx_id) noexcept {
    if (dns_len < kDnsHeaderLen) {
        return std::unexpected("DNS response too short");
    }

    auto* hdr = reinterpret_cast<const DnsHeader*>(dns_data);

    // Verify transaction ID
    if (hdr->id != tx_id) {
        return std::unexpected("DNS response: transaction ID mismatch");
    }

    // Check QR bit (must be response)
    uint16_t flags = net::ntoh16(hdr->flags);
    if (!(flags & kDnsFlagQr)) {
        return std::unexpected("DNS response: QR bit not set");
    }

    // Check RCODE
    uint16_t rcode = flags & kDnsRcodeMask;
    if (rcode != 0) {
        return std::unexpected(std::format("DNS response: RCODE={}", rcode));
    }

    uint16_t qd_count = net::ntoh16(hdr->qd_count);
    if (qd_count > 16) return std::unexpected("DNS response: unreasonable question count");  // Reject malformed/malicious packet
    uint16_t an_count = net::ntoh16(hdr->an_count);
    if (an_count > 64) return std::unexpected("DNS response: unreasonable answer count");  // Prevent CPU exhaustion from spoofed responses

    if (an_count == 0) {
        return std::unexpected("DNS response: no answer records");
    }

    // Skip question section
    size_t offset = kDnsHeaderLen;
    for (uint16_t i = 0; i < qd_count; ++i) {
        offset = skip_dns_name(dns_data, offset, dns_len);
        if (offset == 0) return std::unexpected("DNS response: malformed question");
        offset += 4;  // QTYPE(2) + QCLASS(2)
        if (offset > dns_len) return std::unexpected("DNS response: truncated question");
    }

    // Parse answer section — find first A record
    for (uint16_t i = 0; i < an_count; ++i) {
        offset = skip_dns_name(dns_data, offset, dns_len);
        if (offset == 0 || offset + 10 > dns_len) {
            return std::unexpected("DNS response: malformed answer RR");
        }

        uint16_t rr_type, rr_class, rdlength;
        std::memcpy(&rr_type, &dns_data[offset], 2);
        std::memcpy(&rr_class, &dns_data[offset + 2], 2);
        // Skip TTL (4 bytes)
        std::memcpy(&rdlength, &dns_data[offset + 8], 2);
        rr_type  = net::ntoh16(rr_type);
        rr_class = net::ntoh16(rr_class);
        rdlength = net::ntoh16(rdlength);

        offset += 10;  // TYPE(2) + CLASS(2) + TTL(4) + RDLENGTH(2)

        if (rdlength > kMaxDnsPacketLen || offset + rdlength > dns_len) {
            return std::unexpected("DNS response: RDATA exceeds packet");
        }

        if (rr_type == kDnsTypeA && rr_class == kDnsClassIn && rdlength == 4) {
            // A record: 4-byte IPv4 address in network byte order
            uint32_t ip_net;
            std::memcpy(&ip_net, &dns_data[offset], 4);
            return net::ntoh32(ip_net);
        }

        offset += rdlength;  // Skip this RR, try next
    }

    return std::unexpected("DNS response: no A record found");
}

// ─────────────────────────────────────────────────────────────────────────────
// Build complete DNS query packet (Ethernet + IPv4 + UDP + DNS)
// ─────────────────────────────────────────────────────────────────────────────

/// Build a DNS query UDP packet on an mbuf.
/// @return Allocated mbuf, or nullptr on failure
inline rte_mbuf* build_dns_packet(
    rte_mempool* pool,
    const rte_ether_addr& src_mac,
    const rte_ether_addr& dst_mac,
    uint32_t src_ip,          // host byte order
    uint32_t dst_ip,          // host byte order (nameserver)
    uint16_t src_port,        // host byte order
    uint16_t dst_port,        // host byte order
    const uint8_t* dns_payload,
    size_t dns_len) noexcept {

    // Guard against dns_len overflow when cast to uint16_t
    if (dns_len > kMaxDnsPacketLen) return nullptr;

    auto* mbuf = rte_pktmbuf_alloc(pool);
    if (!mbuf) return nullptr;

    constexpr uint16_t eth_len = net::kEtherHeaderLen;
    constexpr uint16_t ip_len  = net::kIpv4HeaderLen;
    uint16_t udp_total = kUdpHeaderLen + static_cast<uint16_t>(dns_len);
    uint16_t ip_total  = ip_len + udp_total;
    uint16_t frame_len = eth_len + ip_total;

    auto* pkt = reinterpret_cast<uint8_t*>(rte_pktmbuf_append(mbuf, frame_len));
    if (!pkt) {
        rte_pktmbuf_free(mbuf);
        return nullptr;
    }

    // ── Ethernet header ──
    auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt);
    rte_ether_addr_copy(&dst_mac, &eth->dst_addr);
    rte_ether_addr_copy(&src_mac, &eth->src_addr);
    eth->ether_type = net::hton16(net::kEtherTypeIpv4);

    // ── IPv4 header ──
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(pkt + eth_len);
    std::memset(ip, 0, ip_len);
    ip->version_ihl     = 0x45;  // IPv4, IHL=5 (20 bytes)
    ip->total_length    = net::hton16(ip_total);
    ip->time_to_live    = 64;
    ip->next_proto_id   = net::kIpProtoUdp;
    ip->src_addr        = net::hton32(src_ip);
    ip->dst_addr        = net::hton32(dst_ip);
    ip->hdr_checksum    = 0;
    ip->hdr_checksum    = net::internet_checksum(ip, ip_len);

    // ── UDP header ──
    auto* udp = reinterpret_cast<UdpHeader*>(pkt + eth_len + ip_len);
    udp->src_port = net::hton16(src_port);
    udp->dst_port = net::hton16(dst_port);
    udp->length   = net::hton16(udp_total);
    udp->checksum = 0;  // Optional for IPv4 UDP

    // ── DNS payload ──
    std::memcpy(pkt + eth_len + ip_len + kUdpHeaderLen, dns_payload, dns_len);

    return mbuf;
}

// ─────────────────────────────────────────────────────────────────────────────
// Parse received packet for DNS response
// ─────────────────────────────────────────────────────────────────────────────

/// Try to extract a DNS response from a received mbuf.
/// @return IPv4 address (host order) if valid DNS response, nullopt otherwise
inline std::optional<uint32_t>
try_parse_dns_packet(const rte_mbuf* mbuf, uint16_t tx_id,
                     uint32_t nameserver_ip) noexcept {
    constexpr size_t min_len = net::kEtherHeaderLen + net::kIpv4HeaderLen
                             + kUdpHeaderLen + kDnsHeaderLen;
    if (mbuf->data_len < min_len) return std::nullopt;

    auto* pkt = rte_pktmbuf_mtod(mbuf, const uint8_t*);

    // Check EtherType = IPv4
    auto* eth = reinterpret_cast<const rte_ether_hdr*>(pkt);
    if (net::ntoh16(eth->ether_type) != net::kEtherTypeIpv4) return std::nullopt;

    // Check IP protocol = UDP and source = nameserver
    auto* ip = reinterpret_cast<const rte_ipv4_hdr*>(pkt + net::kEtherHeaderLen);
    uint8_t ihl = (ip->version_ihl & 0x0F) << 2;
    if (ihl < net::kIpv4HeaderLen) return std::nullopt;
    if (ip->next_proto_id != net::kIpProtoUdp) return std::nullopt;
    if (net::ntoh32(ip->src_addr) != nameserver_ip) return std::nullopt;

    // Re-check total length with actual IHL — IP options can extend the header
    // beyond the 20-byte minimum used in min_len above.
    if (net::kEtherHeaderLen + ihl + kUdpHeaderLen + kDnsHeaderLen > mbuf->data_len)
        return std::nullopt;

    // Check UDP source port = 53
    auto* udp = reinterpret_cast<const UdpHeader*>(
        pkt + net::kEtherHeaderLen + ihl);
    if (net::ntoh16(udp->src_port) != kDnsPort) return std::nullopt;

    // Parse DNS payload
    const uint8_t* dns_data = pkt + net::kEtherHeaderLen + ihl
                            + kUdpHeaderLen;
    uint16_t udp_len = net::ntoh16(udp->length);
    if (udp_len < kUdpHeaderLen + kDnsHeaderLen) return std::nullopt;
    // Guard against truncated packets: the UDP length field may claim more
    // data than the mbuf actually contains.
    if (net::kEtherHeaderLen + ihl + udp_len > mbuf->data_len)
        return std::nullopt;
    size_t dns_len = udp_len - kUdpHeaderLen;

    auto result = detail::parse_dns_response(dns_data, dns_len, tx_id);
    if (result) return *result;

    SPDLOG_LOGGER_DEBUG(dns_logger(),
        "DNS packet from {} discarded: {}",
        net::format_ipv4(nameserver_ip).data(), result.error());
    return std::nullopt;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Public API: blocking DNS resolution over DPDK
// ─────────────────────────────────────────────────────────────────────────────

/// Resolve a hostname to an IPv4 address via DNS over DPDK.
///
/// Sends UDP DNS queries directly through the NIC and polls for responses.
/// Follows the same blocking send-poll-retry pattern as arp::resolve().
///
/// Must be called AFTER ARP resolution (needs gateway MAC for Ethernet framing)
/// but BEFORE TCP connection establishment.
///
/// @param port_id    DPDK port to send/receive on
/// @param queue_id   TX/RX queue to use
/// @param pool       Mempool for mbuf allocation
/// @param src_mac    Our NIC's MAC address
/// @param dst_mac    Gateway MAC address (from ARP resolution)
/// @param src_ip     Our IPv4 address (host byte order)
/// @param hostname   Hostname to resolve (e.g. "example.com")
/// @param cfg        DNS configuration (nameserver, port, timeout)
/// @return Resolved IPv4 address in host byte order, or error string
///
/// @note Security: DNS transaction IDs are 16-bit per protocol spec, making them
///       theoretically vulnerable to birthday attacks (~256 queries for 50% collision).
///       CSPRNG is used for tx_id generation to prevent prediction. For production use,
///       consider DNS-over-TLS or pre-resolved IPs to eliminate DNS attack surface entirely.
///       The response is validated against both tx_id and expected nameserver IP.
[[nodiscard]] inline std::expected<uint32_t, std::string>
resolve(uint16_t port_id,
        uint16_t queue_id,
        rte_mempool* pool,
        const rte_ether_addr& src_mac,
        const rte_ether_addr& dst_mac,
        uint32_t src_ip,
        const std::string& hostname,
        const DnsConfig& cfg = {}) {

    // Fast path: dotted-decimal IPv4 needs no pool, no nameserver, no network
    uint32_t ip = net::parse_ipv4(hostname.c_str());
    if (ip != 0) return ip;

    if (auto err = cfg.validate(); !err.empty()) {
        return std::unexpected(std::format("Invalid DNS config: {}", err));
    }

    [[maybe_unused]] auto log = detail::dns_logger();

    if (hostname.empty()) {
        return std::unexpected("DNS resolve: hostname is empty");
    }
    if (!pool) {
        return std::unexpected("DNS resolve: mempool is null");
    }
    if (cfg.nameserver_ip == 0) {
        return std::unexpected("DNS resolve: nameserver_ip is 0");
    }

    SPDLOG_LOGGER_DEBUG(log, "DNS resolve: {} via {} on port {} queue {}",
        hostname, net::format_ipv4(cfg.nameserver_ip).data(),
        port_id, queue_id);

    // Generate random transaction ID and ephemeral source port
    if (RAND_status() != 1) {
        SPDLOG_LOGGER_ERROR(log, "DNS resolve: OpenSSL CSPRNG not seeded — "
            "entropy pool uninitialized, cannot generate secure tx_id");
        return std::unexpected("DNS resolve: CSPRNG not seeded (call RAND_poll())");
    }
    uint16_t tx_id;
    if (RAND_bytes(reinterpret_cast<uint8_t*>(&tx_id), sizeof(tx_id)) != 1) {
        return std::unexpected("DNS resolve: CSPRNG failure for transaction ID");
    }
    uint16_t src_port = detail::random_ephemeral_port();
    if (src_port == 0) {
        return std::unexpected("DNS resolve: CSPRNG failure for ephemeral port");
    }

    // Build DNS query payload
    uint8_t dns_buf[kMaxDnsPacketLen];
    size_t dns_len = detail::build_dns_query(dns_buf, net::hton16(tx_id), hostname);
    if (dns_len == 0) {
        return std::unexpected(std::format(
            "DNS resolve: failed to encode query for '{}'", hostname));
    }

    auto deadline = std::chrono::steady_clock::now() + cfg.timeout;
    auto retry_interval = cfg.timeout / 3;
    if (retry_interval < std::chrono::milliseconds{100}) {
        retry_interval = std::chrono::milliseconds{100};
    }
    auto next_send = std::chrono::steady_clock::time_point{};
    int requests_sent = 0;

    // tx_id in network byte order for matching responses
    uint16_t tx_id_net = net::hton16(tx_id);

    while (std::chrono::steady_clock::now() < deadline) {
        // Send (or resend) DNS query
        auto now = std::chrono::steady_clock::now();
        if (now >= next_send && requests_sent < 3) {
            auto* pkt = detail::build_dns_packet(
                pool, src_mac, dst_mac, src_ip,
                cfg.nameserver_ip, src_port, cfg.port,
                dns_buf, dns_len);
            if (!pkt) {
                SPDLOG_LOGGER_ERROR(log, "DNS resolve: mbuf allocation failed");
                return std::unexpected("DNS resolve: mbuf allocation failed");
            }

            uint16_t sent = rte_eth_tx_burst(port_id, queue_id, &pkt, 1);
            if (sent != 1) {
                rte_pktmbuf_free(pkt);
                SPDLOG_LOGGER_WARN(log, "DNS resolve: tx_burst failed");
            } else {
                ++requests_sent;
                [[maybe_unused]] auto remaining_ms = std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                SPDLOG_LOGGER_DEBUG(log,
                    "DNS query #{} sent for '{}' (txid=0x{:04x}, {}ms remaining)",
                    requests_sent, hostname, tx_id, remaining_ms);
            }

            next_send = now + retry_interval;
        }

        // Poll for DNS response
        rte_mbuf* pkts[16];
        uint16_t nb_rx = rte_eth_rx_burst(port_id, queue_id, pkts, 16);

        for (uint16_t i = 0; i < nb_rx; ++i) {
            auto resolved_ip = detail::try_parse_dns_packet(
                pkts[i], tx_id_net, cfg.nameserver_ip);
            if (resolved_ip) {
                SPDLOG_LOGGER_INFO(log,
                    "DNS resolved: {} -> {} (after {} query/queries)",
                    hostname, net::format_ipv4(*resolved_ip).data(),
                    requests_sent);

                // Free remaining packets
                for (uint16_t j = i; j < nb_rx; ++j) {
                    rte_pktmbuf_free(pkts[j]);
                }
                return *resolved_ip;
            }
            rte_pktmbuf_free(pkts[i]);
        }
    }

    SPDLOG_LOGGER_ERROR(log,
        "DNS resolve timeout: '{}' not resolved after {}ms ({} queries sent)",
        hostname, cfg.timeout.count(), requests_sent);

    return std::unexpected(std::format(
        "DNS resolve timeout: '{}' not resolved after {}ms",
        hostname, cfg.timeout.count()));
}

} // namespace eph::dpdk::dns
