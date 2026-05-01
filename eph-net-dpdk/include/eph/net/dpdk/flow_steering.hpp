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
///
/// ## Error type policy
///
/// The fallible APIs in this header return `std::expected<T, std::string>`
/// rather than the project-wide `std::expected<T, eph::core::ErrorInfo>`
/// used elsewhere (Stream / Datagram / Poller). This is intentional:
///
///   1. flow_steering is a *control-plane* helper invoked once per
///      Platform bring-up. There is no hot path here — the cost of
///      heap-allocating a small string is irrelevant on a path that
///      already calls `rte_eth_dev_rss_hash_update` and friends.
///   2. The errors are inherently free-form ("RETA collapse rejected
///      by PMD: rc=-95 (ENOTSUP)" / "configure_rss already installed
///      key 0x{:08x} but query reports 0x{:08x}") — the string is
///      the diagnostic. ErrorInfo's enum + const-char* doesn't reduce
///      to a small set of named conditions here without losing fidelity.
///   3. The boundary where these errors meet the rest of the codebase
///      is `Platform::create()`, which converts the std::string into
///      an ErrorInfo with `core::Error::InvalidConfig` + the `.what()`
///      string interned as a static const char* — see platform.hpp.
///
/// If a future refactor surfaces enough flow_steering call-sites in
/// hot paths to make string allocation a measurable cost, migrating
/// to ErrorInfo is mechanical (the strings already serve as the
/// `.detail` field). Until then the dual-track is the path of least
/// surprise: each layer's error type matches its own callers' needs.
/// (Tracked by audit-rss-rollout-20260421-065000.md "可关注点 #1".)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <rte_ethdev.h>
#include <rte_flow.h>
#include <rte_udp.h>

