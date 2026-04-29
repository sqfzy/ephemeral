#pragma once

/// @file multicast.hpp
/// UDP multicast receiver for equity market data feeds.
///
/// Supports CME MDP3.0 and Nasdaq TotalView (via MoldUDP64) style multicast
/// feeds over DPDK user-space networking. Joins multicast groups via DPDK
/// MAC filter management (RFC 1112 multicast MAC derivation) and delivers
/// raw UDP payloads to user callbacks with zero-copy from NIC RX.
///
/// Usage:
///   MulticastConfig cfg{.port_id = 0, .rx_queue_id = 0};
///   MulticastReceiver receiver(cfg);
///   receiver.join_group({.group_ip = net::parse_ipv4("233.54.12.111"),
///                        .group_port = 26477});
///   receiver.on_packet([](const uint8_t* data, size_t len) { ... });
///   receiver.start();
///   // ... receiver delivers UDP payloads via callback ...
///   receiver.stop();

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>

#include "eph/dpdk/net_header.hpp"
#include "eph/utils/cpu.hpp"

namespace eph::dpdk {

// ─────────────────────────────────────────────────────────────────────────────
// Logger
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {
inline spdlog::logger* multicast_logger() { return get_logger<LoggerName{"dpdk.multicast"}>(); }
} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Maximum multicast groups a single MulticastReceiver can manage.
///
/// Fixed-size array avoids heap allocation on the RX hot path. Increase this
/// if subscribing to more than 8 concurrent feeds (rare in practice — most
/// exchange feeds use 2-4 groups).
inline constexpr size_t kMaxMulticastGroups = 8;

/// UdpHeader and kUdpHeaderLen are defined in net_header.hpp (shared with dns.hpp).
using net::UdpHeader;
using net::kUdpHeaderLen;

/// Minimum Ethernet + IPv4 + UDP header length for a valid multicast packet.
inline constexpr uint16_t kMinUdpPacketLen = net::kUdpAllHeadersLen;

// ParsedUdpPacket and parse_udp_packet live in packet_parse.hpp (brought
// in transitively via net_header.hpp's umbrella). Aliases preserved for
// backward compatibility with pre-refactor `eph::dpdk::ParsedUdpPacket`
// call sites.
using net::ParsedUdpPacket;
using net::parse_udp_packet;

// ─────────────────────────────────────────────────────────────────────────────
// Multicast MAC computation (RFC 1112)
// ─────────────────────────────────────────────────────────────────────────────

/// Derive the multicast Ethernet MAC address from an IPv4 multicast group IP.
///
/// Per RFC 1112, the multicast MAC is formed by placing the low 23 bits of
/// the IPv4 group address into 01:00:5e:XX:XX:XX. The high 9 bits of the IP
/// are discarded, which means multiple IPs can map to the same MAC — the IP
/// layer filters the rest.
///
/// @param group_ip  IPv4 multicast group address in host byte order
/// @return Multicast Ethernet MAC address
[[nodiscard]] constexpr rte_ether_addr multicast_mac_from_ip(uint32_t group_ip) noexcept {
    rte_ether_addr mac{};
    mac.addr_bytes[0] = 0x01;
    mac.addr_bytes[1] = 0x00;
    mac.addr_bytes[2] = 0x5e;
    mac.addr_bytes[3] = static_cast<uint8_t>((group_ip >> 16) & 0x7F); // bit 22..16, mask high bit
    mac.addr_bytes[4] = static_cast<uint8_t>((group_ip >> 8) & 0xFF);  // bits 15..8
    mac.addr_bytes[5] = static_cast<uint8_t>(group_ip & 0xFF);         // bits 7..0
    return mac;
}

/// Check whether an IPv4 address is a valid multicast address (224.0.0.0/4).
/// @param ip  IPv4 address in host byte order
[[nodiscard]] constexpr bool is_multicast_ip(uint32_t ip) noexcept {
    // Multicast range: 224.0.0.0 to 239.255.255.255 (high nibble == 0xE)
    return (ip >> 28) == 0x0E;
}

// ─────────────────────────────────────────────────────────────────────────────
// Multicast group descriptor
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Describes a single multicast group to join.
///
/// Specifies the group IP, UDP port, and an optional source-specific multicast
/// (SSM) filter. Use validate() to check parameters before passing to
/// MulticastReceiver::join_group().
struct MulticastGroup {
    uint32_t group_ip   = 0;   ///< Multicast group IPv4 address (host byte order, must be in 224.0.0.0/4)
    uint16_t group_port = 0;   ///< UDP destination port to listen on (host byte order)
    uint32_t source_ip  = 0;   ///< Source-specific multicast filter (0 = accept any source)

    [[nodiscard]] bool operator==(const MulticastGroup&) const = default;

    /// Validate that the group IP is in the multicast range.
    [[nodiscard]] constexpr std::string_view validate() const noexcept {
        if (group_ip == 0) return "group_ip must not be zero";
        if (!is_multicast_ip(group_ip)) return "group_ip is not in multicast range (224.0.0.0/4)";
        if (group_port == 0) return "group_port must not be zero";
        return {};
    }

