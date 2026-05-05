#pragma once

/// @file tcp.hpp
/// Minimal user-space TCP state machine for DPDK data plane.
///
/// Design constraints (from architecture discussion):
///   - Implements: seq/ack tracking, ACK generation, window management,
///     FIN/RST handling, keepalive
///   - Does NOT implement: retransmission, Nagle, delayed ACK, congestion
///     control, SACK, TCP timestamps
///   - Loss strategy: detect packet loss → immediate reconnect (~2ms)
///
/// The state machine handles TCP three-way handshake, data transfer, and
/// graceful close. All operations go through DPDK tx_burst/rx_burst — no
/// kernel sockets are used on the data path.
///
/// ## Error contract
///
/// All fallible methods return `std::expected<T, core::ErrorInfo>` — the
/// same type as the public Stream API. `ErrorInfo::detail` is a static
/// string literal (never formatted / allocated); formatted diagnostic
/// context (timeout values, sequence numbers, effective MSS, peer state)
/// is emitted via `SPDLOG_LOGGER_*` at the error site before the return,
/// so ops can still see the full picture in logs while programmatic
/// callers get a uniformly-typed error for dispatch.

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <sys/random.h>   // getrandom(2) for ISN generation

#include <rte_ethdev.h>
#include <rte_mbuf.h>

#include "eph/core/error.hpp"
#include "eph/core/tcp_concept.hpp"
#include "eph/dpdk/arp.hpp"
#include "eph/dpdk/net_header.hpp"
#include "eph/utils/time.hpp"

namespace eph::dpdk {

using eph::net::TcpState;
using eph::net::tcp_state_name;

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Configuration for a DPDK TcpSession.
///
/// Specifies the connection 4-tuple, Ethernet MAC addresses, TCP parameters
/// (MSS, receive window), and DPDK port/queue identifiers. Validate with
/// validate() before passing to TcpSession constructor.
///
/// @note All IP addresses and ports in the tuple are in HOST byte order.
///       MAC addresses are in network (wire) byte order as required by DPDK.
struct TcpConfig {
    net::ConnectionTuple tuple{};            ///< TCP 4-tuple (src/dst IP + port, host order)
    rte_ether_addr       src_mac{};          ///< Source (local NIC) MAC address
    rte_ether_addr       dst_mac{};          ///< Destination (gateway/peer) MAC address
    uint16_t             mss          = net::kDefaultMss;  ///< Maximum Segment Size (bytes)
    uint32_t             recv_window  = 65535; ///< Advertised receive window (no window scaling)
    uint16_t             port_id      = 0;    ///< DPDK port ID for TX/RX
    uint16_t             tx_queue_id  = 0;    ///< TX queue index on the port
    uint16_t             rx_queue_id  = 0;    ///< RX queue index on the port

    /// Maximum packets per rte_eth_rx_burst call. 0 = auto-calculate from
    /// kDefaultRxBudgetBytes / mss (22 for standard MTU, 3 for jumbo).
    /// Non-zero overrides the auto value (clamped to [1, 32]).
    uint16_t             max_rx_burst = 0;

    /// @name Keepalive
    /// @{

    /// Idle interval before a keepalive probe is emitted. Zero = disabled
    /// (default). Typical setting for idle-sensitive paths behind NAT /
    /// firewalls is ~30s; HFT data-plane usage leaves this at 0 because
    /// application-layer heartbeats (FIX, WS ping, ITCH periodic messages)
    /// already cover liveness detection.
    std::chrono::milliseconds keepalive_interval = std::chrono::milliseconds::zero();

    /// Consecutive un-answered probes before the session is declared dead
    /// and transitions to Closed. Honored only when keepalive_interval > 0.
    /// Must be in [1, 10].
    uint8_t              keepalive_probes = 3;

    /// @}

    /// Validate configuration, returning an error description or empty string on success.
    /// Call before TcpSession construction to get early, actionable error messages.
    [[nodiscard]] constexpr std::string_view validate() const noexcept {
        if (tuple.src_ip == 0)
            return "src_ip must not be zero";
        if (tuple.dst_ip == 0)
            return "dst_ip must not be zero";
        if (tuple.src_port == 0)
            return "src_port must be explicit (DPDK has no ephemeral port allocator)";
        if (tuple.dst_port == 0)
            return "dst_port must be explicit (DPDK has no ephemeral port allocator)";
        if (mss == 0)
            return "mss must be > 0";
        if (mss > net::kJumboMaxMss)
            return "mss exceeds jumbo frame limit (9000)";
        if (recv_window == 0)
            return "recv_window must be > 0";
        if (recv_window > 65535)
            return "recv_window exceeds 65535 (window scaling not implemented)";
        if (max_rx_burst > 32)
            return "max_rx_burst must be in [0, 32] (0 = auto)";
        // chrono::milliseconds is a signed int64; a caller building
        // TcpConfig directly (bypassing the public KeepaliveConfig path)
        // can plant a negative value. tick_keepalive's TSC-cycle conversion
        // (`to_cycles(double)` returns nullopt for negatives) then falls
        // back to `static_cast<uint64_t>(negative * 3.0)`, which is
        // implementation-defined for negative→unsigned (typically 0 or a
        // huge wrapped value, both wrong). Reject at the validate boundary
        // so the misuse surfaces as a clear "invalid config" string rather
        // than a probe-storm or an infinite-idle window observed weeks
        // later.
        if (keepalive_interval.count() < 0)
            return "keepalive_interval must be >= 0 (use 0 to disable)";
        if (keepalive_interval.count() > 0 &&
            (keepalive_probes == 0 || keepalive_probes > 10))
            return "keepalive_probes must be in [1, 10] when keepalive_interval > 0";
        return {};
    }

    /// Equality comparison (manual because rte_ether_addr is a C struct).
    /// All user-visible config fields (including keepalive) are compared so
    /// two configs that produce materially different session behaviour do
    /// not collapse to equal — catches accidental misuse like "did I forget
    /// to copy this field?" in snapshot / round-trip tests.
    [[nodiscard]] friend bool operator==(const TcpConfig& a,
                                         const TcpConfig& b) noexcept {
        return a.tuple        == b.tuple
            && std::memcmp(&a.src_mac, &b.src_mac, sizeof(rte_ether_addr)) == 0
            && std::memcmp(&a.dst_mac, &b.dst_mac, sizeof(rte_ether_addr)) == 0
            && a.mss          == b.mss
            && a.recv_window  == b.recv_window
            && a.port_id      == b.port_id
            && a.tx_queue_id  == b.tx_queue_id
            && a.rx_queue_id  == b.rx_queue_id
            && a.max_rx_burst == b.max_rx_burst
            && a.keepalive_interval == b.keepalive_interval
            && a.keepalive_probes   == b.keepalive_probes;
    }

    /// Check for non-fatal contradictions or likely misconfigurations.
    /// Returns a list of warning messages (empty if no issues).
    /// Unlike validate() which blocks construction, these are advisory.
    [[nodiscard]] std::vector<std::string> warnings() const {
        std::vector<std::string> w;
        // Loopback IP in src or dst suggests a misconfiguration -- DPDK
        // bypasses the kernel, so loopback traffic never reaches the NIC.
        if ((tuple.src_ip >> 24) == 127)
            w.emplace_back("src_ip is in 127.0.0.0/8 (loopback) -- "
                           "DPDK bypasses the kernel, loopback traffic "
                           "will not reach the NIC");
        if ((tuple.dst_ip >> 24) == 127)
            w.emplace_back("dst_ip is in 127.0.0.0/8 (loopback) -- "
                           "DPDK bypasses the kernel, loopback traffic "
                           "will not reach the NIC");
        // Same src and dst IP likely means self-connect.
        //
        // Skip when both are zero — that's an uninitialised config which
        // `validate()` already rejects with "src_ip must not be zero",
        // and the "self-connect" diagnostic is misleading in that case.
        // Mirrors `wire::UdpConfig::warnings`'s `src_ip != 0 &&` guard
        // (the two transports share the same misconfig taxonomy).
        if (tuple.src_ip != 0 && tuple.src_ip == tuple.dst_ip)
            w.emplace_back(std::format(
                "src_ip == dst_ip ({}) -- self-connect is unusual for DPDK",
                net::format_ipv4(tuple.src_ip).data()));
        // MSS below typical Ethernet minimum (536) may indicate a mistake
        if (mss < 536)
            w.emplace_back(std::format(
                "mss={} is below the recommended minimum (536) -- "
                "may cause excessive fragmentation", mss));
        // MSS above 1460 (standard Ethernet) requires jumbo frames
        if (mss > net::kDefaultMss && mss <= net::kJumboMaxMss)
            w.emplace_back(std::format(
                "mss={} exceeds standard Ethernet MTU (1460) -- "
                "requires jumbo frame support on NIC and switches", mss));
        // Zero MAC addresses likely mean uninitialized config
        rte_ether_addr zero_mac{};
        if (std::memcmp(&src_mac, &zero_mac, sizeof(rte_ether_addr)) == 0)
            w.emplace_back("src_mac is all zeros -- likely uninitialized");
        if (std::memcmp(&dst_mac, &zero_mac, sizeof(rte_ether_addr)) == 0)
            w.emplace_back("dst_mac is all zeros -- likely uninitialized");
        // TX and RX on the same queue may cause contention
        if (tx_queue_id == rx_queue_id && tx_queue_id > 0)
            w.emplace_back(std::format(
                "tx_queue_id == rx_queue_id == {} -- shared queue may "
                "introduce contention on multi-queue NICs", tx_queue_id));
        return w;
    }

    /// Default RX budget per poll_rx call (bytes). Used to auto-calculate
    /// max_rx_burst when max_rx_burst == 0: burst = kDefaultRxBudgetBytes / mss.
    /// 32KB provides ~2 TLS records of headroom in the Transport reassembly buffer.
    static constexpr uint32_t kDefaultRxBudgetBytes = 32768;

    /// Multi-line formatted dump for logging/debugging.
    [[nodiscard]] std::string dump() const {
        return std::format(
            "TcpConfig:\n"
            "  src: {}:{}, dst: {}:{}\n"
            "  src_mac: {}, dst_mac: {}\n"
            "  mss: {}, recv_window: {}, max_rx_burst: {}\n"
            "  port_id: {}, tx_queue: {}, rx_queue: {}",
            net::format_ipv4(tuple.src_ip).data(), tuple.src_port,
            net::format_ipv4(tuple.dst_ip).data(), tuple.dst_port,
            net::format_mac(src_mac).data(), net::format_mac(dst_mac).data(),
            mss, recv_window, max_rx_burst,
            port_id, tx_queue_id, rx_queue_id);
    }

