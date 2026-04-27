#pragma once

/// @file flow_steering.hpp
/// NIC hardware RX dispatch — RSS configuration and rte_flow steering.
///
/// Provides runtime detection of NIC capabilities and automatic selection
/// of the best RX dispatch strategy:
///   - Software:       NIC has no RSS — single Poller fans out by 5-tuple
///   - RssPartitioned: NIC supports RSS — traffic hashed across queues
///   - FlowDirector:   NIC supports rte_flow 5-tuple — per-connection queue
///
/// Usage:
///   auto mode = detect_rx_dispatch_mode(port_id);
///   if (mode == RxDispatchMode::FlowDirector) {
///       auto rule = install_flow_rule(port_id, queue_id, tuple);
///       // session polls from a dedicated queue — zero dispatcher overhead
///   }

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <rte_ethdev.h>
#include <rte_flow.h>
#include <rte_udp.h>

#include "eph/dpdk/detail/logger.hpp"
#include "eph/dpdk/net_header.hpp"

namespace eph::net::dpdk {

namespace detail {
inline spdlog::logger* flow_logger() {
    return ::eph::dpdk::detail::get_logger<
        ::eph::dpdk::detail::LoggerName{"dpdk.flow"}>();
}
} // namespace detail

// ---------------------------------------------------------------------------
// Flow rule protocol type
// ---------------------------------------------------------------------------

/// Protocol type for flow rule pattern matching.
enum class FlowProtocol : uint8_t {
    Tcp,  ///< Match TCP 4-tuple (default, backward compatible)
    Udp,  ///< Match UDP 4-tuple
};

/// Human-readable name for FlowProtocol.
[[nodiscard]] constexpr std::string_view flow_protocol_name(FlowProtocol p) noexcept {
    switch (p) {
    case FlowProtocol::Tcp: return "TCP";
    case FlowProtocol::Udp: return "UDP";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// RX dispatch mode
// ---------------------------------------------------------------------------

/// NIC hardware dispatch capabilities detected at runtime.
enum class RxDispatchMode : uint8_t {
    Software,          ///< No RSS, no flow director → single-Poller fallback
    RssPartitioned,    ///< RSS hash to multiple queues → reduced contention
    FlowDirector,      ///< rte_flow 5-tuple exact match → per-connection queue
};

/// Human-readable name for RxDispatchMode.
[[nodiscard]] constexpr std::string_view rx_dispatch_mode_name(RxDispatchMode m) noexcept {
    switch (m) {
    case RxDispatchMode::Software:       return "Software (single-Poller fallback)";
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
        "Port {} has no RSS/FlowDirector support (Software / single-Poller mode)",
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
    ///
    /// After remove(), `handle` is null but `port_id` / `queue_id` retain
    /// the coordinates the rule was last active at. This is intentional —
    /// monitoring/audit consumers reading `to_json()` post-remove still
    /// learn *where* the rule was, with `"active":false` distinguishing
    /// the live-vs-removed state. `dump()` returns `"FlowRule(inactive)"`
    /// without the coordinates so log noise stays terse; `to_json()`
    /// preserves them for structured pipelines that want the audit trail.
    void remove() noexcept {
        if (!handle) return;
        rte_flow_error error{};
        int ret = rte_flow_destroy(port_id, handle, &error);
        if (ret != 0) {
            // Same rationale as rte_flow_create: error.message can be
            // empty on some PMDs; rte_errno is the fallback signal so
            // teardown failures stay diagnosable.
            const int err = rte_errno;
            SPDLOG_LOGGER_WARN(detail::flow_logger(),
                "rte_flow_destroy failed: port={}, ret={}, msg={}, "
                "type={} rte_errno={} ({})",
                port_id, ret, error.message ? error.message : "unknown",
                static_cast<int>(error.type), err, rte_strerror(err));
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

/// Install a rte_flow rule that steers packets matching a 4-tuple
/// to a specific RX queue. Supports both TCP and UDP protocols.
/// Returns RAII FlowRule handle.
///
/// @param port_id  DPDK port ID
/// @param queue_id Target RX queue for matching packets
/// @param tuple    4-tuple (src/dst IP + port) in HOST byte order
/// @param proto    Protocol to match (default: TCP for backward compatibility)
/// @return FlowRule handle on success, error string on failure
[[nodiscard]] inline std::expected<FlowRule, std::string>
install_flow_rule(uint16_t port_id, uint16_t queue_id,
                  const ::eph::dpdk::net::ConnectionTuple& tuple,
                  FlowProtocol proto = FlowProtocol::Tcp) noexcept {
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

    // L4 match: protocol-specific header.
    // Declared in outer scope so pointers in l4_item remain valid for
    // rte_flow_create below. Only the active branch's fields are initialized.
    rte_flow_item_tcp tcp_spec{};
    rte_flow_item_tcp tcp_mask{};
    rte_flow_item_udp udp_spec{};
    rte_flow_item_udp udp_mask{};
    rte_flow_item l4_item{};

    if (proto == FlowProtocol::Tcp) {
        tcp_spec.hdr.src_port = rte_cpu_to_be_16(tuple.dst_port);
        tcp_spec.hdr.dst_port = rte_cpu_to_be_16(tuple.src_port);
        tcp_mask.hdr.src_port = 0xFFFF;
        tcp_mask.hdr.dst_port = 0xFFFF;
        l4_item = {.type = RTE_FLOW_ITEM_TYPE_TCP,
                   .spec = &tcp_spec, .last = nullptr, .mask = &tcp_mask};
    } else {
        udp_spec.hdr.src_port = rte_cpu_to_be_16(tuple.dst_port);
        udp_spec.hdr.dst_port = rte_cpu_to_be_16(tuple.src_port);
        udp_mask.hdr.src_port = 0xFFFF;
        udp_mask.hdr.dst_port = 0xFFFF;
        l4_item = {.type = RTE_FLOW_ITEM_TYPE_UDP,
                   .spec = &udp_spec, .last = nullptr, .mask = &udp_mask};
    }

    rte_flow_item pattern[] = {
        {.type = RTE_FLOW_ITEM_TYPE_ETH, .spec = nullptr, .last = nullptr, .mask = nullptr},
        {.type = RTE_FLOW_ITEM_TYPE_IPV4,
         .spec = &ipv4_spec, .last = nullptr, .mask = &ipv4_mask},
        l4_item,
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
        // rte_errno is set by the PMD on top of error.message — they are
        // complementary diagnostics. PMDs that fail with no message
        // (Mellanox / ENA on certain firmware) still set rte_errno
        // (typically -ENOTSUP / -EINVAL / -EAGAIN), which is the
        // operator's only signal in that case.
        const int err = rte_errno;
        auto msg = std::format(
            "rte_flow_create failed: port={}, queue={}, proto={}, "
            "error={} type={} rte_errno={} ({})",
            port_id, queue_id, flow_protocol_name(proto),
            error.message ? error.message : "unknown",
            static_cast<int>(error.type),
            err, rte_strerror(err));
        SPDLOG_LOGGER_WARN(log, "{}", msg);
        return std::unexpected(msg);
    }

    SPDLOG_LOGGER_INFO(log,
        "{} flow rule installed: {}:{} -> {}:{} queue={} (NIC src/dst swapped)",
        flow_protocol_name(proto),
        ::eph::dpdk::net::format_ipv4(tuple.dst_ip).data(), tuple.dst_port,
        ::eph::dpdk::net::format_ipv4(tuple.src_ip).data(), tuple.src_port,
        queue_id);

    FlowRule rule;
    rule.port_id = port_id;
    rule.queue_id = queue_id;
    rule.handle = flow;
    return rule;
}

// ---------------------------------------------------------------------------
// RSS hash prediction (Step 4) — Toeplitz over IPv4 5-tuple
// ---------------------------------------------------------------------------

/// DPDK / Microsoft default RSS Toeplitz key (40 bytes). Used as the
/// fallback when the driver does not implement `rte_eth_dev_rss_hash_conf_get`
/// (notably AWS ENA on some kernel versions). The key matches the one
/// applied by `configure_rss()` (which passes `rss_key = nullptr` to let
/// DPDK use this same default).
inline constexpr std::array<uint8_t, 40> kRssDefaultKey = {
    0x6D, 0x5A, 0x56, 0xDA, 0x25, 0x5B, 0x0E, 0xC2,
    0x41, 0x67, 0x25, 0x3D, 0x43, 0xA3, 0x8F, 0xB0,
    0xD0, 0xCA, 0x2B, 0xCB, 0xAE, 0x7B, 0x30, 0xB4,
    0x77, 0xCB, 0x2D, 0xA3, 0x80, 0x30, 0xF2, 0x0C,
    0x6A, 0x42, 0xB7, 0x3B, 0xBE, 0xAC, 0x01, 0xFA,
};

/// Compute the Toeplitz hash of an arbitrary input byte string under the
/// given RSS key, per Microsoft's "Verifying the RSS Hash Calculation"
/// spec.
///
/// The reference impl walks every input bit MSB-first; for each set bit
/// it XORs into the result a 32-bit window of the key starting at that
/// same bit offset. Performance is irrelevant on the connect path; clarity
/// over micro-optimisation.
///
/// @param key   RSS key bytes (typically 40); must be at least
///              `input.size() + 4` bytes long, otherwise high bits of
///              the trailing window read as zero (matches MSFT spec).
/// @param input The packet field tuple in NETWORK byte order.
[[nodiscard]] inline uint32_t
toeplitz_hash(std::span<const uint8_t> key,
              std::span<const uint8_t> input) noexcept {
    uint32_t result = 0;
    const size_t total_bits = input.size() * 8;
    for (size_t k = 0; k < total_bits; ++k) {
        const size_t input_byte = k / 8;
        const size_t input_bit  = 7 - (k % 8); // MSB-first
        if (((input[input_byte] >> input_bit) & 1u) == 0) continue;

        // 32-bit window of key starting at bit k.
        uint32_t window = 0;
        for (int w = 0; w < 32; ++w) {
            const size_t kbit = k + w;
            const size_t kby  = kbit / 8;
            const size_t kbi  = 7 - (kbit % 8);
            if (kby < key.size() && (key[kby] >> kbi) & 1u) {
                window |= (1u << (31 - w));
            }
        }
        result ^= window;
    }
    return result;
}

/// Convenience wrapper: hash an IPv4 + L4 5-tuple per Microsoft RSS spec.
///
/// The Microsoft layout for IPv4 + TCP/UDP RSS is:
///   src_ip (BE32) | dst_ip (BE32) | src_port (BE16) | dst_port (BE16)
///
/// All inputs are in HOST byte order; the function serialises them to
/// network-byte-order before feeding `toeplitz_hash`.
///
/// @note `src` here is the RSS-input "source" — for a packet arriving
/// at the NIC it is the REMOTE peer (incoming src). When predicting the
/// queue for a connection you opened locally, pass remote-as-src and
/// local-as-dst.
[[nodiscard]] inline uint32_t
toeplitz_hash_ipv4(std::span<const uint8_t> rss_key,
                   uint32_t src_ip, uint16_t src_port,
                   uint32_t dst_ip, uint16_t dst_port) noexcept {
    std::array<uint8_t, 12> buf{};
    buf[0]  = uint8_t(src_ip   >> 24); buf[1]  = uint8_t(src_ip   >> 16);
    buf[2]  = uint8_t(src_ip   >>  8); buf[3]  = uint8_t(src_ip   >>  0);
    buf[4]  = uint8_t(dst_ip   >> 24); buf[5]  = uint8_t(dst_ip   >> 16);
    buf[6]  = uint8_t(dst_ip   >>  8); buf[7]  = uint8_t(dst_ip   >>  0);
    buf[8]  = uint8_t(src_port >>  8); buf[9]  = uint8_t(src_port >>  0);
    buf[10] = uint8_t(dst_port >>  8); buf[11] = uint8_t(dst_port >>  0);
    return toeplitz_hash(rss_key, std::span<const uint8_t>(buf));
}

/// Look up which RX queue a Toeplitz hash routes to via the RETA table.
/// The standard RSS rule is `queue = reta[hash & (reta_size - 1)]`.
///
/// @param hash         Toeplitz hash of the packet's 5-tuple.
/// @param reta_table   Flattened RETA — one queue id per slot. Size MUST
///                     be a power of two (RSS spec); otherwise behaviour
///                     is implementation-defined.
/// @return Queue id, or 0 when `reta_table` is empty or not a power of two
///         (log a WARN in those cases — reaching this branch means the
///         caller supplied a malformed RSS snapshot, which is a bug).
///         Returning 0 keeps the caller on the default queue rather than
///         producing UB via an out-of-bounds read / wrong-mask index.
[[nodiscard]] inline uint16_t
queue_for_hash(uint32_t hash, std::span<const uint16_t> reta_table) noexcept {
    if (reta_table.empty()) [[unlikely]] {
        SPDLOG_LOGGER_WARN(detail::flow_logger(),
            "queue_for_hash: reta_table is empty -- returning queue 0");
        return 0;
    }
    const size_t sz = reta_table.size();
    // Power-of-two check: (sz & (sz - 1)) == 0 for non-zero powers of two.
    // Driver bugs / tests sometimes supply oddly-sized tables; degrade
    // gracefully rather than produce a silently wrong queue index.
    if ((sz & (sz - 1)) != 0) [[unlikely]] {
        SPDLOG_LOGGER_WARN(detail::flow_logger(),
            "queue_for_hash: reta_table size={} is not a power of two "
            "-- falling back to modulo (RSS routing will be approximate)",
            sz);
        return reta_table[hash % sz];
    }
    return reta_table[hash & (sz - 1)];
}

/// Snapshot of a port's RSS state (Toeplitz key + RETA). Query once via
/// `query_rss_state(port_id)`, then look up arbitrary 5-tuples with
/// `queue_for_tuple(state, ...)` — pure CPU, no NIC syscalls. Use this
/// in tight loops like `find_src_port_for_queue` where the RSS config
/// is known constant during the search.
struct RssState {
    /// Storage for the key bytes returned by `rte_eth_dev_rss_hash_conf_get`.
    /// `key_len == 0` means readback was unsupported and `key()` falls
    /// back to `kRssDefaultKey`.
    std::array<uint8_t, 64> key_buf{};
    uint8_t  key_len = 0;
    uint16_t reta_size = 0;
    /// RETA entries (max 512 slots / 64 entries-per-bucket = 8 buckets).
    rte_eth_rss_reta_entry64
        reta[RTE_ETH_RSS_RETA_SIZE_512 / RTE_ETH_RETA_GROUP_SIZE]{};

    /// Active RSS key — driver readback if available, else
    /// `kRssDefaultKey`. The returned span aliases either `key_buf` or
    /// `kRssDefaultKey`; do not retain past `RssState`'s lifetime.
    [[nodiscard]] std::span<const uint8_t> key() const noexcept {
        return key_len > 0 ? std::span(key_buf.data(),
                                       static_cast<size_t>(key_len))
                           : std::span(kRssDefaultKey);
    }
};

/// Query a port's RSS state with two NIC syscalls
/// (`rte_eth_dev_rss_hash_conf_get` + `rte_eth_dev_info_get` +
/// `rte_eth_dev_rss_reta_query`). Some PMDs (notably ENA) reject
/// `rss_hash_conf_get`; in that case `RssState::key_len` stays 0 and
/// `RssState::key()` falls back to `kRssDefaultKey` — the prediction
/// remains accurate as long as nobody overwrote the key (default
/// `configure_rss` installs exactly `kRssDefaultKey`).
[[nodiscard]] inline std::expected<RssState, std::string>
query_rss_state(uint16_t port_id) noexcept {
    [[maybe_unused]] auto* log = detail::flow_logger();
    RssState state;

    // 1. RSS key — try driver readback first.
    rte_eth_rss_conf rss_conf{};
    rss_conf.rss_key = state.key_buf.data();
    rss_conf.rss_key_len = static_cast<uint8_t>(state.key_buf.size());
    int ret = rte_eth_dev_rss_hash_conf_get(port_id, &rss_conf);
    if (ret == 0 && rss_conf.rss_key_len > 0) {
        state.key_len = rss_conf.rss_key_len;
    } else {
        SPDLOG_LOGGER_DEBUG(log,
            "rte_eth_dev_rss_hash_conf_get unsupported on port {} (ret={}), "
            "falling back to kRssDefaultKey", port_id, ret);
        // state.key_len stays 0 → state.key() returns kRssDefaultKey.
    }

    // 2. RETA — query the live indirection table.
    // RssState's reta[] is value-initialized in the struct definition,
    // so partial-fill (which DPDK PMDs are NOT supposed to do — the
    // contract is "fully success or fail entirely" — but if one ever
    // does) leaves un-touched slots at queue 0, which is a sane
    // fallback rather than reading uninitialized garbage.
    rte_eth_dev_info dev_info{};
    if (rte_eth_dev_info_get(port_id, &dev_info) != 0) {
        return std::unexpected("rte_eth_dev_info_get failed");
    }
    state.reta_size = dev_info.reta_size;
    if (state.reta_size == 0) state.reta_size = 128;
    state.reta_size = std::min(state.reta_size,
                               static_cast<uint16_t>(RTE_ETH_RSS_RETA_SIZE_512));
    const uint16_t groups = state.reta_size / RTE_ETH_RETA_GROUP_SIZE;
    for (uint16_t i = 0; i < groups; ++i) state.reta[i].mask = ~uint64_t(0);
    if (rte_eth_dev_rss_reta_query(port_id, state.reta, state.reta_size) != 0) {
        return std::unexpected("rte_eth_dev_rss_reta_query failed");
    }
    return state;
}

/// Pure-CPU lookup of which RX queue a given 5-tuple hashes to under
/// the supplied RssState snapshot. No NIC syscalls.
///
/// @warning `src_ip / src_port` describe the RSS *input* "source" — i.e.
/// the REMOTE end as seen by the NIC on incoming packets. To predict
/// the queue for a TCP connection you initiated as the local end, pass
/// `(remote_ip, remote_port, local_ip, local_port)`. **Do NOT pass your
/// own local IP/port as `src_*`** — the function silently produces the
/// wrong queue id otherwise.
[[nodiscard]] inline uint16_t
queue_for_tuple(const RssState& state,
                uint32_t src_ip, uint16_t src_port,
                uint32_t dst_ip, uint16_t dst_port) noexcept {
    const uint32_t h = toeplitz_hash_ipv4(state.key(), src_ip, src_port,
                                          dst_ip, dst_port);
    // Parity with `queue_for_hash`: gracefully degrade on zero-sized or
    // non-power-of-two reta_size rather than produce UB via a wrong mask.
    // RSS spec says RETA is always a POT, but driver bugs and tests do
    // hand us oddly-sized tables occasionally — log once and fall back.
    const uint16_t sz = state.reta_size;
    if (sz == 0) [[unlikely]] {
        SPDLOG_LOGGER_WARN(detail::flow_logger(),
            "queue_for_tuple: reta_size=0 -- returning queue 0");
        return 0;
    }
    size_t reta_idx;
    if ((sz & (sz - 1)) != 0) [[unlikely]] {
        SPDLOG_LOGGER_WARN(detail::flow_logger(),
            "queue_for_tuple: reta_size={} is not a power of two "
            "-- falling back to modulo (RSS routing will be approximate)",
            sz);
        reta_idx = h % sz;
    } else {
        reta_idx = h & (sz - 1);
    }
    const size_t group = reta_idx / RTE_ETH_RETA_GROUP_SIZE;
    const size_t bit   = reta_idx % RTE_ETH_RETA_GROUP_SIZE;
    return state.reta[group].reta[bit];
}

/// Predict which RX queue a packet with the given 5-tuple will land on.
/// Convenience over `query_rss_state` + `queue_for_tuple` for one-shot
/// callers; pays 2 NIC syscalls per call. For repeated lookups against
/// the same NIC, snapshot once with `query_rss_state` and call
/// `queue_for_tuple` in the loop instead.
///
/// @note `src_ip / src_port` describe the RSS *input* "source" — see
/// `queue_for_tuple` for the full RX-direction caveat.
[[nodiscard]] inline std::expected<uint16_t, std::string>
predict_rss_queue(uint16_t port_id,
                  uint32_t src_ip, uint16_t src_port,
                  uint32_t dst_ip, uint16_t dst_port) noexcept {
    auto state = query_rss_state(port_id);
    if (!state) return std::unexpected(state.error());
    return queue_for_tuple(*state, src_ip, src_port, dst_ip, dst_port);
}

/// Find a `src_port` in the given range so that the resulting 5-tuple
/// (with `src_ip` / `dst_ip` / `dst_port` fixed) RSS-hashes to
/// `target_queue`. Linear scan; returns the first match.
///
/// Used by `Stream::create_and_attach` in `RssPartitioned` mode when
/// the user explicitly pinned the connection to a specific queue: we
/// rebind the socket's local port until RSS lands the connection where
/// the user asked.
///
/// Default range matches the Linux ephemeral-port window
/// `/proc/sys/net/ipv4/ip_local_port_range` (32768..60999).
///
/// @return the chosen src_port, or an error string starting with
/// "RssHashPredictExhausted" if no port in the range hashes to the
/// target queue.
[[nodiscard]] inline std::expected<uint16_t, std::string>
find_src_port_for_queue(uint16_t port_id, uint16_t target_queue,
                        uint32_t src_ip,
                        uint32_t dst_ip, uint16_t dst_port,
                        uint16_t port_range_start = 32768,
                        uint16_t port_range_end   = 60999) noexcept {
    if (port_range_start > port_range_end) {
        return std::unexpected(
            "find_src_port_for_queue: port_range_start > port_range_end");
    }
    // Snapshot the NIC's RSS state ONCE (2 syscalls). The loop below is
    // pure CPU — Toeplitz hash + RETA lookup. Pre-refactor (commit 8b10661)
    // the loop did 2 syscalls per iteration, scaling to ~56k DPDK calls
    // for the default ephemeral-port range. See PR-1 perf rationale in
    // .artifacts/review-rss-eph-net-dpdk-20260421-052500.md (Major M1).
    auto state_r = query_rss_state(port_id);
    if (!state_r) return std::unexpected(state_r.error());
    const auto& state = *state_r;
    for (uint32_t sp = port_range_start; sp <= port_range_end; ++sp) {
        if (queue_for_tuple(state, src_ip, static_cast<uint16_t>(sp),
                            dst_ip, dst_port) == target_queue) {
            return static_cast<uint16_t>(sp);
        }
    }
    return std::unexpected(std::format(
        "RssHashPredictExhausted: no src_port in [{},{}] hashes to queue {}",
        port_range_start, port_range_end, target_queue));
}

} // namespace eph::net::dpdk

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specialization for RxDispatchMode
// ─────────────────────────────────────────────────────────────────────────────

/// @brief std::formatter specialization for RxDispatchMode.
///
/// Formats as the human-readable name from rx_dispatch_mode_name().
template <>
struct std::formatter<eph::net::dpdk::RxDispatchMode> : std::formatter<std::string_view> {
    auto format(eph::net::dpdk::RxDispatchMode m, auto& ctx) const {
        return std::formatter<std::string_view>::format(
            eph::net::dpdk::rx_dispatch_mode_name(m), ctx);
    }
};

/// @brief std::formatter specialization for FlowProtocol.
template <>
struct std::formatter<eph::net::dpdk::FlowProtocol> : std::formatter<std::string_view> {
    auto format(eph::net::dpdk::FlowProtocol p, auto& ctx) const {
        return std::formatter<std::string_view>::format(
            eph::net::dpdk::flow_protocol_name(p), ctx);
    }
};

/// @brief std::formatter specialization for FlowRule.
///
/// Formats as a compact one-line summary showing port, queue, and status.
template <>
struct std::formatter<eph::net::dpdk::FlowRule> : std::formatter<std::string> {
    auto format(const eph::net::dpdk::FlowRule& r, auto& ctx) const {
        return std::formatter<std::string>::format(r.dump(), ctx);
    }
};