    /// Check for non-fatal contradictions or likely misconfigurations.
    /// Returns a list of warning messages (empty if no issues).
    /// Unlike validate() which blocks operation, these are advisory.
    [[nodiscard]] std::vector<std::string> warnings() const {
        std::vector<std::string> w;
        // 224.0.0.x is the "local network control block" (RFC 5771 §4)
        // -- these addresses are not routable and typically reserved for
        // protocols like IGMP, OSPF, VRRP. Using them for market data is
        // almost certainly a mistake.
        if (group_ip != 0 && (group_ip >> 8) == 0xE00000)
            w.emplace_back(std::format(
                "group_ip {} is in 224.0.0.0/24 (local network control block, "
                "RFC 5771) -- these are reserved for protocol use, not market data",
                net::format_ipv4(group_ip).data()));
        // Source-specific multicast with loopback source is unusual
        if (source_ip != 0 && (source_ip >> 24) == 127)
            w.emplace_back(std::format(
                "source_ip {} is in 127.0.0.0/8 (loopback) -- SSM filter "
                "will never match traffic from a real source",
                net::format_ipv4(source_ip).data()));
        // Well-known ports (< 1024) are unusual for multicast data feeds
        if (group_port > 0 && group_port < 1024)
            w.emplace_back(std::format(
                "group_port={} is in the well-known range (< 1024) -- "
                "unusual for market data feeds", group_port));
        return w;
    }

    /// Multi-line formatted dump for logging/debugging.
    [[nodiscard]] std::string dump() const {
        return std::format("MulticastGroup(ip={}, port={}, source={})",
            net::format_ipv4(group_ip).data(), group_port,
            source_ip == 0 ? "any" :
                std::string(net::format_ipv4(source_ip).data()));
    }

    /// JSON-formatted group for monitoring system integration.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"group_ip\":\"{}\",\"group_port\":{},\"source_ip\":\"{}\"}}",
            net::format_ipv4(group_ip).data(), group_port,
            source_ip == 0 ? "0.0.0.0" :
                std::string(net::format_ipv4(source_ip).data()));
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// MulticastConfig
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Configuration for the MulticastReceiver.
///
/// Controls which NIC port/queue to poll, optional CPU pinning for the
/// RX thread, and the burst size per poll cycle.
/// @brief Maximum rx_burst accepted by MulticastConfig::validate() and the
///        corresponding stack-allocated burst pointer array inside
///        MulticastReceiver::rx_loop. Kept as a named constant so the two
///        sites cannot drift out of sync. 512 mbuf pointers = 4 KiB of
///        stack which is comfortable even in worker contexts.
inline constexpr uint16_t kMulticastMaxRxBurst = 512;

struct MulticastConfig {
    uint16_t port_id      = 0;       ///< DPDK port ID to poll
    uint16_t rx_queue_id  = 0;       ///< RX queue index on the port
    int      rx_cpu       = -1;      ///< CPU affinity for RX thread (-1 = no pin)
    uint16_t rx_burst     = 32;      ///< Max packets per rte_eth_rx_burst call

    /// Set to `Platform::dispatch_mode() == eph::net::dpdk::RxDispatchMode::RssPartitioned`
    /// when this MulticastReceiver runs
    /// on a Platform that has RSS multi-queue dispatch enabled. When true,
    /// `start()` fail-fasts unless the caller has *also* installed
    /// FlowDirector rules pinning each multicast group's traffic to
    /// `rx_queue_id` (in which case set this back to false to acknowledge
    /// the explicit pin).
    ///
    /// Why fail-fast: multicast UDP is in the project's RSS hash set
    /// (`platform.hpp:1113-1115` — `RTE_ETH_RSS_NONFRAG_IPV4_UDP`), so
    /// the NIC hashes inbound packets across queues using the 5-tuple
    /// `(sender_ip, sender_port, group_ip, group_port)`. The receiver
    /// only polls one `rx_queue_id`, so packets hashed to other queues
    /// are silently dropped. There is no `src_port`-style trick (cf. DNS
    /// `select_dns_src_port`) because the receiver does not control the
    /// sender's 5-tuple. Caller must either:
    ///   * disable RSS / use single queue (Platform single-queue config), or
    ///   * install a FlowDirector rule pinning the group's traffic to
    ///     `rx_queue_id` before `start()`, and set this flag false.
    bool rss_active_multi_queue = false;

    [[nodiscard]] friend bool operator==(const MulticastConfig&,
                                          const MulticastConfig&) = default;

    /// Validate configuration parameters.
    [[nodiscard]] constexpr std::string_view validate() const noexcept {
        if (rx_burst == 0) return "rx_burst must not be zero";
        if (rx_burst > kMulticastMaxRxBurst) return "rx_burst too large (max 512)";
        if (rx_cpu < -1) return "rx_cpu must be >= -1 (-1 = no pinning)";
        return {};
    }