    /// JSON-formatted config for monitoring system integration.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"src_ip\":\"{}\",\"dst_ip\":\"{}\","
            "\"src_port\":{},\"dst_port\":{},"
            "\"src_mac\":\"{}\",\"dst_mac\":\"{}\","
            "\"mss\":{},\"recv_window\":{},\"max_rx_burst\":{},"
            "\"port_id\":{},\"tx_queue_id\":{},\"rx_queue_id\":{}}}",
            net::format_ipv4(tuple.src_ip).data(),
            net::format_ipv4(tuple.dst_ip).data(),
            tuple.src_port, tuple.dst_port,
            net::format_mac(src_mac).data(), net::format_mac(dst_mac).data(),
            mss, recv_window, max_rx_burst,
            port_id, tx_queue_id, rx_queue_id);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Logger
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

inline spdlog::logger* tcp_logger() { return get_logger<LoggerName{"dpdk.tcp"}>(); }

/// Pure delayed-ACK timer expiry check (RFC 1122 §4.2.3.2 style).
///
/// Returns `true` iff a deferred ACK is owed AND the delay window has
/// elapsed — i.e., the caller should emit a bare ACK now. Returns `false`
/// in all other cases (no pending ACK, or pending but still inside the
/// delay window where the next outgoing data send can piggyback).
///
/// This is the core logic of `TcpSession::flush_pending_ack()`, factored
/// out as a free function so it can be unit-tested without constructing
/// a real TcpSession (which requires a DPDK mempool and is otherwise
/// hard to drive deterministically).
///
/// @param pending_since_tsc  TSC at which the first un-ACK'd segment
///                            arrived; 0 means "no ACK owed".
/// @param now_tsc            Current TSC reading.
/// @param delay_cycles       Delayed-ACK window in CPU cycles (e.g.,
///                            40 µs converted via TSC::to_cycles).
/// @return `true` iff `pending_since_tsc != 0 && (now_tsc - pending_since_tsc) >= delay_cycles`.
///
/// @note Subtraction is unsigned and wraparound-safe: `cntvct_el0` is
///       a 64-bit monotonic counter that won't wrap for ~500 years at
///       1 GHz, but the function works correctly even at the wrap point
///       because `(now - pending) mod 2^64` gives the right elapsed.
[[nodiscard]] inline bool ack_timer_expired(uint64_t pending_since_tsc,
                                             uint64_t now_tsc,
                                             uint64_t delay_cycles) noexcept {
    if (pending_since_tsc == 0) return false;
    return (now_tsc - pending_since_tsc) >= delay_cycles;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// TCP Session
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Minimal user-space TCP session for DPDK data plane.
///
/// Implements the TCP three-way handshake, data send/recv with sequence/ACK
/// tracking, window management, out-of-order segment reordering, FIN/RST
/// handling, and TIME_WAIT (2MSL). All I/O goes through DPDK rte_eth_tx_burst
/// and rte_eth_rx_burst — no kernel sockets are used.
///
/// Does NOT implement: retransmission, Nagle, delayed ACK, congestion
/// control, SACK, or TCP timestamps. Loss strategy: detect packet loss
/// (reorder buffer overflow) and signal the caller to reconnect (~2ms).
///
/// @tparam ReorderSlots  Number of out-of-order segment buffer slots.
///         Must be <= 255 (stored in uint8_t). Default 64 covers typical
///         NIC reordering within a burst without excessive memory usage.
///
/// @note Not thread-safe. All send/receive operations must run on a single
///       DPDK poll-mode lcore. For multi-connection setups, register each
///       `TcpSession` (or the higher-level `eph::net::dpdk::DpdkTcpStream`
///       wrapping it) with `eph::net::dpdk::DpdkPoller<>::add()` — the
///       Poller demultiplexes incoming packets to the matching session by
///       5-tuple. Across multiple RX queues, drive one Poller per
///       `(port, queue)` pair on its own lcore (see `flow_steering.hpp`
///       for RSS / FlowDirector hash steering).
template <size_t ReorderSlots = 64>
class TcpSession {
    static_assert(ReorderSlots <= 255,
                  "ReorderSlots must fit in uint8_t (reorder_count_)");
public:
    /// @brief DPDK RX burst size used by the TcpSession's own poll paths
    ///        (connect handshake retry loop, poll_rx wrapper). Matches the
    ///        32-mbuf canonical burst used everywhere else in the codebase
    ///        (DpdkPoller::kBurstSize, process_rx's internal kMaxBurst,
    ///        send_batch's kMaxBatchSize). Named so the three sites that
    ///        size `rte_mbuf* pkts[N]` stay synced when tuning bursts.
    static constexpr uint16_t kRxBurstSize = 32;


    /// @brief TCP session statistics (packets, bytes, reorder events, gap telemetry).
    ///
    /// All counters are cumulative. Use operator- to compute deltas between
    /// two snapshots for interval-based monitoring.
    struct Stats {
        uint64_t tx_packets      = 0;  ///< Total TCP segments transmitted (including ACKs, SYN, FIN)
        uint64_t rx_packets      = 0;  ///< Total TCP segments received (matching this connection)
        uint64_t rx_bursts       = 0;  ///< Non-empty rte_eth_rx_burst calls (via poll_rx only)
        uint64_t tx_bytes        = 0;  ///< Total TCP payload bytes transmitted
        uint64_t rx_bytes        = 0;  ///< Total TCP payload bytes received (delivered to callback)
        uint64_t acks_sent       = 0;  ///< Bare ACK packets sent
        uint64_t out_of_order    = 0;  ///< Out-of-order segments detected (buffered, dropped, or duplicate)
        uint64_t resets_received = 0;  ///< RST packets received from peer

        // ── MSS negotiation / PMTU / keepalive telemetry ──
        uint64_t mss_negotiations_applied  = 0;  ///< Connect attempts where peer MSS < local MSS caused a downgrade
        uint64_t icmp_frag_needed_received = 0;  ///< ICMP Type 3 Code 4 packets that triggered an effective_mss clamp
        uint64_t keepalive_probes_sent     = 0;  ///< TCP keepalive probes emitted by tick_keepalive
        /// Keepalive ticks where the probe could not be transmitted — mbuf
        /// alloc failed (mempool exhausted) or `rte_eth_tx_burst` returned 0
        /// (TX queue full / driver back-pressure / link down). Each failure
        /// still consumes a "miss slot" so a stuck NIC eventually times out
        /// the connection rather than silently looping forever (the previous
        /// behaviour). Disjoint from `keepalive_probes_sent`: a single tick
        /// increments exactly one of the two.
        uint64_t keepalive_send_failures   = 0;

        // ── Reorder / loss telemetry ──
        uint64_t reorder_hits      = 0;  ///< Segments successfully buffered & delivered via reorder buf
        uint64_t reorder_overflows = 0;  ///< Reorder buffer full events (triggered reconnect)
        uint32_t max_gap_size      = 0;  ///< Largest observed seq gap in bytes (absolute, not delta)

        // ── Drop-cause attribution (TD-5, symmetric to UDP side) ──
        /// Segments that failed L2+L3+L4 parsing (non-IPv4, truncated,
        /// bad IHL, non-TCP, bad data offset) or matched an unrelated
        /// 4-tuple that the Poller routed to us by mistake. Disjoint
        /// from `fragment_rejected` (which attributes specifically to
        /// IPv4 fragments via `is_ip_fragment` peek).
        uint64_t packets_dropped = 0;
        /// Segments rejected at `parse_ip_header` because the mbuf is
        /// an IPv4 fragment (MF=1 or non-zero offset). HFT workloads
        /// set DF + negotiate MSS so this should stay at 0 in steady
        /// state; a rise signals path-MTU misconfiguration.
        uint64_t fragment_rejected = 0;
        /// Duplicate or past-window segments — peer re-delivered bytes
        /// we already acknowledged (retransmit from peer, or a delayed
        /// segment that raced a new one). Distinct from `out_of_order`,
        /// which captures forward gaps.
        uint64_t dup_segments = 0;
        /// Numerical-anomaly counter (T3.5 from 2026-05-05 action list).
        /// Bumped whenever a session-level guard observes a saturating /
        /// non-finite / overflow path that would otherwise silently
        /// graceful-degrade (e.g. the uncalibrated TSC keepalive_interval
        /// fallback hit because TSC::ns_per_cycle has not been calibrated
        /// yet). Should stay at 0 in steady state on a calibrated host;
        /// non-zero is the canonical "we silently saved the pipeline from
        /// bad inputs" signal — operators paying attention will want to
        /// know about it. Surfaced via `DpdkTcpStream::metric(
        /// kNumericalAnomaliesDetected)` and the matching JSON / dump
        /// fields below.
        uint64_t numerical_anomalies = 0;
        /// Log2 gap size histogram: bucket[i] = count of gaps in [2^i, 2^(i+1)).
        /// bucket[0] = [1,2), bucket[1] = [2,4), ..., bucket[31] = [2^31, 2^32).
        /// Recording is O(1) via __builtin_clz / std::countl_zero.
        std::array<uint64_t, 32> gap_histogram{};

        /// Compute the log2 bucket index for a gap size (O(1), single CLZ instruction).
        static constexpr uint8_t gap_bucket(uint32_t gap) noexcept {
            if (gap == 0) return 0;
            // floor(log2(gap)) = 31 - countl_zero(gap)
            return static_cast<uint8_t>(31u - static_cast<unsigned>(std::countl_zero(gap)));
        }

        /// Multi-line formatted dump for logging/debugging.
        [[nodiscard]] std::string dump() const {
            auto s = std::format(
                "TcpSession::Stats:\n"
                "  tx_packets: {}\n"
                "  rx_packets: {}\n"
                "  rx_bursts: {}\n"
                "  tx_bytes: {}\n"
                "  rx_bytes: {}\n"
                "  acks_sent: {}\n"
                "  out_of_order: {}\n"
                "  resets_received: {}\n"
                "  reorder_hits: {}\n"
                "  reorder_overflows: {}\n"
                "  max_gap_size: {}\n"
                "  packets_dropped: {}\n"
                "  fragment_rejected: {}\n"
                "  dup_segments: {}\n"
                "  keepalive_probes_sent: {}\n"
                "  keepalive_send_failures: {}\n"
                "  numerical_anomalies: {}",
                tx_packets, rx_packets, rx_bursts, tx_bytes, rx_bytes,
                acks_sent, out_of_order, resets_received,
                reorder_hits, reorder_overflows, max_gap_size,
                packets_dropped, fragment_rejected, dup_segments,
                keepalive_probes_sent, keepalive_send_failures,
                numerical_anomalies);

            // Append non-zero gap histogram buckets
            for (size_t i = 0; i < gap_histogram.size(); ++i) {
                if (gap_histogram[i] > 0) {
                    s += std::format("\n  gap[2^{}..2^{}): {}",
                                     i, i + 1, gap_histogram[i]);
                }
            }
            return s;
        }

        /// JSON-formatted stats for monitoring system integration.
        [[nodiscard]] std::string to_json() const {
            auto s = std::format(
                "{{\"tx_packets\":{},\"rx_packets\":{},\"rx_bursts\":{},"
                "\"tx_bytes\":{},\"rx_bytes\":{},\"acks_sent\":{},"
                "\"out_of_order\":{},\"resets_received\":{},\"reorder_hits\":{},"
                "\"reorder_overflows\":{},\"max_gap_size\":{},"
                "\"packets_dropped\":{},\"fragment_rejected\":{},"
                "\"dup_segments\":{},"
                "\"keepalive_probes_sent\":{},"
                "\"keepalive_send_failures\":{},"
                "\"numerical_anomalies\":{}",
                tx_packets, rx_packets, rx_bursts, tx_bytes, rx_bytes,
                acks_sent, out_of_order, resets_received,
                reorder_hits, reorder_overflows, max_gap_size,
                packets_dropped, fragment_rejected, dup_segments,
                keepalive_probes_sent, keepalive_send_failures,
                numerical_anomalies);

            // Append non-zero gap histogram buckets as sparse array
            bool has_gap = false;
            for (size_t i = 0; i < gap_histogram.size(); ++i) {
                if (gap_histogram[i] > 0) {
                    if (!has_gap) { s += ",\"gap_histogram\":{"; has_gap = true; }
                    else { s += ","; }
                    s += std::format("\"{}\":{}", i, gap_histogram[i]);
                }
            }
            if (has_gap) s += "}";
            s += "}";
            return s;
        }

        /// Compute delta between two snapshots for interval-based monitoring.
        [[nodiscard]] friend Stats operator-(const Stats& lhs, const Stats& rhs) noexcept {
            Stats result{
                .tx_packets        = lhs.tx_packets        - rhs.tx_packets,
                .rx_packets        = lhs.rx_packets        - rhs.rx_packets,
                .rx_bursts         = lhs.rx_bursts         - rhs.rx_bursts,
                .tx_bytes          = lhs.tx_bytes          - rhs.tx_bytes,
                .rx_bytes          = lhs.rx_bytes          - rhs.rx_bytes,
                .acks_sent         = lhs.acks_sent         - rhs.acks_sent,
                .out_of_order      = lhs.out_of_order      - rhs.out_of_order,
                .resets_received   = lhs.resets_received   - rhs.resets_received,
                .mss_negotiations_applied  = lhs.mss_negotiations_applied  - rhs.mss_negotiations_applied,
                .icmp_frag_needed_received = lhs.icmp_frag_needed_received - rhs.icmp_frag_needed_received,
                .keepalive_probes_sent     = lhs.keepalive_probes_sent     - rhs.keepalive_probes_sent,
                .keepalive_send_failures   = lhs.keepalive_send_failures   - rhs.keepalive_send_failures,
                .reorder_hits      = lhs.reorder_hits      - rhs.reorder_hits,
                .reorder_overflows = lhs.reorder_overflows - rhs.reorder_overflows,
                .max_gap_size      = lhs.max_gap_size,  // Point-in-time (not diffable)
                .packets_dropped   = lhs.packets_dropped   - rhs.packets_dropped,
                .fragment_rejected = lhs.fragment_rejected - rhs.fragment_rejected,
                .dup_segments      = lhs.dup_segments      - rhs.dup_segments,
                .numerical_anomalies = lhs.numerical_anomalies - rhs.numerical_anomalies,
            };
            for (size_t i = 0; i < 32; ++i) {
                result.gap_histogram[i] = lhs.gap_histogram[i] - rhs.gap_histogram[i];
            }
            return result;
        }

        [[nodiscard]] friend bool operator==(const Stats&, const Stats&) = default;
    };

    /// Create a TCP session (does NOT connect yet).
    /// @pre pool must not be nullptr — the session allocates mbufs from it.
    explicit TcpSession(const TcpConfig& config, rte_mempool* pool) noexcept
        : config_(config)
        , pool_(pool)
        , state_(TcpState::Closed)
        , snd_nxt_(generate_isn().value_or(1))
        , snd_una_(snd_nxt_)
        , rcv_nxt_(0)
        , rcv_wnd_(static_cast<uint16_t>(config.recv_window))
        , snd_wnd_(0)
        , effective_mss_(config.mss) {

        if (!pool_) [[unlikely]] {
            SPDLOG_LOGGER_ERROR(detail::tcp_logger(),
                "TcpSession created with null mempool — "
                "all packet allocations will fail");
        }

        if (config.tuple.src_ip == 0 || config.tuple.dst_ip == 0) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::tcp_logger(),
                "TcpSession created with zero IP address: src={}, dst={}",
                net::format_ipv4(config.tuple.src_ip).data(),
                net::format_ipv4(config.tuple.dst_ip).data());
        }

        // Surface TcpConfig::warnings() (loopback IPs, zero MACs, shared
        // TX/RX queue, unusual MSS, etc.) — advisory, does not block
        // construction. Mirrors Platform::create / UdpSender::create.
        for (const auto& w : config.warnings()) {
            SPDLOG_LOGGER_WARN(detail::tcp_logger(),
                "TcpConfig advisory: {}", w);
        }

        pkt_template_.src_mac = config.src_mac;
        pkt_template_.dst_mac = config.dst_mac;
        pkt_template_.tuple   = config.tuple;
        pkt_template_.mss     = config.mss;

        SPDLOG_LOGGER_DEBUG(detail::tcp_logger(),
            "TcpSession created: {}:{} -> {}:{}, pool={}",
            net::format_ipv4(config.tuple.src_ip).data(),
            config.tuple.src_port,
            net::format_ipv4(config.tuple.dst_ip).data(),
            config.tuple.dst_port,
            static_cast<const void*>(pool_));
    }

    ~TcpSession() {
        // Best-effort RST on destruction — only for states where the
        // peer still believes the connection is live (Established,
        // SynSent, SynReceived, CloseWait). If we're already in a
        // mid-close state (FinWait*/Closing/LastAck/TimeWait) the peer
        // has seen (or sent) a FIN and is winding down; injecting a
        // RST on top of that would needlessly replace an orderly close
        // with a reset that looks like an abort in tcptrace / tshark.
        //
        // Additionally gated on `pool_ != nullptr` because:
        //   * unit-test paths construct with pool=nullptr intentionally
        //   * at program shutdown the mempool may already be freed
        // Both would turn `reset()`'s mbuf build into UB.
        if (!should_rst_on_destroy_(state_)) return;
        if (pool_ == nullptr) {
            SPDLOG_LOGGER_DEBUG(detail::tcp_logger(),
                "~TcpSession: state={} but pool is null; skipping RST",
                tcp_state_name(state_));
            return;
        }
        SPDLOG_LOGGER_DEBUG(detail::tcp_logger(),
            "~TcpSession: state={} -> best-effort RST",
            tcp_state_name(state_));
        reset();
    }

