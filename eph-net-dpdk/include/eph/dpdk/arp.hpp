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
#include <format>
#include <optional>
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

/// @name ARP protocol constants (RFC 826)
/// @{
inline constexpr uint16_t kEtherTypeArp      = 0x0806;  ///< EtherType for ARP
inline constexpr uint16_t kArpHwTypeEthernet = 1;        ///< Hardware type: Ethernet
inline constexpr uint16_t kArpProtoIpv4      = 0x0800;   ///< Protocol type: IPv4
inline constexpr uint16_t kArpOpRequest      = 1;         ///< ARP operation: Request
inline constexpr uint16_t kArpOpReply        = 2;         ///< ARP operation: Reply
inline constexpr uint8_t  kArpHwAddrLen      = 6;         ///< MAC address length (bytes)
inline constexpr uint8_t  kArpProtoAddrLen   = 4;         ///< IPv4 address length (bytes)
inline constexpr size_t   kArpPacketLen      = 28;         ///< ARP payload size (bytes, after Ethernet header)
/// @}

/// @brief Ethernet broadcast MAC address (ff:ff:ff:ff:ff:ff).
inline constexpr rte_ether_addr kBroadcastMac = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};

/// @brief Burst size for the ARP-resolve RX poll. ARP resolve is a
///        startup-only blocking helper (not hot-path), so a small burst
///        suffices — a bigger burst just holds the mempool longer with
///        no throughput benefit. Kept separate from DpdkPoller::kBurstSize
///        (32) so arp.hpp stays independent of poller.hpp.
inline constexpr uint16_t kArpResolveBurstSize = 16;

// ─────────────────────────────────────────────────────────────────────────────
// ARP packet structure (RFC 826)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief ARP packet payload (RFC 826). Follows the Ethernet header. 28 bytes, packed.
///
/// All multi-byte fields are in network byte order on the wire.
struct ArpPacket {
    uint16_t hw_type;         ///< Hardware type (1 = Ethernet)
    uint16_t proto_type;      ///< Protocol type (0x0800 = IPv4)
    uint8_t  hw_addr_len;     ///< Hardware address length (6 for Ethernet)
    uint8_t  proto_addr_len;  ///< Protocol address length (4 for IPv4)
    uint16_t opcode;          ///< Operation: 1 = request, 2 = reply
    uint8_t  sender_mac[6];   ///< Sender hardware (MAC) address
    uint32_t sender_ip;       ///< Sender protocol (IPv4) address, network byte order
    uint8_t  target_mac[6];   ///< Target hardware (MAC) address (zero in requests)
    uint32_t target_ip;       ///< Target protocol (IPv4) address, network byte order
} __attribute__((packed));

static_assert(sizeof(ArpPacket) == kArpPacketLen,
    "ArpPacket must be exactly 28 bytes");

// ─────────────────────────────────────────────────────────────────────────────
// Logger
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

inline spdlog::logger* arp_logger() { return eph::dpdk::detail::get_logger<eph::dpdk::detail::LoggerName{"dpdk.arp"}>(); }

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
[[nodiscard]] inline rte_mbuf* build_arp_request(rte_mempool* pool,
                                    const rte_ether_addr& src_mac,
                                    uint32_t src_ip,
                                    uint32_t target_ip) noexcept {
    // Defensive: rte_pktmbuf_alloc dereferences pool with no NULL check
    // and segfaults on nullptr.  Match the contract of
    // PacketTemplate::build_packet (returns nullptr on null pool) so
    // callers can fail fast without a crash.
    if (!pool) [[unlikely]] {
        SPDLOG_LOGGER_ERROR(detail::arp_logger(),
            "ARP request: null mempool — cannot allocate");
        return nullptr;
    }
    auto* mbuf = rte_pktmbuf_alloc(pool);
    if (!mbuf) {
        SPDLOG_LOGGER_ERROR(detail::arp_logger(),
            "ARP request: rte_pktmbuf_alloc failed, mempool may be exhausted");
        return nullptr;
    }

    constexpr uint16_t frame_len = net::kEtherHeaderLen + sizeof(ArpPacket);
    auto* pkt = reinterpret_cast<uint8_t*>(
        rte_pktmbuf_append(mbuf, frame_len));
    if (!pkt) {
        SPDLOG_LOGGER_ERROR(detail::arp_logger(),
            "ARP request: rte_pktmbuf_append failed for {} bytes (pool_avail={})",
            frame_len, rte_mempool_avail_count(pool));
        rte_pktmbuf_free(mbuf);
        return nullptr;
    }

    // Ethernet header: broadcast destination
    auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt);
    rte_ether_addr_copy(&kBroadcastMac, &eth->dst_addr);
    rte_ether_addr_copy(&src_mac, &eth->src_addr);
    eth->ether_type = net::hton16(kEtherTypeArp);

    // ARP payload
    auto* arp = reinterpret_cast<ArpPacket*>(pkt + net::kEtherHeaderLen);
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