    /// Check for non-fatal contradictions or likely misconfigurations.
    /// Returns a list of warning messages (empty if no issues).
    /// Unlike validate() which blocks construction, these are advisory.
    [[nodiscard]] std::vector<std::string> warnings() const {
        std::vector<std::string> w;
        if (rx_cpu == -1)
            w.emplace_back("rx_cpu=-1 (no pinning) -- multicast RX thread "
                           "may migrate across cores, increasing jitter");
        if (rx_burst < 8)
            w.emplace_back(std::format(
                "rx_burst={} is unusually small -- may increase poll "
                "overhead per packet", rx_burst));
        if (rx_burst > 128)
            w.emplace_back(std::format(
                "rx_burst={} is unusually large -- may increase "
                "per-iteration latency variance", rx_burst));
        return w;
    }

    /// Multi-line formatted dump for logging/debugging.
    [[nodiscard]] std::string dump() const {
        return std::format(
            "MulticastConfig:\n"
            "  port_id: {}, rx_queue: {}, rx_cpu: {}, rx_burst: {}, "
            "rss_active_multi_queue: {}",
            port_id, rx_queue_id, rx_cpu, rx_burst,
            rss_active_multi_queue);
    }

    /// JSON-formatted config for monitoring system integration.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"port_id\":{},\"rx_queue_id\":{},\"rx_cpu\":{},"
            "\"rx_burst\":{},\"rss_active_multi_queue\":{}}}",
            port_id, rx_queue_id, rx_cpu, rx_burst,
            rss_active_multi_queue);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Multicast group entry (internal tracking)
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// Internal state for a joined multicast group.
struct MulticastGroupEntry {
    MulticastGroup group{};
    rte_ether_addr mcast_mac{};   ///< Derived multicast MAC for NIC filter
    bool           active = false;

    /// Packet counter for diagnostics.
    /// Atomic: written by RX thread, readable from any thread for stats.
    std::atomic<uint64_t> rx_packets = 0;
};

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Packet callback type
// ─────────────────────────────────────────────────────────────────────────────

/// Callback invoked for each received UDP multicast payload.
/// @param data  Pointer to UDP payload (valid only during callback)
/// @param len   Payload length in bytes
using MulticastPacketCallback = std::function<void(const uint8_t* data, size_t len)>;

// ─────────────────────────────────────────────────────────────────────────────
// MulticastReceiver
// ─────────────────────────────────────────────────────────────────────────────

/// UDP multicast receiver using DPDK for zero-copy packet capture.
///
/// Manages multicast group membership via DPDK NIC MAC address filters
/// and delivers UDP payloads to a user callback. Designed for market data
/// feeds (CME MDP3.0, Nasdaq TotalView/MoldUDP64).
///
/// Threading model:
///   - One RX thread (owned by MulticastReceiver) polls the NIC
///   - Payload delivery via callback in the RX thread context
///   - Group join/leave must be called before start() (no hot-path locking)
///
/// Example:
/// @code
///   MulticastConfig cfg{.port_id = 0, .rx_queue_id = 0};
///   MulticastReceiver receiver(cfg);
///   receiver.join_group({.group_ip = net::parse_ipv4("233.54.12.111"),
///                        .group_port = 26477});
///   receiver.on_packet([](const uint8_t* data, size_t len) {
///       auto hdr = eph::itch::parse_moldudp64_header(data, len);
///       // process ...
///   });
///   receiver.start();
/// @endcode
class MulticastReceiver {
public:
    explicit MulticastReceiver(MulticastConfig config) noexcept
        : config_(config) {
        SPDLOG_LOGGER_DEBUG(detail::multicast_logger(),
            "MulticastReceiver created: port={}, queue={}, rx_burst={}",
            config_.port_id, config_.rx_queue_id, config_.rx_burst);
        // Surface non-fatal misconfigurations (no CPU pinning, unusual
        // rx_burst). Advisory only — operators see them in production
        // logs. Parallel to Platform / UdpSender / TcpSession / DNS.
        for (const auto& w : config_.warnings()) {
            SPDLOG_LOGGER_WARN(detail::multicast_logger(),
                "MulticastConfig advisory: {}", w);
        }
    }

    ~MulticastReceiver() {
        stop();
        // Leave all joined groups to clean up NIC MAC filters
        leave_all_groups();
    }

    MulticastReceiver(const MulticastReceiver&) = delete;
    MulticastReceiver& operator=(const MulticastReceiver&) = delete;
    MulticastReceiver(MulticastReceiver&&) = delete;
    MulticastReceiver& operator=(MulticastReceiver&&) = delete;

    // ─────────────────────────────────────────────────────────────────────
    // Group management
    // ─────────────────────────────────────────────────────────────────────