    /// @brief Per-state policy: does destroying a TcpSession in this
    ///        state justify emitting a best-effort RST to the peer?
    ///
    /// True iff the peer still considers the connection open from its
    /// side — Established (full-duplex), SynSent / SynReceived
    /// (handshake in flight), or CloseWait (we haven't sent FIN yet).
    /// False for any state where a FIN has already crossed the wire
    /// in at least one direction; a gratuitous RST there would corrupt
    /// an otherwise orderly shutdown.
    [[nodiscard]] static constexpr bool
    should_rst_on_destroy_(TcpState s) noexcept {
        switch (s) {
            case TcpState::Established:
            case TcpState::SynSent:
            case TcpState::SynReceived:
            case TcpState::CloseWait:  return true;
            case TcpState::Closed:
            case TcpState::Listen:
            case TcpState::FinWait1:
            case TcpState::FinWait2:
            case TcpState::Closing:
            case TcpState::LastAck:
            case TcpState::TimeWait:   return false;
        }
        return false;  // exhaustive switch above; silences -Wreturn-type
    }

    TcpSession(const TcpSession&)            = delete;
    TcpSession& operator=(const TcpSession&) = delete;

    TcpSession(TcpSession&& other) noexcept
        : config_(std::move(other.config_))
        , pool_(other.pool_)
        , state_(other.state_)
        , pkt_template_(std::move(other.pkt_template_))
        , snd_nxt_(other.snd_nxt_)
        , snd_una_(other.snd_una_)
        , rcv_nxt_(other.rcv_nxt_)
        , rcv_wnd_(other.rcv_wnd_)
        , snd_wnd_(other.snd_wnd_)
        , effective_mss_(other.effective_mss_)
        , peer_mss_(other.peer_mss_)
        , peer_mss_negotiated_(other.peer_mss_negotiated_)
        , reorder_count_(other.reorder_count_)
        , stats_(other.stats_)
        , ack_pending_since_tsc_(other.ack_pending_since_tsc_)
        , time_wait_deadline_(other.time_wait_deadline_)
        , last_rx_tsc_(other.last_rx_tsc_)
        , last_keepalive_tsc_(other.last_keepalive_tsc_)
        , keepalive_misses_(other.keepalive_misses_)
        , last_rx_burst_tsc_(other.last_rx_burst_tsc_.load(std::memory_order_relaxed))
{
        // reorder_count_ is initialized from other.reorder_count_ in the
        // member initializer list above, so the loop bound is correct.
        for (uint8_t i = 0; i < reorder_count_; ++i)
            reorder_buf_[i] = other.reorder_buf_[i];
        other.pool_ = nullptr;
        other.state_ = TcpState::Closed;
        other.reorder_count_ = 0;
        other.ack_pending_since_tsc_ = 0;
    }

    TcpSession& operator=(TcpSession&& other) noexcept {
        if (this != &other) {
            // Symmetry with ~TcpSession: if *this currently holds a live
            // session (peer still believes the connection is open) we
            // must emit the same best-effort RST the dtor would, before
            // overwriting our state. Without this, `live = std::move
            // (other);` silently abandons the peer — no FIN, no RST,
            // half-open connection from the peer's view until its
            // keepalive / read-timeout fires (minutes to hours). The
            // dtor's gating mirrors this exactly: same state policy,
            // same `pool_ != nullptr` precondition.
            if (should_rst_on_destroy_(state_) && pool_ != nullptr) {
                SPDLOG_LOGGER_DEBUG(detail::tcp_logger(),
                    "TcpSession::operator=(&&): state={} on target -> "
                    "best-effort RST before overwrite",
                    tcp_state_name(state_));
                reset();
            }
            config_ = std::move(other.config_);
            pool_ = other.pool_;
            state_ = other.state_;
            pkt_template_ = std::move(other.pkt_template_);
            snd_nxt_ = other.snd_nxt_;
            snd_una_ = other.snd_una_;
            rcv_nxt_ = other.rcv_nxt_;
            rcv_wnd_ = other.rcv_wnd_;
            snd_wnd_ = other.snd_wnd_;
            effective_mss_ = other.effective_mss_;
            peer_mss_ = other.peer_mss_;
            peer_mss_negotiated_ = other.peer_mss_negotiated_;
            reorder_count_ = other.reorder_count_;
            for (uint8_t i = 0; i < reorder_count_; ++i)
                reorder_buf_[i] = other.reorder_buf_[i];
            stats_ = other.stats_;
            ack_pending_since_tsc_ = other.ack_pending_since_tsc_;
            time_wait_deadline_ = other.time_wait_deadline_;
            last_rx_tsc_ = other.last_rx_tsc_;
            last_keepalive_tsc_ = other.last_keepalive_tsc_;
            keepalive_misses_ = other.keepalive_misses_;
            last_rx_burst_tsc_.store(other.last_rx_burst_tsc_.load(std::memory_order_relaxed), std::memory_order_relaxed);
            other.pool_ = nullptr;
            other.state_ = TcpState::Closed;
            other.reorder_count_ = 0;
            other.ack_pending_since_tsc_ = 0;
        }
        return *this;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Connection establishment
    // ─────────────────────────────────────────────────────────────────────────

    /// Perform TCP three-way handshake (blocking, polls DPDK rx).
    /// @param timeout  Maximum time to wait for SYN-ACK
    /// @return ErrorInfo on failure; code is ConnectFailed for peer-side
    ///         refusal / timeout, InvalidConfig for misuse, OutOfMemory
    ///         for mbuf exhaustion, BufferFull for NIC backpressure.
    [[nodiscard]] std::expected<void, core::ErrorInfo>
    connect(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
        [[maybe_unused]] auto log = detail::tcp_logger();

        if (state_ == TcpState::TimeWait) {
            // Allow reconnection once the 2MSL timer has expired (RFC 793 §3.5).
            if (std::chrono::steady_clock::now() >= time_wait_deadline_) {
                SPDLOG_LOGGER_DEBUG(log,
                    "TIME_WAIT 2MSL expired — allowing reconnect");
                state_ = TcpState::Closed;
            } else {
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "TcpSession::connect: session in TIME_WAIT (2MSL not yet expired)"});
            }
        }

