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
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <sys/random.h>  // getrandom(2) for CSPRNG

#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_mbuf.h>

#include "eph/core/error.hpp"
#include "eph/dpdk/net_header.hpp"
#include "eph/net/dpdk/flow_steering.hpp"  // query_rss_state / queue_for_tuple — RSS-aware src_port selection
#include "eph/net/dpdk/poller.hpp"  // DpdkPoller<void> — needed for ~AsyncDnsResolverT auto-detach
#include "eph/utils/time.hpp"  // TSC::now() — canonical timer per CLAUDE.md

namespace eph::dpdk::dns {

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

/// @name Protocol constants
/// @{
inline constexpr uint16_t kDnsHeaderLen    = 12;   ///< DNS header length (RFC 1035)
inline constexpr uint16_t kDnsPort         = 53;   ///< Standard DNS port
inline constexpr uint16_t kMaxDnsPacketLen = 512;  ///< RFC 1035 max UDP DNS message length

/// @brief Burst size for the DNS-resolve RX poll. DNS resolve is a
///        startup-only blocking helper (not hot-path); same rationale
///        as `arp::kArpResolveBurstSize`.
inline constexpr uint16_t kDnsResolveBurstSize = 16;
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

/// UdpHeader is defined in net_header.hpp (shared with multicast.hpp).
using net::UdpHeader;
using net::kUdpHeaderLen;

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

