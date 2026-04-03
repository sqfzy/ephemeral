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

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <openssl/rand.h>

#include <rte_ethdev.h>
#include <rte_mbuf.h>

#include "eph/dpdk/net_header.hpp"
#include "eph/core/tcp_concept.hpp"
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

    /// Validate configuration, returning an error description or empty string on success.
    /// Call before TcpSession construction to get early, actionable error messages.
    [[nodiscard]] constexpr std::string_view validate() const noexcept {
        if (tuple.src_ip == 0)
            return "src_ip must not be zero";
        if (tuple.dst_ip == 0)
            return "dst_ip must not be zero";
        if (tuple.src_port == 0)
            return "src_port must be > 0";
        if (tuple.dst_port == 0)
            return "dst_port must be > 0";
        if (mss == 0)
            return "mss must be > 0";
        if (mss > 9000)
            return "mss exceeds jumbo frame limit (9000)";
        if (recv_window == 0)
            return "recv_window must be > 0";
        if (recv_window > 65535)
            return "recv_window exceeds 65535 (window scaling not implemented)";
        if (max_rx_burst > 32)
            return "max_rx_burst must be in [0, 32] (0 = auto)";
        return {};
    }

    /// Equality comparison (manual because rte_ether_addr is a C struct).
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
            && a.max_rx_burst == b.max_rx_burst;
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
        // Same src and dst IP likely means self-connect
        if (tuple.src_ip == tuple.dst_ip)
            w.emplace_back(std::format(
                "src_ip == dst_ip ({}) -- self-connect is unusual for DPDK",
                net::format_ipv4(tuple.src_ip).data()));
        // MSS below typical Ethernet minimum (536) may indicate a mistake
        if (mss < 536)
            w.emplace_back(std::format(
                "mss={} is below the recommended minimum (536) -- "
                "may cause excessive fragmentation", mss));
        // MSS above 1460 (standard Ethernet) requires jumbo frames
        if (mss > 1460 && mss <= 9000)
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

    /// Format a MAC address as "xx:xx:xx:xx:xx:xx".
    [[nodiscard]] static std::string format_mac(const rte_ether_addr& m) {
        auto buf = net::format_mac(m);
        return std::string(buf.data());
    }

    /// Multi-line formatted dump for logging/debugging.
    [[nodiscard]] std::string dump() const {
        return std::format(
            "TcpConfig:\n"
            "  src: {}:{}, dst: {}:{}\n"
            "  src_mac: {}, dst_mac: {}\n"
            "  mss: {}, recv_window: {}\n"
            "  port_id: {}, tx_queue: {}, rx_queue: {}",
            net::format_ipv4(tuple.src_ip).data(), tuple.src_port,
            net::format_ipv4(tuple.dst_ip).data(), tuple.dst_port,
            format_mac(src_mac), format_mac(dst_mac),
            mss, recv_window,
            port_id, tx_queue_id, rx_queue_id);
    }

    /// JSON-formatted config for monitoring system integration.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"src_ip\":\"{}\",\"dst_ip\":\"{}\","
            "\"src_port\":{},\"dst_port\":{},"
            "\"src_mac\":\"{}\",\"dst_mac\":\"{}\","
            "\"mss\":{},\"recv_window\":{},"
            "\"port_id\":{},\"tx_queue_id\":{},\"rx_queue_id\":{}}}",
            net::format_ipv4(tuple.src_ip).data(),
            net::format_ipv4(tuple.dst_ip).data(),
            tuple.src_port, tuple.dst_port,
            format_mac(src_mac), format_mac(dst_mac),
            mss, recv_window,
            port_id, tx_queue_id, rx_queue_id);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Logger
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

