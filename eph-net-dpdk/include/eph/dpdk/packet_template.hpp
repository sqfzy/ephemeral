#pragma once

/// @file packet_template.hpp
/// Precomputed packet templates for fast TCP and UDP packet construction.
///
/// PacketTemplate (TCP) and UdpPacketTemplate pre-fill static header fields
/// at setup time. On the hot path, only dynamic fields are updated per packet.

#include "eph/dpdk/packet_core.hpp"

#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_tcp.h>

#ifndef NDEBUG
#include <cassert>
#include <thread>
#endif

namespace eph::dpdk::net {

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

#ifndef NDEBUG
    /// Debug-only owner thread id. Lazily captured on first ip_id-mutating
    /// call; asserts every subsequent call arrives on the same thread so
    /// silent concurrent corruption of ip_id is caught in tests. Release
    /// builds (NDEBUG) compile this out completely.
    mutable std::thread::id owner_tid_{};
#endif

#ifndef NDEBUG
    /// Debug-only single-thread check. Lazily captures the first caller's
    /// thread id and asserts all subsequent ip_id-mutating calls originate
    /// from the same thread. Inline so the release build drops it entirely.
    void debug_check_single_thread_() const noexcept {
        const auto tid = std::this_thread::get_id();
        if (owner_tid_ == std::thread::id{}) {
            owner_tid_ = tid;
        } else {
            assert(owner_tid_ == tid &&
                   "PacketTemplate used from multiple threads; ip_id "
                   "increment is not synchronised — each TX thread must "
                   "own its own PacketTemplate instance");
        }
    }
#endif

    /// Validate template fields. Returns empty string_view if valid.
    [[nodiscard]] constexpr std::string_view validate() const noexcept {
        if (tuple.src_ip == 0) return "src_ip must not be zero";
        if (tuple.dst_ip == 0) return "dst_ip must not be zero";
        if (tuple.src_port == 0) return "src_port must be explicit (DPDK has no ephemeral port allocator)";
        if (tuple.dst_port == 0) return "dst_port must be explicit (DPDK has no ephemeral port allocator)";
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
#ifndef NDEBUG
        debug_check_single_thread_();
#endif
        rte_mbuf* mbuf = rte_pktmbuf_alloc(pool);
        if (!mbuf) return nullptr;

        // SYN packets include TCP options (MSS, SACK, WinScale) to avoid
        // being dropped by middleboxes that expect standard TCP options.
        const bool is_syn = (flags & kTcpSyn) != 0;
        const uint16_t tcp_hdr_len = is_syn ? kSynTcpHeaderLen : kTcpHeaderLen;
        // Use uint32_t for the addition to prevent uint16_t overflow when
        // payload_len is close to UINT16_MAX. Then validate the result fits
        // in a uint16_t (max Ethernet frame) before allocating the mbuf.
        const uint32_t total_len32 = static_cast<uint32_t>(kEtherHeaderLen)
                                   + kIpv4HeaderLen + tcp_hdr_len + payload_len;
        if (total_len32 > UINT16_MAX) [[unlikely]] {
            rte_pktmbuf_free(mbuf);
            return nullptr;
        }
        const uint16_t total_len = static_cast<uint16_t>(total_len32);

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
#ifndef NDEBUG
        debug_check_single_thread_();
#endif

        // SYN requires TCP options (MSS, SACK, WScale) — use build_packet() instead.
        if (flags & kTcpSyn) [[unlikely]] {
            // Programming error: use build_packet() for SYN segments
            return 0;
        }

        // Mirror build_packet's overflow guard: kAllHeadersLen + payload_len
        // can wrap a uint16_t when payload_len approaches UINT16_MAX (-54
        // headroom). Without this, a wrapped total_len would let
        // rte_pktmbuf_append succeed for a tiny size and the subsequent
        // payload memcpy below would walk off the mbuf data buffer. Compute
        // in uint32_t and reject anything past UINT16_MAX.
        const uint32_t total_len32 = static_cast<uint32_t>(kAllHeadersLen)
                                   + payload_len;
        if (total_len32 > UINT16_MAX) [[unlikely]] return 0;
        const uint16_t total_len = static_cast<uint16_t>(total_len32);

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

#ifndef NDEBUG
    /// Debug-only owner thread id. Lazily captured on first fill() call.
    /// Subsequent calls from a different thread trigger an assertion. Release
    /// builds compile this member (and the accompanying check) out entirely
    /// so there is zero hot-path cost.
    mutable std::thread::id owner_tid_{};
#endif

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

#ifndef NDEBUG
        // Debug-only single-thread assertion: ip_id_ is incremented without
        // synchronization, so concurrent fill() calls would corrupt it. Lazily
        // capture the first caller's thread id and assert every subsequent
        // call comes from the same thread. Release builds compile this out.
        const auto tid = std::this_thread::get_id();
        if (owner_tid_ == std::thread::id{}) {
            owner_tid_ = tid;
        } else {
            assert(owner_tid_ == tid &&
                   "UdpPacketTemplate::fill called from multiple threads; "
                   "each TX thread must own its own UdpPacketTemplate");
        }
#endif

        // Use uint32_t to detect overflow before truncating to uint16_t.
        // kUdpAllHeadersLen (42) + payload_len could exceed 65535 with jumbo payloads.
        const uint32_t total_len32 = static_cast<uint32_t>(kUdpAllHeadersLen) + payload_len;
        if (total_len32 > UINT16_MAX) [[unlikely]] return 0;
        const auto total_len = static_cast<uint16_t>(total_len32);

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

} // namespace eph::dpdk::net

/// @brief std::formatter specialization for PacketTemplate.
///
/// Formats as a compact one-line summary with MACs, IPs, ports, and MSS.
template <>
struct std::formatter<eph::dpdk::net::PacketTemplate> : std::formatter<std::string> {
    auto format(const eph::dpdk::net::PacketTemplate& t, auto& ctx) const {
        return std::formatter<std::string>::format(t.dump(), ctx);
    }
};