        if (state_ != TcpState::Closed) {
            SPDLOG_LOGGER_WARN(log,
                "TcpSession::connect: session in state {}", tcp_state_name(state_));
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "TcpSession::connect: session not in Closed state"});
        }

        // Always regenerate ISN on each connect attempt.
        // This ensures stale snd_nxt_/snd_una_ from a timed-out handshake
        // (where state_ was reset to Closed but sequence numbers were already
        // incremented) do not bleed into the new connection.
        auto isn_result = generate_isn();
        if (!isn_result) {
            SPDLOG_LOGGER_ERROR(log,
                "TcpSession::connect: ISN generation failed — CSPRNG unavailable: {}",
                isn_result.error());
            return std::unexpected(core::ErrorInfo{
                core::Error::OutOfMemory,
                "TcpSession::connect: ISN generation failed (CSPRNG unavailable)"});
        }
        snd_nxt_ = *isn_result;
        snd_una_ = *isn_result;
        rcv_nxt_ = 0;
        reorder_count_ = 0;
        ack_pending_since_tsc_ = 0;
        // Reset any MSS / keepalive state carried over from a prior
        // connection attempt on this session. effective_mss_ restarts at
        // the configured local MSS; a fresh SYN-ACK negotiation below may
        // clamp it further.
        effective_mss_       = config_.mss;
        peer_mss_            = 0;
        peer_mss_negotiated_ = false;
        last_rx_tsc_         = 0;
        last_keepalive_tsc_  = 0;
        keepalive_misses_    = 0;

        // Send SYN
        SPDLOG_LOGGER_DEBUG(log, "Sending SYN, isn={}", snd_nxt_);
        auto* syn = pkt_template_.build_packet(
            pool_, snd_nxt_, 0, net::kTcpSyn, rcv_wnd_);
        if (!syn) {
            // Pool exhaustion at connect time — surface peer + port + queue
            // so the operator can pinpoint the offending session vs the
            // generic "mbuf alloc failed" they'd otherwise see.
            SPDLOG_LOGGER_ERROR(log,
                "TcpSession::connect: mbuf allocation failed for SYN "
                "peer={}:{} port={} queue={}",
                net::format_ipv4(config_.tuple.dst_ip).data(),
                config_.tuple.dst_port,
                config_.port_id, config_.tx_queue_id);
            return std::unexpected(core::ErrorInfo{
                core::Error::OutOfMemory,
                "TcpSession::connect: mbuf allocation failed for SYN"});
        }

        uint16_t sent = rte_eth_tx_burst(config_.port_id, config_.tx_queue_id, &syn, 1);
        if (sent != 1) {
            rte_pktmbuf_free(syn);
            // sent=0 means the TX ring was full — show the actual count
            // and the peer so the operator can correlate with TX-queue
            // back-pressure on a specific session.
            SPDLOG_LOGGER_ERROR(log,
                "TcpSession::connect: tx_burst returned sent={}/1 for SYN "
                "peer={}:{} port={} queue={}",
                sent,
                net::format_ipv4(config_.tuple.dst_ip).data(),
                config_.tuple.dst_port,
                config_.port_id, config_.tx_queue_id);
            return std::unexpected(core::ErrorInfo{
                core::Error::BufferFull,
                "TcpSession::connect: tx_burst failed for SYN"});
        }
        stats_.tx_packets++;

        state_ = TcpState::SynSent;
        snd_nxt_++; // SYN consumes one sequence number

        // Wait for SYN-ACK, retransmitting SYN every 200ms if no response.
        // Unlike full TCP RTO (RFC 6298, initial 1s), we use a shorter interval
        // because we target data center environments with <1ms RTT.
        constexpr auto kSynRetransmitInterval = std::chrono::milliseconds(200);
        auto deadline = std::chrono::steady_clock::now() + timeout;
        auto next_syn_retransmit = std::chrono::steady_clock::now() + kSynRetransmitInterval;

        while (std::chrono::steady_clock::now() < deadline) {
            // Retransmit SYN if interval elapsed without SYN-ACK
            auto now = std::chrono::steady_clock::now();
            if (now >= next_syn_retransmit) {
                auto* resyn = pkt_template_.build_packet(
                    pool_, snd_nxt_ - 1, 0, net::kTcpSyn, rcv_wnd_);
                if (resyn) {
                    uint16_t resent = rte_eth_tx_burst(
                        config_.port_id, config_.tx_queue_id, &resyn, 1);
                    if (resent == 1) {
                        stats_.tx_packets++;
                        SPDLOG_LOGGER_DEBUG(log, "SYN retransmit (seq={})",
                                            snd_nxt_ - 1);
                    } else {
                        rte_pktmbuf_free(resyn);
                    }
                }
                next_syn_retransmit = now + kSynRetransmitInterval;
            }

            rte_mbuf* pkts[kRxBurstSize];
            uint16_t nb_rx = rte_eth_rx_burst(
                config_.port_id, config_.rx_queue_id, pkts, kRxBurstSize);

            for (uint16_t i = 0; i < nb_rx; ++i) {
                auto parsed = net::parse_packet(pkts[i]);
                if (!parsed.tcp || !parsed.matches(config_.tuple)) {
                    rte_pktmbuf_free(pkts[i]);
                    continue;
                }

                stats_.rx_packets++;

                if (parsed.has_flag(net::kTcpRst)) {
                    // Peer-driven connection refusal during 3-way handshake.
                    // Surface peer + our SYN seq so the operator can match
                    // this against a wireshark capture or peer-side log.
                    SPDLOG_LOGGER_ERROR(log,
                        "TcpSession::connect: received RST during handshake "
                        "peer={}:{} our_isn={} rcvd_seq={} rcvd_ack={}",
                        net::format_ipv4(config_.tuple.dst_ip).data(),
                        config_.tuple.dst_port,
                        snd_nxt_ - 1,  // SYN consumed one seq, recover ISN
                        parsed.seq(), parsed.ack());
                    state_ = TcpState::Closed;
                    stats_.resets_received++;
                    // Free current mbuf BEFORE the tail — `free_remaining`
                    // starts at `i + 1` per its contract, so without this
                    // the RST mbuf leaks back to the mempool only on
                    // ~TcpSession (mempool cleanup), or never if the
                    // session is moved/leaked. Match the SYN-ACK success
                    // path's pattern at line 871-872.
                    rte_pktmbuf_free(pkts[i]);
                    free_remaining(pkts, i + 1, nb_rx);
                    return std::unexpected(core::ErrorInfo{
                        core::Error::ConnectFailed,
                        "TcpSession::connect: connection refused (RST)"});
                }

                // Expecting SYN+ACK
                if (parsed.has_flag(net::kTcpSyn) && parsed.has_flag(net::kTcpAck)) {
                    if (parsed.ack() != snd_nxt_) {
                        SPDLOG_LOGGER_WARN(log,
                            "SYN-ACK with wrong ack: expected={}, got={}",
                            snd_nxt_, parsed.ack());
                        rte_pktmbuf_free(pkts[i]);
                        continue;
                    }

                    rcv_nxt_ = parsed.seq() + 1; // SYN-ACK consumes one seq
                    snd_una_ = parsed.ack();
                    snd_wnd_ = parsed.window();

                    // Negotiate effective MSS from the peer's SYN-ACK options.
                    // We advertise config_.mss in our SYN; the peer may
                    // advertise something smaller (or no MSS option at all,
                    // in which case we keep the local value). Picking the
                    // minimum protects against silent black-holing on paths
                    // where a switch / vSwitch between us and the peer has
                    // a smaller MTU than either endpoint assumes.
                    auto peer_opts = net::parse_tcp_options(parsed);
                    if (peer_opts.has_mss && peer_opts.mss > 0) {
                        const uint16_t before = effective_mss_;
                        // Record peer's *raw* advertisement before any
                        // local clamping — survives later ICMP shrinks
                        // so snapshot.tcp.peer_mss reports the truth.
                        peer_mss_ = peer_opts.mss;
                        effective_mss_ =
                            std::min(effective_mss_, peer_opts.mss);
                        peer_mss_negotiated_ = true;
                        if (effective_mss_ < before) {
                            stats_.mss_negotiations_applied++;
                            SPDLOG_LOGGER_DEBUG(log,
                                "MSS negotiated down: local={} peer={} effective={}",
                                before, peer_opts.mss, effective_mss_);
                        } else {
                            SPDLOG_LOGGER_DEBUG(log,
                                "MSS negotiated (no change): local={} peer={}",
                                before, peer_opts.mss);
                        }
                    }

                    SPDLOG_LOGGER_DEBUG(log,
                        "Received SYN-ACK: peer_isn={}, window={}, effective_mss={}",
                        parsed.seq(), snd_wnd_, effective_mss_);

                    // Send ACK to complete handshake
                    rte_pktmbuf_free(pkts[i]);
                    free_remaining(pkts, i + 1, nb_rx);

                    auto ack_result = send_ack();
                    if (!ack_result) return ack_result;

                    state_ = TcpState::Established;
                    SPDLOG_LOGGER_INFO(log,
                        "TCP connection established: {}:{} -> {}:{}",
                        net::format_ipv4(config_.tuple.src_ip).data(),
                        config_.tuple.src_port,
                        net::format_ipv4(config_.tuple.dst_ip).data(),
                        config_.tuple.dst_port);
                    return {};
                }

                rte_pktmbuf_free(pkts[i]);
            }
        }

        state_ = TcpState::Closed;
        SPDLOG_LOGGER_ERROR(log,
            "TcpSession::connect: handshake timeout after {}ms",
            timeout.count());
        return std::unexpected(core::ErrorInfo{
            core::Error::Timeout,
            "TcpSession::connect: handshake timeout"});
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Data transfer
    // ─────────────────────────────────────────────────────────────────────────

    /// Send data over the established TCP connection.
    /// @param data     Payload data
    /// @param len      Payload length (must be <= MSS)
    /// @return Number of bytes sent, or error. Disconnected on non-
    ///         Established state; InvalidConfig on oversized payload;
    ///         OutOfMemory / BufferFull on NIC-side issues.
    [[nodiscard]] std::expected<size_t, core::ErrorInfo>
    send(const void* data, size_t len) {
        if (state_ != TcpState::Established) {
            SPDLOG_LOGGER_DEBUG(detail::tcp_logger(),
                "TcpSession::send: state={} (expected Established)",
                tcp_state_name(state_));
            return std::unexpected(core::ErrorInfo{
                core::Error::Disconnected,
                "TcpSession::send: session not Established"});
        }

        if (len > effective_mss_) {
            SPDLOG_LOGGER_WARN(detail::tcp_logger(),
                "TcpSession::send: payload too large ({} > effective MSS {})",
                len, effective_mss_);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "TcpSession::send: payload exceeds effective MSS"});
        }

        // HFT design: we intentionally do NOT block on a zero send window.
        // The application is expected to self-regulate send rate. However,
        // exceeding the peer's advertised window is logged at DEBUG level
        // so it surfaces in debug builds without polluting release perf.
        if (snd_wnd_ > 0 && len > snd_wnd_) {
            SPDLOG_LOGGER_DEBUG(detail::tcp_logger(),
                "send: payload {} exceeds peer window {} (snd_una={}, snd_nxt={})",
                len, snd_wnd_, snd_una_, snd_nxt_);
        }

        // Outgoing data packet carries the latest cumulative ACK number
        // (rcv_nxt_), so any deferred ACK is now satisfied. We clear the
        // pending-ACK timestamp only AFTER tx_burst succeeds — clearing
        // it eagerly would silently drop the pending-ACK obligation
        // whenever mbuf alloc / tx_burst fails, and the timer-based
        // flush_pending_ack() would never re-fire for that ACK. The
        // caller retries the send; the pending ACK then gets piggybacked
        // on a later successful segment (or flushed by the timer).
        auto* mbuf = pkt_template_.build_packet(
            pool_, snd_nxt_, rcv_nxt_,
            net::kTcpAck | net::kTcpPsh,
            rcv_wnd_, data, static_cast<uint16_t>(len));
        if (!mbuf) {
            SPDLOG_LOGGER_ERROR(detail::tcp_logger(),
                "TcpSession::send: mbuf alloc failed (len={})", len);
            return std::unexpected(core::ErrorInfo{
                core::Error::OutOfMemory,
                "TcpSession::send: mbuf allocation failed"});
        }

        uint16_t sent = rte_eth_tx_burst(
            config_.port_id, config_.tx_queue_id, &mbuf, 1);
        if (sent != 1) {
            rte_pktmbuf_free(mbuf);
            SPDLOG_LOGGER_ERROR(detail::tcp_logger(),
                "TcpSession::send: tx_burst failed (len={}, snd_nxt={})",
                len, snd_nxt_);
            return std::unexpected(core::ErrorInfo{
                core::Error::BufferFull,
                "TcpSession::send: tx_burst failed"});
        }

        // Success: piggyback satisfied any pending delayed ACK.
        ack_pending_since_tsc_ = 0;
        snd_nxt_ += static_cast<uint32_t>(len);
        stats_.tx_packets++;
        stats_.tx_bytes += len;
        return len;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // TX batch send
    // ─────────────────────────────────────────────────────────────────────────

    /// Result of a batch send operation.
    struct BatchSendResult {
        uint16_t sent = 0;       ///< Number of segments successfully transmitted
        uint16_t requested = 0;  ///< Total segments requested
    };

    /// Send multiple data segments in a single rte_eth_tx_burst call.
    ///
    /// Allocates mbufs from the mempool, fills TCP headers and payloads, then
    /// submits all packets to the NIC in one burst. On partial TX (NIC backpressure),
    /// unsent mbufs are freed and snd_nxt_ is only advanced for successfully sent
    /// segments.
    ///
    /// @param segments  Array of {data, len} pairs. Each len must be <= MSS.
    /// @param count     Number of segments (clamped to 32)
    /// @return Number of segments sent vs requested
    /// @pre state == Established; all segment lengths <= MSS
    [[nodiscard]] BatchSendResult
    send_batch(const std::pair<const void*, uint16_t>* segments, uint16_t count) {
        [[maybe_unused]] auto log = detail::tcp_logger();

        // Preserve the caller's original count for the BatchSendResult
        // `requested` field. Pre-fix the function returned the *clamped*
        // count, so a caller passing 50 segments would see {32, 32} when
        // only the first 32 were attempted — the silent loss of segments
        // 32..49 was indistinguishable from a clean full success. With
        // `requested = original_count`, `sent < requested` truthfully
        // reflects "some segments did not reach the wire", matching the
        // partial-tx_burst case below.
        const uint16_t original_count = count;

        if (state_ != TcpState::Established) {
            SPDLOG_LOGGER_WARN(log, "send_batch: not established (state={})",
                               tcp_state_name(state_));
            return {0, original_count};
        }

        static constexpr uint16_t kMaxBatchSize = 32;
        if (count > kMaxBatchSize) [[unlikely]] {
            SPDLOG_LOGGER_WARN(log,
                "send_batch: count={} clamped to kMaxBatchSize={}; "
                "{} trailing segments will be reported via "
                "(requested - sent) — caller should chunk before calling",
                count, kMaxBatchSize, count - kMaxBatchSize);
            count = kMaxBatchSize;
        }

        rte_mbuf* mbufs[kMaxBatchSize];
        uint32_t seg_lengths[kMaxBatchSize];  // track per-segment payload length
        uint16_t prepared = 0;

        // Data send carries the latest cumulative ACK; piggybacks any
        // deferred ACK. We clear ack_pending_since_tsc_ only after the
        // tx_burst actually places >= 1 segment on the wire — see
        // send()'s comment for the full rationale: eager clearing would
        // silently drop the pending-ACK obligation on full-failure paths
        // and prevent the timer-based flush from ever re-firing.

        // Step 1: allocate and fill all mbufs
        for (uint16_t i = 0; i < count; ++i) {
            if (segments[i].second > effective_mss_) {
                SPDLOG_LOGGER_WARN(log,
                    "send_batch: segment[{}] too large ({} > effective MSS {})",
                    i, segments[i].second, effective_mss_);
                break;
            }

            auto* mbuf = pkt_template_.build_packet(
                pool_, snd_nxt_ + total_payload_len_(seg_lengths, prepared),
                rcv_nxt_,
                net::kTcpAck | net::kTcpPsh,
                rcv_wnd_, segments[i].first, segments[i].second);
            if (!mbuf) {
                SPDLOG_LOGGER_ERROR(log,
                    "send_batch: mbuf alloc failed at segment {}/{}", i, count);
                break;
            }

            mbufs[prepared] = mbuf;
            seg_lengths[prepared] = segments[i].second;
            ++prepared;
        }

        if (prepared == 0) return {0, original_count};

        // Step 2: single burst send
        uint16_t sent = rte_eth_tx_burst(
            config_.port_id, config_.tx_queue_id, mbufs, prepared);

        // Advance snd_nxt_ only for successfully sent segments
        uint32_t sent_bytes = total_payload_len_(seg_lengths, sent);
        snd_nxt_ += sent_bytes;
        stats_.tx_packets += sent;
        stats_.tx_bytes += sent_bytes;

        // At least one segment landed on the wire → any deferred ACK was
        // piggybacked. If every tx_burst slot was dropped by the NIC
        // (sent==0), the pending-ACK obligation survives and the timer
        // can re-emit later.
        if (sent > 0) {
            ack_pending_since_tsc_ = 0;
        }

        // Free unsent mbufs
        if (sent < prepared) {
            SPDLOG_LOGGER_DEBUG(log,
                "send_batch: partial tx_burst {}/{} (NIC backpressure)",
                sent, prepared);
            for (uint16_t i = sent; i < prepared; ++i) {
                rte_pktmbuf_free(mbufs[i]);
            }
        }

        SPDLOG_LOGGER_TRACE(log,
            "send_batch: sent {}/{} segments, {} bytes",
            sent, original_count, sent_bytes);

        return {sent, original_count};
    }

    // ─────────────────────────────────────────────────────────────────────────
    // ARP cache refresh
    // ─────────────────────────────────────────────────────────────────────────

    /// Enable periodic ARP cache refresh (gratuitous ARP request).
    ///
    /// Sends a unicast ARP request to the gateway at the configured interval
    /// to prevent ARP cache expiry on intermediate switches and the gateway.
    /// Call this after connect() with the gateway IP used during ARP resolution.
    ///
    /// @param gateway_ip  Gateway IPv4 address (host byte order)
    /// @param interval    Refresh interval (default 60s, 0 = disable)
    void set_arp_refresh(uint32_t gateway_ip,
                         std::chrono::seconds interval = std::chrono::seconds{60}) noexcept {
        arp_refresh_gateway_ip_ = gateway_ip;
        arp_refresh_interval_ = interval;
        if (interval.count() > 0) {
            arp_next_refresh_ = std::chrono::steady_clock::now() + interval;
            SPDLOG_LOGGER_DEBUG(detail::tcp_logger(),
                "ARP refresh enabled: gateway={}, interval={}s",
                net::format_ipv4(gateway_ip).data(), interval.count());
        } else {
            arp_next_refresh_ = std::chrono::steady_clock::time_point::max();
            SPDLOG_LOGGER_DEBUG(detail::tcp_logger(), "ARP refresh disabled");
        }
    }

    /// Check if ARP refresh is due and send if needed.
    /// Call from the poll loop (e.g., after poll_rx). Non-blocking:
    /// sends a single ARP request without waiting for reply.
    void maybe_refresh_arp() noexcept {
        if (arp_refresh_interval_.count() == 0) return;

        auto now = std::chrono::steady_clock::now();
        if (now < arp_next_refresh_) return;

        [[maybe_unused]] auto log = detail::tcp_logger();

        auto* req = arp::build_arp_request(
            pool_, config_.src_mac, config_.tuple.src_ip,
            arp_refresh_gateway_ip_);
        if (!req) {
            // Pool exhaustion is the dominant cause; surface gateway +
            // port so the operator can correlate with which TX queue is
            // back-pressured. rte_errno is not reliably set by
            // rte_pktmbuf_alloc, so we omit it.
            SPDLOG_LOGGER_WARN(log,
                "ARP refresh: mbuf alloc failed gateway={} port={} queue={}",
                net::format_ipv4(arp_refresh_gateway_ip_).data(),
                config_.port_id, config_.tx_queue_id);
            // Retry next interval
            arp_next_refresh_ = now + arp_refresh_interval_;
            return;
        }

        uint16_t sent = rte_eth_tx_burst(
            config_.port_id, config_.tx_queue_id, &req, 1);
        if (sent != 1) {
            rte_pktmbuf_free(req);
            // sent=0 is valid back-pressure (TX ring full); not strictly
            // an error but the refresh did not go out, so log enough
            // context to spot a stuck queue from the noise.
            SPDLOG_LOGGER_WARN(log,
                "ARP refresh: tx_burst sent={}/1 gateway={} port={} queue={}",
                sent,
                net::format_ipv4(arp_refresh_gateway_ip_).data(),
                config_.port_id, config_.tx_queue_id);
        } else {
            SPDLOG_LOGGER_DEBUG(log, "ARP refresh sent for gateway {}",
                net::format_ipv4(arp_refresh_gateway_ip_).data());
        }

        arp_next_refresh_ = now + arp_refresh_interval_;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Data transfer (single segment)
    // ─────────────────────────────────────────────────────────────────────────

    /// Build a data packet into a pre-allocated mbuf (hot path, no alloc).
    /// Returns the mbuf ready for tx_burst, or nullptr on error.
    /// @warning snd_nxt_ is advanced immediately. Caller MUST transmit the
    ///          returned mbuf via tx_burst. If TX fails, the session sequence
    ///          numbers are inconsistent and must be reset().
    rte_mbuf* build_data_packet(rte_mbuf* mbuf,
                                const void* data, uint16_t len) noexcept {
        // Hot-path send: state_ should always be Established and len <= MSS
        // on a healthy connection. Both reject branches are user-error or
        // post-FIN paths — hint cold so the success path stays straight.
        if (state_ != TcpState::Established || len > effective_mss_) [[unlikely]] {
            return nullptr;
        }

        uint16_t written = pkt_template_.fill_packet(
            mbuf, snd_nxt_, rcv_nxt_,
            net::kTcpAck | net::kTcpPsh,
            rcv_wnd_, data, len);
        if (written == 0) [[unlikely]] return nullptr;

        snd_nxt_ += len;
        stats_.tx_packets++;
        stats_.tx_bytes += len;
        return mbuf;
    }

    /// Process received packets. Calls data_callback for each payload.
    /// Automatically sends ACKs and handles control packets (FIN, RST).
    /// @param pkts     Array of received mbufs (will be freed)
    /// @param nb_pkts  Number of packets
    /// @param data_callback  Called with (payload_ptr, payload_len) for each data packet
    /// @return Number of data packets processed, or error
    template <typename F>
        requires std::invocable<F, const uint8_t*, uint16_t>
    [[nodiscard]] std::expected<uint16_t, core::ErrorInfo>
    process_rx(rte_mbuf** pkts, uint16_t nb_pkts, F&& data_callback) {
        [[maybe_unused]] auto log = detail::tcp_logger();
        uint16_t data_count = 0;
        bool need_ack = false;

        // Collect mbufs for batched free at loop end.
        // Per-packet rte_pktmbuf_free inside the loop costs ~20-40ns each
        // and falls within the latency-measured path.
        //
        // Safety: free_list is sized to kMaxBurst, and nb_pkts is bounded
        // by it via a compile-time link to kRxBurstSize. The only caller —
        // poll_rx — declares `rte_mbuf* pkts[kRxBurstSize]` and asks DPDK
        // for at most kRxBurstSize packets, so `nb_pkts ≤ kMaxBurst` is
        // enforced by construction. The static_assert below makes the
        // coupling explicit; if a future caller raises kRxBurstSize the
        // compile fails until kMaxBurst is raised in lockstep.
        //
        // Note: we previously kept a runtime tail-free loop here as a
        // belt-and-suspenders for hypothetical future callers passing a
        // larger pkts[]. GCC 14 -Warray-bounds correctly flagged that path
        // as OOB given the current caller's pkts[kRxBurstSize] array, and
        // inflicted the warning on every TU that included this header
        // (examples + tests). Removing the dead loop in favour of the
        // static_assert eliminates the warning without weakening safety —
        // any new caller that wants to pass a larger array would have to
        // raise kMaxBurst, which the static_assert forces.
        static constexpr uint16_t kMaxBurst = 32;
        static_assert(kMaxBurst == kRxBurstSize,
                      "kMaxBurst must match kRxBurstSize so process_rx "
                      "never reads past the caller's pkts[] stack array");
        rte_mbuf* free_list[kMaxBurst];
        uint16_t free_count = 0;

        for (uint16_t i = 0; i < nb_pkts; ++i) {
            auto parsed = net::parse_packet(pkts[i]);

            // Skip non-matching packets. Attribute the drop cause so
            // operators can distinguish "upstream routing wrong" /
            // "malformed" / "fragment" — symmetric to DpdkUdpSocket's
            // process_burst_ drop-cause accounting. Disjoint from the
            // bad-checksum drop that runs before us in
            // DpdkTcpStream::process_burst_ (see TD-3 fix).
            if (!parsed.tcp || !parsed.matches(config_.tuple)) {
                if (!parsed.tcp && net::is_ip_fragment(pkts[i])) {
                    stats_.fragment_rejected++;
                } else {
                    stats_.packets_dropped++;
                }
                free_list[free_count++] = pkts[i];
                continue;
            }

            stats_.rx_packets++;
            // Any matching segment counts as liveness evidence. Refresh
            // the keepalive baseline to the most recent burst's TSC (set
            // by poll_rx() before calling us, or via set_last_rx_burst_tsc
            // on the DpdkPoller path: tcp_stream.hpp's process_burst_
            // calls set_last_rx_burst_tsc before process_rx so the same
            // single-thread invariant holds — RxDispatcher predecessor
            // was retired in the RSS / multi-queue rework). Clearing
            // keepalive_misses_ also resets the "N probes unanswered →
            // Closed" counter, so a single live reply undoes prior
            // uncertainty.
            //
            // Memory order: relaxed is correct here because every store of
            // last_rx_burst_tsc_ that this load can possibly observe happens
            // earlier in program order on the SAME thread (poll_rx publishes
            // it via release-store before invoking process_rx; the Poller
            // path calls set_last_rx_burst_tsc in tcp_stream.hpp's
            // process_burst_ which is also followed by sess_.process_rx
            // on the same lcore).
            // Cross-thread observers go through the public last_rx_burst_tsc()
            // getter, which uses memory_order_acquire to pair with the
            // release stores. Audit-20260413 #16 noted the relaxed/acquire
            // mix as a stylistic concern; the asymmetry is load-bearing.
            last_rx_tsc_ = last_rx_burst_tsc_.load(std::memory_order_relaxed);
            keepalive_misses_ = 0;

            // RST — immediate close
            if (parsed.has_flag(net::kTcpRst)) {
                // RFC 5961 §3.2: validate RST sequence is within receive window
                // to prevent off-path RST injection attacks.
                uint32_t rst_seq = parsed.seq();
                bool in_window = (rst_seq == rcv_nxt_) ||
                                 (seq_after(rst_seq, rcv_nxt_) &&
                                  !seq_after(rst_seq, static_cast<uint32_t>(rcv_nxt_ + rcv_wnd_)));
                if (!in_window) {
                    SPDLOG_LOGGER_DEBUG(log,
                        "RST seq {} outside receive window [{}, +{}), ignored (RFC 5961)",
                        rst_seq, rcv_nxt_, rcv_wnd_);
                    free_list[free_count++] = pkts[i];
                    continue;
                }
                SPDLOG_LOGGER_WARN(log,
                    "Received RST, closing connection: {}:{} -> {}:{}",
                    net::format_ipv4(config_.tuple.src_ip).data(),
                    config_.tuple.src_port,
                    net::format_ipv4(config_.tuple.dst_ip).data(),
                    config_.tuple.dst_port);
                state_ = TcpState::Closed;
                stats_.resets_received++;
                abort_rx_cleanup(pkts, i, nb_pkts, free_list, free_count);
                return std::unexpected(core::ErrorInfo{
                    core::Error::Disconnected,
                    "TcpSession::process_rx: connection reset by peer"});
            }

            // Update send window from peer's advertisements
            if (parsed.has_flag(net::kTcpAck)) {
                uint32_t peer_ack = parsed.ack();
                // Advance SND.UNA if the ACK is valid
                if (seq_after(peer_ack, snd_una_) &&
                    !seq_after(peer_ack, snd_nxt_)) {
                    snd_una_ = peer_ack;
                }
                snd_wnd_ = parsed.window();

                // RFC 793 §3.5: ACK of our FIN (snd_una_ catches up to snd_nxt_)
                // drives state transitions depending on current state.
                if (snd_una_ == snd_nxt_) {
                    if (state_ == TcpState::FinWait1) {
                        SPDLOG_LOGGER_DEBUG(log, "FIN_WAIT_1: received ACK of FIN -> FIN_WAIT_2");
                        state_ = TcpState::FinWait2;
                    } else if (state_ == TcpState::Closing) {
                        SPDLOG_LOGGER_DEBUG(log, "CLOSING: received ACK of FIN -> TIME_WAIT");
                        enter_time_wait();
                    } else if (state_ == TcpState::LastAck) {
                        SPDLOG_LOGGER_DEBUG(log, "LAST_ACK: received ACK of FIN -> CLOSED");
                        state_ = TcpState::Closed;
                    }
                }
            }

            // Check sequence number ordering — deliver data in all states
            // where the receive path is still active. CloseWait is included
            // because the peer has sent FIN but we haven't, so reordered
            // data segments from before the FIN may still arrive.
            if (state_ == TcpState::Established ||
                state_ == TcpState::FinWait1 ||
                state_ == TcpState::FinWait2 ||
                state_ == TcpState::CloseWait ||
                state_ == TcpState::Closing) {

                uint32_t seg_seq = parsed.seq();

                // Handle out-of-order: buffer the segment for later delivery.
                // af_packet commonly delivers segments out of order within a burst.
                if (seg_seq != rcv_nxt_ && parsed.payload_len > 0) {
                    stats_.out_of_order++;

                    // Record gap telemetry (forward gaps only)
                    if (seq_after(seg_seq, rcv_nxt_)) {
                        uint32_t gap = seg_seq - rcv_nxt_;
                        if (gap > stats_.max_gap_size) stats_.max_gap_size = gap;
                        stats_.gap_histogram[Stats::gap_bucket(gap)]++;
                    }

                    if (parsed.payload_len > net::kDefaultMss) {
                        // Segment exceeds ReorderEntry buffer capacity.
                        // Jumbo-frame out-of-order segments cannot be buffered —
                        // drop and let the caller handle reconnect via loss detection.
                        SPDLOG_LOGGER_WARN(log,
                            "Reorder buffer: segment too large ({} > {}), "
                            "delivering in-order only",
                            parsed.payload_len, net::kDefaultMss);
                        free_list[free_count++] = pkts[i];
                        continue;
                    }

                    if (seq_after(seg_seq, rcv_nxt_) &&
                        reorder_count_ < ReorderSlots &&
                        parsed.payload_len <= net::kDefaultMss &&
                        parsed.payload != nullptr) {
                        // Future segment — buffer it
                        stats_.reorder_hits++;
                        auto& entry = reorder_buf_[reorder_count_++];
                        entry.seq = seg_seq;
                        entry.len = parsed.payload_len;
                        std::memcpy(entry.data, parsed.payload, parsed.payload_len);
                        SPDLOG_LOGGER_DEBUG(log,
                            "Buffered out-of-order: expected={}, got={}, buffered={}",
                            rcv_nxt_, seg_seq, reorder_count_);
                    } else if (!seq_after(seg_seq, rcv_nxt_)) {
                        // Duplicate/past segment — peer re-delivered bytes
                        // we already ACKed. Counted separately from
                        // `out_of_order` (forward gap) and from the
                        // reorder-buffer-full genuine-loss branch below.
                        stats_.dup_segments++;
                        SPDLOG_LOGGER_DEBUG(log,
                            "Dropping duplicate: expected={}, got={}", rcv_nxt_, seg_seq);
                    } else {
                        // Reorder buffer full — genuine loss
                        stats_.reorder_overflows++;
                        SPDLOG_LOGGER_WARN(log,
                            "TcpSession::process_rx: reorder buffer full "
                            "({} slots): expected={}, got={}",
                            ReorderSlots, rcv_nxt_, seg_seq);
                        abort_rx_cleanup(pkts, i, nb_pkts, free_list, free_count);
                        return std::unexpected(core::ErrorInfo{
                            core::Error::Disconnected,
                            "TcpSession::process_rx: packet loss detected (reorder buffer full)"});
                    }
                    free_list[free_count++] = pkts[i];
                    continue;
                }

                // Process in-order data payload
                if (parsed.payload_len > 0 && parsed.payload != nullptr) {
                    std::invoke(std::forward<F>(data_callback),
                                parsed.payload, parsed.payload_len);
                    rcv_nxt_ += parsed.payload_len;
                    stats_.rx_bytes += parsed.payload_len;
                    data_count++;
                    need_ack = true;

                    // Drain any buffered segments that are now in-order
                    data_count += drain_reorder_buf(
                        std::forward<F>(data_callback));
                }
            }

            // FIN handling — RFC 793 §3.5: a FIN-bearing segment may also
            // carry payload data. The FIN consumes one sequence number AT
            // `seg_seq + payload_len`, not at `seg_seq`. The in-order delivery
            // block above has already advanced rcv_nxt_ by payload_len when
            // the segment was in-order; the FIN is in-order iff it now sits
            // at exactly the new rcv_nxt_. Equivalently:
            //   parsed.seq() + parsed.payload_len == rcv_nxt_
            // An out-of-order FIN-only segment (payload_len==0) bypasses the
            // reorder buffer (which gates on payload_len > 0) and lands here
            // with parsed.seq() != rcv_nxt_ — drop it and let the peer
            // retransmit once the gap is filled. The previous comparison
            // `parsed.seq() == rcv_nxt_` was correct only for FIN-only
            // segments; a normal graceful close that piggy-backs FIN onto
            // the last data segment was silently dropped (regression covered
            // by FaultTolerance.FinWithData* tests).
            if (parsed.has_flag(net::kTcpFin)) {
                const uint32_t fin_seq =
                    parsed.seq() + parsed.payload_len;
                if (fin_seq == rcv_nxt_) {
                    // Only advance rcv_nxt_ and ACK for states where a FIN
                    // is expected. Incrementing the sequence counter in an
                    // unexpected state (e.g. CloseWait where we already
                    // consumed the peer's FIN) would desync the connection.
                    switch (state_) {
                        case TcpState::Established:
                            rcv_nxt_++;
                            need_ack = true;
                            SPDLOG_LOGGER_DEBUG(log, "Received FIN in ESTABLISHED");
                            state_ = TcpState::CloseWait;
                            break;
                        case TcpState::FinWait1:
                            rcv_nxt_++;
                            need_ack = true;
                            // RFC 793 §3.5: in FIN_WAIT_1, a peer FIN promotes us to TIME_WAIT
                            // ONLY if the same packet also ACKs our outgoing FIN; otherwise
                            // it is simultaneous close → CLOSING.
                            //
                            // We cannot key off `parsed.has_flag(kTcpAck)` because every TCP
                            // segment after SYN carries the ACK flag, regardless of whether
                            // the ack number actually advances past our FIN's sequence. The
                            // load-bearing condition is whether the ACK block above advanced
                            // `snd_una_` to `snd_nxt_` (i.e. our FIN's sequence is now ACK'd).
                            // If the peer ACK'd only earlier data, snd_una_ < snd_nxt_ and we
                            // remain in FIN_WAIT_1 logically — the FIN we just received is the
                            // simultaneous-close path → CLOSING. We move to TIME_WAIT later
                            // when the peer eventually ACKs our FIN while in CLOSING.
                            if (snd_una_ == snd_nxt_) {
                                SPDLOG_LOGGER_DEBUG(log, "Received FIN+ACK-of-FIN in FIN_WAIT_1 -> TIME_WAIT");
                                enter_time_wait();
                            } else {
                                SPDLOG_LOGGER_DEBUG(log,
                                    "Simultaneous close: FIN_WAIT_1 -> CLOSING (peer FIN does not ACK our FIN: snd_una={}, snd_nxt={})",
                                    snd_una_, snd_nxt_);
                                state_ = TcpState::Closing;
                            }
                            break;
                        case TcpState::Closing:
                            rcv_nxt_++;
                            need_ack = true;
                            // ACK of our FIN arrives while in CLOSING — RFC 793: CLOSING → TIME_WAIT.
                            // (The ACK flag is checked via the snd_una_ update above; reaching here
                            //  means the FIN sequence was in-order, which completes the exchange.)
                            SPDLOG_LOGGER_DEBUG(log, "Received ACK in CLOSING -> TIME_WAIT");
                            enter_time_wait();
                            break;
                        case TcpState::FinWait2:
                            rcv_nxt_++;
                            need_ack = true;
                            SPDLOG_LOGGER_DEBUG(log, "Received FIN in FIN_WAIT_2 -> TIME_WAIT");
                            enter_time_wait();
                            break;
                        default:
                            // FIN in unexpected state (e.g. CloseWait, LastAck,
                            // TimeWait) — likely a retransmit of an already-consumed
                            // FIN. Do NOT advance rcv_nxt_; just log and drop.
                            SPDLOG_LOGGER_WARN(log,
                                "Ignoring FIN in unexpected state {}: seq={}",
                                tcp_state_name(state_), parsed.seq());
                            break;
                    }
                } else {
                    SPDLOG_LOGGER_DEBUG(log,
                        "Out-of-order FIN dropped: fin_seq={}, "
                        "seg_seq={}, payload_len={}, rcv_nxt_={}",
                        fin_seq, parsed.seq(),
                        parsed.payload_len, rcv_nxt_);
                }
            }

            free_list[free_count++] = pkts[i];
        }

        // Batch-free all mbufs at once — avoids per-packet mempool
        // accounting overhead on the latency-critical path.
        if (free_count > 0) {
            rte_pktmbuf_free_bulk(free_list, free_count);
        }

        // Defer ACK emission. Records the time of the *first* burst that
        // left data un-ACK'd; subsequent bursts within the timer window
        // keep the original timestamp so the delayed-ACK timer measures
        // from the earliest pending burst, not the latest. flush_pending_ack
        // will either emit a bare ACK on timer expiry or piggyback via the
        // next outgoing data send.
        if (need_ack && ack_pending_since_tsc_ == 0) {
            ack_pending_since_tsc_ = eph::utils::TSC::now();
        }

        return data_count;
    }

    /// Apply ICMP Frag Needed (Type 3 Code 4) feedback from the network.
    /// Clamps effective_mss_ to `next_hop_mtu - 40` (IPv4 header + base
    /// TCP header) so subsequent segments fit inside the smallest
    /// observed path MTU. Never grows effective_mss_ beyond the current
    /// value — PMTU shrinks are monotonic until the next connect().
    ///
    /// Bumps `stats_.icmp_frag_needed_received` regardless of whether
    /// the clamp actually shrank the MSS, so operators can tell
    /// "we are receiving ICMP feedback but it's redundant" from
    /// "we never see ICMP at all".
    void on_icmp_frag_needed(uint16_t next_hop_mtu) noexcept {
        ++stats_.icmp_frag_needed_received;
        // RFC 1191 allows an MTU of zero to mean "the router does not
        // know" (ICMP Type 3 Code 4 legacy format). Ignore zero and any
        // MTU that can't accommodate even an empty TCP segment.
        constexpr uint16_t kMinHeaders = net::kIpv4HeaderLen + net::kTcpHeaderLen;
        if (next_hop_mtu <= kMinHeaders) {
            SPDLOG_LOGGER_DEBUG(detail::tcp_logger(),
                "ICMP Frag Needed: next_hop_mtu={} too small to derive MSS",
                next_hop_mtu);
            return;
        }
        const uint16_t new_mss =
            static_cast<uint16_t>(next_hop_mtu - kMinHeaders);
        const uint16_t before = effective_mss_;
        effective_mss_ = std::min(effective_mss_, new_mss);
        if (effective_mss_ < before) {
            SPDLOG_LOGGER_INFO(detail::tcp_logger(),
                "ICMP Frag Needed: next_hop_mtu={} effective_mss {} -> {}",
                next_hop_mtu, before, effective_mss_);
        } else {
            SPDLOG_LOGGER_DEBUG(detail::tcp_logger(),
                "ICMP Frag Needed: next_hop_mtu={} (current effective_mss "
                "{} already <= derived {}); no-op",
                next_hop_mtu, effective_mss_, new_mss);
        }
    }

    /// Periodic keepalive driver. No-op when disabled
    /// (`config_.keepalive_interval == 0`) or when the session is not
    /// Established. Intended to be called from the stream's poll loop
    /// — e.g. `DpdkTcpStream::poll_once_`, or a per-entry tick inside
    /// `DpdkPoller`.
    ///
    /// Semantics:
    ///   * If no RX has been observed yet (last_rx_tsc_ == 0), anchor
    ///     the baseline to `now_tsc` — fresh connections don't probe
    ///     immediately.
    ///   * If (now - last_rx_tsc_) < interval → no-op.
    ///   * Else, if we've already emitted a probe inside this interval
    ///     window, do not probe again (rate-limited to one per window).
    ///   * Else, if we've already burned through `keepalive_probes`
    ///     unanswered probes (whether the probes actually went on the
    ///     wire or not) → declare the connection dead (state_ = Closed).
    ///   * Else → attempt a probe and consume one miss slot:
    ///     `keepalive_misses_` advances unconditionally, while exactly
    ///     one of `keepalive_probes_sent` (probe placed on wire) or
    ///     `keepalive_send_failures` (mbuf alloc / tx_burst failed)
    ///     advances. Counting send-failures into the miss slot is
    ///     deliberate: a NIC stuck on TX is just as terminal as a peer
    ///     that has gone silent — both are unrecoverable in place, and
    ///     declaring the connection dead at the same threshold lets
    ///     the application reconnect rather than spin forever.
    ///
    /// An incoming packet (any TCP segment matching our 4-tuple)
    /// resets `keepalive_misses_` back to 0 via process_rx().
    void tick_keepalive(uint64_t now_tsc) noexcept {
        if (config_.keepalive_interval.count() == 0) return;
        if (state_ != TcpState::Established) return;
        if (last_rx_tsc_ == 0) {
            last_rx_tsc_ = now_tsc;
            return;
        }
        const uint64_t interval_cycles = keepalive_interval_cycles_();
        if (now_tsc - last_rx_tsc_ < interval_cycles) return;
        // Rate-limit probes to at most one per window.
        if (last_keepalive_tsc_ != 0 &&
            now_tsc - last_keepalive_tsc_ < interval_cycles) return;

        if (keepalive_misses_ >= config_.keepalive_probes) {
            SPDLOG_LOGGER_WARN(detail::tcp_logger(),
                "keepalive: {} consecutive probes unanswered — "
                "declaring connection dead ({}:{} -> {}:{})",
                keepalive_misses_,
                net::format_ipv4(config_.tuple.src_ip).data(),
                config_.tuple.src_port,
                net::format_ipv4(config_.tuple.dst_ip).data(),
                config_.tuple.dst_port);
            state_ = TcpState::Closed;
            return;
        }

        auto r = send_keepalive_probe_();
        last_keepalive_tsc_ = now_tsc;
        // Whether the probe was actually placed on the wire or not, the
        // tick consumed a "miss slot": treating a TX failure as "no probe
        // sent, keep trying" caused the connection to silently loop
        // forever when the NIC was stuck (mempool exhausted, TX ring full,
        // link bounced, …). The cumulative count is the same dead-peer
        // declaration trigger as a peer that goes silent — both are
        // observability problems we cannot recover from in-place.
        ++keepalive_misses_;
        if (r) {
            ++stats_.keepalive_probes_sent;
            SPDLOG_LOGGER_TRACE(detail::tcp_logger(),
                "keepalive probe #{} sent", keepalive_misses_);
        } else {
            ++stats_.keepalive_send_failures;
            SPDLOG_LOGGER_WARN(detail::tcp_logger(),
                "keepalive probe #{} send failed: {} ({}:{} -> {}:{}); "
                "miss-slot consumed (probes={}/{})",
                keepalive_misses_, r.error().detail,
                net::format_ipv4(config_.tuple.src_ip).data(),
                config_.tuple.src_port,
                net::format_ipv4(config_.tuple.dst_ip).data(),
                config_.tuple.dst_port,
                keepalive_misses_, config_.keepalive_probes);
        }
    }

    /// Delayed-ACK driver: emit a bare ACK only if the timer has expired.
    ///
    /// Called by the Transport layer after every process_rx() / poll_rx()
    /// cycle. The implementation is a linux-style delayed-ACK:
    ///   - If no ACK is owed (`ack_pending_since_tsc_ == 0`) → no-op.
    ///   - If an ACK is owed but the timer has NOT expired → no-op;
    ///     the ACK will piggyback on the next outgoing data send, which
    ///     is what we want in request/response flows.
    ///   - If an ACK is owed AND the timer HAS expired → emit a bare ACK
    ///     now. This handles RX-only flows (e.g., market data) where no
    ///     outgoing data would ever arrive to piggyback with.
    ///
    /// The timer window is `ack_delay_cycles_()` (40 µs @datacenter). In
    /// request/response flows with ≤20 µs RTT, the timer never fires and
    /// every ACK piggybacks — this is the Linux TCP receiver-side fast
    /// path precondition (bare ACK packets hit `tcp_data_queue` slow
    /// path and add ~1-3 µs of server-side processing per packet).
    void flush_pending_ack() noexcept {
        const uint64_t now = eph::utils::TSC::now();
        if (!detail::ack_timer_expired(ack_pending_since_tsc_, now,
                                        ack_delay_cycles_())) {
            return;  // no-op: no pending ACK, or timer not yet expired
        }
        [[maybe_unused]] const uint64_t elapsed = now - ack_pending_since_tsc_;
        // TRACE: useful for diagnosing RX-only flows where the timer
        // (rather than piggyback) is what's keeping the connection alive.
        // Compiled out at most levels via SPDLOG_ACTIVE_LEVEL.
        SPDLOG_LOGGER_TRACE(detail::tcp_logger(),
            "delayed ACK fired after {}ns (cycles={})",
            eph::utils::TSC::to_ns(elapsed).value_or(0.0), elapsed);
        auto r = send_ack();
        if (!r) {
            // Keep `ack_pending_since_tsc_` intact so the next poll
            // cycle retries. Clearing eagerly (old behaviour) silently
            // lost the pending-ACK obligation on tx_burst failure,
            // stalling RX-only flows behind the peer's nagle / delayed
            // ACK of our prior data. Aligns with send() / send_batch()
            // post-fix.
            SPDLOG_LOGGER_WARN(detail::tcp_logger(),
                "Failed to send delayed ACK: {} — keeping pending state "
                "for retry on next poll", r.error().detail);
            return;
        }
        ack_pending_since_tsc_ = 0;
    }

    /// Poll DPDK rx and process received packets through the TCP state machine.
    /// This is the TcpTransport concept-compatible interface that encapsulates
    /// rte_eth_rx_burst + process_rx into a single call.
    ///
    /// For multi-session sharing a single NIC RX queue, register each
    /// session with `eph::net::dpdk::DpdkPoller<>` and call its `poll()`
    /// loop instead — the Poller's burst-then-demultiplex hot path
    /// dispatches directly via process_rx with zero ring overhead.
    ///
    /// @return On success: count of data packets processed (may be 0 if no
    ///         data packets in this burst, e.g. pure ACKs). On error: returns
    ///         unexpected with error message; all received packets are freed.
    template <typename F>
        requires std::invocable<F, const uint8_t*, uint16_t>
    [[nodiscard]] std::expected<uint16_t, core::ErrorInfo> poll_rx(F&& data_callback) {
        rte_mbuf* pkts[kRxBurstSize];

        // Limit burst size to prevent upstream reassembly buffer overflow.
        // Auto-calculate from MSS when max_rx_burst == 0. Defensive against
        // a contract violation: TcpConfig::validate() rejects `mss == 0`,
        // but the constructor only logs warnings — a caller that bypasses
        // validate() could still construct a `mss == 0` session and hit
        // signed integer division-by-zero UB here. Fall back to the full
        // burst budget when mss is zero so the session at least keeps
        // running long enough for the operator to notice via the warning
        // logs rather than triggering UB on the very first poll.
        const uint16_t burst_limit = config_.max_rx_burst > 0
            ? config_.max_rx_burst
            : (config_.mss > 0
                ? std::min(kRxBurstSize,
                           static_cast<uint16_t>(TcpConfig::kDefaultRxBudgetBytes / config_.mss))
                : kRxBurstSize);

        uint16_t nb_rx = rte_eth_rx_burst(
            config_.port_id, config_.rx_queue_id, pkts, burst_limit);

        if (nb_rx == 0) {
            // Even on an empty burst, give the delayed-ACK timer a chance
            // to fire — idle RX-only connections rely on repeated poll_rx
            // calls to flush a pending ACK after the 40 µs window.
            flush_pending_ack();
            return uint16_t{0};
        }
        ++stats_.rx_bursts;
        // Capture TSC right after rx_burst — matches SocketTransport's
        // post-recvmsg() timing for fair cross-backend latency comparison.
        last_rx_burst_tsc_.store(eph::utils::TSC::now(), std::memory_order_release);
        return process_rx(pkts, nb_rx, std::forward<F>(data_callback));
    }

    /// Get the connection 4-tuple (src/dst IP + port).
    [[nodiscard]] const net::ConnectionTuple& connection_tuple() const noexcept {
        return config_.tuple;
    }

    /// Get TCP-level statistics (packets, bursts, bytes, etc.).
    [[nodiscard]] const Stats& tcp_stats() const noexcept { return stats_; }

    /// Set the RX burst TSC externally (used by `DpdkPoller<>` to propagate
    /// the NIC arrival timestamp to the session without going through poll_rx).
    void set_last_rx_burst_tsc(uint64_t tsc) noexcept {
        last_rx_burst_tsc_.store(tsc, std::memory_order_release);
    }

    /// TSC captured right after rte_eth_rx_burst returned data.
    /// Transport uses this as the RX arrival baseline instead of
    /// timestamping after poll_rx returns, which would miss the
    /// TCP parsing + reorder + memcpy cost inside process_rx.
    [[nodiscard]] uint64_t last_rx_burst_tsc() const noexcept {
        return last_rx_burst_tsc_.load(std::memory_order_acquire);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Connection close
    // ─────────────────────────────────────────────────────────────────────────

    /// Initiate graceful TCP close (send FIN).
    [[nodiscard]] std::expected<void, core::ErrorInfo> close() {
        [[maybe_unused]] auto log = detail::tcp_logger();

        if (state_ != TcpState::Established &&
            state_ != TcpState::CloseWait) {
            SPDLOG_LOGGER_DEBUG(log,
                "TcpSession::close: state={} (need Established or CloseWait)",
                tcp_state_name(state_));
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "TcpSession::close: state not Established or CloseWait"});
        }

        SPDLOG_LOGGER_DEBUG(log, "Sending FIN");
        auto* fin = pkt_template_.build_packet(
            pool_, snd_nxt_, rcv_nxt_,
            net::kTcpFin | net::kTcpAck, rcv_wnd_);
        if (!fin) {
            SPDLOG_LOGGER_ERROR(log,
                "TcpSession::close: mbuf allocation failed for FIN");
            return std::unexpected(core::ErrorInfo{
                core::Error::OutOfMemory,
                "TcpSession::close: mbuf allocation failed for FIN"});
        }

        uint16_t sent = rte_eth_tx_burst(
            config_.port_id, config_.tx_queue_id, &fin, 1);
        if (sent != 1) {
            rte_pktmbuf_free(fin);
            SPDLOG_LOGGER_ERROR(log,
                "TcpSession::close: tx_burst failed for FIN");
            return std::unexpected(core::ErrorInfo{
                core::Error::BufferFull,
                "TcpSession::close: tx_burst failed for FIN"});
        }

        snd_nxt_++; // FIN consumes one sequence number
        stats_.tx_packets++;

        if (state_ == TcpState::Established) {
            state_ = TcpState::FinWait1;
        } else { // CloseWait
            state_ = TcpState::LastAck;
        }

        SPDLOG_LOGGER_DEBUG(log, "FIN sent, state -> {}",
                            tcp_state_name(state_));
        return {};
    }

    /// Force-close the connection (send RST).
    void reset() noexcept {
        [[maybe_unused]] auto log = detail::tcp_logger();
        SPDLOG_LOGGER_DEBUG(log, "Sending RST");

        auto* rst = pkt_template_.build_packet(
            pool_, snd_nxt_, rcv_nxt_, net::kTcpRst | net::kTcpAck, 0);
        if (rst) {
            uint16_t sent = rte_eth_tx_burst(config_.port_id, config_.tx_queue_id, &rst, 1);
            if (sent != 1) {
                rte_pktmbuf_free(rst);
            } else {
                // Keep tx_packets consistent with connect() / close() / send() —
                // every successfully-burst segment counts, including RSTs.
                // Without this, operators see reset-heavy workloads under-
                // report TX throughput in telemetry.
                stats_.tx_packets++;
            }
        }

        state_ = TcpState::Closed;
        reorder_count_ = 0;  // Prevent stale data delivery on reconnect
        ack_pending_since_tsc_ = 0;
        // Clear peer-specific window state so a stale value from the
        // broken connection cannot leak into any inspection/telemetry
        // between reset() and the next SYN-ACK. The normal send() path is
        // already gated on is_established(), so this is defense in depth.
        snd_wnd_ = 0;
        rcv_nxt_ = 0;
        // Symmetrize keepalive state with connect()'s reset block (lines
        // 719-721). connect() already zeros these on reconnect, so the
        // happy path is unaffected — but if any path between reset() and
        // the next connect() inspects keepalive state (telemetry, future
        // accessor, or a tick_keepalive racing the close), a stale
        // keepalive_misses_ near the threshold would falsify the next
        // session's "probes unanswered" tally. Same defense-in-depth
        // rationale as snd_wnd_ / rcv_nxt_ above.
        last_rx_tsc_ = 0;
        last_keepalive_tsc_ = 0;
        keepalive_misses_ = 0;
        // Symmetrize MSS state with connect()'s reset block (lines
        // 710-712). connect() already restores these at the next
        // reconnect, but between reset() and that next connect()
        // any caller of effective_mss() / peer_mss_negotiated() /
        // peer_mss() observes the dead session's negotiated values
        // (e.g. shrunk by ICMP Frag Needed), which is misleading
        // telemetry. Same defense-in-depth rationale as snd_wnd_ /
        // keepalive_misses_ above.
        effective_mss_       = config_.mss;
        peer_mss_            = 0;
        peer_mss_negotiated_ = false;
        SPDLOG_LOGGER_DEBUG(log, "RST sent, state -> Closed");
    }

    // ─────────────────────────────────────────────────────────────────────────
    // State queries
    // ─────────────────────────────────────────────────────────────────────────

    /// @brief Current TCP state (Closed, SynSent, Established, etc.).
    [[nodiscard]] TcpState state()       const noexcept { return state_; }
    /// @brief Next sequence number to send (SND.NXT).
    [[nodiscard]] uint32_t snd_nxt()     const noexcept { return snd_nxt_; }
    /// @brief Oldest unacknowledged sequence number (SND.UNA).
    [[nodiscard]] uint32_t snd_una()     const noexcept { return snd_una_; }
    /// @brief Next expected receive sequence number from peer (RCV.NXT).
    [[nodiscard]] uint32_t rcv_nxt()     const noexcept { return rcv_nxt_; }
    /// @brief Our advertised receive window (RCV.WND).
    [[nodiscard]] uint16_t rcv_wnd()     const noexcept { return rcv_wnd_; }
    /// @brief Peer's advertised receive window (SND.WND).
    [[nodiscard]] uint16_t snd_wnd()     const noexcept { return snd_wnd_; }
    /// @brief Maximum Segment Size for this session (effective after
    ///        negotiation / ICMP feedback). Before connect() it equals the
    ///        configured local MSS; after a SYN-ACK with a peer MSS option
    ///        it is min(local, peer); any subsequent ICMP Frag Needed may
    ///        reduce it further.
    [[nodiscard]] uint16_t mss()         const noexcept { return effective_mss_; }
    /// @brief Effective MSS in use — alias for `mss()` with an explicit
    ///        name that makes the negotiated-vs-configured distinction
    ///        clear at call sites.
    [[nodiscard]] uint16_t effective_mss() const noexcept { return effective_mss_; }
    /// @brief True iff the peer advertised an MSS option in SYN-ACK.
    ///        When false, effective_mss_ equals the configured local MSS.
    [[nodiscard]] bool peer_mss_negotiated() const noexcept { return peer_mss_negotiated_; }
    /// @brief Peer's raw SYN-ACK MSS advertisement, untouched by local
    ///        clamping. 0 when the peer omitted the option (in which
    ///        case `peer_mss_negotiated()` is also false). Distinct from
    ///        `effective_mss()` because ICMP PMTU feedback can shrink
    ///        the latter while leaving this field at its handshake value.
    [[nodiscard]] uint16_t peer_mss() const noexcept { return peer_mss_; }
    /// @brief Access the session's TcpConfig.
    [[nodiscard]] const TcpConfig& config() const noexcept { return config_; }
    /// @brief Copy of the current statistics snapshot.
    [[nodiscard]] Stats    stats()       const noexcept { return stats_; }

    [[nodiscard]] bool is_established() const noexcept {
        return state_ == TcpState::Established;
    }

    /// Get the packet template for hot-path direct mbuf construction.
    /// Non-const overload: use only when writing headers directly into a
    /// pre-allocated mbuf via fill_packet() — not for general inspection.
    [[nodiscard]] net::PacketTemplate& packet_template() noexcept {
        return pkt_template_;
    }

    /// Const overload for read-only inspection of the packet template.
    [[nodiscard]] const net::PacketTemplate& packet_template() const noexcept {
        return pkt_template_;
    }

    // ─────────────────────────────────────────────────────────────────────
    // Test-only API
    // ─────────────────────────────────────────────────────────────────────
    //
    // Production code drives state_ via connect() / process_rx() /
    // close() / RST handling.  Unit tests need to fast-forward into a
    // particular state without going through a real handshake against
    // a real peer — these helpers are the controlled escape hatch.
    //
    // The functions are named *_for_testing to make every call site
    // unmistakably visible in code review and grep.  They are not
    // private because tests live in a separate translation unit and
    // friend declarations get tangled with the template parameter.

    /// @brief Test-only: jump the session into a specific TCP state.
    /// @warning Production code MUST NOT call this.
    void inject_state_for_testing(TcpState state) noexcept {
        state_ = state;
    }

    /// @brief Test-only: set send sequence numbers (SND.NXT, SND.UNA).
    /// @warning Production code MUST NOT call this.
    void inject_send_seq_for_testing(uint32_t snd_nxt, uint32_t snd_una) noexcept {
        snd_nxt_ = snd_nxt;
        snd_una_ = snd_una;
    }

    /// @brief Test-only: set receive next sequence (RCV.NXT) and window.
    /// @warning Production code MUST NOT call this.
    void inject_recv_seq_for_testing(uint32_t rcv_nxt, uint16_t rcv_wnd) noexcept {
        rcv_nxt_ = rcv_nxt;
        rcv_wnd_ = rcv_wnd;
    }

    /// @brief Test-only: expose the static seq comparator.
    /// @warning Production code uses the private overload directly.
    [[nodiscard]] static bool seq_after_for_testing(uint32_t a, uint32_t b) noexcept {
        return seq_after(a, b);
    }