#include "eph/core/error.hpp"
#include "eph/dpdk/detail/icmp_directory.hpp"   // g_active_self_proc_index
#include "eph/dpdk/detail/logger.hpp"
#include "eph/dpdk/detail/mp_ipc.hpp"
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
    auto* log = detail::flow_logger();

    if (num_queues < 2) {
        SPDLOG_LOGGER_ERROR(log,
            "configure_rss: num_queues={} < 2 — RSS requires at least 2 queues",
            num_queues);
        return std::unexpected("RSS requires at least 2 queues");
    }

    rte_eth_dev_info dev_info{};
    if (rte_eth_dev_info_get(port_id, &dev_info) != 0) {
        SPDLOG_LOGGER_ERROR(log,
            "configure_rss: rte_eth_dev_info_get(port={}) failed errno={}",
            port_id, rte_errno);
        return std::unexpected("rte_eth_dev_info_get failed");
    }

    // Clamp to device max
    uint16_t actual = std::min(num_queues,
                                dev_info.max_rx_queues);

    // Defense in depth: a malfunctioning PMD reporting max_rx_queues == 0
    // (or any future caller bypassing the num_queues >= 2 check above)
    // would cause a divide-by-zero in the `i % actual` step of the RETA
    // population loop below. Reject explicitly with a clear error rather
    // than risk UB.
    if (actual == 0) {
        return std::unexpected(std::format(
            "RSS configure: effective queue count is zero "
            "(num_queues={}, max_rx_queues={}); cannot populate RETA",
            num_queues, dev_info.max_rx_queues));
    }

    // Build RSS configuration. Hash on TCP **and** UDP IPv4 (plus the
    // bare-IPv4 catch-all), matching `Platform::create`'s pre-configure
    // eth_conf.rx_adv_conf.rss_conf.rss_hf set (platform.hpp around
    // line 1112-1116). Stripping UDP here (the prior TCP-only flag
    // set) silently broke multicast / UDP RSS distribution on PMDs
    // where `rte_eth_dev_rss_hash_update` overrides the earlier
    // configure-time mask: every UDP packet then landed on queue 0
    // (or the un-hashed default) instead of distributing — and only
    // TCP streams saw multi-queue RSS. The caller-visible symptom was
    // "MulticastReceiver bound to rx_queue=N drops everything" under
    // RssPartitioned mode. The shared mask keeps the two code paths
    // in lock-step.
    rte_eth_rss_conf rss_conf{};
    rss_conf.rss_key = nullptr;  // Use default hash key
    rss_conf.rss_key_len = 0;
    rss_conf.rss_hf = dev_info.flow_type_rss_offloads &
                       (RTE_ETH_RSS_NONFRAG_IPV4_TCP |
                        RTE_ETH_RSS_NONFRAG_IPV4_UDP |
                        RTE_ETH_RSS_IPV4);

    if (rss_conf.rss_hf == 0) {
        SPDLOG_LOGGER_ERROR(log,
            "configure_rss: NIC port={} reports flow_type_rss_offloads=0x{:x} — "
            "no IPv4 TCP/UDP RSS hash flags supported",
            port_id, dev_info.flow_type_rss_offloads);
        return std::unexpected(
            "NIC does not support IPv4 TCP/UDP RSS hash");
    }

    int ret = rte_eth_dev_rss_hash_update(port_id, &rss_conf);
    if (ret != 0) {
        SPDLOG_LOGGER_ERROR(log,
            "configure_rss: rte_eth_dev_rss_hash_update(port={}, "
            "rss_hf=0x{:x}) failed: rc={}",
            port_id, rss_conf.rss_hf, ret);
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
// ─── FlowDir IPC primitives (used by FlowRule + stage 6+ handlers) ──────────
//
// `FlowRule` may own one of two kinds of NIC flow rule:
//   - `LocalFlowHandle`: a `rte_flow*` installed by THIS process via
//     `rte_flow_create`. Destruction is a local `rte_flow_destroy`.
//   - `RemoteFlowHandle`: a rule installed BY THE PRIMARY ON OUR
//     BEHALF via the `eph_fd_install` IPC fallback path (stage 7).
//     The opaque rule handle stays in the primary's address space;
//     this struct just records `(owner_proc, handle_id)` so the dtor
//     can fire `eph_fd_destroy` IPC to ask the primary to destroy it.
//
// The IPC payload structs (`FdInstallMsg`, `FdInstallReply`,
// `FdDestroyMsg`, `FdDestroyReply`) are POD wire formats consumed by
// the rte_mp action handlers wired up in stage 6 / stage 7. They are
// version-tagged for cross-build compatibility.

struct LocalFlowHandle {
    rte_flow* h = nullptr;

    [[nodiscard]] friend bool operator==(const LocalFlowHandle&,
                                         const LocalFlowHandle&) = default;
};

struct RemoteFlowHandle {
    uint8_t  owner_proc = 0;
    uint8_t  _pad[7]    = {};
    uint64_t handle_id  = 0;

    [[nodiscard]] friend bool operator==(const RemoteFlowHandle&,
                                         const RemoteFlowHandle&) = default;
};

/// @brief Variant of "no rule held" / "local rte_flow*" / "remote
/// rule on primary". Default state is `monostate` (no rule).
using FlowHandleVariant = std::variant<std::monostate,
                                       LocalFlowHandle,
                                       RemoteFlowHandle>;

inline constexpr std::string_view kFdInstallActionName = "eph_fd_install";
inline constexpr std::string_view kFdDestroyActionName = "eph_fd_destroy";

/// @brief Install-rule IPC request payload. Stage 7 sends this from
/// secondary to primary when local `rte_flow_create` returns nullptr.
struct alignas(8) FdInstallMsg {
    uint8_t  version;
    uint8_t  proto;        ///< 6 = TCP, 17 = UDP
    uint8_t  requester_proc;
    uint8_t  reserved0;
    uint16_t target_queue;
    uint16_t reserved1;
    uint32_t src_ip;       ///< host byte order
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t port_id;
    uint16_t reserved2;
    uint32_t request_id;   ///< caller-correlation, opaque to handler
};
static_assert(std::is_trivially_copyable_v<FdInstallMsg>);

/// @brief Install-rule IPC reply payload. Sent by primary back to
/// secondary; carries a synthetic 64-bit handle the secondary stores
/// in `RemoteFlowHandle` so `eph_fd_destroy` later can find the
/// underlying `rte_flow*` in primary's `remote_owned_rules_` map.
struct alignas(8) FdInstallReply {
    uint8_t  version;
    uint8_t  status;       ///< 0 = success, non-zero = error
    uint16_t reserved0;
    uint32_t reserved1;
    uint64_t handle_id;
};
static_assert(std::is_trivially_copyable_v<FdInstallReply>);

struct alignas(8) FdDestroyMsg {
    uint8_t  version;
    uint8_t  reserved[7];
    uint64_t handle_id;
    uint32_t request_id;
    uint32_t reserved2;
};
static_assert(std::is_trivially_copyable_v<FdDestroyMsg>);

struct alignas(8) FdDestroyReply {
    uint8_t  version;
    uint8_t  status;
    uint8_t  reserved[6];
};
static_assert(std::is_trivially_copyable_v<FdDestroyReply>);

namespace detail {

/// @brief Forward-declaration of the IPC destroy helper. Stage 6/7
/// provides the implementation via `rte_mp_request_sync`. Stage 5
/// uses the stub below — RemoteFlowHandle dtors log a debug message
/// and treat the destroy as best-effort. Once stage 6 wires the
/// real handler, this same function does the actual IPC send.
[[nodiscard]] inline std::expected<void, ::eph::core::ErrorInfo>
fd_destroy_via_ipc(uint8_t  owner_proc,
                   uint64_t handle_id) noexcept;

} // namespace detail

struct FlowRule {
    uint16_t          port_id  = 0;   ///< DPDK port the rule is installed on
    uint16_t          queue_id = 0;   ///< Target RX queue for matched packets
    /// @brief Tagged handle. Default = `monostate` (no rule held).
    /// `LocalFlowHandle{rte_flow*}` for rules this process installed
    /// directly; `RemoteFlowHandle{owner_proc, handle_id}` for rules
    /// the primary installed on our behalf via IPC fallback.
    FlowHandleVariant handle{};

    /// @brief Default constructor creates an invalid (empty) rule.
    FlowRule() = default;

    /// @brief Destructor removes the flow rule from the NIC if still valid.
    ~FlowRule() { remove(); }

    FlowRule(const FlowRule&) = delete;
    FlowRule& operator=(const FlowRule&) = delete;

    /// @brief Move constructor. Transfers ownership; source becomes invalid.
    FlowRule(FlowRule&& other) noexcept
        : port_id(other.port_id), queue_id(other.queue_id),
          handle(std::move(other.handle)) {
        other.handle = std::monostate{};
    }

    /// @brief Move assignment. Removes any existing rule before taking ownership.
    FlowRule& operator=(FlowRule&& other) noexcept {
        if (this != &other) {
            remove();
            port_id  = other.port_id;
            queue_id = other.queue_id;
            handle   = std::move(other.handle);
            other.handle = std::monostate{};
        }
        return *this;
    }

    /// Remove the flow rule from the NIC. Safe to call multiple times.
    ///
    /// Visit-dispatches on the variant:
    ///   - monostate          → noop
    ///   - LocalFlowHandle    → rte_flow_destroy on the local rte_flow*
    ///   - RemoteFlowHandle   → fire `eph_fd_destroy` IPC to owner proc
    ///
    /// After remove(), `handle` is `monostate` but `port_id` /
    /// `queue_id` retain the coordinates the rule was last active at,
    /// matching the pre-variant audit-friendly behavior.
    void remove() noexcept {
        std::visit([this](auto& h) {
            using T = std::decay_t<decltype(h)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                // Nothing to do.
            } else if constexpr (std::is_same_v<T, LocalFlowHandle>) {
                if (h.h == nullptr) return;
                rte_flow_error error{};
                int ret = rte_flow_destroy(port_id, h.h, &error);
                if (ret != 0) {
                    const int err = rte_errno;
                    SPDLOG_LOGGER_WARN(detail::flow_logger(),
                        "rte_flow_destroy failed: port={}, ret={}, msg={}, "
                        "type={} rte_errno={} ({})",
                        port_id, ret,
                        error.message ? error.message : "unknown",
                        static_cast<int>(error.type), err, rte_strerror(err));
                } else {
                    SPDLOG_LOGGER_DEBUG(detail::flow_logger(),
                        "Flow rule removed: port={}, queue={}",
                        port_id, queue_id);
                }
                h.h = nullptr;
            } else if constexpr (std::is_same_v<T, RemoteFlowHandle>) {
                if (h.handle_id == 0) return;
                auto r = detail::fd_destroy_via_ipc(h.owner_proc, h.handle_id);
                if (!r) {
                    SPDLOG_LOGGER_WARN(detail::flow_logger(),
                        "Remote flow rule destroy IPC failed: owner_proc={}, "
                        "handle_id={}, err={} — primary may have died first; "
                        "rule will be cleaned up at primary teardown",
                        h.owner_proc, h.handle_id, r.error().detail);
                } else {
                    SPDLOG_LOGGER_DEBUG(detail::flow_logger(),
                        "Remote flow rule destroyed: owner_proc={}, handle_id={}",
                        h.owner_proc, h.handle_id);
                }
                h.handle_id = 0;
            }
        }, handle);
        handle = std::monostate{};
    }

    /// @brief Check if this rule is still active on the NIC.
    /// @return true if the variant holds either LocalFlowHandle (with
    /// non-null rte_flow*) or RemoteFlowHandle (with non-zero
    /// handle_id). Monostate, null/zero internals → false.
    [[nodiscard]] bool valid() const noexcept {
        return std::visit([](const auto& h) -> bool {
            using T = std::decay_t<decltype(h)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return false;
            } else if constexpr (std::is_same_v<T, LocalFlowHandle>) {
                return h.h != nullptr;
            } else if constexpr (std::is_same_v<T, RemoteFlowHandle>) {
                return h.handle_id != 0;
            }
        }, handle);
    }

    /// Convenience bool conversion for validity checking.
    /// Usage: if (rule) { /* rule is active */ }
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

    /// Human-readable dump for logging/debugging.
    [[nodiscard]] std::string dump() const {
        if (!valid())
            return "FlowRule(inactive)";
        return std::visit([this](const auto& h) -> std::string {
            using T = std::decay_t<decltype(h)>;
            if constexpr (std::is_same_v<T, LocalFlowHandle>) {
                return std::format(
                    "FlowRule(port={}, queue={}, active, local)",
                    port_id, queue_id);
            } else if constexpr (std::is_same_v<T, RemoteFlowHandle>) {
                return std::format(
                    "FlowRule(port={}, queue={}, active, remote owner={} id={})",
                    port_id, queue_id, h.owner_proc, h.handle_id);
            } else {
                return "FlowRule(inactive)";
            }
        }, handle);
    }

    /// @brief Telemetry accessor: a 64-bit "rule handle id" that
    /// callers can publish as an opaque audit token.
    ///
    /// For LocalFlowHandle this is `(uint64_t)rte_flow*` — the same
    /// value the pre-variant code reported. For RemoteFlowHandle it
    /// is the primary-side `handle_id` (also a uint64_t). For
    /// monostate / null/zero internals, returns 0. The caller treats
    /// the value as opaque; it is not re-resolvable to either kind.
    [[nodiscard]] uint64_t opaque_handle_id() const noexcept {
        return std::visit([](const auto& h) -> uint64_t {
            using T = std::decay_t<decltype(h)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return 0;
            } else if constexpr (std::is_same_v<T, LocalFlowHandle>) {
                return reinterpret_cast<uint64_t>(h.h);
            } else if constexpr (std::is_same_v<T, RemoteFlowHandle>) {
                return h.handle_id;
            }
        }, handle);
    }

    /// JSON-formatted status for monitoring system integration.
    [[nodiscard]] std::string to_json() const {
        const char* origin = std::visit(
            [](const auto& h) -> const char* {
                using T = std::decay_t<decltype(h)>;
                if constexpr (std::is_same_v<T, LocalFlowHandle>) return "local";
                if constexpr (std::is_same_v<T, RemoteFlowHandle>) return "remote";
                return "none";
            }, handle);
        return std::format(
            "{{\"port_id\":{},\"queue_id\":{},\"active\":{},\"origin\":\"{}\"}}",
            port_id, queue_id, valid() ? "true" : "false", origin);
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
    rule.port_id  = port_id;
    rule.queue_id = queue_id;
    rule.handle   = LocalFlowHandle{flow};
    return rule;
}

