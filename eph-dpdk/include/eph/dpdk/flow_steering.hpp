#pragma once

/// @file flow_steering.hpp
/// NIC hardware RX dispatch — RSS configuration and rte_flow steering.
///
/// Provides runtime detection of NIC capabilities and automatic selection
/// of the best RX dispatch strategy:
///   - Software (Reactor): NIC has no RSS — epoll-style multiplexing
///   - RssPartitioned: NIC supports RSS — traffic hashed across queues
///   - FlowDirector: NIC supports rte_flow 5-tuple — per-connection queue
///
/// Usage:
///   auto mode = detect_rx_dispatch_mode(port_id);
///   if (mode == RxDispatchMode::FlowDirector) {
///       auto rule = install_flow_rule(port_id, queue_id, tuple);
///       // session polls from dedicated queue — zero dispatcher overhead
///   }

#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <rte_ethdev.h>
#include <rte_flow.h>

#include "eph/dpdk/net_header.hpp"

namespace eph::dpdk {

namespace detail {
inline spdlog::logger* flow_logger() { return get_logger<LoggerName{"dpdk.flow"}>(); }
} // namespace detail

// ---------------------------------------------------------------------------
// RX dispatch mode
// ---------------------------------------------------------------------------

/// NIC hardware dispatch capabilities detected at runtime.
enum class RxDispatchMode : uint8_t {
    Software,          ///< No RSS, no flow director → use Reactor (epoll)
    RssPartitioned,    ///< RSS hash to multiple queues → reduced contention
    FlowDirector,      ///< rte_flow 5-tuple exact match → per-connection queue
};

/// Human-readable name for RxDispatchMode.
[[nodiscard]] constexpr std::string_view rx_dispatch_mode_name(RxDispatchMode m) noexcept {
    switch (m) {
    case RxDispatchMode::Software:       return "Software (Reactor)";
    case RxDispatchMode::RssPartitioned: return "RSS Partitioned";
    case RxDispatchMode::FlowDirector:   return "Flow Director (rte_flow)";
    }
    return "unknown";
}

/// Detect the best RX dispatch mode for a given NIC port.
///
/// Probes in order: rte_flow 5-tuple → RSS TCP → fallback to Software.
/// Should be called once at Platform initialization and cached.
///
/// @param port_id DPDK port ID (must be valid and configured)
/// @return The highest-capability mode the NIC supports
[[nodiscard]] inline RxDispatchMode
detect_rx_dispatch_mode(uint16_t port_id) noexcept {
    [[maybe_unused]] auto* log = detail::flow_logger();

    // 1. Query device info for RSS offload capabilities
    rte_eth_dev_info dev_info{};
    if (rte_eth_dev_info_get(port_id, &dev_info) != 0) {
        SPDLOG_LOGGER_WARN(log,
            "rte_eth_dev_info_get failed for port {}, assuming Software mode",
            port_id);
        return RxDispatchMode::Software;
    }

    // 2. Check for rte_flow 5-tuple support (best case: per-connection queue)
    //    Validate a dummy flow rule to see if the NIC/driver supports it.
    {
        rte_flow_attr attr{};
        attr.ingress = 1;
        attr.priority = 0;

        // Pattern: match IPv4 TCP with specific 5-tuple
        rte_flow_item_ipv4 ipv4_spec{};
        ipv4_spec.hdr.src_addr = rte_cpu_to_be_32(0x0A000001);
        ipv4_spec.hdr.dst_addr = rte_cpu_to_be_32(0x0A000002);
        rte_flow_item_ipv4 ipv4_mask{};
        ipv4_mask.hdr.src_addr = 0xFFFFFFFF;
        ipv4_mask.hdr.dst_addr = 0xFFFFFFFF;

        rte_flow_item_tcp tcp_spec{};
        tcp_spec.hdr.src_port = rte_cpu_to_be_16(443);
        tcp_spec.hdr.dst_port = rte_cpu_to_be_16(12345);
        rte_flow_item_tcp tcp_mask{};
        tcp_mask.hdr.src_port = 0xFFFF;
        tcp_mask.hdr.dst_port = 0xFFFF;

        rte_flow_item pattern[] = {
            {.type = RTE_FLOW_ITEM_TYPE_ETH, .spec = nullptr, .last = nullptr, .mask = nullptr},
            {.type = RTE_FLOW_ITEM_TYPE_IPV4,
             .spec = &ipv4_spec, .last = nullptr, .mask = &ipv4_mask},
            {.type = RTE_FLOW_ITEM_TYPE_TCP,
             .spec = &tcp_spec, .last = nullptr, .mask = &tcp_mask},
            {.type = RTE_FLOW_ITEM_TYPE_END, .spec = nullptr, .last = nullptr, .mask = nullptr},
        };

        rte_flow_action_queue queue_action{};
        queue_action.index = 0;

        rte_flow_action actions[] = {
            {.type = RTE_FLOW_ACTION_TYPE_QUEUE, .conf = &queue_action},
            {.type = RTE_FLOW_ACTION_TYPE_END, .conf = nullptr},
        };

        rte_flow_error error{};
        int ret = rte_flow_validate(port_id, &attr, pattern, actions, &error);
        if (ret == 0) {
            SPDLOG_LOGGER_INFO(log,
                "Port {} supports rte_flow 5-tuple steering (FlowDirector mode)",
                port_id);
            return RxDispatchMode::FlowDirector;
        }
        SPDLOG_LOGGER_DEBUG(log,
            "Port {} rte_flow validate failed: {} ({}), trying RSS",
            port_id, error.message ? error.message : "unknown",
            ret);
    }

    // 3. Check for RSS TCP hash support
    constexpr uint64_t kRssTcp =
        RTE_ETH_RSS_NONFRAG_IPV4_TCP | RTE_ETH_RSS_IPV4;
    if (dev_info.flow_type_rss_offloads & kRssTcp) {
        SPDLOG_LOGGER_INFO(log,
            "Port {} supports RSS TCP hash (RssPartitioned mode), "
            "offloads=0x{:016x}",
            port_id, dev_info.flow_type_rss_offloads);
        return RxDispatchMode::RssPartitioned;
    }

    SPDLOG_LOGGER_INFO(log,
        "Port {} has no RSS/FlowDirector support (Software/Reactor mode)",
        port_id);
    return RxDispatchMode::Software;
}

// ---------------------------------------------------------------------------
// RSS configuration (Step 2)
// ---------------------------------------------------------------------------

/// Configure RSS on a port to distribute TCP traffic across multiple queues.
///
/// Must be called BEFORE rte_eth_dev_start(). The port should be configured
/// with nb_rx_queues >= num_queues.
///
/// @param port_id   DPDK port ID
/// @param num_queues Number of RX queues to distribute across
/// @return Number of queues actually configured, or error
[[nodiscard]] inline std::expected<uint16_t, std::string>
configure_rss(uint16_t port_id, uint16_t num_queues) noexcept {
    [[maybe_unused]] auto* log = detail::flow_logger();

    if (num_queues < 2) {
        return std::unexpected("RSS requires at least 2 queues");
    }

    rte_eth_dev_info dev_info{};
    if (rte_eth_dev_info_get(port_id, &dev_info) != 0) {
        return std::unexpected("rte_eth_dev_info_get failed");
    }

    // Clamp to device max
    uint16_t actual = std::min(num_queues,
                                dev_info.max_rx_queues);

    // Build RSS configuration
    rte_eth_rss_conf rss_conf{};
    rss_conf.rss_key = nullptr;  // Use default hash key
    rss_conf.rss_key_len = 0;
    rss_conf.rss_hf = dev_info.flow_type_rss_offloads &
                       (RTE_ETH_RSS_NONFRAG_IPV4_TCP | RTE_ETH_RSS_IPV4);

    if (rss_conf.rss_hf == 0) {
        return std::unexpected("NIC does not support TCP RSS hash");
    }

    int ret = rte_eth_dev_rss_hash_update(port_id, &rss_conf);
    if (ret != 0) {
        return std::unexpected(std::format(
            "rte_eth_dev_rss_hash_update failed: {}", ret));
    }

    // Configure RETA (Redirection Table) to spread evenly
    rte_eth_rss_reta_entry64 reta[RTE_ETH_RSS_RETA_SIZE_512 / RTE_ETH_RETA_GROUP_SIZE]{};
    uint16_t reta_size = dev_info.reta_size;
    if (reta_size == 0) reta_size = 128;  // Common default
    // Cap to array capacity: reta[] has RTE_ETH_RSS_RETA_SIZE_512/RTE_ETH_RETA_GROUP_SIZE
    // entries (8 at 64 bits each). A driver reporting reta_size > 512 would cause OOB write.
    reta_size = std::min(reta_size, static_cast<uint16_t>(RTE_ETH_RSS_RETA_SIZE_512));

    for (uint16_t i = 0; i < reta_size; ++i) {
        uint16_t group = i / RTE_ETH_RETA_GROUP_SIZE;
        uint16_t bit = i % RTE_ETH_RETA_GROUP_SIZE;
        reta[group].mask |= (1ULL << bit);
        reta[group].reta[bit] = static_cast<uint16_t>(i % actual);
    }

    ret = rte_eth_dev_rss_reta_update(port_id, reta, reta_size);
    if (ret != 0) {
        SPDLOG_LOGGER_WARN(log,
            "RETA update failed (ret={}), RSS may not distribute evenly", ret);
        // Non-fatal: RSS hash is still configured, just RETA might be uneven
    }

    SPDLOG_LOGGER_INFO(log,
        "RSS configured: port={}, queues={}, rss_hf=0x{:016x}, reta_size={}",
        port_id, actual, rss_conf.rss_hf, reta_size);

    return actual;
}

// ---------------------------------------------------------------------------
// rte_flow steering (Step 3)
// ---------------------------------------------------------------------------

/// @brief RAII handle for an rte_flow rule installed on a NIC port.
///
/// Destructor automatically removes the rule from the NIC via rte_flow_destroy.
/// Move-only: the moved-from instance has handle == nullptr and is safe to
/// destroy without side effects.
///
/// @note Created by install_flow_rule(). Do not construct directly.
struct FlowRule {
    uint16_t port_id = 0;        ///< DPDK port the rule is installed on
    uint16_t queue_id = 0;       ///< Target RX queue for matched packets
    rte_flow* handle = nullptr;  ///< Opaque DPDK flow rule handle (null if invalid/moved)