private:
    // ── Reorder buffer ──
    // Buffers out-of-order segments (payload copied from mbuf) so they can be
    // delivered once the gap is filled. Avoids treating normal reordering
    // (common with af_packet) as packet loss.
    struct ReorderEntry {
        uint32_t seq = 0;
        uint16_t len = 0;
        // Fixed-size buffer capped at kDefaultMss (1460 bytes).
        // Jumbo-frame sessions (config_.mss > kDefaultMss) cannot buffer
        // oversized reordered segments here — they are delivered in-order only
        // or dropped if out-of-order. A runtime guard in process_rx enforces this.
        uint8_t  data[net::kDefaultMss]{};

        // Pair the runtime guard at process_rx (payload_len <= kDefaultMss)
        // with a compile-time check so a future resize of the buffer cannot
        // silently shrink below the runtime-permitted copy bound.
        static_assert(sizeof(data) >= net::kDefaultMss,
                      "ReorderEntry::data must fit a full kDefaultMss payload");
    };

    /// Try to deliver buffered segments that are now in-order.
    /// @return Number of segments delivered
    ///
    /// Complexity: O(N²) in the number of buffered segments (N = reorder_count_).
    /// Each delivery restarts the linear scan from index 0 (indices shift after
    /// swap-with-last removal). The worst-case bound is ReorderSlots² = 64² = 4096
    /// comparisons per drain call, which is negligible on the trading hot path and
    /// far cheaper than the alternative (sorted insertion or skip-list overhead).
    // PERF: drain_reorder_buffer scans slots linearly. With default ReorderSlots=64,
    // worst-case is O(64²) = 4096 iterations per drain — acceptable for typical use.
    // If profiling shows this as a hotspot with large ReorderSlots, consider a sorted
    // index or bitmap to track filled slots.
    template <typename F>
        requires std::invocable<F, const uint8_t*, uint16_t>
    uint16_t drain_reorder_buf(F&& cb) {
        uint16_t delivered = 0;
        bool progress = true;
        while (progress) {
            progress = false;
            for (uint8_t i = 0; i < reorder_count_; ++i) {
                if (reorder_buf_[i].seq == rcv_nxt_) {
                    std::invoke(std::forward<F>(cb),
                                reorder_buf_[i].data, reorder_buf_[i].len);
                    rcv_nxt_ += reorder_buf_[i].len;
                    stats_.rx_bytes += reorder_buf_[i].len;
                    delivered++;
                    // Remove by swapping with last
                    reorder_buf_[i] = reorder_buf_[--reorder_count_];
                    progress = true;
                    break; // Restart scan (indices shifted)
                }
            }
        }
        return delivered;
    }

    /// Sum payload lengths for the first N segments (used by send_batch).
    static uint32_t total_payload_len_(const uint32_t* lengths, uint16_t n) noexcept {
        uint32_t total = 0;
        for (uint16_t i = 0; i < n; ++i) total += lengths[i];
        return total;
    }

    /// Delayed-ACK delay in CPU cycles. Computed lazily on first use after
    /// TSC::init() has calibrated the counter. 40 µs is chosen so that:
    ///   - Typical datacenter RTTs (~20 µs) complete before timeout, so
    ///     request/response flows never emit a bare ACK (full piggyback).
    ///   - RX-only flows (e.g., market data subscriptions) emit a bare ACK
    ///     at most every 40 µs of received activity, keeping the peer's
    ///     retransmission timer at bay without adding measurable overhead.
    /// Linux's TCP_DELACK_MIN is ~4 ms and TCP_DELACK_MAX is 200 ms
    /// (HZ-dependent) for public internet; we tighten to 40 µs because we
    /// target datacenter.
    ///
    /// Implementation note: we cache the computed value via an atomic, but
    /// only AFTER TSC::to_cycles() succeeds. If TSC::init() has not yet run
    /// when first called, we return the 40k-cycle fallback (correct on the
    /// 1 GHz Graviton system counter, suboptimal on x86 with rdtsc) WITHOUT
    /// caching — so a subsequent call after TSC::init() picks up the
    /// calibrated value. This avoids permanently caching a wrong fallback
    /// when the first call races with TSC initialization.
    static uint64_t ack_delay_cycles_() noexcept {
        static std::atomic<uint64_t> cached{0};
        uint64_t v = cached.load(std::memory_order_relaxed);
        if (v != 0) [[likely]] return v;
        auto opt = eph::utils::TSC::to_cycles(40'000.0 /* ns */);
        if (!opt) [[unlikely]] {
            // TSC not initialised yet — use the 1 GHz fallback but DON'T
            // cache it; let a later call (post TSC::init) refresh.
            return 40'000;
        }
        cached.store(*opt, std::memory_order_relaxed);
        return *opt;
    }

    TcpConfig           config_;
    rte_mempool*        pool_;
    TcpState            state_;
    net::PacketTemplate pkt_template_;

    // TCP sequence tracking — non-atomic by design.
    // All send/receive paths run on a single DPDK poll-mode lcore;
    // concurrent access from multiple threads is not supported.
    // If multi-threaded access is ever needed, these must become atomic.
    uint32_t snd_nxt_;    // SND.NXT: next sequence number to send
    uint32_t snd_una_;    // SND.UNA: oldest unacknowledged sequence number
    uint32_t rcv_nxt_;    // RCV.NXT: next expected sequence number from peer
    uint16_t rcv_wnd_;    // RCV.WND: our receive window advertisement
    uint16_t snd_wnd_;    // SND.WND: peer's receive window

    // Effective send-side MSS. Starts at config_.mss, may drop to the peer's
    // advertisement on SYN-ACK, and may drop further on ICMP Frag Needed.
    // Never grows — that would require a second round-trip signal we don't
    // implement. Resets to config_.mss at every connect() attempt.
    uint16_t effective_mss_;
    // Peer's *raw* SYN-ACK MSS option, recorded **before** clamping into
    // effective_mss_. Distinct from effective_mss_ because the latter can
    // shrink further on ICMP Frag Needed; reporting effective_mss as
    // "peer_mss" in StreamSnapshot would lie post-shrink. 0 when peer
    // omitted the option (peer_mss_negotiated_ stays false in that case).
    uint16_t peer_mss_         = 0;
    bool     peer_mss_negotiated_ = false;

    ReorderEntry reorder_buf_[ReorderSlots]{};
    uint8_t      reorder_count_ = 0;

    Stats stats_{};

    // Delayed-ACK timestamp. 0 means "no ACK owed". Non-zero records the
    // TSC at which the first un-ACKed segment arrived — flush_pending_ack()
    // compares TSC::now() against this to decide whether to emit a bare ACK
    // now (timer expired) or defer further (waiting for piggyback).
    // Set by process_rx(), cleared by send() / send_batch() (piggyback) or
    // by flush_pending_ack() on timer expiry.
    uint64_t ack_pending_since_tsc_ = 0;

    // 2MSL timer for TIME_WAIT state (RFC 793 §3.5).
    // MSL = 60s (common implementation default). Deadline is set when entering
    // TIME_WAIT; connect() checks it to allow re-use after expiry.
    std::chrono::steady_clock::time_point time_wait_deadline_{};

    // ── Keepalive state ──
    // Driven by tick_keepalive(now_tsc). last_rx_tsc_ tracks the TSC of the
    // most recent rx packet that matched this connection; if it falls more
    // than config_.keepalive_interval behind, a probe is emitted and
    // keepalive_misses_ is incremented. A reply resets keepalive_misses_
    // to 0 (last_rx_tsc_ updates naturally). After keepalive_probes
    // consecutive misses, state_ transitions to Closed.
    uint64_t last_rx_tsc_        = 0;
    uint64_t last_keepalive_tsc_ = 0;
    uint8_t  keepalive_misses_   = 0;

    // TSC captured right after rte_eth_rx_burst returns data.
    // Used by Transport as the true RX arrival baseline.
    // alignas(64): placed on its own cache line to avoid false sharing with
    // stats_ (written by the RX path) when a separate thread reads the TSC.
    alignas(64) std::atomic<uint64_t> last_rx_burst_tsc_{0};

    // ── ARP cache refresh state ──
    uint32_t arp_refresh_gateway_ip_ = 0;
    std::chrono::seconds arp_refresh_interval_{0};
    std::chrono::steady_clock::time_point arp_next_refresh_{
        std::chrono::steady_clock::time_point::max()};

    /// Enter TIME_WAIT state and start the 2MSL timer (RFC 793 §3.5).
    /// MSL = 60s; 2MSL = 120s. This prevents old duplicate segments from
    /// confusing a new connection on the same 4-tuple.
    void enter_time_wait() noexcept {
        static constexpr auto kMsl = std::chrono::seconds(60);
        state_ = TcpState::TimeWait;
        time_wait_deadline_ = std::chrono::steady_clock::now() + 2 * kMsl;
        SPDLOG_LOGGER_DEBUG(detail::tcp_logger(),
            "Entered TIME_WAIT, 2MSL deadline in 120s");
    }

    /// Generate initial sequence number using the kernel CSPRNG via
    /// `getrandom(2)` (decoupled from OpenSSL to avoid vcpkg-openssl vs.
    /// aws-lc conflicts). Returns an error if the syscall fails — propagate
    /// to caller rather than masking failure with a sentinel value like 0 or 1.
    [[nodiscard]] static std::expected<uint32_t, core::ErrorInfo> generate_isn() noexcept {
        uint32_t isn = 0;
        // GRND_NONBLOCK: do not block if the urandom pool is not yet
        // initialised. On any post-boot Linux ≥ 3.17 this returns immediately.
        ssize_t n = ::getrandom(&isn, sizeof(isn), GRND_NONBLOCK);
        if (n != static_cast<ssize_t>(sizeof(isn))) {
            SPDLOG_LOGGER_CRITICAL(detail::tcp_logger(),
                "getrandom() failed for ISN generation (ret={}) — cannot "
                "establish secure TCP connection", n);
            return std::unexpected(core::ErrorInfo{
                core::Error::OutOfMemory,
                "TcpSession::generate_isn: CSPRNG (getrandom) failed"});
        }
        return isn;
    }

    /// Sequence number comparison: is a after b? (handles wrap-around)
    /// Sequence number comparison handling 32-bit wrap-around (RFC 1323).
    /// Treats a and b as positions on a circular 32-bit timeline:
    /// returns true if a is "after" b in the circular sense.
    static bool seq_after(uint32_t a, uint32_t b) noexcept {
        return static_cast<int32_t>(a - b) > 0;
    }

    /// Send a standard Linux-style TCP keepalive probe: a zero-length
    /// ACK with `seq = snd_nxt - 1`. The sequence number is before the
    /// peer's `rcv_nxt`, so RFC 1122 §4.2.3.6 requires the peer to
    /// respond with an ACK — and if the peer is gone, nothing comes
    /// back and the tick_keepalive misses counter advances.
    [[nodiscard]] std::expected<void, core::ErrorInfo> send_keepalive_probe_() noexcept {
        // snd_nxt_ is the next byte we would send; snd_nxt_-1 is a byte
        // we believe the peer has already acknowledged. Wrap is handled
        // implicitly by uint32_t underflow at seq=0 (peer interprets via
        // 32-bit modular arithmetic, which is correct).
        const uint32_t probe_seq = snd_nxt_ - 1;
        auto* pkt = pkt_template_.build_packet(
            pool_, probe_seq, rcv_nxt_, net::kTcpAck, rcv_wnd_);
        if (!pkt) {
            SPDLOG_LOGGER_WARN(detail::tcp_logger(),
                "TcpSession::send_keepalive_probe_: mbuf alloc failed");
            return std::unexpected(core::ErrorInfo{
                core::Error::OutOfMemory,
                "TcpSession::send_keepalive_probe_: mbuf alloc failed"});
        }
        uint16_t sent = rte_eth_tx_burst(
            config_.port_id, config_.tx_queue_id, &pkt, 1);
        if (sent != 1) {
            rte_pktmbuf_free(pkt);
            SPDLOG_LOGGER_WARN(detail::tcp_logger(),
                "TcpSession::send_keepalive_probe_: tx_burst failed");
            return std::unexpected(core::ErrorInfo{
                core::Error::BufferFull,
                "TcpSession::send_keepalive_probe_: tx_burst failed"});
        }
        ++stats_.tx_packets;
        return {};
    }

    /// Convert `config_.keepalive_interval` (milliseconds) into TSC
    /// cycles once per call. Uses a 3 GHz ns→cycles fallback if
    /// TSC::init() has not yet run — same direction the DNS resolver
    /// fallback already picked (`AsyncDnsResolver::start`): when in
    /// doubt the upper-bound CPU frequency makes the timer fire LATE
    /// rather than EARLY. Picking 1 GHz (the legacy choice) on a real
    /// 3 GHz host scaled the keepalive interval down by 3× and produced
    /// keepalive probe storms in tests where TSC::init() had been
    /// skipped (typical mock setups). Production paths call TSC::init()
    /// at startup and never hit the fallback.
    [[nodiscard]] uint64_t keepalive_interval_cycles_() noexcept {
        const double ns =
            static_cast<double>(config_.keepalive_interval.count()) * 1'000'000.0;
        if (auto opt = eph::utils::TSC::to_cycles(ns)) return *opt;
        // Fallback: assume 3 GHz upper-bound. cycles = ns × 3.
        // Saturate the cast: per [conv.fpint], static_cast<uint64_t>(x)
        // is UB when `x` is outside [0, UINT64_MAX]. Caller-supplied
        // `keepalive_interval = milliseconds::max()` would push `ns × 3`
        // far beyond UINT64_MAX (~1.8e19) and modern GCC may emit any
        // value, including 0 (probe storm) or a huge number (probe
        // never). TSC::to_cycles already saturates on the calibrated
        // path; mirror that here so the uncalibrated fallback behaves
        // identically and the tick comparator stays well-defined.
        // Hitting this branch on a calibrated host means TSC::init()
        // was skipped — bump the numerical-anomaly counter so operators
        // can detect "we silently used a fallback" via the public metric
        // surface. (T3.5)
        ++stats_.numerical_anomalies;
        const double cycles = ns * 3.0;
        if (!(cycles >= 0.0)) return 0ULL;  // NaN-safe (also catches negative)
        if (cycles >= static_cast<double>(std::numeric_limits<uint64_t>::max()))
            return std::numeric_limits<uint64_t>::max();
        return static_cast<uint64_t>(cycles);
    }

    /// Send a bare ACK packet.
    /// noexcept-marked to align with `send_keepalive_probe_` and to keep
    /// the inner DpdkPoller hot path crash-safe: if a `std::format` or
    /// spdlog allocation in this function ever threw, the unwind would
    /// escape `process_burst_fn` (a noexcept function pointer) and call
    /// `std::terminate`. None of the callees actually throw in practice
    /// — `pkt_template_.build_packet`, `rte_eth_tx_burst`,
    /// `rte_pktmbuf_free`, and the SPDLOG_LOGGER_* macros all reduce to
    /// noexcept ops under SPDLOG_NO_EXCEPTIONS — but the contract is
    /// what the Poller's hot path actually relies on. Making the
    /// signature match the body forces the compiler to confirm.
    [[nodiscard]] std::expected<void, core::ErrorInfo> send_ack() noexcept {
        auto* ack_pkt = pkt_template_.build_packet(
            pool_, snd_nxt_, rcv_nxt_, net::kTcpAck, rcv_wnd_);
        if (!ack_pkt) {
            SPDLOG_LOGGER_ERROR(detail::tcp_logger(),
                "TcpSession::send_ack: mbuf allocation failed");
            return std::unexpected(core::ErrorInfo{
                core::Error::OutOfMemory,
                "TcpSession::send_ack: mbuf allocation failed"});
        }

        uint16_t sent = rte_eth_tx_burst(
            config_.port_id, config_.tx_queue_id, &ack_pkt, 1);
        if (sent != 1) {
            rte_pktmbuf_free(ack_pkt);
            SPDLOG_LOGGER_ERROR(detail::tcp_logger(),
                "TcpSession::send_ack: tx_burst failed");
            return std::unexpected(core::ErrorInfo{
                core::Error::BufferFull,
                "TcpSession::send_ack: tx_burst failed"});
        }

        stats_.acks_sent++;
        stats_.tx_packets++;
        SPDLOG_LOGGER_TRACE(detail::tcp_logger(),
            "ACK sent: seq={}, ack={}", snd_nxt_, rcv_nxt_);
        return {};
    }

    /// Free remaining mbufs in an array starting from index `from`.
    static void free_remaining(rte_mbuf** pkts, uint16_t from, uint16_t total) noexcept {
        for (uint16_t j = from; j < total; ++j) {
            rte_pktmbuf_free(pkts[j]);
        }
    }

    /// Unified cleanup for early-return paths inside process_rx().
    /// Adds pkts[current_idx] to free_list, frees the tail of pkts[], then
    /// batch-frees the accumulated free_list — ensuring no mbuf is leaked
    /// regardless of which early-exit path is taken.
    static void abort_rx_cleanup(rte_mbuf** pkts, uint16_t current_idx,
                                  uint16_t nb_pkts, rte_mbuf** free_list,
                                  uint16_t free_count) noexcept {
        free_list[free_count++] = pkts[current_idx];
        for (uint16_t j = current_idx + 1; j < nb_pkts; ++j)
            rte_pktmbuf_free(pkts[j]);
        rte_pktmbuf_free_bulk(free_list, free_count);
    }
};

static_assert(eph::net::TcpTransport<TcpSession<>>,
    "TcpSession must satisfy TcpTransport concept");

} // namespace eph::dpdk

// std::formatter specialization for TcpConfig.
template <>
struct std::formatter<eph::dpdk::TcpConfig> : std::formatter<std::string> {
    auto format(const eph::dpdk::TcpConfig& c, auto& ctx) const {
        return std::formatter<std::string>::format(
            std::format("TcpConfig({}:{} -> {}:{} mss={} wnd={})",
                eph::dpdk::net::format_ipv4(c.tuple.src_ip).data(), c.tuple.src_port,
                eph::dpdk::net::format_ipv4(c.tuple.dst_ip).data(), c.tuple.dst_port,
                c.mss, c.recv_window),
            ctx);
    }
};

// std::formatter specialization for TcpSession<>::Stats (default ReorderSlots=64).
// Non-default instantiations share the same Stats layout; call dump() directly.
template <>
struct std::formatter<eph::dpdk::TcpSession<>::Stats> : std::formatter<std::string> {
    auto format(const eph::dpdk::TcpSession<>::Stats& s, auto& ctx) const {
        return std::formatter<std::string>::format(s.dump(), ctx);
    }
};