namespace detail {

/// @brief Real IPC implementation of the FlowRule RemoteFlowHandle
/// destroy path. Stage 6 wires this once the on-primary
/// `eph_fd_destroy` handler exists. Best-effort: a 2 s timeout is
/// generous enough for the cold-path IPC roundtrip; if primary has
/// died first, the request_sync returns `Timeout` and the caller
/// (FlowRule's RAII dtor) only logs — primary's process exit will
/// free the rule's hugepage state regardless.
///
/// The `owner_proc` parameter is currently unused because DPDK's
/// rte_mp 1:1 primary↔secondary model makes the responder
/// implicit; carried here for diagnostic logs and future N:M
/// extensibility.
[[nodiscard]] inline std::expected<void, ::eph::core::ErrorInfo>
fd_destroy_via_ipc([[maybe_unused]] uint8_t  owner_proc,
                   uint64_t                  handle_id) noexcept {
    static std::atomic<uint32_t> next_req_id{1};
    FdDestroyMsg req{};
    req.version    = 1;
    req.handle_id  = handle_id;
    req.request_id = next_req_id.fetch_add(1, std::memory_order_relaxed);

    auto reply = ::eph::dpdk::detail::mp_ipc_request_sync<
        FdDestroyMsg, FdDestroyReply>(
            kFdDestroyActionName, req, std::chrono::milliseconds{2000});
    if (!reply) {
        SPDLOG_LOGGER_DEBUG(flow_logger(),
            "fd_destroy_via_ipc: IPC request failed (owner_proc={}, "
            "handle_id={}, err={})",
            owner_proc, handle_id, reply.error().detail);
        return std::unexpected(reply.error());
    }
    if (reply->version != 1 || reply->status != 0) {
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::InvalidConfig,
            "fd_destroy_via_ipc: primary returned non-zero status "
            "(handle_id not known? version mismatch?)"});
    }
    return {};
}

