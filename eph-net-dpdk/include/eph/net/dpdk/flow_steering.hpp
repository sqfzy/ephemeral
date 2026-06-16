#pragma once

/// @file flow_steering.hpp
/// NIC hardware RX dispatch — capability detection and rte_flow steering.
/// (RSS enablement lives in Platform::create / configure_port; RSS queue
///  landing is measured empirically — see docs/cpu-no-cross-core.md.)
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
///   2. The errors are inherently free-form ("rte_flow rule rejected
///      by PMD: rc=-95 (ENOTSUP)" / "flow validate failed for 5-tuple
///      ...") — the string is the diagnostic. ErrorInfo's enum +
///      const-char* doesn't reduce
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

#include "eph/core/log.hpp"

#include <rte_ethdev.h>
#include <rte_flow.h>
#include <rte_udp.h>

#include "eph/core/error.hpp"
#include "eph/dpdk/detail/icmp_directory.hpp"   // g_active_self_proc_index
#include "eph/dpdk/detail/mp_ipc.hpp"
#include "eph/dpdk/net_header.hpp"

namespace eph::net::dpdk {

namespace detail {
inline spdlog::logger* flow_logger() {
    static spdlog::logger* l = ::eph::log::get("net.dpdk.flow");
    return l;
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
        EPH_LOG_WARN(log,
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
            EPH_LOG_INFO(log,
                "Port {} supports rte_flow 5-tuple steering (FlowDirector mode)",
                port_id);
            return RxDispatchMode::FlowDirector;
        }
        EPH_LOG_DEBUG(log,
            "Port {} rte_flow validate failed: {} ({}), trying RSS",
            port_id, error.message ? error.message : "unknown",
            ret);
    }

    // 3. Check for RSS TCP hash support
    constexpr uint64_t kRssTcp =
        RTE_ETH_RSS_NONFRAG_IPV4_TCP | RTE_ETH_RSS_IPV4;
    if (dev_info.flow_type_rss_offloads & kRssTcp) {
        EPH_LOG_INFO(log,
            "Port {} supports RSS TCP hash (RssPartitioned mode), "
            "offloads=0x{:016x}",
            port_id, dev_info.flow_type_rss_offloads);
        return RxDispatchMode::RssPartitioned;
    }