    /// Join a multicast group. Adds the derived multicast MAC to the NIC
    /// filter so matching packets are delivered to the RX queue.
    ///
    /// Must be called BEFORE start() — modifying group state while the RX
    /// loop is running is not safe (no lock on the hot path by design).
    ///
    /// @param group  Multicast group descriptor
    /// @return Group index on success, or error description
    [[nodiscard]] std::expected<size_t, std::string>
    join_group(const MulticastGroup& group) {
        if (running_.load(std::memory_order_acquire)) {
            return std::unexpected("join_group() must be called before start()");
        }

        // Validate group parameters
        if (auto err = group.validate(); !err.empty()) {
            SPDLOG_LOGGER_ERROR(detail::multicast_logger(),
                "Invalid multicast group: {}", err);
            return std::unexpected(std::format("Invalid group: {}", err));
        }

        // Check for duplicate
        for (size_t i = 0; i < group_count_; ++i) {
            if (groups_[i].active && groups_[i].group == group) {
                SPDLOG_LOGGER_WARN(detail::multicast_logger(),
                    "Group {}:{} already joined at index {}",
                    net::format_ipv4(group.group_ip).data(),
                    group.group_port, i);
                return i; // Idempotent — return existing index
            }
        }

        // Find a free slot (may reuse a previously left slot)
        size_t idx = group_count_;
        for (size_t i = 0; i < group_count_; ++i) {
            if (!groups_[i].active) {
                idx = i;
                break;
            }
        }

        if (idx >= kMaxMulticastGroups) {
            SPDLOG_LOGGER_ERROR(detail::multicast_logger(),
                "Cannot join group: receiver full ({}/{} groups)",
                group_count_, kMaxMulticastGroups);
            return std::unexpected(std::format(
                "receiver full ({}/{} groups)", group_count_, kMaxMulticastGroups));
        }

        // Derive multicast MAC from group IP (RFC 1112)
        rte_ether_addr mcast_mac = multicast_mac_from_ip(group.group_ip);

        // Add multicast MAC to NIC filter via DPDK
        // rte_eth_dev_mac_addr_add with RTE_ETH_DEV_NCAST flag would work
        // but rte_eth_dev_set_mc_addr_list is the standard multicast API.
        // We rebuild the full MC addr list each time a group is added.
        auto& entry = groups_[idx];
        entry.group     = group;
        entry.mcast_mac = mcast_mac;
        entry.active    = true;
        // operator= on std::atomic is seq-cst; explicit relaxed is the
        // semantics we actually want here (init before start, no
        // cross-thread ordering needed) and matches the relaxed loads
        // / fetch_adds used everywhere else for this counter.
        entry.rx_packets.store(0, std::memory_order_relaxed);

        // Capture pre-state so a NIC-level failure can roll the high-water
        // mark back. Without this, a failed join would leave group_count_
        // pointing past an inactive slot, so `group_count()` over-reports
        // and the next free-slot scan still finds the stale slot — correct
        // by accident but observably wrong to operators.
        const size_t prev_group_count = group_count_;
        if (idx >= group_count_) {
            group_count_ = idx + 1;
        }

        // Apply the updated multicast MAC list to the NIC
        auto result = apply_mc_addr_list();
        if (!result) {
            // Rollback both the active flag and (if we extended it) the
            // total group_count_ — keep `group_count()` an honest count of
            // observed slots.
            entry.active = false;
            group_count_ = prev_group_count;
            return std::unexpected(result.error());
        }

        SPDLOG_LOGGER_INFO(detail::multicast_logger(),
            "Joined multicast group {}: {}:{} -> MAC {} (source_filter={})",
            idx,
            net::format_ipv4(group.group_ip).data(),
            group.group_port,
            net::format_mac(mcast_mac).data(),
            group.source_ip == 0 ? "any" :
                std::string(net::format_ipv4(group.source_ip).data()));

        return idx;
    }

    /// Leave a multicast group by index. Removes the MAC filter from the NIC.
    ///
    /// Must be called BEFORE start() — modifying group state while the RX
    /// loop is running is not safe.
    ///
    /// @param group_idx  Group index returned by join_group()
    /// @return Success or error description
    [[nodiscard]] std::expected<void, std::string>
    leave_group(size_t group_idx) {
        if (running_.load(std::memory_order_acquire)) {
            return std::unexpected("leave_group() must be called before start()");
        }

        if (group_idx >= group_count_ || !groups_[group_idx].active) {
            return std::unexpected(std::format(
                "Invalid group index {} (count={})", group_idx, group_count_));
        }

        auto& entry = groups_[group_idx];
        SPDLOG_LOGGER_INFO(detail::multicast_logger(),
            "Leaving multicast group {}: {}:{} (rx_packets={})",
            group_idx,
            net::format_ipv4(entry.group.group_ip).data(),
            entry.group.group_port,
            entry.rx_packets.load(std::memory_order_relaxed));

        entry.active = false;

        // Rebuild the MC addr list without this group's MAC
        auto result = apply_mc_addr_list();
        if (!result) {
            // Re-enable on failure to keep state consistent
            entry.active = true;
            return std::unexpected(result.error());
        }

        return {};
    }

    /// Leave a multicast group by group descriptor.
    [[nodiscard]] std::expected<void, std::string>
    leave_group(const MulticastGroup& group) {
        for (size_t i = 0; i < group_count_; ++i) {
            if (groups_[i].active && groups_[i].group == group) {
                return leave_group(i);
            }
        }
        return std::unexpected("Group not found");
    }