// ─── RemoteFlowRulesMap: primary-side storage of rules installed
//                        on behalf of secondaries ─────────────────
//
// Owned by `Platform::Impl` (one per Platform). Maps a synthetic
// 64-bit handle_id (returned to secondary in FdInstallReply, stored
// in RemoteFlowHandle) back to the primary-local rte_flow* + the
// port the rule was installed on.
//
// Thread-safe under `mu_`. The thunks below load the active
// instance via `g_active_remote_flow_rules` set by
// `Platform::create_primary` (cleared in ~Impl).

class RemoteFlowRulesMap {
public:
    /// @brief Insert a primary-installed rule and return its id.
    /// Returns 0 on internal failure (extremely rare — id wrap would
    /// take centuries at HFT-scale install rates).
    [[nodiscard]] uint64_t
    insert(uint16_t port_id, rte_flow* flow) noexcept {
        if (flow == nullptr) return 0;
        const uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
        if (id == 0) return 0;   // wraparound sentinel; effectively never
        std::lock_guard<std::mutex> g(mu_);
        rules_.emplace(id, std::pair<uint16_t, rte_flow*>{port_id, flow});
        return id;
    }

    /// @brief Look up a rule by id, rte_flow_destroy it, remove the
    /// map entry. Returns true on success.
    [[nodiscard]] bool destroy_by_id(uint64_t handle_id) noexcept {
        rte_flow* victim = nullptr;
        uint16_t  victim_port = 0;
        {
            std::lock_guard<std::mutex> g(mu_);
            auto it = rules_.find(handle_id);
            if (it == rules_.end()) return false;
            victim_port = it->second.first;
            victim      = it->second.second;
            rules_.erase(it);
        }
        // rte_flow_destroy outside the lock — it can take the PMD's
        // own internal locks and we don't want to nest.
        if (victim != nullptr) {
            rte_flow_error err{};
            const int rc = rte_flow_destroy(victim_port, victim, &err);
            if (rc != 0) {
                SPDLOG_LOGGER_WARN(flow_logger(),
                    "RemoteFlowRulesMap::destroy_by_id: rte_flow_destroy "
                    "failed (handle_id={}, port={}, rc={}, msg={})",
                    handle_id, victim_port, rc,
                    err.message ? err.message : "unknown");
            }
        }
        return true;
    }

    /// @brief ~Platform::Impl helper: destroy every still-tracked
    /// rule. Called once on Platform teardown so a primary that dies
    /// before its peer secondaries doesn't leak NIC flow state.
    void destroy_all() noexcept {
        std::vector<std::pair<uint16_t, rte_flow*>> snapshot;
        {
            std::lock_guard<std::mutex> g(mu_);
            snapshot.reserve(rules_.size());
            for (auto& kv : rules_) snapshot.push_back(kv.second);
            rules_.clear();
        }
        for (auto& [port, flow] : snapshot) {
            if (flow == nullptr) continue;
            rte_flow_error err{};
            (void)rte_flow_destroy(port, flow, &err);
        }
    }

    [[nodiscard]] size_t size_for_test() const noexcept {
        std::lock_guard<std::mutex> g(mu_);
        return rules_.size();
    }

private:
    mutable std::mutex                                          mu_;
    std::unordered_map<uint64_t, std::pair<uint16_t, rte_flow*>> rules_;
    std::atomic<uint64_t>                                        next_id_{1};
};

/// @brief Process-level pointer to the active RemoteFlowRulesMap.
/// Set by `Platform::create_primary` when the eph_fd_install IPC
/// handler is registered; cleared (CAS) by `~Impl`. Loaded by the
/// static `on_fd_install_thunk` / `on_fd_destroy_thunk` below.
inline std::atomic<RemoteFlowRulesMap*> g_active_remote_flow_rules{nullptr};

