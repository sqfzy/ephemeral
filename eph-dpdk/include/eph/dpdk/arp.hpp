#pragma once

/// @file arp.hpp
/// Stateless ARP resolution for DPDK data plane.
///
/// Provides a single blocking function to resolve an IPv4 address to a MAC
/// address via ARP request/reply. Designed to be called ONCE before TCP
/// connection establishment — not on the hot path.
///
/// No ARP cache, no background threads, no state between calls.

#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_mbuf.h>

#include "eph/dpdk/net_header.hpp"

namespace eph::dpdk::arp {

// ─────────────────────────────────────────────────────────────────────────────
// ARP constants
// ─────────────────────────────────────────────────────────────────────────────

inline constexpr uint16_t kEtherTypeArp      = 0x0806;
inline constexpr uint16_t kArpHwTypeEthernet = 1;
inline constexpr uint16_t kArpProtoIpv4      = 0x0800;
inline constexpr uint16_t kArpOpRequest      = 1;
inline constexpr uint16_t kArpOpReply        = 2;
inline constexpr uint8_t  kArpHwAddrLen      = 6;  // MAC address length
inline constexpr uint8_t  kArpProtoAddrLen   = 4;  // IPv4 address length
inline constexpr size_t   kArpPacketLen      = 28;  // ARP payload size

// Broadcast MAC address
inline constexpr rte_ether_addr kBroadcastMac = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};

// ─────────────────────────────────────────────────────────────────────────────
// ARP packet structure (RFC 826)
// ─────────────────────────────────────────────────────────────────────────────

/// ARP packet payload (follows Ethernet header). 28 bytes, packed.
struct ArpPacket {
    uint16_t hw_type;         // Hardware type (1 = Ethernet)
    uint16_t proto_type;      // Protocol type (0x0800 = IPv4)
    uint8_t  hw_addr_len;     // Hardware address length (6)
    uint8_t  proto_addr_len;  // Protocol address length (4)
    uint16_t opcode;          // Operation: 1=request, 2=reply
    uint8_t  sender_mac[6];   // Sender hardware address
    uint32_t sender_ip;       // Sender protocol address (network byte order)
    uint8_t  target_mac[6];   // Target hardware address
    uint32_t target_ip;       // Target protocol address (network byte order)
} __attribute__((packed));

static_assert(sizeof(ArpPacket) == kArpPacketLen,
    "ArpPacket must be exactly 28 bytes");

// ─────────────────────────────────────────────────────────────────────────────
// Logger
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

inline std::shared_ptr<spdlog::logger> arp_logger() {
    static auto l = [] {
        auto lg = spdlog::stdout_color_mt("dpdk.arp");
        // Inherit level from spdlog global default
        return lg;
    }();
    return l;
}

/// Format a MAC address as "xx:xx:xx:xx:xx:xx".
inline std::array<char, 18> format_mac(const rte_ether_addr& mac) noexcept {
    std::array<char, 18> buf{};
    snprintf(buf.data(), buf.size(), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac.addr_bytes[0], mac.addr_bytes[1], mac.addr_bytes[2],
             mac.addr_bytes[3], mac.addr_bytes[4], mac.addr_bytes[5]);
    return buf;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// ARP resolution
// ─────────────────────────────────────────────────────────────────────────────

/// Build an ARP request frame on an mbuf.
///
/// @param pool       Mempool for mbuf allocation
/// @param src_mac    Our NIC's MAC address
/// @param src_ip     Our IPv4 address (host byte order)
/// @param target_ip  IPv4 address to resolve (host byte order)
/// @return Allocated mbuf with ARP request, or nullptr on allocation failure
inline rte_mbuf* build_arp_request(rte_mempool* pool,
                                    const rte_ether_addr& src_mac,
                                    uint32_t src_ip,
                                    uint32_t target_ip) noexcept {
    auto* mbuf = rte_pktmbuf_alloc(pool);
    if (!mbuf) return nullptr;

    constexpr uint16_t frame_len = sizeof(rte_ether_hdr) + sizeof(ArpPacket);
    auto* pkt = reinterpret_cast<uint8_t*>(
        rte_pktmbuf_append(mbuf, frame_len));
    if (!pkt) {
        rte_pktmbuf_free(mbuf);
        return nullptr;
    }

    // Ethernet header: broadcast destination
    auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt);
    rte_ether_addr_copy(&kBroadcastMac, &eth->dst_addr);
    rte_ether_addr_copy(&src_mac, &eth->src_addr);
    eth->ether_type = net::hton16(kEtherTypeArp);

    // ARP payload
    auto* arp = reinterpret_cast<ArpPacket*>(pkt + sizeof(rte_ether_hdr));
    arp->hw_type       = net::hton16(kArpHwTypeEthernet);
    arp->proto_type    = net::hton16(kArpProtoIpv4);
    arp->hw_addr_len   = kArpHwAddrLen;
    arp->proto_addr_len = kArpProtoAddrLen;
    arp->opcode        = net::hton16(kArpOpRequest);

    // Sender: our MAC and IP
    std::memcpy(arp->sender_mac, src_mac.addr_bytes, 6);
    arp->sender_ip = net::hton32(src_ip);

    // Target: zero MAC (unknown), target IP
    std::memset(arp->target_mac, 0, 6);
    arp->target_ip = net::hton32(target_ip);

    return mbuf;
}