    // ─────────────────────────────────────────────────────────────────────
    // Callback registration
    // ─────────────────────────────────────────────────────────────────────

    /// Register the packet callback. Must be set before start().
    /// @param cb  Callback invoked with (payload_ptr, payload_len) for each
    ///            received UDP multicast datagram.
    void on_packet(MulticastPacketCallback cb) {
        callback_ = std::move(cb);
        SPDLOG_LOGGER_DEBUG(detail::multicast_logger(),
            "Packet callback registered");
    }

    // ─────────────────────────────────────────────────────────────────────
    // Start / Stop
    // ─────────────────────────────────────────────────────────────────────

    /// Start the multicast receiver RX thread.
    /// Requires at least one joined group and a registered callback.
    [[nodiscard]] std::expected<void, std::string> start() {
        // Acquire to match join_group / leave_group / is_running's
        // synchronisation pattern. A start() racing with another thread's
        // stop() (a rare but possible orchestration glitch) must observe
        // stop()'s release-store.
        if (running_.load(std::memory_order_acquire)) {
            return std::unexpected("Already running");
        }

        // Validate config
        if (auto err = config_.validate(); !err.empty()) {
            SPDLOG_LOGGER_ERROR(detail::multicast_logger(),
                "Invalid config: {}", err);
            return std::unexpected(std::format("Invalid config: {}", err));
        }

        // RSS multi-queue safety check (stage 5 of the DPDK control-plane
        // RSS blind-spot fix). When the underlying Platform has RSS active
        // across multiple queues, every multicast UDP packet's 5-tuple
        // gets hashed across queues — there's no caller-controllable input
        // to steer all of them to `rx_queue_id`. Fail-fast forces the
        // caller to either single-queue the Platform or pin via
        // FlowDirector first (then clear this flag to acknowledge).
        if (config_.rss_active_multi_queue) {
            SPDLOG_LOGGER_ERROR(detail::multicast_logger(),
                "MulticastReceiver: rss_active_multi_queue=true with "
                "rx_queue_id={} — RSS hashes inbound multicast across "
                "all queues; the receiver cannot guarantee delivery to "
                "a single queue without an explicit per-group FlowDirector "
                "pin. Either: (a) disable RSS / use single-queue Platform; "
                "(b) install FlowDirector rule pinning each joined group "
                "to rx_queue_id, then pass rss_active_multi_queue=false "
                "to acknowledge.",
                config_.rx_queue_id);
            return std::unexpected(std::format(
                "MulticastReceiver: rss_active_multi_queue=true unsafe "
                "(rx_queue_id={}); see ERROR log for resolution",
                config_.rx_queue_id));
        }

        if (active_group_count() == 0) {
            SPDLOG_LOGGER_ERROR(detail::multicast_logger(),
                "Cannot start: no multicast groups joined");
            return std::unexpected("No multicast groups joined");
        }

        if (!callback_) {
            SPDLOG_LOGGER_ERROR(detail::multicast_logger(),
                "Cannot start: no packet callback registered");
            return std::unexpected("No packet callback registered");
        }

        // Set running_ BEFORE the std::thread ctor so the lambda's first
        // load sees true (the lambda runs `while (running_.load(acquire))`).
        // If the thread ctor throws on resource exhaustion (rare — pthread
        // EAGAIN, ENOMEM), restore running_ to false so a follow-up start()
        // is not permanently rejected as "Already running" against a thread
        // that does not exist. Pre-fix the inconsistency was unrecoverable.
        running_.store(true, std::memory_order_release);
        try {
            thread_ = std::thread([this] { rx_loop(); });
        } catch (const std::system_error& e) {
            running_.store(false, std::memory_order_release);
            SPDLOG_LOGGER_ERROR(detail::multicast_logger(),
                "MulticastReceiver::start: std::thread ctor failed: {} "
                "(code={}); running_ rolled back so start() may be retried",
                e.what(), e.code().value());
            return std::unexpected(std::format(
                "MulticastReceiver::start: thread ctor failed: {}", e.what()));
        }

        SPDLOG_LOGGER_INFO(detail::multicast_logger(),
            "MulticastReceiver started: {} active groups, port={}, queue={}",
            active_group_count(), config_.port_id, config_.rx_queue_id);

        return {};
    }

    /// Stop the multicast receiver RX thread (blocks until joined).
    void stop() {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        if (thread_.joinable()) thread_.join();
        // Log both matched and unmatched counts so operators can compute
        // the hit-rate (matched / (matched + unmatched)) from the stop
        // event without scraping a separate metric. A high unmatched ratio
        // is the single most useful diagnostic for multicast misconfig.
        const uint64_t rx     = total_rx_packets_.load(std::memory_order_relaxed);
        const uint64_t unm    = rx_unmatched_.load(std::memory_order_relaxed);
        SPDLOG_LOGGER_INFO(detail::multicast_logger(),
            "MulticastReceiver stopped (total_rx={}, rx_unmatched={})",
            rx, unm);
    }