/// @brief Reply helper that uses `rte_mp_reply` from the action
/// handler context. Wrapped here so the static thunks below stay
/// readable.
template <typename ReplyT>
inline void
fd_send_reply_(std::string_view action_name, const ReplyT& payload,
               const void* peer) noexcept {
    rte_mp_msg out{};
    if (!::eph::dpdk::detail::pack_msg(out, action_name, payload)) {
        SPDLOG_LOGGER_ERROR(flow_logger(),
            "fd_send_reply_: pack_msg failed for action '{}'", action_name);
        return;
    }
    const int rc = rte_mp_reply(&out, static_cast<const char*>(peer));
    if (rc != 0) {
        SPDLOG_LOGGER_ERROR(flow_logger(),
            "fd_send_reply_: rte_mp_reply failed for '{}' (rc={})",
            action_name, rc);
    }
}

/// @brief rte_mp_t handler for incoming `eph_fd_install` IPC msgs.
/// Runs on DPDK's IPC thread; calls the local `install_flow_rule`,
/// stashes the resulting rte_flow* in the active RemoteFlowRulesMap,
/// and replies with the synthetic handle_id (0 on failure).
inline int
on_fd_install_thunk(const rte_mp_msg* msg, const void* peer) {
    auto* rules = g_active_remote_flow_rules.load(std::memory_order_acquire);
    auto parsed = ::eph::dpdk::detail::parse_payload<FdInstallMsg>(msg);

    FdInstallReply reply{};
    reply.version = 1;
    reply.status  = 1;       // assume failure until proven otherwise
    reply.handle_id = 0;

    if (rules == nullptr || !parsed) {
        SPDLOG_LOGGER_WARN(flow_logger(),
            "on_fd_install_thunk: rules={} parsed={} — IPC degraded; "
            "replying error",
            static_cast<const void*>(rules),
            parsed.has_value() ? "ok" : "fail");
        fd_send_reply_(kFdInstallActionName, reply, peer);
        return 0;
    }
    if (parsed->version != 1) {
        SPDLOG_LOGGER_WARN(flow_logger(),
            "on_fd_install_thunk: msg version={} != 1, refusing",
            parsed->version);
        fd_send_reply_(kFdInstallActionName, reply, peer);
        return 0;
    }

    ::eph::dpdk::net::ConnectionTuple t{};
    t.src_ip   = parsed->src_ip;
    t.dst_ip   = parsed->dst_ip;
    t.src_port = parsed->src_port;
    t.dst_port = parsed->dst_port;
    const auto proto = (parsed->proto == 17)
        ? FlowProtocol::Udp
        : FlowProtocol::Tcp;

    auto rule = install_flow_rule(parsed->port_id, parsed->target_queue,
                                  t, proto);
    if (!rule) {
        SPDLOG_LOGGER_WARN(flow_logger(),
            "on_fd_install_thunk: install_flow_rule failed: {}",
            rule.error());
        fd_send_reply_(kFdInstallActionName, reply, peer);
        return 0;
    }

    // Pull the rte_flow* out of the rule, neutralise the variant so
    // the rule's RAII dtor doesn't double-destroy.
    auto* local = std::get_if<LocalFlowHandle>(&rule->handle);
    rte_flow* flow = local ? local->h : nullptr;
    rule->handle = std::monostate{};   // owned by RemoteFlowRulesMap from here

    const uint64_t id = rules->insert(parsed->port_id, flow);
    if (id == 0) {
        SPDLOG_LOGGER_ERROR(flow_logger(),
            "on_fd_install_thunk: RemoteFlowRulesMap::insert returned 0 "
            "— flow leak (cleaned up at primary exit)");
        fd_send_reply_(kFdInstallActionName, reply, peer);
        return 0;
    }

    SPDLOG_LOGGER_INFO(flow_logger(),
        "on_fd_install_thunk: installed rule on behalf of secondary "
        "(requester_proc={}, request_id={}, handle_id={}, port={}, queue={})",
        parsed->requester_proc, parsed->request_id, id,
        parsed->port_id, parsed->target_queue);

    reply.status    = 0;
    reply.handle_id = id;
    fd_send_reply_(kFdInstallActionName, reply, peer);
    return 0;
}

/// @brief rte_mp_t handler for `eph_fd_destroy`. Looks up the
/// handle_id in the active RemoteFlowRulesMap, destroys the
/// rte_flow*, replies with status 0/1.
inline int
on_fd_destroy_thunk(const rte_mp_msg* msg, const void* peer) {
    auto* rules = g_active_remote_flow_rules.load(std::memory_order_acquire);
    auto parsed = ::eph::dpdk::detail::parse_payload<FdDestroyMsg>(msg);

    FdDestroyReply reply{};
    reply.version = 1;
    reply.status  = 1;

    if (rules == nullptr || !parsed) {
        fd_send_reply_(kFdDestroyActionName, reply, peer);
        return 0;
    }
    if (parsed->version != 1) {
        fd_send_reply_(kFdDestroyActionName, reply, peer);
        return 0;
    }

    const bool ok = rules->destroy_by_id(parsed->handle_id);
    reply.status = ok ? 0 : 1;
    SPDLOG_LOGGER_DEBUG(flow_logger(),
        "on_fd_destroy_thunk: handle_id={} → status={}",
        parsed->handle_id, reply.status);
    fd_send_reply_(kFdDestroyActionName, reply, peer);
    return 0;
}

} // namespace detail

// ---------------------------------------------------------------------------
// FlowDir IPC fallback (public API; depends on detail above)
// ---------------------------------------------------------------------------