inline spdlog::logger* tcp_logger() {
    static auto l = [] {
        auto lg = spdlog::get("dpdk.tcp");
        if (!lg) lg = spdlog::stdout_color_mt("dpdk.tcp");
        return lg;
    }();
    return l.get();
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
///       DPDK poll-mode lcore. For multi-connection setups on a shared RX
///       queue, use Reactor (reactor.hpp) to demultiplex.
template <size_t ReorderSlots = 64>
class TcpSession {
    static_assert(ReorderSlots <= 255,
                  "ReorderSlots must fit in uint8_t (reorder_count_)");
public:
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

        // ── Reorder / loss telemetry ──
        uint64_t reorder_hits      = 0;  ///< Segments successfully buffered & delivered via reorder buf
        uint64_t reorder_overflows = 0;  ///< Reorder buffer full events (triggered reconnect)
        uint32_t max_gap_size      = 0;  ///< Largest observed seq gap in bytes (absolute, not delta)
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
                "  max_gap_size: {}",
                tx_packets, rx_packets, rx_bursts, tx_bytes, rx_bytes,
                acks_sent, out_of_order, resets_received,
                reorder_hits, reorder_overflows, max_gap_size);

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
                "\"reorder_overflows\":{},\"max_gap_size\":{}",
                tx_packets, rx_packets, rx_bursts, tx_bytes, rx_bytes,
                acks_sent, out_of_order, resets_received,
                reorder_hits, reorder_overflows, max_gap_size);

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
                .reorder_hits      = lhs.reorder_hits      - rhs.reorder_hits,
                .reorder_overflows = lhs.reorder_overflows - rhs.reorder_overflows,
                .max_gap_size      = lhs.max_gap_size,  // Point-in-time (not diffable)
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
        , snd_wnd_(0) {

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
        if (state_ != TcpState::Closed) {
            SPDLOG_LOGGER_DEBUG(detail::tcp_logger(),
                "TcpSession destroyed in state {}", tcp_state_name(state_));
        }
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
        , reorder_count_(other.reorder_count_)
        , stats_(other.stats_)
        , ack_pending_(other.ack_pending_)
        , time_wait_deadline_(other.time_wait_deadline_)
        , last_rx_burst_tsc_(other.last_rx_burst_tsc_.load(std::memory_order_relaxed))
{
        // reorder_count_ is initialized from other.reorder_count_ in the
        // member initializer list above, so the loop bound is correct.
        for (uint8_t i = 0; i < reorder_count_; ++i)
            reorder_buf_[i] = other.reorder_buf_[i];
        other.pool_ = nullptr;
        other.state_ = TcpState::Closed;
        other.reorder_count_ = 0;
        other.ack_pending_ = false;
    }

    TcpSession& operator=(TcpSession&& other) noexcept {
        if (this != &other) {
            config_ = std::move(other.config_);
            pool_ = other.pool_;
            state_ = other.state_;
            pkt_template_ = std::move(other.pkt_template_);
            snd_nxt_ = other.snd_nxt_;
            snd_una_ = other.snd_una_;
            rcv_nxt_ = other.rcv_nxt_;
            rcv_wnd_ = other.rcv_wnd_;
            snd_wnd_ = other.snd_wnd_;
            reorder_count_ = other.reorder_count_;
            for (uint8_t i = 0; i < reorder_count_; ++i)
                reorder_buf_[i] = other.reorder_buf_[i];
            stats_ = other.stats_;
            ack_pending_ = other.ack_pending_;
            time_wait_deadline_ = other.time_wait_deadline_;
            last_rx_burst_tsc_.store(other.last_rx_burst_tsc_.load(std::memory_order_relaxed), std::memory_order_relaxed);
            other.pool_ = nullptr;
            other.state_ = TcpState::Closed;
            other.reorder_count_ = 0;
            other.ack_pending_ = false;
        }
        return *this;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Connection establishment
    // ─────────────────────────────────────────────────────────────────────────

    /// Perform TCP three-way handshake (blocking, polls DPDK rx).
    /// @param timeout  Maximum time to wait for SYN-ACK
    /// @return Error string on failure
    [[nodiscard]] std::expected<void, std::string>
    connect(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
        [[maybe_unused]] auto log = detail::tcp_logger();

        if (state_ == TcpState::TimeWait) {
            // Allow reconnection once the 2MSL timer has expired (RFC 793 §3.5).
            if (std::chrono::steady_clock::now() >= time_wait_deadline_) {
                SPDLOG_LOGGER_DEBUG(log,
                    "TIME_WAIT 2MSL expired — allowing reconnect");
                state_ = TcpState::Closed;
            } else {
                return std::unexpected(std::format(
                    "Cannot connect: session in TIME_WAIT (2MSL not yet expired)"));
            }
        }

        if (state_ != TcpState::Closed) {
            return std::unexpected(std::format(
                "Cannot connect: session in state {}", tcp_state_name(state_)));
        }

        // Always regenerate ISN on each connect attempt.
        // This ensures stale snd_nxt_/snd_una_ from a timed-out handshake
        // (where state_ was reset to Closed but sequence numbers were already
        // incremented) do not bleed into the new connection.
        auto isn_result = generate_isn();
        if (!isn_result) {
            SPDLOG_LOGGER_ERROR(log, "ISN generation failed — CSPRNG unavailable");
            return std::unexpected(std::format("ISN generation failed: {}", isn_result.error()));
        }
        snd_nxt_ = *isn_result;
        snd_una_ = *isn_result;
        rcv_nxt_ = 0;
        reorder_count_ = 0;
        ack_pending_ = false;

        // Send SYN
        SPDLOG_LOGGER_DEBUG(log, "Sending SYN, isn={}", snd_nxt_);
        auto* syn = pkt_template_.build_packet(
            pool_, snd_nxt_, 0, net::kTcpSyn, rcv_wnd_);
        if (!syn) {
            SPDLOG_LOGGER_ERROR(log, "Failed to allocate mbuf for SYN");
            return std::unexpected("mbuf allocation failed for SYN");
        }

        uint16_t sent = rte_eth_tx_burst(config_.port_id, config_.tx_queue_id, &syn, 1);
        if (sent != 1) {
            rte_pktmbuf_free(syn);
            SPDLOG_LOGGER_ERROR(log, "tx_burst failed for SYN");
            return std::unexpected("tx_burst failed for SYN");
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

            rte_mbuf* pkts[32];
            uint16_t nb_rx = rte_eth_rx_burst(
                config_.port_id, config_.rx_queue_id, pkts, 32);

            for (uint16_t i = 0; i < nb_rx; ++i) {
                auto parsed = net::parse_packet(pkts[i]);
                if (!parsed.tcp || !parsed.matches(config_.tuple)) {
                    rte_pktmbuf_free(pkts[i]);
                    continue;
                }

                stats_.rx_packets++;

                if (parsed.has_flag(net::kTcpRst)) {
                    SPDLOG_LOGGER_ERROR(log, "Received RST during handshake");
                    state_ = TcpState::Closed;
                    stats_.resets_received++;
                    free_remaining(pkts, i + 1, nb_rx);
                    return std::unexpected("Connection refused (RST)");
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

                    SPDLOG_LOGGER_DEBUG(log,
                        "Received SYN-ACK: peer_isn={}, window={}",
                        parsed.seq(), snd_wnd_);

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
        SPDLOG_LOGGER_ERROR(log, "TCP handshake timeout ({}ms)", timeout.count());
        return std::unexpected(std::format(
            "TCP handshake timeout after {}ms", timeout.count()));
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Data transfer
    // ─────────────────────────────────────────────────────────────────────────

    /// Send data over the established TCP connection.
    /// @param data     Payload data
    /// @param len      Payload length (must be <= MSS)
    /// @return Number of bytes sent, or error
    [[nodiscard]] std::expected<size_t, std::string>
    send(const void* data, size_t len) {
        if (state_ != TcpState::Established) {
            return std::unexpected(std::format(
                "Cannot send: state={}", tcp_state_name(state_)));
        }

        if (len > config_.mss) {
            return std::unexpected(std::format(
                "Payload too large: {} > MSS {}", len, config_.mss));
        }

        auto* mbuf = pkt_template_.build_packet(
            pool_, snd_nxt_, rcv_nxt_,
            net::kTcpAck | net::kTcpPsh,
            rcv_wnd_, data, static_cast<uint16_t>(len));
        if (!mbuf) {
            SPDLOG_LOGGER_ERROR(detail::tcp_logger(),
                "mbuf alloc failed in send (len={})", len);
            return std::unexpected("mbuf allocation failed");
        }

        uint16_t sent = rte_eth_tx_burst(
            config_.port_id, config_.tx_queue_id, &mbuf, 1);
        if (sent != 1) {
            rte_pktmbuf_free(mbuf);
            SPDLOG_LOGGER_ERROR(detail::tcp_logger(),
                "tx_burst failed in send: len={}, snd_nxt_={}", len, snd_nxt_);
            return std::unexpected("tx_burst failed");
        }

        snd_nxt_ += static_cast<uint32_t>(len);
        stats_.tx_packets++;
        stats_.tx_bytes += len;
        return len;
    }

    /// Build a data packet into a pre-allocated mbuf (hot path, no alloc).
    /// Returns the mbuf ready for tx_burst, or nullptr on error.
    /// @warning snd_nxt_ is advanced immediately. Caller MUST transmit the
    ///          returned mbuf via tx_burst. If TX fails, the session sequence
    ///          numbers are inconsistent and must be reset().
    rte_mbuf* build_data_packet(rte_mbuf* mbuf,
                                const void* data, uint16_t len) noexcept {
        if (state_ != TcpState::Established || len > config_.mss) return nullptr;

        uint16_t written = pkt_template_.fill_packet(
            mbuf, snd_nxt_, rcv_nxt_,
            net::kTcpAck | net::kTcpPsh,
            rcv_wnd_, data, len);
        if (written == 0) return nullptr;

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
    [[nodiscard]] std::expected<uint16_t, std::string>
    process_rx(rte_mbuf** pkts, uint16_t nb_pkts, F&& data_callback) {
        [[maybe_unused]] auto log = detail::tcp_logger();
        uint16_t data_count = 0;
        bool need_ack = false;

        // Collect mbufs for batched free at loop end.
        // Per-packet rte_pktmbuf_free inside the loop costs ~20-40ns each
        // and falls within the latency-measured path.
        // Safety: free_list is sized to the max burst the callers (poll_rx,
        // connect) ever pass. Clamp nb_pkts to prevent stack overflow.
        static constexpr uint16_t kMaxBurst = 32;
        nb_pkts = std::min(nb_pkts, kMaxBurst);
        rte_mbuf* free_list[kMaxBurst];
        uint16_t free_count = 0;

        for (uint16_t i = 0; i < nb_pkts; ++i) {
            auto parsed = net::parse_packet(pkts[i]);

            // Skip non-matching packets
            if (!parsed.tcp || !parsed.matches(config_.tuple)) {
                free_list[free_count++] = pkts[i];
                continue;
            }

            stats_.rx_packets++;

            // RST — immediate close
            if (parsed.has_flag(net::kTcpRst)) {
                SPDLOG_LOGGER_WARN(log,
                    "Received RST, closing connection: {}:{} -> {}:{}",
                    net::format_ipv4(config_.tuple.src_ip).data(),
                    config_.tuple.src_port,
                    net::format_ipv4(config_.tuple.dst_ip).data(),
                    config_.tuple.dst_port);
                state_ = TcpState::Closed;
                stats_.resets_received++;
                abort_rx_cleanup(pkts, i, nb_pkts, free_list, free_count);
                return std::unexpected("Connection reset by peer");
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
                        parsed.payload_len <= net::kDefaultMss) {
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
                        // Duplicate/past segment — just drop
                        SPDLOG_LOGGER_DEBUG(log,
                            "Dropping duplicate: expected={}, got={}", rcv_nxt_, seg_seq);
                    } else {
                        // Reorder buffer full — genuine loss
                        stats_.reorder_overflows++;
                        SPDLOG_LOGGER_WARN(log,
                            "Reorder buffer full ({} slots): expected={}, got={}",
                            ReorderSlots, rcv_nxt_, seg_seq);
                        abort_rx_cleanup(pkts, i, nb_pkts, free_list, free_count);
                        return std::unexpected(std::format(
                            "Packet loss detected (reorder buffer full): expected seq {}, got {}",
                            rcv_nxt_, seg_seq));
                    }
                    free_list[free_count++] = pkts[i];
                    continue;
                }

                // Process in-order data payload
                if (parsed.payload_len > 0) {
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

            // FIN handling — only process in-order FINs (seq == rcv_nxt_).
            // An out-of-order FIN with zero payload bypasses the reorder path
            // above (which guards on payload_len > 0), so the explicit sequence
            // check here prevents a premature state transition. The sender will
            // retransmit the FIN once the gap is filled and rcv_nxt_ advances.
            if (parsed.has_flag(net::kTcpFin)) {
                if (parsed.seq() == rcv_nxt_) {
                    rcv_nxt_++; // FIN consumes one sequence number
                    need_ack = true;

                    switch (state_) {
                        case TcpState::Established:
                            SPDLOG_LOGGER_DEBUG(log, "Received FIN in ESTABLISHED");
                            state_ = TcpState::CloseWait;
                            break;
                        case TcpState::FinWait1:
                            if (parsed.has_flag(net::kTcpAck)) {
                                // FIN+ACK: peer acknowledged our FIN and sent its own.
                                // Simultaneous close completes — RFC 793 §3.5 FIN_WAIT_1 → TIME_WAIT.
                                SPDLOG_LOGGER_DEBUG(log, "Received FIN+ACK in FIN_WAIT_1 -> TIME_WAIT");
                                enter_time_wait();
                            } else {
                                // FIN without ACK: simultaneous close — peer sent FIN but hasn't
                                // ACK'd ours yet. RFC 793 §3.5: FIN_WAIT_1 → CLOSING (not TIME_WAIT).
                                // We'll move to TIME_WAIT when the peer's ACK arrives in CLOSING.
                                SPDLOG_LOGGER_DEBUG(log, "Simultaneous close: FIN_WAIT_1 -> CLOSING");
                                state_ = TcpState::Closing;
                            }
                            break;
                        case TcpState::Closing:
                            // ACK of our FIN arrives while in CLOSING — RFC 793: CLOSING → TIME_WAIT.
                            // (The ACK flag is checked via the snd_una_ update above; reaching here
                            //  means the FIN sequence was in-order, which completes the exchange.)
                            SPDLOG_LOGGER_DEBUG(log, "Received ACK in CLOSING -> TIME_WAIT");
                            enter_time_wait();
                            break;
                        case TcpState::FinWait2:
                            SPDLOG_LOGGER_DEBUG(log, "Received FIN in FIN_WAIT_2 -> TIME_WAIT");
                            enter_time_wait();
                            break;
                        default:
                            break;
                    }
                } else {
                    SPDLOG_LOGGER_DEBUG(log,
                        "Out-of-order FIN dropped: seq={}, rcv_nxt_={}",
                        parsed.seq(), rcv_nxt_);
                }
            }

            free_list[free_count++] = pkts[i];
        }

        // Batch-free all mbufs at once — avoids per-packet mempool
        // accounting overhead on the latency-critical path.
        if (free_count > 0) {
            rte_pktmbuf_free_bulk(free_list, free_count);
        }

        // Defer ACK to keep send_ack() off the RX critical path.
        // Caller (Transport) must call flush_pending_ack() after
        // processing the data (e.g. after TLS decrypt).
        if (need_ack) {
            ack_pending_ = true;
        }

        return data_count;
    }

    /// Send any deferred ACK accumulated by process_rx().
    /// Call this after data processing (TLS decrypt, WS decode) to keep
    /// the ACK's rte_eth_tx_burst off the RX latency measurement path.
    void flush_pending_ack() noexcept {
        if (!ack_pending_) return;
        ack_pending_ = false;
        auto r = send_ack();
        if (!r) {
            SPDLOG_LOGGER_WARN(detail::tcp_logger(),
                "Failed to send deferred ACK: {}", r.error());
        }
    }

    /// Poll DPDK rx and process received packets through the TCP state machine.
    /// This is the TcpTransport concept-compatible interface that encapsulates
    /// rte_eth_rx_burst + process_rx into a single call.
    ///
    /// For multi-session sharing a single NIC RX queue, use Reactor
    /// (reactor.hpp) which dispatches directly via process_rx with zero
    /// ring overhead.
    ///
    /// @return On success: count of data packets processed (may be 0 if no
    ///         data packets in this burst, e.g. pure ACKs). On error: returns
    ///         unexpected with error message; all received packets are freed.
    template <typename F>
        requires std::invocable<F, const uint8_t*, uint16_t>
    [[nodiscard]] std::expected<uint16_t, std::string> poll_rx(F&& data_callback) {
        rte_mbuf* pkts[32];

        // Limit burst size to prevent upstream reassembly buffer overflow.
        // Auto-calculate from MSS when max_rx_burst == 0.
        const uint16_t burst_limit = config_.max_rx_burst > 0
            ? config_.max_rx_burst
            : std::min(uint16_t{32},
                       static_cast<uint16_t>(TcpConfig::kDefaultRxBudgetBytes / config_.mss));

        uint16_t nb_rx = rte_eth_rx_burst(
            config_.port_id, config_.rx_queue_id, pkts, burst_limit);

        if (nb_rx == 0) return uint16_t{0};
        ++stats_.rx_bursts;
        // Capture TSC right after rx_burst — matches SocketTransport's
        // post-recvmsg() timing for fair cross-backend latency comparison.
        last_rx_burst_tsc_.store(eph::utils::TSC::now(), std::memory_order_relaxed);
        return process_rx(pkts, nb_rx, std::forward<F>(data_callback));
    }

    /// Get the connection 4-tuple (src/dst IP + port).
    [[nodiscard]] const net::ConnectionTuple& connection_tuple() const noexcept {
        return config_.tuple;
    }

    /// Get TCP-level statistics (packets, bursts, bytes, etc.).
    [[nodiscard]] const Stats& tcp_stats() const noexcept { return stats_; }

    /// Set the RX burst TSC externally (used by Reactor to propagate
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
    [[nodiscard]] std::expected<void, std::string> close() {
        [[maybe_unused]] auto log = detail::tcp_logger();

        if (state_ != TcpState::Established &&
            state_ != TcpState::CloseWait) {
            return std::unexpected(std::format(
                "Cannot close: state={}", tcp_state_name(state_)));
        }

        SPDLOG_LOGGER_DEBUG(log, "Sending FIN");
        auto* fin = pkt_template_.build_packet(
            pool_, snd_nxt_, rcv_nxt_,
            net::kTcpFin | net::kTcpAck, rcv_wnd_);
        if (!fin) {
            return std::unexpected("mbuf allocation failed for FIN");
        }

        uint16_t sent = rte_eth_tx_burst(
            config_.port_id, config_.tx_queue_id, &fin, 1);
        if (sent != 1) {
            rte_pktmbuf_free(fin);
            SPDLOG_LOGGER_ERROR(log, "tx_burst failed for FIN");
            return std::unexpected("tx_burst failed for FIN");
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
            if (sent != 1) rte_pktmbuf_free(rst);
        }

        state_ = TcpState::Closed;
        reorder_count_ = 0;  // Prevent stale data delivery on reconnect
        ack_pending_ = false;
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
    /// @brief Maximum Segment Size for this session.
    [[nodiscard]] uint16_t mss()         const noexcept { return config_.mss; }
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

    ReorderEntry reorder_buf_[ReorderSlots]{};
    uint8_t      reorder_count_ = 0;

    Stats stats_{};

    // Deferred ACK flag — set by process_rx(), cleared by flush_pending_ack().
    // Keeps rte_eth_tx_burst off the RX latency-critical path.
    bool ack_pending_ = false;

    // 2MSL timer for TIME_WAIT state (RFC 793 §3.5).
    // MSL = 60s (common implementation default). Deadline is set when entering
    // TIME_WAIT; connect() checks it to allow re-use after expiry.
    std::chrono::steady_clock::time_point time_wait_deadline_{};

    // TSC captured right after rte_eth_rx_burst returns data.
    // Used by Transport as the true RX arrival baseline.
    // alignas(64): placed on its own cache line to avoid false sharing with
    // stats_ (written by the RX path) when a separate thread reads the TSC.
    alignas(64) std::atomic<uint64_t> last_rx_burst_tsc_{0};

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

    /// Generate initial sequence number using CSPRNG.
    /// Returns an error if RAND_bytes fails — propagate to caller rather than
    /// masking failure with a sentinel value like 0 or 1.
    static std::expected<uint32_t, std::string> generate_isn() noexcept {
        uint32_t isn = 0;
        if (RAND_bytes(reinterpret_cast<uint8_t*>(&isn), sizeof(isn)) != 1) {
            SPDLOG_LOGGER_CRITICAL(detail::tcp_logger(),
                "RAND_bytes failed for ISN generation — cannot establish "
                "secure TCP connection");
            return std::unexpected("CSPRNG failed");
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

    /// Send a bare ACK packet.
    [[nodiscard]] std::expected<void, std::string> send_ack() {
        auto* ack_pkt = pkt_template_.build_packet(
            pool_, snd_nxt_, rcv_nxt_, net::kTcpAck, rcv_wnd_);
        if (!ack_pkt) {
            return std::unexpected("mbuf allocation failed for ACK");
        }

        uint16_t sent = rte_eth_tx_burst(
            config_.port_id, config_.tx_queue_id, &ack_pkt, 1);
        if (sent != 1) {
            rte_pktmbuf_free(ack_pkt);
            return std::unexpected("tx_burst failed for ACK");
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