    // ─────────────────────────────────────────────────────────────────────
    // Accessors
    // ─────────────────────────────────────────────────────────────────────

    /// @brief Check if the RX polling thread is currently running.
    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    /// @brief Total number of group slots used (including inactive/left groups).
    [[nodiscard]] size_t group_count() const noexcept {
        return group_count_;
    }

    /// Number of currently active (joined) groups.
    [[nodiscard]] size_t active_group_count() const noexcept {
        size_t count = 0;
        for (size_t i = 0; i < group_count_; ++i) {
            if (groups_[i].active) ++count;
        }
        return count;
    }

    /// Access a group entry by index (for stats, diagnostics).
    [[nodiscard]] const detail::MulticastGroupEntry& group_entry(size_t i) const noexcept {
        if (i >= group_count_) [[unlikely]] {
            static const detail::MulticastGroupEntry empty{};
            return empty;
        }
        return groups_[i];
    }

    /// @brief Total received packets across all groups (approximate, relaxed ordering).
    [[nodiscard]] uint64_t total_rx_packets() const noexcept {
        return total_rx_packets_.load(std::memory_order_relaxed);
    }

    /// @brief Packets received that matched no joined group.
    ///
    /// MAC-hash collisions (RFC 1112: 23-bit truncation maps multiple IPs to
    /// the same MAC) cause the NIC to deliver unwanted multicast packets. A
    /// non-zero value here is **not** an error in steady state — but a
    /// rapidly-rising counter signals either (a) NIC filter
    /// disabled/promiscuous, or (b) accidental subscription to a too-broad
    /// MAC group. Operators want to graph this alongside `total_rx_packets`
    /// to compute a hit-rate. Relaxed ordering matches `total_rx_packets`.
    [[nodiscard]] uint64_t rx_unmatched_packets() const noexcept {
        return rx_unmatched_.load(std::memory_order_relaxed);
    }

    /// @brief Access the receiver's configuration.
    [[nodiscard]] const MulticastConfig& config() const noexcept {
        return config_;
    }

private:
    // ─────────────────────────────────────────────────────────────────────
    // NIC multicast MAC management
    // ─────────────────────────────────────────────────────────────────────

    /// Rebuild and apply the multicast MAC address list to the NIC.
    /// Called after every join/leave to keep the NIC filter in sync.
    [[nodiscard]] std::expected<void, std::string> apply_mc_addr_list() {
        // Collect active multicast MACs (deduplicated — multiple IPs can
        // map to the same MAC due to the 23-bit truncation in RFC 1112)
        rte_ether_addr mc_list[kMaxMulticastGroups]{};
        uint32_t mc_count = 0;

        for (size_t i = 0; i < group_count_; ++i) {
            if (!groups_[i].active) continue;

            // Deduplicate: check if this MAC is already in the list.
            // Multiple group IPs can hash to the same MAC (RFC 1112 collision).
            bool dup = false;
            for (uint32_t j = 0; j < mc_count; ++j) {
                if (std::memcmp(&mc_list[j], &groups_[i].mcast_mac,
                                sizeof(rte_ether_addr)) == 0) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                mc_list[mc_count++] = groups_[i].mcast_mac;
            }
        }

        SPDLOG_LOGGER_DEBUG(detail::multicast_logger(),
            "Applying MC addr list: {} unique MACs on port {}",
            mc_count, config_.port_id);

        int ret = rte_eth_dev_set_mc_addr_list(
            config_.port_id, mc_count > 0 ? mc_list : nullptr, mc_count);
        if (ret != 0) {
            // Capture rte_errno alongside ret — DPDK returns negative POSIX
            // errnos (-ENOTSUP for unsupported PMDs, -ENOSPC when the NIC
            // multicast filter table is full, -EINVAL for malformed list).
            // Without these, operators see only "ret=-95" and have to guess.
            const int err = rte_errno;
            SPDLOG_LOGGER_ERROR(detail::multicast_logger(),
                "rte_eth_dev_set_mc_addr_list failed: port={} mc_count={} "
                "ret={} rte_errno={} ({})",
                config_.port_id, mc_count, ret, err, rte_strerror(err));
            return std::unexpected(std::format(
                "rte_eth_dev_set_mc_addr_list failed: ret={}, rte_errno={} ({})",
                ret, err, rte_strerror(err)));
        }

        return {};
    }