    /// @brief Default constructor creates an invalid (empty) rule.
    FlowRule() = default;

    /// @brief Destructor removes the flow rule from the NIC if still valid.
    ~FlowRule() { remove(); }

    FlowRule(const FlowRule&) = delete;
    FlowRule& operator=(const FlowRule&) = delete;

    /// @brief Move constructor. Transfers ownership; source becomes invalid.
    FlowRule(FlowRule&& other) noexcept
        : port_id(other.port_id), queue_id(other.queue_id),
          handle(other.handle) {
        other.handle = nullptr;
    }

    /// @brief Move assignment. Removes any existing rule before taking ownership.
    FlowRule& operator=(FlowRule&& other) noexcept {
        if (this != &other) {
            remove();
            port_id = other.port_id;
            queue_id = other.queue_id;
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }

    /// Remove the flow rule from the NIC. Safe to call multiple times.
    void remove() noexcept {
        if (!handle) return;
        rte_flow_error error{};
        int ret = rte_flow_destroy(port_id, handle, &error);
        if (ret != 0) {
            SPDLOG_LOGGER_WARN(detail::flow_logger(),
                "rte_flow_destroy failed: port={}, ret={}, msg={}",
                port_id, ret, error.message ? error.message : "unknown");
        } else {
            SPDLOG_LOGGER_DEBUG(detail::flow_logger(),
                "Flow rule removed: port={}, queue={}", port_id, queue_id);
        }
        handle = nullptr;
    }

    /// @brief Check if this rule is still active on the NIC.
    /// @return true if the flow rule handle is non-null
    [[nodiscard]] bool valid() const noexcept { return handle != nullptr; }

    /// Convenience bool conversion for validity checking.
    /// Usage: if (rule) { /* rule is active */ }
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

    /// Human-readable dump for logging/debugging.
    [[nodiscard]] std::string dump() const {
        if (!handle)
            return "FlowRule(inactive)";
        return std::format("FlowRule(port={}, queue={}, active)",
                           port_id, queue_id);
    }

    /// JSON-formatted status for monitoring system integration.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"port_id\":{},\"queue_id\":{},\"active\":{}}}",
            port_id, queue_id, handle != nullptr ? "true" : "false");
    }
};