    EPH_LOG_INFO(log,
        "Port {} has no RSS/FlowDirector support (Software / single-Poller mode)",
        port_id);
    return RxDispatchMode::Software;
}

// ---------------------------------------------------------------------------
// RSS configuration (Step 2)
// ---------------------------------------------------------------------------

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
                    EPH_LOG_WARN(detail::flow_logger(),
                        "rte_flow_destroy failed: port={}, ret={}, msg={}, "
                        "type={} rte_errno={} ({})",
                        port_id, ret,
                        error.message ? error.message : "unknown",
                        static_cast<int>(error.type), err, rte_strerror(err));
                } else {
                    EPH_LOG_DEBUG(detail::flow_logger(),
                        "Flow rule removed: port={}, queue={}",
                        port_id, queue_id);
                }
                h.h = nullptr;
            } else if constexpr (std::is_same_v<T, RemoteFlowHandle>) {
                if (h.handle_id == 0) return;
                auto r = detail::fd_destroy_via_ipc(h.owner_proc, h.handle_id);
                if (!r) {
                    EPH_LOG_WARN(detail::flow_logger(),
                        "Remote flow rule destroy IPC failed: owner_proc={}, "
                        "handle_id={}, err={} — primary may have died first; "
                        "rule will be cleaned up at primary teardown",
                        h.owner_proc, h.handle_id, r.error().detail);
                } else {
                    EPH_LOG_DEBUG(detail::flow_logger(),
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
        EPH_LOG_WARN(log, "{}", msg);
        return std::unexpected(msg);
    }

    EPH_LOG_INFO(log,
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
        EPH_LOG_DEBUG(flow_logger(),
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
// `Platform::primary_bringup_` (the impl_ helper invoked by
// `Platform::serve_nic` — the daemon entry, post-2026-05-02
// reshape; cleared in ~Impl).

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
                EPH_LOG_WARN(flow_logger(),
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
    /// Per-rule rte_flow_destroy failures are logged at WARN — teardown
    /// is best-effort but the operator must still get a signal when
    /// the PMD refuses (e.g. port already closed by a parallel path),
    /// otherwise NIC state diverges silently from the registry.
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
            const int rc = rte_flow_destroy(port, flow, &err);
            if (rc != 0) {
                EPH_LOG_WARN(flow_logger(),
                    "RemoteFlowRulesMap::destroy_all: rte_flow_destroy "
                    "failed during teardown (port={}, rc={}, msg={}); "
                    "NIC flow state may be stale until port close",
                    port, rc, err.message ? err.message : "unknown");
            }
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
/// Set by `Platform::primary_bringup_` (the impl_ helper invoked
/// by `Platform::serve_nic` — the daemon entry, post-2026-05-02
/// reshape) when the eph_fd_install IPC handler is registered;
/// cleared (CAS) by `~Impl`. Loaded by the static
/// `on_fd_install_thunk` / `on_fd_destroy_thunk` below.
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
        EPH_LOG_ERROR(flow_logger(),
            "fd_send_reply_: pack_msg failed for action '{}'", action_name);
        return;
    }
    const int rc = rte_mp_reply(&out, static_cast<const char*>(peer));
    if (rc != 0) {
        EPH_LOG_ERROR(flow_logger(),
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
        EPH_LOG_WARN(flow_logger(),
            "on_fd_install_thunk: rules={} parsed={} — IPC degraded; "
            "replying error",
            static_cast<const void*>(rules),
            parsed.has_value() ? "ok" : "fail");
        fd_send_reply_(kFdInstallActionName, reply, peer);
        return 0;
    }
    if (parsed->version != 1) {
        EPH_LOG_WARN(flow_logger(),
            "on_fd_install_thunk: msg version={} != 1, refusing",
            parsed->version);
        fd_send_reply_(kFdInstallActionName, reply, peer);
        return 0;
    }
    // Strict proto validation. The wire format documents proto∈{6,17}; a
    // ternary `(proto == 17) ? Udp : Tcp` would silently install a TCP
    // 5-tuple match for any non-17 byte (a corrupt / forged / older-schema
    // sender could land an SCTP / ICMP / 0 byte and we'd happily program
    // the NIC for the wrong protocol). Reject anything outside the two
    // sanctioned values up front.
    if (parsed->proto != ::eph::dpdk::net::kIpProtoTcp &&
        parsed->proto != ::eph::dpdk::net::kIpProtoUdp) {
        EPH_LOG_WARN(flow_logger(),
            "on_fd_install_thunk: rejecting unsupported proto={} "
            "(expected {} TCP or {} UDP) — possible schema drift or "
            "corrupt msg from peer",
            parsed->proto,
            static_cast<int>(::eph::dpdk::net::kIpProtoTcp),
            static_cast<int>(::eph::dpdk::net::kIpProtoUdp));
        fd_send_reply_(kFdInstallActionName, reply, peer);
        return 0;
    }

    ::eph::dpdk::net::ConnectionTuple t{};
    t.src_ip   = parsed->src_ip;
    t.dst_ip   = parsed->dst_ip;
    t.src_port = parsed->src_port;
    t.dst_port = parsed->dst_port;
    const auto proto = (parsed->proto == ::eph::dpdk::net::kIpProtoUdp)
        ? FlowProtocol::Udp
        : FlowProtocol::Tcp;

    auto rule = install_flow_rule(parsed->port_id, parsed->target_queue,
                                  t, proto);
    if (!rule) {
        EPH_LOG_WARN(flow_logger(),
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
        EPH_LOG_ERROR(flow_logger(),
            "on_fd_install_thunk: RemoteFlowRulesMap::insert returned 0 "
            "— flow leak (cleaned up at primary exit)");
        fd_send_reply_(kFdInstallActionName, reply, peer);
        return 0;
    }

    EPH_LOG_INFO(flow_logger(),
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
    EPH_LOG_DEBUG(flow_logger(),
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
    // Wire byte must use the same named constants the receiver checks
    // against (on_fd_install_thunk uses kIpProtoTcp/Udp). Magic-number
    // literals here drift silently from the receiver if anyone ever
    // remaps the named constants — same-source-of-truth on both sides.
    req.proto          = (proto == FlowProtocol::Udp)
                             ? ::eph::dpdk::net::kIpProtoUdp
                             : ::eph::dpdk::net::kIpProtoTcp;
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

    EPH_LOG_INFO(detail::flow_logger(),
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
