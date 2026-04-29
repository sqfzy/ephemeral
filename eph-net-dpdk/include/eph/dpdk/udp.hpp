#pragma once

/// @file udp.hpp
/// UDP unicast sender — precomputed packet template with resource binding.
///
/// Provides UdpSender (connected UDP TX handle) and build_udp_packet()
/// (one-shot convenience function). Designed for fixed-peer, high-frequency
/// UDP transmission in DPDK user-space networking.
///
/// Usage:
///   auto sender = UdpSender::create(config);
///   sender->send(payload, len);             // single packet
///   sender->send_batch(segments, count);    // burst of packets
///
/// For one-shot sends without UdpSender:
///   auto* mbuf = build_udp_packet(pool, src_mac, dst_mac, ...);
///   rte_eth_tx_burst(port, queue, &mbuf, 1);

#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>

#include "eph/dpdk/net_header.hpp"

namespace eph::dpdk {

namespace detail {
inline spdlog::logger* udp_logger() { return get_logger<LoggerName{"dpdk.udp"}>(); }
} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Configuration for UdpSender — fixed-peer UDP TX parameters.
///
/// All IP/port fields are in host byte order. Conversion to network byte
/// order happens internally in UdpPacketTemplate::init().
struct UdpConfig {
    uint32_t src_ip{};              ///< Source IPv4 address (host byte order)
    uint32_t dst_ip{};              ///< Destination IPv4 address (host byte order)
    uint16_t src_port{};            ///< Source UDP port (host byte order)
    uint16_t dst_port{};            ///< Destination UDP port (host byte order)
    rte_ether_addr src_mac{};       ///< Source Ethernet address
    rte_ether_addr dst_mac{};       ///< Destination Ethernet address (usually gateway)
    uint16_t port_id{};             ///< DPDK port ID for TX
    uint16_t tx_queue_id{};         ///< TX queue index on the DPDK port
    rte_mempool* pool{nullptr};     ///< mbuf pool for packet allocation
    bool hw_cksum{false};           ///< Enable NIC checksum offload (IP + UDP)

    // Note: this struct is TX-only. RX of UDP datagrams goes
    // through `eph::net::dpdk::DpdkUdpSocket::process_burst_`, which
    // is dispatched by `DpdkPoller` using `PollerConfig::rx_queue_id`.
    // There is no RX queue field on this struct on purpose.

    /// Validate that all required fields are set.
    /// constexpr for compile-time validation of IP/port fields; pool check is runtime-only.
    /// @return Empty string_view if valid, error description otherwise.
    [[nodiscard]] constexpr std::string_view validate() const noexcept {
        if (src_ip == 0) return "src_ip must not be zero";
        if (dst_ip == 0) return "dst_ip must not be zero";
        if (src_port == 0) return "src_port must be explicit (DPDK has no ephemeral port allocator)";
        if (dst_port == 0) return "dst_port must be explicit (DPDK has no ephemeral port allocator)";
        if (pool == nullptr) return "pool must not be null";
        return {};
    }

    /// Human-readable dump for logging/diagnostics.
    [[nodiscard]] std::string dump() const {
        return std::format("UdpConfig({}:{} -> {}:{}, port={}, tx_q={}, hw_cksum={})",
                           net::format_ipv4(src_ip).data(), src_port,
                           net::format_ipv4(dst_ip).data(), dst_port,
                           port_id, tx_queue_id, hw_cksum);
    }