/// Install a rte_flow rule that steers packets matching a TCP 5-tuple
/// to a specific RX queue. Returns RAII FlowRule handle.
///
/// @param port_id  DPDK port ID
/// @param queue_id Target RX queue for matching packets
/// @param tuple    TCP 4-tuple (src/dst IP + port) in HOST byte order
/// @return FlowRule handle on success, error string on failure
[[nodiscard]] inline std::expected<FlowRule, std::string>
install_flow_rule(uint16_t port_id, uint16_t queue_id,
                  const net::ConnectionTuple& tuple) noexcept {
    [[maybe_unused]] auto* log = detail::flow_logger();

    rte_flow_attr attr{};
    attr.ingress = 1;

    // IPv4 match: remote IP → src, local IP → dst (NIC perspective: incoming)
    rte_flow_item_ipv4 ipv4_spec{};
    ipv4_spec.hdr.src_addr = rte_cpu_to_be_32(tuple.dst_ip);  // Remote = source
    ipv4_spec.hdr.dst_addr = rte_cpu_to_be_32(tuple.src_ip);  // Local = destination
    rte_flow_item_ipv4 ipv4_mask{};
    ipv4_mask.hdr.src_addr = 0xFFFFFFFF;
    ipv4_mask.hdr.dst_addr = 0xFFFFFFFF;

    // TCP match: remote port → src, local port → dst
    rte_flow_item_tcp tcp_spec{};
    tcp_spec.hdr.src_port = rte_cpu_to_be_16(tuple.dst_port);
    tcp_spec.hdr.dst_port = rte_cpu_to_be_16(tuple.src_port);
    rte_flow_item_tcp tcp_mask{};
    tcp_mask.hdr.src_port = 0xFFFF;
    tcp_mask.hdr.dst_port = 0xFFFF;

    rte_flow_item pattern[] = {
        {.type = RTE_FLOW_ITEM_TYPE_ETH, .spec = nullptr, .last = nullptr, .mask = nullptr},
        {.type = RTE_FLOW_ITEM_TYPE_IPV4,
         .spec = &ipv4_spec, .last = nullptr, .mask = &ipv4_mask},
        {.type = RTE_FLOW_ITEM_TYPE_TCP,
         .spec = &tcp_spec, .last = nullptr, .mask = &tcp_mask},
        {.type = RTE_FLOW_ITEM_TYPE_END, .spec = nullptr, .last = nullptr, .mask = nullptr},
    };

    rte_flow_action_queue queue_conf{};
    queue_conf.index = queue_id;

    rte_flow_action actions[] = {
        {.type = RTE_FLOW_ACTION_TYPE_QUEUE, .conf = &queue_conf},
        {.type = RTE_FLOW_ACTION_TYPE_END, .conf = nullptr},
    };

    rte_flow_error error{};
    auto* flow = rte_flow_create(port_id, &attr, pattern, actions, &error);
    if (!flow) {
        auto msg = std::format(
            "rte_flow_create failed: port={}, queue={}, error={}",
            port_id, queue_id,
            error.message ? error.message : "unknown");
        SPDLOG_LOGGER_WARN(log, "{}", msg);
        return std::unexpected(msg);
    }

    SPDLOG_LOGGER_INFO(log,
        "Flow rule installed: {}:{} -> {}:{} queue={} (NIC src/dst swapped)",
        net::format_ipv4(tuple.dst_ip).data(), tuple.dst_port,
        net::format_ipv4(tuple.src_ip).data(), tuple.src_port,
        queue_id);

    FlowRule rule;
    rule.port_id = port_id;
    rule.queue_id = queue_id;
    rule.handle = flow;
    return rule;
}

} // namespace eph::dpdk

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specialization for RxDispatchMode
// ─────────────────────────────────────────────────────────────────────────────

/// @brief std::formatter specialization for RxDispatchMode.
///
/// Formats as the human-readable name from rx_dispatch_mode_name().
template <>
struct std::formatter<eph::dpdk::RxDispatchMode> : std::formatter<std::string_view> {
    auto format(eph::dpdk::RxDispatchMode m, auto& ctx) const {
        return std::formatter<std::string_view>::format(
            eph::dpdk::rx_dispatch_mode_name(m), ctx);
    }
};

/// @brief std::formatter specialization for FlowRule.
///
/// Formats as a compact one-line summary showing port, queue, and status.
template <>
struct std::formatter<eph::dpdk::FlowRule> : std::formatter<std::string> {
    auto format(const eph::dpdk::FlowRule& r, auto& ctx) const {
        return std::formatter<std::string>::format(r.dump(), ctx);
    }
};