    /// Leave all active groups (called from destructor).
    void leave_all_groups() {
        bool had_active = false;
        for (size_t i = 0; i < group_count_; ++i) {
            if (groups_[i].active) {
                SPDLOG_LOGGER_DEBUG(detail::multicast_logger(),
                    "Auto-leaving group {}: {}:{}",
                    i,
                    net::format_ipv4(groups_[i].group.group_ip).data(),
                    groups_[i].group.group_port);
                groups_[i].active = false;
                had_active = true;
            }
        }

        if (had_active) {
            // Clear the NIC MC list (ignore errors in destructor — but
            // surface rte_errno to make post-mortem log analysis tractable).
            int ret = rte_eth_dev_set_mc_addr_list(config_.port_id, nullptr, 0);
            if (ret != 0) {
                const int err = rte_errno;
                SPDLOG_LOGGER_WARN(detail::multicast_logger(),
                    "Failed to clear MC addr list on cleanup: port={} "
                    "ret={} rte_errno={} ({})",
                    config_.port_id, ret, err, rte_strerror(err));
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    // RX loop
    // ─────────────────────────────────────────────────────────────────────

    void rx_loop() {
        // Pin local to this thread's loop scope: when rx_loop returns
        // (thread is exiting anyway), guard's destructor unregisters the
        // cpu — the OS implicitly releases the pthread's affinity at
        // thread death, so registry cleanup at the same point is correct.
        eph::utils::PinGuard pin_guard;
        if (config_.rx_cpu >= 0) {
            if (auto r = eph::utils::pin_thread(config_.rx_cpu, "multicast_rx"); r) {
                pin_guard = std::move(*r);
            }
            // else: best-effort — log inside pin_thread already covers
            // the failure; we continue without a registry entry.
        }

        [[maybe_unused]] auto* log = detail::multicast_logger();
        SPDLOG_LOGGER_INFO(log,
            "Multicast RX loop started: {} active groups, port={}, queue={}, cpu={}",
            active_group_count(), config_.port_id, config_.rx_queue_id, config_.rx_cpu);

        // Allocate burst array on stack. Sized to the configured upper
        // bound (kMulticastMaxRxBurst = 512 pointers = 4 KiB) so validate()
        // and this loop cannot drift out of sync. rte_eth_rx_burst is
        // capped via the runtime `burst_size` below.
        rte_mbuf* pkts[kMulticastMaxRxBurst];
        const uint16_t burst_size = config_.rx_burst;

        // INTENTIONAL 100%-CPU busy-spin. HFT market-data feeds run on
        // a pinned dedicated lcore (validated by `pin_thread` above) and
        // every microsecond of poll latency translates to filled-or-
        // missed quote ticks. We deliberately do NOT insert `rte_pause`
        // / `usleep` / cond-var / `rte_eth_rx_queue_intr_enable`
        // backpressure on the empty-burst path — those would amortize
        // wakeup cost across batches and hurt single-tick worst-case
        // latency, which is the metric this loop optimizes for.
        //
        // The loop exits cleanly via the SC-acquire load on `running_`;
        // shutdown is observable within the next NIC RX-poll cycle.
        // Power thermals are managed at the deployment level (HFT colos
        // budget for sustained 100% CPU on dedicated DPDK lcores), not
        // inside this hot path.
        while (running_.load(std::memory_order_acquire)) {
            uint16_t nb_rx = rte_eth_rx_burst(
                config_.port_id, config_.rx_queue_id, pkts, burst_size);

            if (nb_rx == 0) continue;  // intentional spin — see block comment above

            for (uint16_t i = 0; i < nb_rx; ++i) {
                process_packet(pkts[i]);
                rte_pktmbuf_free(pkts[i]);
            }
        }

        SPDLOG_LOGGER_DEBUG(log, "Multicast RX loop exited");
    }

    /// Process a single received packet: parse, match against joined groups,
    /// and deliver payload via callback.
    void process_packet(const rte_mbuf* mbuf) noexcept {
        auto parsed = parse_udp_packet(mbuf);
        if (!parsed) return; // Not a valid UDP/IPv4 packet

        const uint32_t dst_ip   = parsed.dst_ip();
        const uint16_t dst_port = parsed.dst_port();
        const uint32_t src_ip   = parsed.src_ip();

        // Quick reject: not a multicast destination
        if (!is_multicast_ip(dst_ip)) return;

        // Match against joined groups (linear scan, <= 8 entries)
        for (size_t i = 0; i < group_count_; ++i) {
            auto& entry = groups_[i];
            if (!entry.active) continue;
            if (entry.group.group_ip != dst_ip) continue;
            if (entry.group.group_port != dst_port) continue;

            // Source-specific multicast filter (SSM): if source_ip is set,
            // only accept packets from that specific source.
            if (entry.group.source_ip != 0 && entry.group.source_ip != src_ip) {
                SPDLOG_LOGGER_TRACE(detail::multicast_logger(),
                    "SSM filter: dropping packet from {} (expected {})",
                    net::format_ipv4(src_ip).data(),
                    net::format_ipv4(entry.group.source_ip).data());
                continue;
            }

            // Match found — deliver payload
            entry.rx_packets.fetch_add(1, std::memory_order_relaxed);
            total_rx_packets_.fetch_add(1, std::memory_order_relaxed);

            if (parsed.payload && parsed.payload_len > 0) {
                SPDLOG_LOGGER_TRACE(detail::multicast_logger(),
                    "Delivering packet: group={} src={}:{} len={}",
                    i, net::format_ipv4(src_ip).data(),
                    parsed.src_port(), parsed.payload_len);

                callback_(parsed.payload, parsed.payload_len);
            }
            return; // A packet matches at most one group
        }

        // No group matched — packet may have been received due to MAC hash
        // collision (RFC 1112: multiple IPs map to same MAC). This is normal
        // but operators need a signal: count it so a rate-of-change > 0
        // distinguishes "NIC filter disabled" (every miss landed) from
        // "rare collision" (near-zero rate).
        rx_unmatched_.fetch_add(1, std::memory_order_relaxed);
    }

    MulticastConfig config_;
    std::array<detail::MulticastGroupEntry, kMaxMulticastGroups> groups_{};
    size_t group_count_ = 0;
    MulticastPacketCallback callback_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    // Atomic: written by RX thread, readable from any thread for stats.
    std::atomic<uint64_t> total_rx_packets_ = 0;
    // Packets the NIC delivered but no joined group claimed (MAC-hash
    // collision, NIC promiscuous, or stale filter). See rx_unmatched_packets().
    std::atomic<uint64_t> rx_unmatched_     = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Generic payload adapter
// ─────────────────────────────────────────────────────────────────────────────

/// Create a MulticastPacketCallback that applies a parser/dispatcher to each
/// received UDP payload. This is the generic building block for protocol-
/// specific adapters (MoldUDP64, CME MDP3.0, etc.).
///
/// @tparam Parser  Callable with signature (const uint8_t* data, size_t len)
/// @param parser   The parser/dispatcher to invoke on each UDP payload
/// @return MulticastPacketCallback suitable for MulticastReceiver::on_packet()
template <typename Parser>
    requires std::invocable<Parser, const uint8_t*, size_t>
[[nodiscard]] MulticastPacketCallback make_payload_adapter(Parser&& parser) {
    return MulticastPacketCallback(std::forward<Parser>(parser));
}

/// Create a MulticastPacketCallback that parses MoldUDP64 packets and
/// delivers individual messages to the inner callback.
///
/// This bridges the MulticastReceiver (which delivers raw UDP payloads)
/// with any MoldUDP64 parser. The caller supplies the parse function
/// to avoid a hard dependency on eph-itch.
///
/// Usage with eph-itch:
/// @code
///   #include "eph/itch/moldudp64.hpp"
///
///   receiver.on_packet(make_moldudp64_adapter(
///       eph::itch::parse_moldudp64<decltype(my_cb)>,
///       [](const uint8_t* msg, uint16_t len, uint64_t seq) {
///           // Process individual ITCH message
///       }));
/// @endcode
///
/// Or more concisely:
/// @code
///   receiver.on_packet(make_moldudp64_adapter(
///       [](const uint8_t* data, size_t len, auto& cb) {
///           eph::itch::parse_moldudp64(data, len, cb);
///       },
///       [](const uint8_t* msg, uint16_t msg_len, uint64_t seq) {
///           // Process individual ITCH message
///       }));
/// @endcode
///
/// @tparam ParseFn    Callable with signature (const uint8_t*, size_t, MsgCb&)
/// @tparam MsgCb      Per-message callback type
/// @param parse_fn    MoldUDP64 parse function
/// @param msg_callback  Per-message callback
/// @return MulticastPacketCallback suitable for MulticastReceiver::on_packet()
template <typename ParseFn, typename MsgCb>
    requires std::invocable<ParseFn, const uint8_t*, size_t, MsgCb&>
[[nodiscard]] MulticastPacketCallback
make_moldudp64_adapter(ParseFn&& parse_fn, MsgCb&& msg_callback) {
    auto parser = std::forward<ParseFn>(parse_fn);
    auto cb     = std::forward<MsgCb>(msg_callback);
    return [parser = std::move(parser),
            cb     = std::move(cb)](const uint8_t* data, size_t len) mutable {
        parser(data, len, cb);
    };
}

} // namespace eph::dpdk

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specializations
// ─────────────────────────────────────────────────────────────────────────────

// std::formatter for ParsedUdpPacket is in net_header.hpp (canonical location).

/// @brief std::formatter for MulticastGroup — compact "ip:port" summary.
template <>
struct std::formatter<eph::dpdk::MulticastGroup> : std::formatter<std::string> {
    auto format(const eph::dpdk::MulticastGroup& g, auto& ctx) const {
        return std::formatter<std::string>::format(
            std::format("MulticastGroup({}:{} src={})",
                eph::dpdk::net::format_ipv4(g.group_ip).data(),
                g.group_port,
                g.source_ip == 0 ? "any" :
                    std::string(eph::dpdk::net::format_ipv4(g.source_ip).data())),
            ctx);
    }
};

/// @brief std::formatter for MulticastConfig — compact one-line summary.
template <>
struct std::formatter<eph::dpdk::MulticastConfig> : std::formatter<std::string> {
    auto format(const eph::dpdk::MulticastConfig& c, auto& ctx) const {
        return std::formatter<std::string>::format(
            std::format("MulticastConfig(port={}, q={}, cpu={}, burst={})",
                c.port_id, c.rx_queue_id, c.rx_cpu, c.rx_burst),
            ctx);
    }
};