    /// JSON-formatted config for monitoring system integration.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"src_ip\":\"{}\",\"src_port\":{},\"dst_ip\":\"{}\",\"dst_port\":{},"
            "\"port_id\":{},\"tx_queue_id\":{},\"hw_cksum\":{}}}",
            net::format_ipv4(src_ip).data(), src_port,
            net::format_ipv4(dst_ip).data(), dst_port,
            port_id, tx_queue_id, hw_cksum ? "true" : "false");
    }

    /// Equality comparison (pool pointer compared by identity, MACs by memcmp).
    [[nodiscard]] friend bool operator==(const UdpConfig& a,
                                          const UdpConfig& b) noexcept {
        return a.src_ip == b.src_ip && a.dst_ip == b.dst_ip
            && a.src_port == b.src_port && a.dst_port == b.dst_port
            && std::memcmp(&a.src_mac, &b.src_mac, sizeof(rte_ether_addr)) == 0
            && std::memcmp(&a.dst_mac, &b.dst_mac, sizeof(rte_ether_addr)) == 0
            && a.port_id == b.port_id && a.tx_queue_id == b.tx_queue_id
            && a.pool == b.pool
            && a.hw_cksum == b.hw_cksum;
    }

    /// Check for non-fatal misconfigurations. Unlike validate() which blocks
    /// construction, these are advisory.
    ///
    /// Symmetric with TcpConfig::warnings — both transports run on the same
    /// underlying DPDK PMD and share the same failure modes (loopback IPs
    /// don't reach the NIC, all-zero MACs survive validate but produce
    /// silent-drop packets at L2). Until this round the UDP path only
    /// flagged hw_cksum, leaving the same uninitialised-config classes that
    /// the TCP path catches as silent failures for UDP callers.
    [[nodiscard]] std::vector<std::string> warnings() const {
        std::vector<std::string> w;
        if (hw_cksum)
            w.emplace_back("hw_cksum=true — ensure NIC supports UDP checksum offload");
        // Loopback IPs in src or dst — DPDK bypasses the kernel, so
        // 127.0.0.0/8 traffic never reaches the wire. Mirror TcpConfig.
        if ((src_ip >> 24) == 127)
            w.emplace_back("src_ip is in 127.0.0.0/8 (loopback) -- "
                           "DPDK bypasses the kernel, loopback traffic "
                           "will not reach the NIC");
        if ((dst_ip >> 24) == 127)
            w.emplace_back("dst_ip is in 127.0.0.0/8 (loopback) -- "
                           "DPDK bypasses the kernel, loopback traffic "
                           "will not reach the NIC");
        // Zero MACs survive validate (it only checks IPs / ports / pool)
        // but produce frames the switch silently drops. Surface as advisory
        // so the operator sees it before chasing a "no traffic on the wire"
        // ghost — same shape TcpConfig::warnings has carried for months.
        rte_ether_addr zero_mac{};
        if (std::memcmp(&src_mac, &zero_mac, sizeof(rte_ether_addr)) == 0)
            w.emplace_back("src_mac is all zeros -- likely uninitialized");
        if (std::memcmp(&dst_mac, &zero_mac, sizeof(rte_ether_addr)) == 0)
            w.emplace_back("dst_mac is all zeros -- likely uninitialized");
        return w;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Batch send types
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Describes a single UDP payload segment for batch sending.
struct UdpSegment {
    const void* data{nullptr};  ///< Payload data pointer
    uint16_t len{0};            ///< Payload length in bytes
};

// ─────────────────────────────────────────────────────────────────────────────
// Statistics
// ─────────────────────────────────────────────────────────────────────────────

/// @brief UDP sender statistics (non-atomic, snapshot semantics).
struct UdpSenderStats {
    uint64_t tx_packets{0};   ///< Successfully transmitted packets
    uint64_t tx_bytes{0};     ///< Total payload bytes transmitted
    uint64_t tx_errors{0};    ///< Failed sends (alloc failure or TX burst returned 0)

    /// JSON-formatted statistics for monitoring.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"tx_packets\":{},\"tx_bytes\":{},\"tx_errors\":{}}}",
            tx_packets, tx_bytes, tx_errors);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// UdpSender
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Connected UDP TX handle — binds a precomputed packet template to
///        DPDK resources (mempool, port, queue).
///
/// Fixed-peer design: destination IP:port is set at create() time and cannot
/// be changed. Each send() only needs (payload, len) — all headers are
/// precomputed. Use separate UdpSender instances for different destinations.
///
/// @note Not thread-safe. Each TX thread must have its own UdpSender instance.
///       For multi-threaded TX to the same destination, create one instance
///       per thread with separate TX queues.
class UdpSender {
    net::UdpPacketTemplate tmpl_{};
    rte_mempool* pool_{nullptr};
    uint16_t port_id_{0};
    uint16_t tx_queue_id_{0};
    UdpSenderStats stats_{};

    UdpSender() = default;

public:
    /// Maximum packets per send_batch call (matches DPDK burst conventions).
    static constexpr uint16_t kMaxBatchSize = 32;

    /// @brief Create a UdpSender from configuration.
    ///
    /// Validates the config, checks NIC checksum offload capability if
    /// hw_cksum is requested, and initializes the internal packet template.
    ///
    /// @param cfg UDP sender configuration (all fields in host byte order)
    /// @return UdpSender on success, error string on validation failure
    [[nodiscard]] static std::expected<UdpSender, std::string>
    create(const UdpConfig& cfg) noexcept {
        [[maybe_unused]] auto* log = detail::udp_logger();

        auto err = cfg.validate();
        if (!err.empty()) {
            SPDLOG_LOGGER_ERROR(log, "UdpSender::create config validation failed: {}", err);
            return std::unexpected(std::string(err));
        }

        // Surface non-fatal misconfigurations (currently just hw_cksum
        // without a verified NIC capability). Advisory only — does not
        // block construction. Parallel to Platform::create emitting
        // PlatformConfig::warnings() at WARN.
        for (const auto& w : cfg.warnings()) {
            SPDLOG_LOGGER_WARN(log, "UdpConfig advisory: {}", w);
        }

        // Verify NIC supports UDP checksum offload if requested
        if (cfg.hw_cksum) {
            rte_eth_dev_info dev_info{};
            int ret = rte_eth_dev_info_get(cfg.port_id, &dev_info);
            if (ret != 0) {
                // rte_errno carries the actual cause (-EINVAL = invalid
                // port_id, -ENOTSUP = unsupported driver, -ENODEV =
                // detached). Capturing it here means callers don't have
                // to grep DPDK source to interpret a bare ret=-22.
                const int err = rte_errno;
                auto msg = std::format(
                    "rte_eth_dev_info_get failed for port {}: ret={}, "
                    "rte_errno={} ({})",
                    cfg.port_id, ret, err, rte_strerror(err));
                SPDLOG_LOGGER_ERROR(log, "{}", msg);
                return std::unexpected(msg);
            }
            if (!(dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_UDP_CKSUM)) {
                auto msg = std::format(
                    "Port {} does not support UDP TX checksum offload "
                    "(tx_offload_capa=0x{:016x})",
                    cfg.port_id, dev_info.tx_offload_capa);
                SPDLOG_LOGGER_ERROR(log, "{}", msg);
                return std::unexpected(msg);
            }
            SPDLOG_LOGGER_DEBUG(log, "Port {} supports UDP TX checksum offload", cfg.port_id);
        }

        UdpSender sender;
        sender.tmpl_.init(cfg.src_mac, cfg.dst_mac,
                          cfg.src_ip, cfg.dst_ip,
                          cfg.src_port, cfg.dst_port,
                          cfg.hw_cksum);
        sender.pool_ = cfg.pool;
        sender.port_id_ = cfg.port_id;
        sender.tx_queue_id_ = cfg.tx_queue_id;

        SPDLOG_LOGGER_INFO(log, "UdpSender created: {}", cfg.dump());

        return sender;
    }

    /// @brief Send a single UDP packet.
    ///
    /// Allocates an mbuf from the pool, fills it with precomputed headers +
    /// payload, and transmits via rte_eth_tx_burst.
    ///
    /// @param data     Payload data
    /// @param len      Payload length in bytes
    /// @return true if the packet was successfully transmitted
    bool send(const void* data, uint16_t len) noexcept {
        rte_mbuf* mbuf = tmpl_.build(pool_, data, len);
        if (!mbuf) [[unlikely]] {
            ++stats_.tx_errors;
            SPDLOG_LOGGER_TRACE(detail::udp_logger(),
                "send: mbuf alloc failed, payload_len={}, port={}, queue={}",
                len, port_id_, tx_queue_id_);
            return false;
        }

        uint16_t sent = rte_eth_tx_burst(port_id_, tx_queue_id_, &mbuf, 1);
        if (sent == 0) [[unlikely]] {
            rte_pktmbuf_free(mbuf);
            ++stats_.tx_errors;
            SPDLOG_LOGGER_TRACE(detail::udp_logger(),
                "send: tx_burst returned 0, payload_len={}, port={}, queue={}",
                len, port_id_, tx_queue_id_);
            return false;
        }

        ++stats_.tx_packets;
        stats_.tx_bytes += len;
        return true;
    }

    /// @brief Send a batch of UDP packets in a single TX burst.
    ///
    /// Builds mbufs for each segment, then transmits them all in one
    /// rte_eth_tx_burst call for maximum throughput. Unreported mbufs
    /// (those not accepted by the NIC) are freed automatically.
    ///
    /// @param segs   Array of payload segments
    /// @param count  Number of segments (capped to kMaxBatchSize)
    /// @return Number of packets actually transmitted
    uint16_t send_batch(const UdpSegment* segs, uint16_t count) noexcept {
        if (!segs || count == 0) [[unlikely]] return 0;

        // Cap to burst limit. Caller passing count > kMaxBatchSize is
        // either a bug or a deliberate "feed me what you've got, keep
        // the rest" usage; either way we must not silently drop the
        // overflow without bumping the error counter, otherwise
        // tx_packets + tx_errors stops summing to the original
        // request size and the operator can't distinguish "NIC is
        // backpressuring" from "we're losing payloads upstream of the
        // NIC".
        if (count > kMaxBatchSize) [[unlikely]] {
            const uint16_t dropped = count - kMaxBatchSize;
            stats_.tx_errors += dropped;
            SPDLOG_LOGGER_TRACE(detail::udp_logger(),
                "send_batch: count={} > kMaxBatchSize={}; "
                "dropping {} trailing segment(s) and bumping tx_errors",
                count, kMaxBatchSize, dropped);
            count = kMaxBatchSize;
        }

        rte_mbuf* mbufs[kMaxBatchSize]{};
        // Track payload lengths of successfully built packets, so stats
        // remain correct even when some segments fail to build (which
        // causes mbufs[] indices to diverge from segs[] indices).
        uint16_t built_lens[kMaxBatchSize]{};
        uint16_t built = 0;

        for (uint16_t i = 0; i < count; ++i) {
            mbufs[built] = tmpl_.build(pool_, segs[i].data, segs[i].len);
            if (mbufs[built]) {
                built_lens[built] = segs[i].len;
                ++built;
            } else {
                ++stats_.tx_errors;
            }
        }

        if (built == 0) return 0;

        uint16_t sent = rte_eth_tx_burst(port_id_, tx_queue_id_, mbufs, built);

        // Free unsent mbufs
        for (uint16_t i = sent; i < built; ++i) {
            rte_pktmbuf_free(mbufs[i]);
            ++stats_.tx_errors;
        }

        // Accumulate stats for sent packets
        for (uint16_t i = 0; i < sent; ++i) {
            ++stats_.tx_packets;
            stats_.tx_bytes += built_lens[i];
        }

        return sent;
    }

    /// @brief Get a snapshot of send statistics.
    [[nodiscard]] const UdpSenderStats& stats() const noexcept { return stats_; }

    /// @brief Access the internal packet template (advanced: direct mbuf manipulation).
    [[nodiscard]] const net::UdpPacketTemplate& packet_template() const noexcept { return tmpl_; }

    /// @brief Human-readable status dump for diagnostics.
    [[nodiscard]] std::string dump() const {
        return std::format("UdpSender({}, port={}, queue={}, stats={})",
                           tmpl_.dump(), port_id_, tx_queue_id_,
                           stats_.to_json());
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Convenience function — one-shot UDP packet construction
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Build a single UDP packet without creating a UdpSender.
///
/// Convenience wrapper that creates a temporary UdpPacketTemplate, initializes
/// it, and builds one packet. Suitable for low-frequency or variable-destination
/// sends (e.g., DNS queries). For repeated sends to the same destination, prefer
/// UdpSender which amortizes the template initialization cost.
///
/// @param pool         mbuf pool (must not be null)
/// @param src_mac      Source MAC address
/// @param dst_mac      Destination MAC address
/// @param src_ip       Source IPv4 (host byte order)
/// @param dst_ip       Destination IPv4 (host byte order)
/// @param src_port     Source UDP port (host byte order)
/// @param dst_port     Destination UDP port (host byte order)
/// @param payload      Payload data
/// @param payload_len  Payload length in bytes
/// @param hw_cksum     Enable NIC checksum offload (default: false)
/// @return Filled mbuf on success, nullptr on failure
[[nodiscard]] inline rte_mbuf* build_udp_packet(
        rte_mempool* pool,
        const rte_ether_addr& src_mac, const rte_ether_addr& dst_mac,
        uint32_t src_ip, uint32_t dst_ip,
        uint16_t src_port, uint16_t dst_port,
        const void* payload, uint16_t payload_len,
        bool hw_cksum = false) noexcept {
    net::UdpPacketTemplate tmpl;
    tmpl.init(src_mac, dst_mac, src_ip, dst_ip, src_port, dst_port, hw_cksum);
    return tmpl.build(pool, payload, payload_len);
}

} // namespace eph::dpdk

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specialization
// ─────────────────────────────────────────────────────────────────────────────

/// @brief std::formatter for UdpConfig — uses dump() for human-readable output.
template <>
struct std::formatter<eph::dpdk::UdpConfig> : std::formatter<std::string> {
    auto format(const eph::dpdk::UdpConfig& c, auto& ctx) const {
        return std::formatter<std::string>::format(c.dump(), ctx);
    }
};

/// @brief std::formatter for UdpSenderStats.
template <>
struct std::formatter<eph::dpdk::UdpSenderStats> : std::formatter<std::string> {
    auto format(const eph::dpdk::UdpSenderStats& s, auto& ctx) const {
        return std::formatter<std::string>::format(s.to_json(), ctx);
    }
};