/// Parse an ARP reply from a received mbuf.
/// Returns the sender MAC if the packet is a valid ARP reply for target_ip.
///
/// @param mbuf       Received packet
/// @param target_ip  The IP we're resolving (host byte order)
/// @return Sender MAC if match, std::nullopt otherwise
inline std::optional<rte_ether_addr>
parse_arp_reply(const rte_mbuf* mbuf, uint32_t target_ip) noexcept {
    constexpr size_t min_len = sizeof(rte_ether_hdr) + sizeof(ArpPacket);
    if (mbuf->data_len < min_len) return std::nullopt;

    auto* pkt = rte_pktmbuf_mtod(mbuf, const uint8_t*);
    auto* eth = reinterpret_cast<const rte_ether_hdr*>(pkt);

    // Check EtherType = ARP
    if (net::ntoh16(eth->ether_type) != kEtherTypeArp) return std::nullopt;

    auto* arp = reinterpret_cast<const ArpPacket*>(pkt + sizeof(rte_ether_hdr));

    // Validate ARP reply for IPv4 over Ethernet
    if (net::ntoh16(arp->hw_type) != kArpHwTypeEthernet) return std::nullopt;
    if (net::ntoh16(arp->proto_type) != kArpProtoIpv4) return std::nullopt;
    if (net::ntoh16(arp->opcode) != kArpOpReply) return std::nullopt;

    // Check sender IP matches the address we're resolving
    if (net::ntoh32(arp->sender_ip) != target_ip) return std::nullopt;

    rte_ether_addr result;
    std::memcpy(result.addr_bytes, arp->sender_mac, 6);
    return result;
}

/// Resolve an IPv4 address to a MAC address via ARP.
///
/// Sends an ARP request as Ethernet broadcast and busy-polls for the reply.
/// Retries the ARP request up to 3 times within the timeout period.
///
/// Must be called BEFORE TCP connection establishment — this function will
/// discard any non-ARP packets received during the poll.
///
/// @param port_id    DPDK port to send/receive on
/// @param queue_id   TX/RX queue to use (default 0)
/// @param pool       Mempool for mbuf allocation
/// @param src_mac    Our NIC's MAC address
/// @param src_ip     Our IPv4 address (host byte order)
/// @param target_ip  IPv4 address to resolve (host byte order)
/// @param timeout    Maximum wait time (default 1s)
/// @return Resolved MAC address, or error string on timeout/failure
inline std::expected<rte_ether_addr, std::string>
resolve(uint16_t port_id,
        uint16_t queue_id,
        rte_mempool* pool,
        const rte_ether_addr& src_mac,
        uint32_t src_ip,
        uint32_t target_ip,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{1000}) {

    auto log = detail::arp_logger();

    if (!pool) {
        return std::unexpected("ARP resolve: mempool is null");
    }

    SPDLOG_LOGGER_DEBUG(log, "ARP resolve: {} -> {} on port {} queue {}",
        net::format_ipv4(src_ip).data(),
        net::format_ipv4(target_ip).data(),
        port_id, queue_id);

    auto deadline = std::chrono::steady_clock::now() + timeout;

    // Retry interval: send ARP request up to 3 times, evenly spaced
    auto retry_interval = timeout / 3;
    if (retry_interval < std::chrono::milliseconds{50}) {
        retry_interval = std::chrono::milliseconds{50};
    }
    auto next_send = std::chrono::steady_clock::time_point{}; // send immediately

    int requests_sent = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        // Send (or resend) ARP request
        auto now = std::chrono::steady_clock::now();
        if (now >= next_send && requests_sent < 3) {
            auto* req = build_arp_request(pool, src_mac, src_ip, target_ip);
            if (!req) {
                SPDLOG_LOGGER_ERROR(log, "ARP resolve: mbuf allocation failed");
                return std::unexpected("ARP resolve: mbuf allocation failed");
            }

            uint16_t sent = rte_eth_tx_burst(port_id, queue_id, &req, 1);
            if (sent != 1) {
                rte_pktmbuf_free(req);
                SPDLOG_LOGGER_WARN(log, "ARP resolve: tx_burst failed");
                // Don't fail immediately — will retry
            } else {
                requests_sent++;
                SPDLOG_LOGGER_DEBUG(log,
                    "ARP request #{} sent for {}",
                    requests_sent, net::format_ipv4(target_ip).data());
            }

            next_send = now + retry_interval;
        }

        // Poll for ARP reply
        rte_mbuf* pkts[16];
        uint16_t nb_rx = rte_eth_rx_burst(port_id, queue_id, pkts, 16);

        for (uint16_t i = 0; i < nb_rx; ++i) {
            auto mac = parse_arp_reply(pkts[i], target_ip);
            if (mac) {
                SPDLOG_LOGGER_INFO(log,
                    "ARP resolved: {} -> {} (after {} request(s))",
                    net::format_ipv4(target_ip).data(),
                    detail::format_mac(*mac).data(),
                    requests_sent);

                // Free remaining packets
                for (uint16_t j = i + 1; j < nb_rx; ++j) {
                    rte_pktmbuf_free(pkts[j]);
                }
                rte_pktmbuf_free(pkts[i]);
                return *mac;
            }
            rte_pktmbuf_free(pkts[i]);
        }
    }

    SPDLOG_LOGGER_ERROR(log,
        "ARP resolve timeout: {} not resolved after {}ms ({} requests sent)",
        net::format_ipv4(target_ip).data(),
        timeout.count(), requests_sent);

    return std::unexpected(std::format(
        "ARP resolve timeout: {} not resolved after {}ms",
        net::format_ipv4(target_ip).data(), timeout.count()));
}

} // namespace eph::dpdk::arp