    /// Check for non-fatal contradictions or likely misconfigurations.
    /// Returns a list of warning messages (empty if no issues).
    /// Unlike validate() which blocks operation, these are advisory.
    [[nodiscard]] std::vector<std::string> warnings() const {
        std::vector<std::string> w;
        // Loopback nameserver is unusual for DPDK (bypasses kernel)
        if ((nameserver_ip >> 24) == 127)
            w.emplace_back("nameserver_ip is in 127.0.0.0/8 (loopback) -- "
                           "DPDK bypasses the kernel, loopback DNS will not work");
        // Very short timeout may fail on WAN nameservers
        if (timeout.count() < 500)
            w.emplace_back(std::format(
                "timeout={}ms is very short -- DNS resolution may fail "
                "on high-latency nameservers", timeout.count()));
        // Non-standard port may be intentional but worth flagging
        if (port != kDnsPort && port != 0)
            w.emplace_back(std::format(
                "port={} is non-standard (DNS uses port 53) -- "
                "ensure your nameserver listens on this port", port));
        return w;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Logger
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

inline spdlog::logger* dns_logger() { return eph::dpdk::detail::get_logger<eph::dpdk::detail::LoggerName{"dpdk.dns"}>(); }

// ─────────────────────────────────────────────────────────────────────────────
// Ephemeral port helper
// ─────────────────────────────────────────────────────────────────────────────

/// Generate a random ephemeral port in [49152, 65535].
/// Returns 0 on CSPRNG failure.
///
/// Uses shared constants from net_header.hpp. Range is a power of 2
/// so the modulo distributes uniformly — no low-value bias.
inline uint16_t random_ephemeral_port() noexcept {
    static_assert((net::kEphemeralPortRange & (net::kEphemeralPortRange - 1)) == 0,
                  "kEphemeralPortRange must be a power of 2 for unbiased modulo");
    uint16_t rnd;
    // Kernel CSPRNG via getrandom(2).
    if (::getrandom(&rnd, sizeof(rnd), GRND_NONBLOCK) != sizeof(rnd))
        return 0;
    return net::kEphemeralPortMin + (rnd % net::kEphemeralPortRange);
}

/// Select a src_port for an outbound DNS query so that the resulting
/// reply's 5-tuple RSS-hashes back to the caller's RX queue.
///
/// On RSS-active NICs (multi-queue: `nb_rx_queues > 1`), delegates to
/// `find_src_port_for_queue` which walks the ephemeral port range and
/// returns the first port whose inbound 5-tuple hashes to @p queue_id.
/// The reply's 5-tuple is `(nameserver_ip, dns_server_port, local_ip,
/// our_chosen_src_port)`; our chosen port lands in the `dst_port` slot
/// of the RSS hash input, which is exactly where `find_src_port_for_queue`
/// puts its searched candidate.
///
/// On non-RSS / single-queue NICs, falls back to a random ephemeral
/// port (the pre-RSS behavior — preserves entropy for cache-poisoning
/// resistance when RSS routing is irrelevant). Detected via the
/// distinguished error prefix `RssHashPredictExhausted`: anything else
/// from `find_src_port_for_queue` (`rte_eth_dev_info_get failed`,
/// `rte_eth_dev_rss_reta_query failed`, etc.) means RSS is not
/// queryable on this port, so we can safely take the random path.
///
/// @param port_id           NIC port id
/// @param queue_id          RX queue caller will poll for the reply
/// @param nameserver_ip     DNS server IP (host byte order)
/// @param local_ip          Local IP (host byte order)
/// @param dns_server_port   DNS server's source port for the reply
///                          (typically 53 — the value the resolver also
///                          places into `udp->dst_port` of the outbound
///                          query; see kDnsPort default in DnsConfig)
/// @return chosen local src_port (host byte order), or unexpected with
///         a string detail (CSPRNG failure / RssHashPredictExhausted).
[[nodiscard]] inline std::expected<uint16_t, std::string>
select_dns_src_port(uint16_t port_id, uint16_t queue_id,
                    uint32_t nameserver_ip, uint32_t local_ip,
                    uint16_t dns_server_port) noexcept {
    [[maybe_unused]] auto* log = dns_logger();

    // RSS-aware reverse-pick. find_src_port_for_queue queries the NIC's
    // RSS state and loops the ephemeral port range, returning the first
    // local port whose inbound 5-tuple `(remote_ip, remote_port,
    // local_ip, sp)` hashes to queue_id.
    auto sp = ::eph::net::dpdk::find_src_port_for_queue(
        port_id, queue_id,
        /*remote_ip=*/nameserver_ip,
        /*remote_port=*/dns_server_port,
        /*local_ip=*/local_ip);
    if (sp) {
        SPDLOG_LOGGER_DEBUG(log,
            "select_dns_src_port: chose src_port={} for queue={} "
            "(nameserver={}, local={}, server_port={})",
            *sp, queue_id,
            net::format_ipv4(nameserver_ip).data(),
            net::format_ipv4(local_ip).data(),
            dns_server_port);
        return sp;
    }

    // Distinguish RSS-active-but-exhausted from RSS-not-configured.
    // The former is a hard caller error (no port hashes to the requested
    // queue under this NIC's RETA + key); the latter is the single-queue
    // / no-RSS path where any random ephemeral port works.
    if (sp.error().starts_with("RssHashPredictExhausted")) {
        return sp;  // surface as-is; caller misconfigured queue
    }

    SPDLOG_LOGGER_DEBUG(log,
        "select_dns_src_port: RSS state unavailable on port {} ({}); "
        "using random ephemeral port",
        port_id, sp.error());
    uint16_t p = random_ephemeral_port();
    if (p == 0) {
        return std::unexpected(
            "select_dns_src_port: CSPRNG failure for ephemeral port");
    }
    return p;
}

/// Pure variant of `select_dns_src_port` parametrized by an explicit
/// `RssState` snapshot — exposes the search loop for unit testing without
/// needing a real NIC.
///
/// Delegates to `find_src_port_for_queue_with_state` with
/// `start_hint=0` so DNS unit tests get the first matching port
/// deterministically (matching pre-dedup behavior). Production
/// callers go through `select_dns_src_port` → `find_src_port_for_queue`
/// which uses the auto-incrementing process-global hint for fan-out
/// distinctness; this test helper preserves the *deterministic* shape
/// the existing tests assert.
[[nodiscard]] inline std::expected<uint16_t, std::string>
select_dns_src_port_with_state(
    const ::eph::net::dpdk::RssState& state,
    uint16_t queue_id,
    uint32_t nameserver_ip, uint32_t local_ip,
    uint16_t dns_server_port,
    uint16_t port_range_start = 32768,
    uint16_t port_range_end   = 60999) noexcept {
    if (port_range_start > port_range_end) {
        return std::unexpected(std::format(
            "select_dns_src_port: inverted range [{},{}]",
            port_range_start, port_range_end));
    }
    auto r = ::eph::net::dpdk::find_src_port_for_queue_with_state(
        state, queue_id,
        /*remote_ip=*/  nameserver_ip,
        /*remote_port=*/dns_server_port,
        /*local_ip=*/   local_ip,
        port_range_start, port_range_end,
        /*start_hint=*/uint32_t{0});
    if (r) return r;
    // The wrapper's exhaustion message reads "find_src_port_for_queue:
    // ..." — re-stamp it to the DNS-flavoured message that the existing
    // tests grep for ("RssHashPredictExhausted: no DNS src_port..."), so
    // the contract surface is unchanged.
    if (r.error().starts_with("RssHashPredictExhausted")) {
        return std::unexpected(std::format(
            "RssHashPredictExhausted: no DNS src_port in [{},{}] hashes to queue {}",
            port_range_start, port_range_end, queue_id));
    }
    return r;  // surface inverted-range etc. as-is
}

// ─────────────────────────────────────────────────────────────────────────────
// DNS query encoding
// ─────────────────────────────────────────────────────────────────────────────

/// Encode a hostname into DNS wire format (QNAME).
/// "example.com" → "\x07example\x03com\x00"
/// @param out    Output buffer (must have at least hostname.size() + 2 bytes)
/// @param hostname  The hostname to encode
/// @return Number of bytes written, or 0 on error (label > 63 chars, total > 253)
[[nodiscard]] inline size_t encode_qname(uint8_t* out, const std::string& hostname) noexcept {
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
[[nodiscard]] inline size_t build_dns_query(uint8_t* out, uint16_t tx_id,
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
[[nodiscard]] inline size_t skip_dns_name(const uint8_t* data, size_t offset, size_t len) noexcept {
    bool followed_pointer = false;
    size_t end_offset = 0;

    // Max iterations guards against pointer loops in malicious packets.
    // A valid DNS name has at most 127 labels (253 chars / 2 chars min per
    // label), but realistic FQDNs have well under 32 labels and the parser
    // also follows compression pointers — each pointer hop costs one
    // iteration. Tightening from 128 → 32 narrows the attacker work budget
    // while still leaving 2-3× headroom for the deepest pathologically-deep
    // legal name (per audit-20260413-eph-net-dpdk.md item #11).
    constexpr int kMaxIterations = 32;
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
        return std::unexpected(std::format(
            "DNS response too short: dns_len={} bytes < kDnsHeaderLen={} bytes",
            dns_len, kDnsHeaderLen));
    }

    auto* hdr = reinterpret_cast<const DnsHeader*>(dns_data);

    // Verify transaction ID
    if (hdr->id != tx_id) {
        return std::unexpected(std::format(
            "DNS response: transaction ID mismatch (got=0x{:04x}, "
            "expected=0x{:04x}) — possible delayed reply from prior query "
            "or spoofed packet",
            hdr->id, tx_id));
    }

    // Check QR bit (must be response)
    uint16_t flags = net::ntoh16(hdr->flags);
    if (!(flags & kDnsFlagQr)) {
        return std::unexpected(std::format(
            "DNS response: QR bit not set (flags=0x{:04x}) — peer sent a "
            "query packet on the response port",
            flags));
    }

    // Check RCODE
    uint16_t rcode = flags & kDnsRcodeMask;
    if (rcode != 0) {
        return std::unexpected(std::format("DNS response: RCODE={}", rcode));
    }

    uint16_t qd_count = net::ntoh16(hdr->qd_count);
    if (qd_count > 16) return std::unexpected(std::format(
        "DNS response: unreasonable question count qd_count={} (cap=16); "
        "rejecting as malformed/malicious",
        qd_count));
    uint16_t an_count = net::ntoh16(hdr->an_count);
    if (an_count > 64) return std::unexpected(std::format(
        "DNS response: unreasonable answer count an_count={} (cap=64); "
        "rejecting to prevent CPU exhaustion from spoofed responses",
        an_count));

    if (an_count == 0) {
        return std::unexpected("DNS response: no answer records");
    }

    // Skip question section
    size_t offset = kDnsHeaderLen;
    for (uint16_t i = 0; i < qd_count; ++i) {
        offset = skip_dns_name(dns_data, offset, dns_len);
        if (offset == 0) return std::unexpected(std::format(
            "DNS response: malformed question name at qd[{}/{}], dns_len={}",
            i, qd_count, dns_len));
        offset += 4;  // QTYPE(2) + QCLASS(2)
        if (offset > dns_len) return std::unexpected(std::format(
            "DNS response: truncated question at qd[{}/{}] (offset={} > dns_len={})",
            i, qd_count, offset, dns_len));
    }

    // Parse answer section — find first A record
    for (uint16_t i = 0; i < an_count; ++i) {
        offset = skip_dns_name(dns_data, offset, dns_len);
        if (offset == 0 || offset + 10 > dns_len) {
            return std::unexpected(std::format(
                "DNS response: malformed answer RR at an[{}/{}] "
                "(offset={}, dns_len={}, need offset+10 bytes for fixed RR header)",
                i, an_count, offset, dns_len));
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
            return std::unexpected(std::format(
                "DNS response: RDATA exceeds packet at an[{}/{}] "
                "(rdlength={}, offset={}, dns_len={}, kMaxDnsPacketLen={})",
                i, an_count, rdlength, offset, dns_len, kMaxDnsPacketLen));
        }

        if (rr_type == kDnsTypeA && rr_class == kDnsClassIn && rdlength == 4) {
            // A record: 4-byte IPv4 address in network byte order
            uint32_t ip_net;
            std::memcpy(&ip_net, &dns_data[offset], 4);
            return net::ntoh32(ip_net);
        }

        offset += rdlength;  // Skip this RR, try next
    }

    return std::unexpected(std::format(
        "DNS response: no A record found among {} answer record(s) "
        "(only AAAA / CNAME / etc. seen)",
        an_count));
}

// ─────────────────────────────────────────────────────────────────────────────
// Build complete DNS query packet (Ethernet + IPv4 + UDP + DNS)
// ─────────────────────────────────────────────────────────────────────────────

/// Build a DNS query UDP packet on an mbuf.
/// @return Allocated mbuf, or nullptr on failure
[[nodiscard]] inline rte_mbuf* build_dns_packet(
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

    // Defensive: rte_pktmbuf_alloc dereferences pool with no NULL check
    // and segfaults on nullptr. Match the contract of
    // arp::build_arp_request (returns nullptr on null pool) so callers
    // fail fast without a crash.
    if (!pool) [[unlikely]] {
        SPDLOG_LOGGER_ERROR(detail::dns_logger(),
            "build_dns_packet: null mempool — cannot allocate");
        return nullptr;
    }

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
/// @param expected_src_port  UDP source port the response must originate
///        from (host byte order).  Mirrors `DnsConfig::port` on the call
///        site — defaults to kDnsPort so adversarial tests using stock
///        port-53 fixtures keep working without modification.
/// @param expected_dst_port  UDP destination port the response must carry
///        (host byte order). Equal to the ephemeral src_port the resolver
///        chose for the outgoing query. 0 means "accept any dst_port" and
///        is the legacy default used by adversarial tests; production
///        callers pass their random ephemeral port for cache-poisoning
///        defense in depth.
/// @param expected_local_ip  IPv4 destination the response must carry
///        (host byte order). Equal to the local src_ip used to send the
///        query. 0 means "accept any dst_ip" and is the legacy default
///        used by adversarial test fixtures that predate this check.
///        On a NIC in promiscuous mode (e.g. DPDK shared L2 with multiple
///        guests) this closes the last off-path-poisoning channel: a
///        forged reply from the nameserver IP carrying the correct
///        src/dst port + tx_id but addressed to a different host's IP
///        would otherwise have been accepted as our reply.
/// @return IPv4 address (host order) if valid DNS response, nullopt otherwise
[[nodiscard]] inline std::optional<uint32_t>
try_parse_dns_packet(const rte_mbuf* mbuf, uint16_t tx_id,
                     uint32_t nameserver_ip,
                     uint16_t expected_src_port = kDnsPort,
                     uint16_t expected_dst_port = 0,
                     uint32_t expected_local_ip = 0) noexcept {
    constexpr size_t min_len = net::kEtherHeaderLen + net::kIpv4HeaderLen
                             + kUdpHeaderLen + kDnsHeaderLen;
    // Reject multi-segment mbufs — our bounds math below reads first-segment
    // bytes via `rte_pktmbuf_data_len`; a scattered packet would have its
    // tail in later segments and defeat those checks. Same defense-in-depth
    // rationale as `parse_ip_header`.
    if (mbuf->nb_segs > 1) [[unlikely]] return std::nullopt;
    if (rte_pktmbuf_data_len(mbuf) < min_len) return std::nullopt;

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
    // Defense-in-depth: if the caller supplied a local IP, the reply must
    // be addressed to it. Skip when expected_local_ip == 0 to preserve the
    // legacy permissive shape adversarial test fixtures depend on.
    if (expected_local_ip != 0 &&
        net::ntoh32(ip->dst_addr) != expected_local_ip) return std::nullopt;

    // Re-check total length with actual IHL — IP options can extend the header
    // beyond the 20-byte minimum used in min_len above.
    if (net::kEtherHeaderLen + ihl + kUdpHeaderLen + kDnsHeaderLen > rte_pktmbuf_data_len(mbuf))
        return std::nullopt;

    // Check UDP source port matches the configured nameserver port.
    // Hardcoding 53 here would silently drop responses whenever the
    // caller pointed DnsConfig::port at a non-standard port (e.g. for
    // a local DNS forwarder or a test fixture).
    auto* udp = reinterpret_cast<const UdpHeader*>(
        pkt + net::kEtherHeaderLen + ihl);
    if (net::ntoh16(udp->src_port) != expected_src_port) return std::nullopt;
    // Defense in depth against off-path cache poisoning: the response
    // must also be addressed to the ephemeral source port we chose when
    // sending. Raising this check from "NIC filters by flow" to "parser
    // enforces it too" narrows the unguessable-state window from
    // (16-bit tx_id) to (16-bit tx_id × ephemeral port width). 0 is
    // a legacy sentinel meaning "accept any dst_port" — used by the
    // adversarial test fixtures that predate this check.
    if (expected_dst_port != 0 &&
        net::ntoh16(udp->dst_port) != expected_dst_port) return std::nullopt;

    // Parse DNS payload
    const uint8_t* dns_data = pkt + net::kEtherHeaderLen + ihl
                            + kUdpHeaderLen;
    uint16_t udp_len = net::ntoh16(udp->length);
    if (udp_len < kUdpHeaderLen + kDnsHeaderLen) return std::nullopt;
    // Guard against truncated packets: the UDP length field may claim more
    // data than the mbuf actually contains.
    if (net::kEtherHeaderLen + ihl + udp_len > rte_pktmbuf_data_len(mbuf))
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
// AsyncDnsResolver — DpdkPollable-driven DNS state machine
// ─────────────────────────────────────────────────────────────────────────────
//
// Design rationale (T1.2 of crypto-HFT review):
//   The blocking `resolve()` below serializes multi-venue reconnect — every
//   venue waits on its predecessor's full DNS RTT. `AsyncDnsResolverT` exposes
//   the same state machine as a `DpdkPollable` so an orchestrator can launch N
//   concurrent resolves on one DpdkPoller cycle.
//
// Concept conformance:
//   AsyncDnsResolverT satisfies `eph::net::dpdk::DpdkPollable`:
//     - `process_burst_(mbufs, n, tsc)` — feed RX mbufs, parse for matching reply
//     - `on_poll_tick_(tsc)`            — periodic resend / timeout check
//     - `tuple_for_poller_(...)`        — register the (src,dst,sport,dport,UDP) tuple
//     - `notify_attached_/notify_detached_` — Poller lifecycle hooks
//   It is therefore registerable via `DpdkPoller::add()`.
//
// Testability:
//   The template parameter `Io` is the NIC TX shim. Production uses
//   `RealNicIo` which calls `rte_eth_tx_burst` directly. Unit tests
//   substitute a `FakeNicIo` that records sent packets without touching
//   real hardware. RX is exercised by feeding mbufs through `process_burst_`.
//
// Error model:
//   `result()` returns `std::expected<uint32_t, eph::core::ErrorInfo>` —
//   the project standard. The blocking `resolve()` wrapper now matches:
//   it returns `expected<uint32_t, eph::core::ErrorInfo>` (T3.18 alignment;
//   prior signature returned `expected<uint32_t, std::string>`). All in-tree
//   callers were migrated; external callers must read `r.error().detail`
//   instead of `r.error()` for the diagnostic message, or simply format the
//   ErrorInfo value (the std::formatter / format_as path renders as
//   "CODE: detail").

/// @brief Default NIC IO shim — calls `rte_eth_tx_burst` directly.
///
/// Pure pass-through, zero overhead. Compiles down to a single call instr.
/// The `noexcept` is genuine — DPDK PMD `tx_burst` is documented noexcept.
struct RealNicIo {
    [[nodiscard]] static uint16_t tx_burst(uint16_t port_id, uint16_t queue_id,
                                            rte_mbuf** pkts, uint16_t n) noexcept {
        return ::rte_eth_tx_burst(port_id, queue_id, pkts, n);
    }
};

/// @brief Lifecycle status of an `AsyncDnsResolverT`.
///
/// Linear progression:
///   `Idle` → `InProgress` → `Ready` | `TimedOut` | `Error`
/// Terminal states (`Ready`, `TimedOut`, `Error`) do not auto-reset; the
/// resolver is single-shot. Re-resolution requires a fresh instance.
enum class ResolveStatus : uint8_t {
    Idle = 0,    ///< Constructed, `start()` not yet called.
    InProgress,  ///< Query sent, awaiting reply or timeout.
    Ready,       ///< A-record received and parsed; result() returns the IP.
    TimedOut,    ///< Deadline elapsed without a valid reply.
    Error,       ///< Send / build / config failure; result() carries detail.
};

/// @brief Stable string for a `ResolveStatus` value (logging / diagnostics).
[[nodiscard]] constexpr const char* to_string(ResolveStatus s) noexcept {
    switch (s) {
        case ResolveStatus::Idle:        return "Idle";
        case ResolveStatus::InProgress:  return "InProgress";
        case ResolveStatus::Ready:       return "Ready";
        case ResolveStatus::TimedOut:    return "TimedOut";
        case ResolveStatus::Error:       return "Error";
    }
    return "UNKNOWN";
}

/// @brief Async DNS resolver as a `DpdkPollable`.
///
/// @tparam Io  NIC IO shim (default `RealNicIo`). Tests substitute a recorder.
///
/// Lifetime contract:
///   - Construct with the same params as blocking `resolve()` minus hostname.
///   - Optionally `add()` to a DpdkPoller (recommended for production).
///   - Call `start(hostname)` to issue the first query and begin the timer.
///   - Drive via the Poller (preferred) OR call `process_burst_` /
///     `on_poll_tick_` directly.
///   - Inspect `status()` and `result()`.
///   - When done, `remove()` from the Poller is optional. Both destruction
///     orders are safe:
///       (a) Poller-first  → ~DpdkPoller calls `notify_detached_` on every
///                           still-attached resolver, clearing `attached_to_`.
///       (b) Resolver-first → ~AsyncDnsResolverT calls `attached_to_->remove
///                           (this)`, removing itself from the Poller's
///                           `entries_` array before the resolver storage
///                           is freed. (Pre-fix this leaked a dangling
///                           void* in `entries_` — UAF on the next poll.)
///
/// Thread safety: not MT-safe. Like the rest of `DpdkPollable`, one driver
/// (lcore or app thread) owns the resolver.
template <class Io = RealNicIo>
class AsyncDnsResolverT {
public:
    /// @brief Maximum number of query attempts before declaring `TimedOut`.
    ///        Mirrors the blocking `resolve()` behavior (3 sends per query).
    static constexpr int kMaxAttempts = 3;

    /// @brief Number of ports the resolver advertises to the Poller for
    ///        routing. Filled into `tuple_for_poller_` as src_port=ephemeral
    ///        chosen at `start()` time, dst_port=cfg.port. Routing matches
    ///        on the swap (incoming src=cfg.port, dst=ephemeral).
    using NicIo = Io;

    // ── Construction ────────────────────────────────────────────────────────

    /// @brief Construct an idle resolver. No allocation, no I/O.
    ///
    /// @param port_id    DPDK port to send/receive on.
    /// @param queue_id   TX/RX queue for the query and response.
    /// @param pool       Mempool for outgoing mbuf allocation.
    /// @param src_mac    Our NIC MAC (for Ethernet framing).
    /// @param dst_mac    Gateway MAC (from prior ARP resolution).
    /// @param src_ip     Our IPv4 address (host byte order).
    /// @param cfg        Nameserver / port / timeout config.
    AsyncDnsResolverT(uint16_t port_id,
                       uint16_t queue_id,
                       rte_mempool* pool,
                       const rte_ether_addr& src_mac,
                       const rte_ether_addr& dst_mac,
                       uint32_t src_ip,
                       DnsConfig cfg = {}) noexcept
        : port_id_(port_id), queue_id_(queue_id), pool_(pool),
          src_mac_(src_mac), dst_mac_(dst_mac), src_ip_(src_ip),
          cfg_(cfg) {
        SPDLOG_LOGGER_TRACE(detail::dns_logger(),
            "AsyncDnsResolver ctor: port={} queue={} ns={} timeout={}ms",
            port_id_, queue_id_, net::format_ipv4(cfg_.nameserver_ip).data(),
            cfg_.timeout.count());
    }

    /// @brief Auto-detach from any still-attached Poller before destruction.
    ///
    /// Symmetric with `~DpdkTcpStream` / `~DpdkUdpSocket`. Without this,
    /// the documented usage pattern "remove() optionally before destruction"
    /// is unsafe: if the Resolver dies before the Poller while still
    /// attached, the Poller's `entries_` array retains a void* to a freed
    /// resolver and the next `poll()` cycle dispatches an mbuf into it
    /// (use-after-free). The Poller's own dtor handles the inverse
    /// (Poller-first) ordering via `notify_detached_`, so both directions
    /// are now covered.
    ~AsyncDnsResolverT() {
        if (attached_to_ != nullptr) {
            SPDLOG_LOGGER_DEBUG(detail::dns_logger(),
                "~AsyncDnsResolverT: auto-detach (resolver outlived Poller "
                "registration without an explicit remove())");
            auto r = attached_to_->remove(this);
            if (!r) {
                SPDLOG_LOGGER_WARN(detail::dns_logger(),
                    "~AsyncDnsResolverT: auto-detach failed: {} — possible "
                    "Poller/Resolver lifecycle mismatch",
                    r.error().detail ? r.error().detail : "unknown");
            }
        }
    }

    AsyncDnsResolverT(const AsyncDnsResolverT&)            = delete;
    AsyncDnsResolverT& operator=(const AsyncDnsResolverT&) = delete;
    AsyncDnsResolverT(AsyncDnsResolverT&&)                 = delete;
    AsyncDnsResolverT& operator=(AsyncDnsResolverT&&)      = delete;

    // ── Lifecycle ───────────────────────────────────────────────────────────

    /// @brief Initiate resolution. Validates inputs, generates tx_id /
    ///        ephemeral src_port, builds the query, and sends it once.
    ///
    /// On any pre-flight failure (empty hostname, null pool, invalid config,
    /// CSPRNG failure, encode failure) the resolver transitions to `Error`
    /// with a detail string and the function returns the same ErrorInfo.
    ///
    /// On success, status becomes `InProgress` and the first query is in
    /// flight. The deadline starts ticking from `now`.
    ///
    /// @param hostname  Hostname to resolve. Dotted-decimal IPv4 inputs
    ///                  short-circuit to `Ready` without any I/O.
    /// @return `void` on success; `ErrorInfo` on the pre-flight failures
    ///         above. Subsequent timeout / parse errors are surfaced via
    ///         `status()` + `result()`, NOT this return value.
    [[nodiscard]] std::expected<void, eph::core::ErrorInfo>
    start(const std::string& hostname) noexcept {
        auto* log = detail::dns_logger();
        SPDLOG_LOGGER_DEBUG(log, "AsyncDnsResolver::start: hostname='{}'",
                            hostname);

        // Repeated start() is a programming error — the resolver is
        // single-shot. Surface it loudly rather than silently re-starting.
        if (status_ != ResolveStatus::Idle) [[unlikely]] {
            SPDLOG_LOGGER_ERROR(log,
                "AsyncDnsResolver::start called in non-Idle state ({}) for '{}'",
                to_string(status_), hostname);
            return std::unexpected(eph::core::ErrorInfo{
                eph::core::Error::InvalidConfig,
                "AsyncDnsResolver::start called from non-Idle state"});
        }
        hostname_ = hostname;

        // Fast path: dotted-decimal IPv4 needs no NIC, pool, or nameserver.
        // Mirrors the blocking resolve() short-circuit so callers can use
        // either form interchangeably.
        uint32_t fast_ip = net::parse_ipv4(hostname.c_str());
        if (fast_ip != 0) {
            result_ip_ = fast_ip;
            status_ = ResolveStatus::Ready;
            SPDLOG_LOGGER_DEBUG(log,
                "AsyncDnsResolver::start: dotted-decimal fast path '{}' -> {}",
                hostname, net::format_ipv4(fast_ip).data());
            return {};
        }

        if (auto err = cfg_.validate(); !err.empty()) {
            return set_error_("Invalid DNS config",
                              eph::core::Error::InvalidConfig);
        }
        if (hostname.empty()) {
            return set_error_("hostname is empty",
                              eph::core::Error::InvalidConfig);
        }
        if (!pool_) {
            return set_error_("mempool is null",
                              eph::core::Error::InvalidConfig);
        }

        // Surface advisory warnings (loopback nameserver on DPDK, etc.)
        // — same advisory pattern as blocking resolve() / Platform / etc.
        for (const auto& w : cfg_.warnings()) {
            SPDLOG_LOGGER_WARN(log, "DnsConfig advisory: {}", w);
        }

        // Generate random transaction ID + ephemeral source port via
        // kernel CSPRNG. Avoids OpenSSL/aws-lc rand to keep the TU clean.
        if (::getrandom(&tx_id_, sizeof(tx_id_), GRND_NONBLOCK)
                != sizeof(tx_id_)) {
            return set_error_("CSPRNG failure for transaction ID",
                              eph::core::Error::InvalidConfig);
        }
        // RSS-aware src_port selection — see detail::select_dns_src_port.
        // On RSS-active multi-queue NICs the reply must hash back to
        // queue_id_; on single-queue NICs we fall back to random_ephemeral_port.
        // The full diagnostic (with port / queue / IPs) is logged by the
        // helper itself; ErrorInfo carries a static-lifetime literal so the
        // caller can branch on the kind without depending on heap detail.
        if (auto sp = detail::select_dns_src_port(
                port_id_, queue_id_, cfg_.nameserver_ip, src_ip_, cfg_.port);
            sp) {
            src_port_ = *sp;
        } else {
            SPDLOG_LOGGER_ERROR(detail::dns_logger(),
                "AsyncDnsResolver: src_port selection failed: {}", sp.error());
            return set_error_("DNS src_port selection failed (RSS hash exhausted "
                              "or CSPRNG failure; see ERROR log for detail)",
                              eph::core::Error::InvalidConfig);
        }
        tx_id_net_ = net::hton16(tx_id_);

        // Build the immutable query payload once. We reuse it across
        // retries — only the surrounding mbuf is freshly allocated each
        // time so freeing-after-tx is straightforward.
        dns_payload_len_ = detail::build_dns_query(
            dns_payload_buf_, tx_id_net_, hostname);
        if (dns_payload_len_ == 0) {
            return set_error_("failed to encode DNS query (bad hostname)",
                              eph::core::Error::InvalidConfig);
        }

        // Establish timing baselines using TSC (per CLAUDE.md: TSC, not
        // steady_clock). All deadline / retry checks below operate on
        // TSC ticks. The timeout is converted once at start().
        start_tsc_ = eph::utils::TSC::now();
        auto timeout_cycles_opt = eph::utils::TSC::to_cycles(
            std::chrono::nanoseconds{cfg_.timeout}.count());
        if (!timeout_cycles_opt) {
            // TSC not yet calibrated. Fall back to a generous fixed
            // estimate (3 GHz upper bound) so we still time out
            // eventually rather than spinning forever; emit a single
            // WARN so misconfiguration is visible. Production paths
            // call TSC::init() at startup and never hit this branch.
            SPDLOG_LOGGER_WARN(log,
                "AsyncDnsResolver::start: TSC not calibrated, "
                "using 3GHz fallback for timeout deadline");
            timeout_cycles_ = static_cast<uint64_t>(3'000'000'000ULL) *
                              static_cast<uint64_t>(cfg_.timeout.count()) / 1000ULL;
        } else {
            timeout_cycles_ = *timeout_cycles_opt;
        }
        // Retry interval: timeout / kMaxAttempts, lower-bounded at 100 ms
        // to mirror the blocking resolve() backoff (which uses the same
        // bound). Lower bound prevents pathological 1-ms timeouts from
        // causing 1ms-spaced retries that exceed pool capacity.
        auto retry_ms = cfg_.timeout / kMaxAttempts;
        if (retry_ms < std::chrono::milliseconds{100}) {
            retry_ms = std::chrono::milliseconds{100};
        }
        auto retry_cycles_opt = eph::utils::TSC::to_cycles(
            std::chrono::nanoseconds{retry_ms}.count());
        retry_interval_cycles_ = retry_cycles_opt.value_or(
            static_cast<uint64_t>(3'000'000'000ULL) *
            static_cast<uint64_t>(retry_ms.count()) / 1000ULL);

        status_ = ResolveStatus::InProgress;
        // Send the first query NOW. If TX fails (rare — usually transient
        // ring fullness), we still transition to InProgress so the
        // periodic tick will retry; first-attempt errors are not fatal.
        if (!send_query_()) {
            SPDLOG_LOGGER_WARN(log,
                "AsyncDnsResolver::start: initial tx failed for '{}', "
                "will retry on next poll tick", hostname);
            // attempts_sent_ stayed at 0 so the first tick attempts again
            // — rather than counting a failed TX against the 3-attempt
            // budget which would unfairly shrink retries.
            next_send_tsc_ = start_tsc_ + retry_interval_cycles_;
        } else {
            ++attempts_sent_;
            next_send_tsc_ = start_tsc_ + retry_interval_cycles_;
            SPDLOG_LOGGER_DEBUG(log,
                "AsyncDnsResolver::start: query #1 in flight for '{}' "
                "(txid=0x{:04x}, src_port={})",
                hostname, tx_id_, src_port_);
        }
        return {};
    }

    // ── Status / result ─────────────────────────────────────────────────────

    [[nodiscard]] ResolveStatus status() const noexcept { return status_; }

    /// @brief True iff status() is one of the terminal states.
    [[nodiscard]] bool is_done() const noexcept {
        return status_ == ResolveStatus::Ready ||
               status_ == ResolveStatus::TimedOut ||
               status_ == ResolveStatus::Error;
    }

    /// @brief Resolved IPv4 (host byte order) on success; ErrorInfo otherwise.
    ///
    /// Calling before `is_done()` returns a `Timeout`-coded error; this lets
    /// the caller treat "not yet" the same way as "failed" if it doesn't
    /// want to inspect status() separately.
    [[nodiscard]] std::expected<uint32_t, eph::core::ErrorInfo>
    result() const noexcept {
        if (status_ == ResolveStatus::Ready) return result_ip_;
        return std::unexpected(error_);
    }

    // ── Diagnostics / metrics ───────────────────────────────────────────────

    /// @brief Number of TX bursts the resolver has issued (1..kMaxAttempts).
    ///        Increments on send success only; failed txs do not count
    ///        toward the attempt budget.
    [[nodiscard]] int attempts_sent() const noexcept { return attempts_sent_; }

    /// @brief TSC delta from `start()` to the moment status became terminal.
    ///        Returns 0 before completion. Convertible to ns via
    ///        `TSC::to_ns()` — mirrors the `eph::utils::TSC` contract.
    [[nodiscard]] uint64_t resolve_duration_tsc() const noexcept {
        return resolve_duration_tsc_;
    }

    /// @brief Transaction ID actually used for this resolve (host order).
    ///        0 if `start()` has not run yet. Diagnostic only.
    [[nodiscard]] uint16_t tx_id() const noexcept { return tx_id_; }

    /// @brief Ephemeral src_port used for this resolve. 0 before `start()`.
    [[nodiscard]] uint16_t src_port() const noexcept { return src_port_; }

    // ── DpdkPollable concept hooks ──────────────────────────────────────────

    /// @brief RX burst dispatch from DpdkPoller. Walks each mbuf, attempts
    ///        to parse it as a DNS reply matching our tx_id / src_port /
    ///        nameserver. First successful match transitions to Ready.
    ///
    /// All mbufs are freed (matched or not) per the poller's ownership
    /// contract — once dispatched to a Pollable the mbufs are the
    /// Pollable's responsibility.
    void process_burst_(rte_mbuf** mbufs, uint16_t n,
                         uint64_t rx_tsc) noexcept {
        for (uint16_t i = 0; i < n; ++i) {
            // Already terminated? Just free remaining mbufs to honour the
            // ownership contract — late-arriving replies after Ready /
            // TimedOut are normal in retry scenarios.
            if (is_done()) {
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }
            auto resolved = detail::try_parse_dns_packet(
                mbufs[i], tx_id_net_, cfg_.nameserver_ip,
                cfg_.port, src_port_, src_ip_);
            if (resolved) {
                result_ip_ = *resolved;
                status_ = ResolveStatus::Ready;
                resolve_duration_tsc_ = rx_tsc - start_tsc_;
                SPDLOG_LOGGER_INFO(detail::dns_logger(),
                    "AsyncDnsResolver: resolved '{}' -> {} "
                    "(after {} attempt(s), {} cycles)",
                    hostname_, net::format_ipv4(*resolved).data(),
                    attempts_sent_, resolve_duration_tsc_);
            }
            rte_pktmbuf_free(mbufs[i]);
        }
    }

    /// @brief Periodic per-cycle tick — invoked by `DpdkPoller::poll()`.
    ///        Implements: (a) deadline check → TimedOut, (b) retry tick →
    ///        send next query if interval elapsed and budget remains.
    void on_poll_tick_(uint64_t tsc) noexcept {
        if (status_ != ResolveStatus::InProgress) return;

        // Deadline check first: even if we have retries left in budget,
        // overall timeout governs.
        if ((tsc - start_tsc_) >= timeout_cycles_) {
            status_ = ResolveStatus::TimedOut;
            error_ = eph::core::ErrorInfo{
                eph::core::Error::Timeout,
                "AsyncDnsResolver: DNS resolve timeout"};
            resolve_duration_tsc_ = tsc - start_tsc_;
            SPDLOG_LOGGER_WARN(detail::dns_logger(),
                "AsyncDnsResolver: timeout for '{}' after {} attempt(s) "
                "({}ms budget)",
                hostname_, attempts_sent_, cfg_.timeout.count());
            return;
        }

        // Retry tick: only if interval elapsed AND budget remains.
        if (attempts_sent_ < kMaxAttempts && tsc >= next_send_tsc_) {
            if (send_query_()) {
                ++attempts_sent_;
                SPDLOG_LOGGER_DEBUG(detail::dns_logger(),
                    "AsyncDnsResolver: retry #{} for '{}'",
                    attempts_sent_, hostname_);
            }
            // Schedule next tick regardless of send success — backoff
            // protects the pool from thrashing when allocation fails.
            next_send_tsc_ = tsc + retry_interval_cycles_;
        }
    }

    /// @brief Tuple-for-routing hook. Tells the Poller our 5-tuple so
    ///        incoming replies route here.
    ///
    /// IMPORTANT: tuple_for_poller_ MUST be called only after start() has
    /// allocated src_port_. Calling add() before start() will register
    /// src_port=0 which is unroutable. The recommended usage order is
    /// `start()` first, then `poller->add()`.
    void tuple_for_poller_(uint32_t* src_ip, uint32_t* dst_ip,
                            uint16_t* src_port, uint16_t* dst_port,
                            uint8_t*  proto) noexcept {
        *src_ip   = src_ip_;
        *dst_ip   = cfg_.nameserver_ip;
        *src_port = src_port_;
        *dst_port = cfg_.port;
        *proto    = eph::dpdk::net::kIpProtoUdp;
    }

    /// @brief Poller attach / detach hooks. The `attached_to_` pointer is
    ///        retained so `~AsyncDnsResolverT` can auto-detach safely
    ///        when the resolver dies before the Poller (UAF prevention) —
    ///        symmetric with `DpdkTcpStream` / `DpdkUdpSocket`. The
    ///        signature uses `DpdkPoller<void>*` (matching the concept
    ///        contract) rather than the previous bare `void*` so the
    ///        dtor can call back via `attached_to_->remove(this)`.
    void notify_attached_(::eph::net::dpdk::DpdkPoller<void>* p) noexcept {
        attached_to_ = p;
    }
    void notify_detached_() noexcept { attached_to_ = nullptr; }

    /// @brief Diagnostic — true iff currently attached to a Poller.
    [[nodiscard]] bool is_attached() const noexcept {
        return attached_to_ != nullptr;
    }

private:
    // Shared error-setter — keeps start()'s pre-flight failures uniform.
    [[nodiscard]] std::expected<void, eph::core::ErrorInfo>
    set_error_(const char* detail, eph::core::Error code) noexcept {
        status_ = ResolveStatus::Error;
        error_ = eph::core::ErrorInfo{code, detail};
        SPDLOG_LOGGER_ERROR(detail::dns_logger(),
            "AsyncDnsResolver error: {}", detail);
        return std::unexpected(error_);
    }

    // Allocate + build + tx a single DNS query mbuf. Returns true iff TX
    // succeeded. Caller increments attempts_sent_ only on true.
    [[nodiscard]] bool send_query_() noexcept {
        auto* pkt = detail::build_dns_packet(
            pool_, src_mac_, dst_mac_, src_ip_,
            cfg_.nameserver_ip, src_port_, cfg_.port,
            dns_payload_buf_, dns_payload_len_);
        if (!pkt) {
            // Pool exhaustion is the typical cause. Logged once per
            // failure so a stuck condition is visible without flooding.
            SPDLOG_LOGGER_WARN(detail::dns_logger(),
                "AsyncDnsResolver: mbuf alloc failed for '{}' "
                "(port={} queue={} attempt={})",
                hostname_, port_id_, queue_id_, attempts_sent_ + 1);
            return false;
        }
        uint16_t sent = Io::tx_burst(port_id_, queue_id_, &pkt, 1);
        if (sent != 1) {
            rte_pktmbuf_free(pkt);
            SPDLOG_LOGGER_WARN(detail::dns_logger(),
                "AsyncDnsResolver: tx_burst rejected packet for '{}' "
                "(port={} queue={} attempt={})",
                hostname_, port_id_, queue_id_, attempts_sent_ + 1);
            return false;
        }
        return true;
    }

    // ── Construction-time inputs (immutable after ctor) ──
    uint16_t              port_id_;
    uint16_t              queue_id_;
    rte_mempool*          pool_;
    rte_ether_addr        src_mac_;
    rte_ether_addr        dst_mac_;
    uint32_t              src_ip_;
    DnsConfig             cfg_;

    // ── start()-set state ──
    std::string           hostname_;
    uint16_t              tx_id_       {0};
    uint16_t              tx_id_net_   {0};
    uint16_t              src_port_    {0};
    uint8_t               dns_payload_buf_[kMaxDnsPacketLen]{};
    size_t                dns_payload_len_{0};

    // Timing — TSC ticks, not steady_clock. start_tsc_ is the baseline.
    uint64_t              start_tsc_           {0};
    uint64_t              next_send_tsc_       {0};
    uint64_t              timeout_cycles_      {0};
    uint64_t              retry_interval_cycles_{0};

    // ── Outputs ──
    ResolveStatus         status_      {ResolveStatus::Idle};
    uint32_t              result_ip_   {0};
    eph::core::ErrorInfo  error_       {eph::core::Error::Timeout, ""};
    int                   attempts_sent_{0};
    uint64_t              resolve_duration_tsc_{0};

    // ── Poller bookkeeping ──
    /// Tracks the Poller this resolver is registered with (nullptr when
    /// not attached). Dtor uses this for auto-detach to prevent the
    /// Poller's `entries_` array from holding a stale `void*` to a freed
    /// resolver when the resolver outlives its Poller registration.
    ::eph::net::dpdk::DpdkPoller<void>* attached_to_ {nullptr};
};

/// @brief Default production typedef using the real NIC TX shim.
///        Tests instantiate `AsyncDnsResolverT<FakeNicIo>` to record TX
///        without touching real hardware.
using AsyncDnsResolver = AsyncDnsResolverT<RealNicIo>;

// ─────────────────────────────────────────────────────────────────────────────
// Public API: blocking DNS resolution over DPDK

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
/// @return Resolved IPv4 address in host byte order on success;
///         `eph::core::ErrorInfo` on failure. Error code mapping mirrors
///         `AsyncDnsResolverT`:
///           - `Error::InvalidConfig` — pre-flight rejection (validate,
///             empty hostname, null pool, CSPRNG failure, bad QNAME)
///           - `Error::OutOfMemory`   — mbuf allocation failed (pool exhausted)
///           - `Error::Timeout`       — deadline elapsed without a valid reply
///         The `detail` field is a static-lifetime string literal —
///         per-call context (hostname, port, queue, attempt) is logged
///         via spdlog at the corresponding level rather than embedded in
///         the ErrorInfo (`detail` requires static storage).
///
/// @note Security: DNS transaction IDs are 16-bit per protocol spec, making them
///       theoretically vulnerable to birthday attacks (~256 queries for 50% collision).
///       CSPRNG is used for tx_id generation to prevent prediction. For production use,
///       consider DNS-over-TLS or pre-resolved IPs to eliminate DNS attack surface entirely.
///       The response is validated against both tx_id and expected nameserver IP.
[[nodiscard]] inline std::expected<uint32_t, eph::core::ErrorInfo>
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
        // err is a string_view into a static literal owned by validate(),
        // so it's safe to surface via SPDLOG context — but the ErrorInfo
        // detail itself uses a fixed literal (no heap concat) to satisfy
        // the static-lifetime contract.
        SPDLOG_LOGGER_ERROR(detail::dns_logger(),
            "DNS resolve: invalid config ({}) for hostname='{}'",
            err, hostname);
        return std::unexpected(eph::core::ErrorInfo{
            eph::core::Error::InvalidConfig,
            "Invalid DNS config"});
    }

    [[maybe_unused]] auto log = detail::dns_logger();

    // Surface non-fatal misconfigurations (loopback nameserver on DPDK,
    // very short timeout, non-standard port) — advisory, does not block
    // the resolve. Parallel to Platform / UdpSender / TcpSession.
    for (const auto& w : cfg.warnings()) {
        SPDLOG_LOGGER_WARN(log, "DnsConfig advisory: {}", w);
    }

    if (hostname.empty()) {
        SPDLOG_LOGGER_ERROR(log,
            "DNS resolve: hostname is empty (port_id={} queue_id={})",
            port_id, queue_id);
        return std::unexpected(eph::core::ErrorInfo{
            eph::core::Error::InvalidConfig,
            "DNS resolve: hostname is empty"});
    }
    if (!pool) {
        SPDLOG_LOGGER_ERROR(log,
            "DNS resolve: mempool is null (hostname='{}' port_id={} queue_id={})",
            hostname, port_id, queue_id);
        return std::unexpected(eph::core::ErrorInfo{
            eph::core::Error::InvalidConfig,
            "DNS resolve: mempool is null"});
    }
    // Note: nameserver_ip == 0 is already caught by cfg.validate() above.

    SPDLOG_LOGGER_DEBUG(log, "DNS resolve: {} via {} on port {} queue {}",
        hostname, net::format_ipv4(cfg.nameserver_ip).data(),
        port_id, queue_id);

    // Generate random transaction ID and ephemeral source port
    // Kernel CSPRNG via getrandom(2) — avoids pulling in <openssl/rand.h>,
    // which would conflict with the aws-lc TLS path in the same TU.
    uint16_t tx_id;
    if (::getrandom(&tx_id, sizeof(tx_id), GRND_NONBLOCK) != sizeof(tx_id)) {
        // GRND_NONBLOCK only returns short on EAGAIN (entropy pool not
        // initialised) — extraordinary on Linux long after boot. Surface
        // errno so operators can diagnose seccomp / kernel-too-old.
        SPDLOG_LOGGER_ERROR(log,
            "DNS resolve: CSPRNG (getrandom) failure: errno={} ({}) "
            "hostname='{}' port_id={} queue_id={}",
            errno, std::strerror(errno), hostname, port_id, queue_id);
        return std::unexpected(eph::core::ErrorInfo{
            eph::core::Error::InvalidConfig,
            "DNS resolve: CSPRNG failure for transaction ID"});
    }
    // RSS-aware src_port selection: ensure the reply lands back on the
    // caller's queue. detail::select_dns_src_port() probes the NIC's RSS
    // state and either reverse-picks a hash-aligned port (RSS active) or
    // falls back to random_ephemeral_port (single-queue / no RSS).
    auto src_port_r = detail::select_dns_src_port(
        port_id, queue_id, cfg.nameserver_ip, src_ip, cfg.port);
    if (!src_port_r) {
        SPDLOG_LOGGER_ERROR(log,
            "DNS resolve: src_port selection failed: {}", src_port_r.error());
        return std::unexpected(eph::core::ErrorInfo{
            eph::core::Error::InvalidConfig,
            "DNS resolve: src_port selection failed (see log for detail)"});
    }
    const uint16_t src_port = *src_port_r;

    // Build DNS query payload
    uint8_t dns_buf[kMaxDnsPacketLen];
    size_t dns_len = detail::build_dns_query(dns_buf, net::hton16(tx_id), hostname);
    if (dns_len == 0) {
        SPDLOG_LOGGER_ERROR(log,
            "DNS resolve: failed to encode query for '{}'", hostname);
        return std::unexpected(eph::core::ErrorInfo{
            eph::core::Error::InvalidConfig,
            "DNS resolve: failed to encode query (bad hostname)"});
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
                // Symmetric with the tx_burst WARN below — surface
                // hostname + port + queue + attempt so a single log
                // line tells the operator which DNS query died and
                // where. mbuf alloc failure is usually pool exhaustion;
                // the lookup terminates rather than retrying because
                // the pool will not refill mid-loop.
                SPDLOG_LOGGER_ERROR(log,
                    "DNS resolve: mbuf allocation failed (hostname='{}' "
                    "port={} queue={} query_attempt={})",
                    hostname, port_id, queue_id, requests_sent + 1);
                return std::unexpected(eph::core::ErrorInfo{
                    eph::core::Error::OutOfMemory,
                    "DNS resolve: mbuf allocation failed"});
            }

            uint16_t sent = rte_eth_tx_burst(port_id, queue_id, &pkt, 1);
            if (sent != 1) {
                rte_pktmbuf_free(pkt);
                // Same actionable-log treatment as arp::resolve(): include
                // port/queue/attempt + the retry_interval so a single grep
                // tells the operator which TX path is broken and when the
                // next attempt will fire. next_send below applies even on
                // failure so the loop self-rate-limits.
                SPDLOG_LOGGER_WARN(log,
                    "DNS resolve: tx_burst failed (port={} queue={} "
                    "query_attempt={} hostname='{}'); will retry after "
                    "{}ms backoff",
                    port_id, queue_id, requests_sent + 1, hostname,
                    retry_interval.count());
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
        rte_mbuf* pkts[kDnsResolveBurstSize];
        uint16_t nb_rx = rte_eth_rx_burst(port_id, queue_id, pkts,
                                           kDnsResolveBurstSize);

        for (uint16_t i = 0; i < nb_rx; ++i) {
            auto resolved_ip = detail::try_parse_dns_packet(
                pkts[i], tx_id_net, cfg.nameserver_ip, cfg.port,
                src_port, src_ip);
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

    // Mirrors AsyncDnsResolverT's timeout detail string so callers can use
    // a single static literal check ("DNS resolve timeout") regardless of
    // sync/async path.
    return std::unexpected(eph::core::ErrorInfo{
        eph::core::Error::Timeout,
        "DNS resolve timeout"});
}

} // namespace eph::dpdk::dns

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specialization for DnsConfig
// ─────────────────────────────────────────────────────────────────────────────

template <>
struct std::formatter<eph::dpdk::dns::DnsConfig> : std::formatter<std::string> {
    auto format(const eph::dpdk::dns::DnsConfig& c, auto& ctx) const {
        return std::formatter<std::string>::format(
            std::format("DnsConfig(ns={}:{}, timeout={}ms)",
                eph::dpdk::net::format_ipv4(c.nameserver_ip).data(),
                c.port, c.timeout.count()),
            ctx);
    }
};