/// @brief Secondary-side fallback: when `install_flow_rule`'s
/// local `rte_flow_create` returns nullptr (PMD doesn't support
/// secondary install), fire `eph_fd_install` IPC at the primary
/// and wrap the returned handle_id in a `RemoteFlowHandle`. The
/// returned `FlowRule`'s RAII dtor will fire `eph_fd_destroy` IPC
/// when it falls out of scope.
///
/// @param owner_proc  primary's proc index (0 by convention under
///                    MpTopology); informational, used for the
///                    RemoteFlowHandle metadata + debug logs.
[[nodiscard]] inline std::expected<FlowRule, std::string>
try_install_flow_rule_via_ipc(uint16_t      port_id,
                              uint16_t      queue_id,
                              const ::eph::dpdk::net::ConnectionTuple& tuple,
                              FlowProtocol  proto,
                              uint8_t       owner_proc = 0) noexcept {
    static std::atomic<uint32_t> next_req_id{1};
    const uint8_t requester_proc =
        ::eph::dpdk::detail::g_active_self_proc_index.load(
            std::memory_order_acquire);
    FdInstallMsg req{};
    req.version        = 1;
    req.proto          = (proto == FlowProtocol::Udp) ? uint8_t{17}
                                                      : uint8_t{6};
    req.requester_proc = requester_proc;
    req.target_queue   = queue_id;
    req.src_ip         = tuple.src_ip;
    req.dst_ip         = tuple.dst_ip;
    req.src_port       = tuple.src_port;
    req.dst_port       = tuple.dst_port;
    req.port_id        = port_id;
    req.request_id     = next_req_id.fetch_add(1, std::memory_order_relaxed);

    auto reply = ::eph::dpdk::detail::mp_ipc_request_sync<
        FdInstallMsg, FdInstallReply>(
            kFdInstallActionName, req, std::chrono::milliseconds{5000});
    if (!reply) {
        return std::unexpected(std::format(
            "FlowDir IPC fallback failed: {} (primary may not be running, "
            "or eph_fd_install handler not registered)",
            reply.error().detail));
    }
    if (reply->version != 1) {
        return std::unexpected(
            "FlowDir IPC fallback: reply version mismatch");
    }
    if (reply->status != 0 || reply->handle_id == 0) {
        return std::unexpected(std::format(
            "FlowDir IPC fallback: primary returned error status={}",
            reply->status));
    }

    SPDLOG_LOGGER_INFO(detail::flow_logger(),
        "FlowDir IPC fallback succeeded: owner_proc={}, handle_id={}, "
        "port={}, queue={}",
        owner_proc, reply->handle_id, port_id, queue_id);

    FlowRule rule;
    rule.port_id  = port_id;
    rule.queue_id = queue_id;
    rule.handle   = RemoteFlowHandle{
        .owner_proc = owner_proc,
        .handle_id  = reply->handle_id,
    };
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
    /// `key_len == 0` means the readback returned no key (PMD doesn't
    /// expose its hash key) and `key()` falls back to `kRssDefaultKey`.
    /// The fallback is only correct when the NIC is actually running
    /// `kRssDefaultKey` — see `key()` for the full safety contract.
    std::array<uint8_t, 64> key_buf{};
    uint8_t  key_len = 0;
    uint16_t reta_size = 0;
    /// RETA entries (max 512 slots / 64 entries-per-bucket = 8 buckets).
    rte_eth_rss_reta_entry64
        reta[RTE_ETH_RSS_RETA_SIZE_512 / RTE_ETH_RETA_GROUP_SIZE]{};

    /// Active RSS key — driver readback when `key_len > 0`, else
    /// `kRssDefaultKey` as a best-effort fallback. The returned span
    /// aliases either `key_buf` or `kRssDefaultKey`; do not retain past
    /// `RssState`'s lifetime.
    ///
    /// **Fallback safety**: when `key_len == 0`, predictions made
    /// against this key are only correct if `configure_rss` succeeded
    /// on this port (and thus installed `kRssDefaultKey`). On PMDs
    /// where `configure_rss` was rejected (notably some ENA driver
    /// versions) the NIC is using its OWN internal default key, not
    /// `kRssDefaultKey`, and predictions are silently wrong.
    /// `Platform::create` enforces this by hard-failing when both
    /// `rss_hash_update` AND `rss_hash_conf_get` are unsupported, so a
    /// `Platform`-managed port never reaches the unsafe fallback path.
    /// Direct callers of `query_rss_state` outside the `Platform`
    /// bring-up are responsible for ensuring the same invariant.
    [[nodiscard]] std::span<const uint8_t> key() const noexcept {
        return key_len > 0 ? std::span(key_buf.data(),
                                       static_cast<size_t>(key_len))
                           : std::span(kRssDefaultKey);
    }
};