/// @note Security: ARP has no built-in authentication. In production DPDK
///       deployments, gateway_mac is typically pre-configured (bypassing ARP
///       entirely) or validated against a known MAC allowlist via the
///       `expected_mac` parameter below. This function validates: opcode,
///       hw/proto types, target IP match, and sender-MAC well-formedness
///       (rejects all-zero and non-unicast / I-G-bit-set source MACs). When
///       `expected_mac` is supplied, replies from any other sender MAC are
///       also rejected. Reflection-style attacks where a forged reply
///       carries a `target_ip` field that is not our local IP are NOT
///       detected — we only filter on `sender_ip == target_ip` (the IP
///       being resolved). Pre-configured gateway MAC + `expected_mac` is
///       the recommended HFT colo posture.
///
/// Parse an ARP reply from a received mbuf.
/// Returns the sender MAC if the packet is a valid ARP reply for target_ip.
///
/// @param mbuf          Received packet
/// @param target_ip     The IP we're resolving (host byte order)
/// @param expected_mac  If set, reject replies whose sender MAC differs (anti-spoof).
///                      In HFT colo, the gateway MAC is typically static and known.
/// @param expected_local_ip  If set, reject replies whose `target_ip` field
///                           does not match (reflection-style attack mitigation
///                           — RFC 826's reply.target_ip should be the
///                           requesting host's IP, i.e. our src_ip).
///                           `nullopt` keeps the legacy permissive behaviour
///                           the fuzzer harness exercises. Production callers
///                           that resolve via `resolve_with_io` get this set
///                           automatically.
/// @return Sender MAC if match, std::nullopt otherwise
[[nodiscard]] inline std::optional<rte_ether_addr>
parse_arp_reply(const rte_mbuf* mbuf, uint32_t target_ip,
                std::optional<rte_ether_addr> expected_mac = std::nullopt,
                std::optional<uint32_t> expected_local_ip = std::nullopt) noexcept {
    constexpr size_t min_len = net::kEtherHeaderLen + sizeof(ArpPacket);
    // Defensive: parse_arp_reply is reachable from the fuzzer harness and
    // from the resolve() loop where a nullptr mbuf should never appear
    // in practice — but the fuzz harness has found off-by-one mistakes
    // before. A null-check keeps us from dereferencing and crashing if
    // a caller inadvertently hands in nullptr.
    if (mbuf == nullptr) [[unlikely]] return std::nullopt;
    // Reject multi-segment mbufs: ARP frames are 42 bytes and never
    // scatter in practice, but single-segment is what our bounds math
    // assumes (see parse_ip_header in packet_parse.hpp for the same
    // defense-in-depth rationale).
    if (mbuf->nb_segs > 1) [[unlikely]] return std::nullopt;
    if (rte_pktmbuf_data_len(mbuf) < min_len) return std::nullopt;

    auto* pkt = rte_pktmbuf_mtod(mbuf, const uint8_t*);
    auto* eth = reinterpret_cast<const rte_ether_hdr*>(pkt);

    // Check EtherType = ARP
    if (net::ntoh16(eth->ether_type) != kEtherTypeArp) return std::nullopt;

    auto* arp = reinterpret_cast<const ArpPacket*>(pkt + net::kEtherHeaderLen);

    // Validate ARP reply for IPv4 over Ethernet (RFC 826)
    if (net::ntoh16(arp->hw_type) != kArpHwTypeEthernet) return std::nullopt;
    if (net::ntoh16(arp->proto_type) != kArpProtoIpv4) return std::nullopt;
    if (arp->hw_addr_len != kArpHwAddrLen) return std::nullopt;
    if (arp->proto_addr_len != kArpProtoAddrLen) return std::nullopt;
    if (net::ntoh16(arp->opcode) != kArpOpReply) return std::nullopt;

    // Check sender IP matches the address we're resolving
    if (net::ntoh32(arp->sender_ip) != target_ip) return std::nullopt;

    // Optional reflection-attack mitigation: ARP reply's `target_ip` field
    // (RFC 826) should be the requester's IP — our local src_ip. A reply
    // carrying a different target_ip is either an unsolicited gratuitous
    // ARP for a different host (legitimate but irrelevant to our resolve)
    // or a forged/reflected packet whose payload happens to use a real
    // gateway as `sender_ip`. Either way, accepting it would let an
    // attacker poison our cache with the gateway's MAC at any time,
    // bypassing the `expected_mac` allowlist. When `expected_local_ip`
    // is supplied (the production path) we reject mismatches; the
    // fuzzer harness leaves this `nullopt` to keep the legacy permissive
    // shape the corpus was built against.
    if (expected_local_ip.has_value()) {
        const uint32_t reply_target = net::ntoh32(arp->target_ip);
        if (reply_target != *expected_local_ip) {
            SPDLOG_LOGGER_WARN(detail::arp_logger(),
                "ARP reply target_ip mismatch: got {}, expected {} — "
                "possible reflection attack, rejecting",
                net::format_ipv4(reply_target).data(),
                net::format_ipv4(*expected_local_ip).data());
            return std::nullopt;
        }
    }

    rte_ether_addr result;
    std::memcpy(result.addr_bytes, arp->sender_mac, 6);

    // Reject degenerate / non-unicast sender MACs up-front. IEEE 802.3
    // source addresses must be individual (unicast) — the I/G bit of the
    // first octet must be clear. In practice attackers sometimes probe with
    // 00:00:00:00:00:00 (unset) or ff:ff:ff:ff:ff:ff (broadcast) hoping a
    // caller caches them blindly; both are unusable as next-hop MACs and
    // would break subsequent L2 forwarding.
    {
        const bool all_zero = (result.addr_bytes[0] | result.addr_bytes[1] |
                               result.addr_bytes[2] | result.addr_bytes[3] |
                               result.addr_bytes[4] | result.addr_bytes[5]) == 0;
        const bool is_group = (result.addr_bytes[0] & 0x01u) != 0;
        if (all_zero || is_group) {
            SPDLOG_LOGGER_WARN(detail::arp_logger(),
                "ARP reply sender MAC invalid ({} — {}), rejecting",
                net::format_mac(result).data(),
                all_zero ? "all-zero" : "non-unicast I/G bit set");
            return std::nullopt;
        }
    }

    // Optional anti-spoof: reject replies from unexpected MACs
    if (expected_mac.has_value()) {
        if (std::memcmp(result.addr_bytes,
                        expected_mac->addr_bytes, 6) != 0) {
            SPDLOG_LOGGER_WARN(detail::arp_logger(),
                "ARP reply sender MAC mismatch: got {}, expected {} — "
                "possible ARP spoofing, rejecting",
                net::format_mac(result).data(),
                net::format_mac(*expected_mac).data());
            return std::nullopt;
        }
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// NIC IO shim — see dns.hpp::RealNicIo for the rationale. Pulling the
// PMD calls behind a static-method facade lets tests substitute a
// `FakeNicIo` recorder while production code remains a single call
// instruction (the compiler inlines the static through). Pure
// pass-through, zero overhead. `noexcept` is genuine — DPDK PMD
// burst calls are documented `noexcept`.
// ─────────────────────────────────────────────────────────────────────────────
struct RealNicIoArp {
    [[nodiscard]] static uint16_t tx_burst(uint16_t port_id, uint16_t queue_id,
                                            rte_mbuf** pkts, uint16_t n) noexcept {
        return ::rte_eth_tx_burst(port_id, queue_id, pkts, n);
    }
    [[nodiscard]] static uint16_t rx_burst(uint16_t port_id, uint16_t queue_id,
                                            rte_mbuf** pkts, uint16_t n) noexcept {
        return ::rte_eth_rx_burst(port_id, queue_id, pkts, n);
    }
};

/// @brief Templated ARP resolver — the testable workhorse.
///
/// Identical algorithm and observability to `resolve()`; the only
/// difference is that NIC I/O is dispatched through the `Io` template
/// parameter. The default `RealNicIoArp` calls the PMD directly so
/// production callers see no behavioural change. Unit tests substitute
/// a `FakeNicIo` shim that records sends and synthesises crafted
/// replies — see `tests/test_arp_resolve.cpp`.
///
/// This split exists *only* for testability; the inner loop is the same
/// state machine the original `resolve()` had (send / retry-on-deadline
/// / RX-poll / parse-and-match / timeout). Behaviour is byte-for-byte
/// identical to `resolve()` in production builds: GCC inlines through
/// the static `Io::tx_burst` / `Io::rx_burst` calls so the generated
/// instructions are unchanged.
///
/// @tparam Io   NIC IO shim. Default `RealNicIoArp`. Must expose static
///              `tx_burst(port, queue, mbuf**, n) -> uint16_t` and
///              `rx_burst(port, queue, mbuf**, n) -> uint16_t`.
template <class Io = RealNicIoArp>
[[nodiscard]] inline std::expected<rte_ether_addr, std::string>
resolve_with_io(uint16_t port_id,
                uint16_t queue_id,
                rte_mempool* pool,
                const rte_ether_addr& src_mac,
                uint32_t src_ip,
                uint32_t target_ip,
                std::chrono::milliseconds timeout = std::chrono::milliseconds{1000},
                std::optional<rte_ether_addr> expected_mac = std::nullopt) {

    [[maybe_unused]] auto log = detail::arp_logger();

    if (!pool) {
        return std::unexpected("ARP resolve: mempool is null");
    }

    SPDLOG_LOGGER_DEBUG(log, "ARP resolve: {} -> {} on port {} queue {}",
        net::format_ipv4(src_ip).data(),
        net::format_ipv4(target_ip).data(),
        port_id, queue_id);

    auto deadline = std::chrono::steady_clock::now() + timeout;

    // Retry interval: send ARP request up to 3 times, evenly spaced.
    // ARP is a LAN protocol — shorter retry is safe since replies
    // are expected within microseconds. DNS crosses routers and
    // requires a higher floor (100ms in dns.hpp).
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
                // Symmetric with the tx_burst WARN below — surface
                // target IP + port + queue + attempt so the operator
                // sees which ARP lookup died and where. Pool exhaustion
                // is the dominant cause; we terminate rather than retry
                // because the pool will not refill mid-loop.
                SPDLOG_LOGGER_ERROR(log,
                    "ARP resolve: mbuf allocation failed (target={} "
                    "port={} queue={} request_attempt={})",
                    net::format_ipv4(target_ip).data(),
                    port_id, queue_id, requests_sent + 1);
                return std::unexpected(std::format(
                    "ARP resolve: mbuf allocation failed for target={} "
                    "(port={} queue={} attempt={})",
                    net::format_ipv4(target_ip).data(),
                    port_id, queue_id, requests_sent + 1));
            }

            uint16_t sent = Io::tx_burst(port_id, queue_id, &req, 1);
            if (sent != 1) {
                rte_pktmbuf_free(req);
                SPDLOG_LOGGER_WARN(log,
                    "ARP resolve: tx_burst failed (port={} queue={} "
                    "request_attempt={}); will retry after {}ms backoff",
                    port_id, queue_id, requests_sent + 1,
                    retry_interval.count());
                // Don't fail immediately — will retry. The
                // `next_send = now + retry_interval` below applies
                // unconditionally so a persistently broken TX path
                // (mempool drained, ring full, link down) can't
                // turn the outer poll loop into a busy-spin: each
                // retry is gated by the same retry_interval as the
                // success path, with the request count NOT
                // incremented (so we still get up to 3 actual sends
                // before giving up).
            } else {
                requests_sent++;
                SPDLOG_LOGGER_DEBUG(log,
                    "ARP request #{} sent for {}",
                    requests_sent, net::format_ipv4(target_ip).data());
            }

            next_send = now + retry_interval;
        }

        // Poll for ARP reply
        rte_mbuf* pkts[kArpResolveBurstSize];
        uint16_t nb_rx = Io::rx_burst(port_id, queue_id, pkts,
                                       kArpResolveBurstSize);

        for (uint16_t i = 0; i < nb_rx; ++i) {
            // Pass our `src_ip` as `expected_local_ip` so a reflection-
            // style reply (forged sender_ip == our gateway, but target_ip
            // pointing at someone else) is rejected — see parse_arp_reply
            // doc for the full rationale.
            auto mac = parse_arp_reply(pkts[i], target_ip, expected_mac,
                                        /*expected_local_ip=*/src_ip);
            if (mac) {
                SPDLOG_LOGGER_INFO(log,
                    "ARP resolved: {} -> {} (after {} request(s))",
                    net::format_ipv4(target_ip).data(),
                    net::format_mac(*mac).data(),
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

/// Resolve an IPv4 address to a MAC address via ARP.
///
/// Sends an ARP request as Ethernet broadcast and busy-polls for the reply.
/// Retries the ARP request up to 3 times within the timeout period.
///
/// Must be called BEFORE TCP connection establishment — this function will
/// discard any non-ARP packets received during the poll.
///
/// **RX queue is hardcoded to 0**. ARP packets carry EtherType 0x0806
/// which is NOT in any RSS hash set the project enables (see
/// `platform.hpp:1113-1115` — `RTE_ETH_RSS_NONFRAG_IPV4_TCP/UDP/IPV4`
/// only). Non-IP traffic falls to the NIC's default RX queue, which is
/// queue 0 on every PMD currently supported (notably AWS ENA). Allowing
/// callers to pass a non-zero `queue_id` (the pre-fix API) led to silent
/// timeouts on RSS-active multi-queue Platforms because the reply
/// landed on queue 0 while the caller polled queue N≠0. The parameter
/// was removed to make that mistake unrepresentable.
///
/// @note Pass `expected_mac` to enable anti-spoof: replies from any MAC
///       other than the configured gateway MAC are rejected. In HFT colo
///       deployments the gateway MAC is static and known in advance —
///       always pass it. When `expected_mac == nullopt` the first reply
///       with a matching sender IP wins (legacy behaviour).
///
/// @param port_id       DPDK port to send/receive on
/// @param pool          Mempool for mbuf allocation
/// @param src_mac       Our NIC's MAC address
/// @param src_ip        Our IPv4 address (host byte order)
/// @param target_ip     IPv4 address to resolve (host byte order)
/// @param timeout       Maximum wait time (default 1s)
/// @param expected_mac  If set, reject replies whose sender MAC differs
///                      (see `parse_arp_reply` doc). `nullopt` = legacy
///                      behaviour (accept any sender MAC for target_ip).
/// @return Resolved MAC address, or error string on timeout/failure
///
/// @note Thin wrapper around `resolve_with_io<RealNicIoArp>` with
///       queue=0. The testability split keeps `resolve_with_io`
///       parametric on queue_id so unit tests can exercise both queues
///       through a fake Io shim.
[[nodiscard]] inline std::expected<rte_ether_addr, std::string>
resolve(uint16_t port_id,
        rte_mempool* pool,
        const rte_ether_addr& src_mac,
        uint32_t src_ip,
        uint32_t target_ip,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{1000},
        std::optional<rte_ether_addr> expected_mac = std::nullopt) {
    return resolve_with_io<RealNicIoArp>(
        port_id, /*queue_id=*/0, pool, src_mac, src_ip, target_ip,
        timeout, expected_mac);
}

} // namespace eph::dpdk::arp