/// Query a port's RSS state with three NIC syscalls
/// (`rte_eth_dev_rss_hash_conf_get` + `rte_eth_dev_info_get` +
/// `rte_eth_dev_rss_reta_query`).
///
/// PMD coverage notes (empirical, DPDK 24.11):
///   * Mellanox / Intel: `rss_hash_conf_get` returns the installed key
///     (whatever `configure_rss` set, defaulting to `kRssDefaultKey`).
///   * ENA (AWS Graviton, current driver): `rss_hash_conf_get` returns
///     the NIC's actual hash key (64 bytes) even though
///     `rss_hash_update` is rejected — so the probe path makes
///     RssPartitioned mode genuinely usable on ENA.
///   * Older / exotic PMDs: may reject `rss_hash_conf_get` entirely
///     (`key_len` stays 0). `Platform::create` treats that as a
///     hard-fail when the caller asked for `nb_rx_queues > 1`; see
///     the `key()` doc for the safety contract on the fallback path.
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
    if (int rc = rte_eth_dev_info_get(port_id, &dev_info); rc != 0) {
        // rc is the negated errno (DPDK convention); rte_errno mirrors
        // it but some PMDs only set one path. Surface both so the
        // caller's std::unexpected detail is actionable.
        const int err = rte_errno;
        return std::unexpected(std::format(
            "rte_eth_dev_info_get failed: port={} rc={} rte_errno={} ({})",
            port_id, rc, err, rte_strerror(err)));
    }
    state.reta_size = dev_info.reta_size;
    if (state.reta_size == 0) state.reta_size = 128;
    state.reta_size = std::min(state.reta_size,
                               static_cast<uint16_t>(RTE_ETH_RSS_RETA_SIZE_512));
    // Ceiling division: a non-power-of-two `reta_size` (legal per the
    // DPDK API even though all current production PMDs report 64/128/
    // 256/512) puts the tail entries in a partial group whose mask
    // must still be set so `rte_eth_dev_rss_reta_query` populates it.
    // Truncating-divide here (the prior shape) left the partial
    // group's mask at 0 and the matching `state.reta[group_n].reta[*]`
    // bytes at value-init zero — `queue_for_tuple` then silently
    // routed every tuple landing in that partial group to queue 0,
    // an ENA / IDPF-class corner case caught by `queue_for_tuple`'s
    // own POT fallback only because the warn-and-fallback path was
    // not reached. Note: the reta[] array sizes 8 buckets =
    // 8 × 64 = 512 entries, the DPDK upper bound — so the loop
    // bound below cannot overflow even at reta_size = 512.
    const uint16_t groups = (state.reta_size + RTE_ETH_RETA_GROUP_SIZE - 1)
                            / RTE_ETH_RETA_GROUP_SIZE;
    for (uint16_t i = 0; i < groups; ++i) state.reta[i].mask = ~uint64_t(0);
    if (int rc = rte_eth_dev_rss_reta_query(port_id, state.reta, state.reta_size); rc != 0) {
        const int err = rte_errno;
        return std::unexpected(std::format(
            "rte_eth_dev_rss_reta_query failed: port={} reta_size={} "
            "rc={} rte_errno={} ({})",
            port_id, state.reta_size, rc, err, rte_strerror(err)));
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

namespace detail {
/// Process-global counter that drives `find_src_port_for_queue`'s
/// search start position. Each call to the helper without an explicit
/// `start_hint` advances this by 1, ensuring multiple stream attaches
/// targeting the same `(remote_ip, remote_port, local_ip, target_queue)`
/// receive distinct local source ports — preventing 5-tuple collision
/// in fan-out patterns (e.g. 15-path producers connecting to a single
/// exchange endpoint, all pinned to the same RSS queue).
///
/// Atomic with relaxed ordering: the counter only needs to advance
/// monotonically per process, no cross-thread happens-before required.
inline std::atomic<uint32_t> g_src_port_search_counter{0};
}  // namespace detail

/// Pure variant of `find_src_port_for_queue` parametrized by an explicit
/// `RssState` snapshot — exposes the wrap-around search loop for unit
/// testing without needing a real NIC. Production code should call the
/// wrapper at the bottom of this section; tests use this overload to
/// verify fan-out distinctness, hint determinism, and exhaustion paths
/// against hand-crafted state.
///
/// Same arguments and return semantics as `find_src_port_for_queue`
/// below — see that function for the full prose.
[[nodiscard]] inline std::expected<uint16_t, std::string>
find_src_port_for_queue_with_state(
    const RssState& state,
    uint16_t target_queue,
    uint32_t remote_ip, uint16_t remote_port,
    uint32_t local_ip,
    uint16_t port_range_start = 32768,
    uint16_t port_range_end   = 60999,
    std::optional<uint32_t> start_hint = std::nullopt) noexcept {
    if (port_range_start > port_range_end) {
        return std::unexpected(std::format(
            "find_src_port_for_queue: inverted range "
            "port_range_start={} > port_range_end={}",
            port_range_start, port_range_end));
    }

    // Hint semantics: "return the (hint mod match_count)-th port in the
    // range that hashes to target_queue", not "first match starting at
    // port_range_start + hint". The latter clusters: with N queues and
    // a uniformly-distributed Toeplitz hash, matches sit ~N apart, so
    // hint=0..N-1 all converge on the first match. Counting matches and
    // indexing by hint guarantees fan-out distinctness up to match_count.
    //
    //   - explicit start_hint  → deterministic (used by unit tests)
    //   - nullopt (production) → fetch_add on the process-global counter,
    //     so each call's hint is distinct and the result is a distinct
    //     match (modulo match_count, which is ~range/queue_count).
    const uint32_t hint = start_hint.value_or(
        detail::g_src_port_search_counter.fetch_add(
            1, std::memory_order_relaxed));

    // Pass 1: count matches in the range. The Toeplitz inner loop is
    // pure CPU (~50 ops per port), and cold-path callers pay this once
    // per stream attach. For N=4 queues + default ephemeral range that's
    // ~28k iterations × 2 passes = ~56k hashes (~1ms wallclock on
    // modern aarch64). Single-pass + collect-into-array would be faster
    // but bound the buffer awkwardly when match_count exceeds it.
    size_t match_count = 0;
    for (uint32_t sp = port_range_start; sp <= port_range_end; ++sp) {
        // The searched `sp` is our LOCAL port; on the inbound SYN-ACK
        // it lands in the dst_port slot of the RSS hash input. The
        // pre-fix (Toeplitz arg-order bug) code put `sp` in the
        // src_port slot, which computed a different hash — Toeplitz
        // is not symmetric so the predicted queue diverged from the
        // queue the NIC actually picked.
        if (queue_for_tuple(state, remote_ip, remote_port,
                            local_ip,  static_cast<uint16_t>(sp))
            == target_queue) {
            ++match_count;
        }
    }
    if (match_count == 0) {
        return std::unexpected(std::format(
            "RssHashPredictExhausted: no src_port in [{},{}] hashes to queue {}",
            port_range_start, port_range_end, target_queue));
    }

    // Pass 2: walk again, returning the (hint mod match_count)-th match.
    const uint32_t target_idx = hint % static_cast<uint32_t>(match_count);
    uint32_t seen = 0;
    for (uint32_t sp = port_range_start; sp <= port_range_end; ++sp) {
        if (queue_for_tuple(state, remote_ip, remote_port,
                            local_ip,  static_cast<uint16_t>(sp))
            == target_queue) {
            if (seen == target_idx) {
                return static_cast<uint16_t>(sp);
            }
            ++seen;
        }
    }
    // Unreachable: match_count > 0 means pass 2 finds the same matches.
    // Surface as an internal error if we somehow get here.
    return std::unexpected(
        "find_src_port_for_queue: pass 2 disagreed with pass 1 (internal bug)");
}

/// Find a local ephemeral source port `sp` in the given range so that
/// the inbound packet `(remote_ip:remote_port → local_ip:sp)`
/// RSS-hashes to `target_queue`. Wrap-around linear scan starting from
/// a caller-supplied (or process-global auto-incrementing) hint;
/// returns the first match.
///
/// Used by `Stream::create_and_attach` in `RssPartitioned` mode when
/// the user explicitly pinned the connection to a specific queue: we
/// rebind the socket's local port until RSS lands the connection where
/// the user asked. Also by `dns::resolve` and `AsyncDnsResolverT::start`
/// to keep DNS replies on the resolver's polling queue.
///
/// **Why a hint instead of just starting from `port_range_start`**: the
/// search is deterministic in input — without a varying hint, two calls
/// with the same `(remote, local, target_queue)` arguments would return
/// the *same* `sp`, and N concurrent fan-out streams (15-path producer
/// → single exchange) would all land on identical 5-tuples and silently
/// trample each other's RX. The hint defaults to a process-global atomic
/// counter that advances on every call, so distinct callers naturally
/// receive distinct ports. Tests that need deterministic output pass an
/// explicit `start_hint`.
///
/// **Argument convention**: `remote_ip` / `remote_port` / `local_ip`
/// describe the *inbound* 5-tuple the NIC sees on the SYN-ACK / first
/// response — the REMOTE end is the packet's source, our LOCAL end is
/// the destination. This matches `queue_for_tuple` / `predict_rss_queue`
/// and the underlying Toeplitz spec, which is **not symmetric in argument
/// order**: putting the local port in the src_port slot produces a
/// different hash than putting it in the dst_port slot.
///
/// Default range matches the Linux ephemeral-port window
/// `/proc/sys/net/ipv4/ip_local_port_range` (32768..60999).
///
/// @param port_id     DPDK port id; used only to snapshot the live
///                    `RssState` once via `query_rss_state` before
///                    delegating to `find_src_port_for_queue_with_state`.
/// @param target_queue The RSS queue id we want the inbound 5-tuple
///                    to hash to.
/// @param remote_ip   Peer IPv4 (host byte order) — RSS input "src" IP.
/// @param remote_port Peer L4 port (host byte order) — RSS input "src" port.
/// @param local_ip    Our IPv4 (host byte order) — RSS input "dst" IP;
///                    the searched `sp` lands in the dst_port slot.
/// @param port_range_start Lower bound (inclusive).
/// @param port_range_end   Upper bound (inclusive).
/// @param start_hint  Search start position (modulo match_count). When
///                    `nullopt` (the default), uses the process-global
///                    `detail::g_src_port_search_counter` and advances
///                    it by 1 — this is the path production callers
///                    take, ensuring distinct outputs on repeated calls.
///                    Tests pass an explicit value for determinism.
/// @return the chosen src_port (== inbound dst_port), or an error string
/// starting with "RssHashPredictExhausted" if no port in the range hashes
/// to the target queue.
[[nodiscard]] inline std::expected<uint16_t, std::string>
find_src_port_for_queue(uint16_t port_id, uint16_t target_queue,
                        uint32_t remote_ip, uint16_t remote_port,
                        uint32_t local_ip,
                        uint16_t port_range_start = 32768,
                        uint16_t port_range_end   = 60999,
                        std::optional<uint32_t> start_hint = std::nullopt) noexcept {
    if (port_range_start > port_range_end) {
        return std::unexpected(std::format(
            "find_src_port_for_queue: inverted range "
            "port_range_start={} > port_range_end={}",
            port_range_start, port_range_end));
    }
    // Snapshot the NIC's RSS state ONCE (2 syscalls). The loop below is
    // pure CPU — Toeplitz hash + RETA lookup. Pre-refactor (commit 8b10661)
    // the loop did 2 syscalls per iteration, scaling to ~56k DPDK calls
    // for the default ephemeral-port range. See PR-1 perf rationale in
    // .artifacts/review-rss-eph-net-dpdk-20260421-052500.md (Major M1).
    auto state_r = query_rss_state(port_id);
    if (!state_r) return std::unexpected(state_r.error());
    return find_src_port_for_queue_with_state(
        *state_r, target_queue, remote_ip, remote_port, local_ip,
        port_range_start, port_range_end, start_hint);
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
