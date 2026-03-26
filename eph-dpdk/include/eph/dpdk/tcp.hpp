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

#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <string>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <openssl/rand.h>

#include <rte_ethdev.h>
#include <rte_mbuf.h>

#include "eph/dpdk/net_header.hpp"
#include "eph/net/tcp_concept.hpp"
#include "eph/utils/time.hpp"

namespace eph::dpdk {

using eph::net::TcpState;
using eph::net::tcp_state_name;

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

struct TcpConfig {
    net::ConnectionTuple tuple{};
    rte_ether_addr       src_mac{};
    rte_ether_addr       dst_mac{};
    uint16_t             mss          = net::kDefaultMss;
    uint32_t             recv_window  = 65535;
    uint16_t             port_id      = 0;
    uint16_t             tx_queue_id  = 0;
    uint16_t             rx_queue_id  = 0;

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
            && a.rx_queue_id  == b.rx_queue_id;
    }

    /// Format a MAC address as "xx:xx:xx:xx:xx:xx".
    [[nodiscard]] static std::string format_mac(const rte_ether_addr& m) {
        return std::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
            m.addr_bytes[0], m.addr_bytes[1], m.addr_bytes[2],
            m.addr_bytes[3], m.addr_bytes[4], m.addr_bytes[5]);
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

inline std::shared_ptr<spdlog::logger> tcp_logger() {
    static auto l = [] {
        auto lg = spdlog::get("dpdk.tcp");
        if (!lg) lg = spdlog::stdout_color_mt("dpdk.tcp");
        // Inherit level from spdlog global default
        return lg;
    }();
    return l;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// TCP Session
// ─────────────────────────────────────────────────────────────────────────────

/// Minimal TCP session for DPDK data plane.
///
/// Supports three-way handshake, data send/recv, ACK generation,
/// and graceful close. No retransmission — packet loss triggers reconnect.
template <size_t ReorderSlots = 8>
class TcpSession {
public:
    struct Stats {
        uint64_t tx_packets      = 0;
        uint64_t rx_packets      = 0;
        uint64_t tx_bytes        = 0;
        uint64_t rx_bytes        = 0;
        uint64_t acks_sent       = 0;
        uint64_t out_of_order    = 0;
        uint64_t resets_received = 0;

        /// Multi-line formatted dump for logging/debugging.
        [[nodiscard]] std::string dump() const {
            return std::format(
                "TcpSession::Stats:\n"
                "  tx_packets: {}\n"
                "  rx_packets: {}\n"
                "  tx_bytes: {}\n"
                "  rx_bytes: {}\n"
                "  acks_sent: {}\n"
                "  out_of_order: {}\n"
                "  resets_received: {}",
                tx_packets, rx_packets, tx_bytes, rx_bytes,
                acks_sent, out_of_order, resets_received);
        }

        /// JSON-formatted stats for monitoring system integration.
        [[nodiscard]] std::string to_json() const {
            return std::format(
                "{{\"tx_packets\":{},\"rx_packets\":{},\"tx_bytes\":{},"
                "\"rx_bytes\":{},\"acks_sent\":{},\"out_of_order\":{},"
                "\"resets_received\":{}}}",
                tx_packets, rx_packets, tx_bytes, rx_bytes,
                acks_sent, out_of_order, resets_received);
        }

        /// Compute delta between two snapshots for interval-based monitoring.
        [[nodiscard]] friend Stats operator-(const Stats& lhs, const Stats& rhs) noexcept {
            return Stats{
                .tx_packets      = lhs.tx_packets      - rhs.tx_packets,
                .rx_packets      = lhs.rx_packets      - rhs.rx_packets,
                .tx_bytes        = lhs.tx_bytes        - rhs.tx_bytes,
                .rx_bytes        = lhs.rx_bytes        - rhs.rx_bytes,
                .acks_sent       = lhs.acks_sent       - rhs.acks_sent,
                .out_of_order    = lhs.out_of_order    - rhs.out_of_order,
                .resets_received = lhs.resets_received  - rhs.resets_received,
            };
        }

        [[nodiscard]] friend bool operator==(const Stats&, const Stats&) = default;
    };

    /// Create a TCP session (does NOT connect yet).
    /// @pre pool must not be nullptr — the session allocates mbufs from it.
    explicit TcpSession(const TcpConfig& config, rte_mempool* pool) noexcept
        : config_(config)
        , pool_(pool)
        , state_(TcpState::Closed)
        , snd_nxt_(generate_isn())
        , snd_una_(snd_nxt_)
        , rcv_nxt_(0)
        , rcv_wnd_(config.recv_window)
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
    TcpSession(TcpSession&&) noexcept        = default;
    TcpSession& operator=(TcpSession&&) noexcept = default;

    // ─────────────────────────────────────────────────────────────────────────
    // Connection establishment
    // ─────────────────────────────────────────────────────────────────────────

    /// Perform TCP three-way handshake (blocking, polls DPDK rx).
    /// @param timeout  Maximum time to wait for SYN-ACK
    /// @return Error string on failure
    [[nodiscard]] std::expected<void, std::string>
    connect(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
        auto log = detail::tcp_logger();

        if (state_ != TcpState::Closed) {
            return std::unexpected(std::format(
                "Cannot connect: session in state {}", tcp_state_name(state_)));
        }

        if (snd_nxt_ == 0 && snd_una_ == 0) {
            SPDLOG_LOGGER_ERROR(log, "ISN generation failed — CSPRNG unavailable");
            return std::unexpected("ISN generation failed: CSPRNG unavailable");
        }

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

        // Wait for SYN-ACK
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
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
            return std::unexpected("tx_burst failed");
        }

        snd_nxt_ += static_cast<uint32_t>(len);
        stats_.tx_packets++;
        stats_.tx_bytes += len;
        return len;
    }

    /// Build a data packet into a pre-allocated mbuf (hot path, no alloc).
    /// Returns the mbuf ready for tx_burst, or nullptr on error.
    /// Caller is responsible for tx_burst and updating stats.
    rte_mbuf* build_data_packet(rte_mbuf* mbuf,
                                const void* data, uint16_t len) noexcept {
        if (state_ != TcpState::Established || len > config_.mss) return nullptr;

        uint16_t written = pkt_template_.fill_packet(
            mbuf, snd_nxt_, rcv_nxt_,
            net::kTcpAck | net::kTcpPsh,
            rcv_wnd_, data, len);
        if (written == 0) return nullptr;

        snd_nxt_ += len;
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
        auto log = detail::tcp_logger();
        uint16_t data_count = 0;
        bool need_ack = false;

        for (uint16_t i = 0; i < nb_pkts; ++i) {
            auto parsed = net::parse_packet(pkts[i]);

            // Skip non-matching packets
            if (!parsed.tcp || !parsed.matches(config_.tuple)) {
                rte_pktmbuf_free(pkts[i]);
                continue;
            }

            stats_.rx_packets++;

            // RST — immediate close
            if (parsed.has_flag(net::kTcpRst)) {
                SPDLOG_LOGGER_WARN(log, "Received RST, closing connection");
                state_ = TcpState::Closed;
                stats_.resets_received++;
                rte_pktmbuf_free(pkts[i]);
                free_remaining(pkts, i + 1, nb_pkts);
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
            }

            // Check sequence number ordering
            if (state_ == TcpState::Established ||
                state_ == TcpState::FinWait1 ||
                state_ == TcpState::FinWait2) {

                uint32_t seg_seq = parsed.seq();

                // Handle out-of-order: buffer the segment for later delivery.
                // af_packet commonly delivers segments out of order within a burst.
                if (seg_seq != rcv_nxt_ && parsed.payload_len > 0) {
                    stats_.out_of_order++;
                    if (seq_after(seg_seq, rcv_nxt_) &&
                        reorder_count_ < ReorderSlots &&
                        parsed.payload_len <= net::kDefaultMss) {
                        // Future segment — buffer it
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
                        SPDLOG_LOGGER_WARN(log,
                            "Reorder buffer full ({} slots): expected={}, got={}",
                            ReorderSlots, rcv_nxt_, seg_seq);
                        rte_pktmbuf_free(pkts[i]);
                        free_remaining(pkts, i + 1, nb_pkts);
                        return std::unexpected(std::format(
                            "Packet loss detected (reorder buffer full): expected seq {}, got {}",
                            rcv_nxt_, seg_seq));
                    }
                    rte_pktmbuf_free(pkts[i]);
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

            // FIN handling
            if (parsed.has_flag(net::kTcpFin)) {
                rcv_nxt_++; // FIN consumes one sequence number
                need_ack = true;

                switch (state_) {
                    case TcpState::Established:
                        SPDLOG_LOGGER_DEBUG(log, "Received FIN in ESTABLISHED");
                        state_ = TcpState::CloseWait;
                        break;
                    case TcpState::FinWait1:
                        if (parsed.has_flag(net::kTcpAck)) {
                            SPDLOG_LOGGER_DEBUG(log, "Received FIN+ACK in FIN_WAIT_1");
                            state_ = TcpState::TimeWait;
                        } else {
                            SPDLOG_LOGGER_DEBUG(log, "Simultaneous close");
                            state_ = TcpState::TimeWait;
                        }
                        break;
                    case TcpState::FinWait2:
                        SPDLOG_LOGGER_DEBUG(log, "Received FIN in FIN_WAIT_2");
                        state_ = TcpState::TimeWait;
                        break;
                    default:
                        break;
                }
            }

            rte_pktmbuf_free(pkts[i]);
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
    /// @return On success: count of data packets processed (may be 0 if no
    ///         data packets in this burst, e.g. pure ACKs). On error: returns
    ///         unexpected with error message; all received packets are freed.
    template <typename F>
        requires std::invocable<F, const uint8_t*, uint16_t>
    [[nodiscard]] std::expected<uint16_t, std::string> poll_rx(F&& data_callback) {
        rte_mbuf* pkts[32];
        uint16_t nb_rx = rte_eth_rx_burst(
            config_.port_id, config_.rx_queue_id, pkts, 32);
        if (nb_rx == 0) return uint16_t{0};
        // Capture TSC immediately after rx_burst — the closest
        // userspace proxy for "packet arrived at NIC ring".
        last_rx_burst_tsc_ = eph::utils::TSC::now();
        return process_rx(pkts, nb_rx, std::forward<F>(data_callback));
    }

    /// TSC captured right after rte_eth_rx_burst returned data.
    /// Transport uses this as the RX arrival baseline instead of
    /// timestamping after poll_rx returns, which would miss the
    /// TCP parsing + reorder + memcpy cost inside process_rx.
    [[nodiscard]] uint64_t last_rx_burst_tsc() const noexcept {
        return last_rx_burst_tsc_;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Connection close
    // ─────────────────────────────────────────────────────────────────────────

    /// Initiate graceful TCP close (send FIN).
    [[nodiscard]] std::expected<void, std::string> close() {
        auto log = detail::tcp_logger();

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
        auto log = detail::tcp_logger();
        SPDLOG_LOGGER_DEBUG(log, "Sending RST");

        auto* rst = pkt_template_.build_packet(
            pool_, snd_nxt_, rcv_nxt_, net::kTcpRst | net::kTcpAck, 0);
        if (rst) {
            rte_eth_tx_burst(config_.port_id, config_.tx_queue_id, &rst, 1);
            // If tx_burst fails, mbuf will be freed by DPDK on port close
        }

        state_ = TcpState::Closed;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // State queries
    // ─────────────────────────────────────────────────────────────────────────

    [[nodiscard]] TcpState state()       const noexcept { return state_; }
    [[nodiscard]] uint32_t snd_nxt()     const noexcept { return snd_nxt_; }
    [[nodiscard]] uint32_t snd_una()     const noexcept { return snd_una_; }
    [[nodiscard]] uint32_t rcv_nxt()     const noexcept { return rcv_nxt_; }
    [[nodiscard]] uint16_t rcv_wnd()     const noexcept { return rcv_wnd_; }
    [[nodiscard]] uint16_t snd_wnd()     const noexcept { return snd_wnd_; }
    [[nodiscard]] uint16_t mss()         const noexcept { return config_.mss; }
    [[nodiscard]] const TcpConfig& config() const noexcept { return config_; }
    [[nodiscard]] Stats    stats()       const noexcept { return stats_; }

    [[nodiscard]] bool is_established() const noexcept {
        return state_ == TcpState::Established;
    }

    /// Get the packet template (for hot path direct mbuf construction).
    [[nodiscard]] net::PacketTemplate& packet_template() noexcept {
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
        uint8_t  data[net::kDefaultMss]{};
    };

    /// Try to deliver buffered segments that are now in-order.
    /// @return Number of segments delivered
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

    // TCP sequence tracking
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

    // TSC captured right after rte_eth_rx_burst returns data.
    // Used by Transport as the true RX arrival baseline.
    uint64_t last_rx_burst_tsc_ = 0;

    /// Generate initial sequence number using CSPRNG.
    /// Returns 0 on CSPRNG failure — caller must treat ISN=0 as connection error.
    static uint32_t generate_isn() noexcept {
        uint32_t isn = 0;
        if (RAND_bytes(reinterpret_cast<uint8_t*>(&isn), sizeof(isn)) != 1) {
            SPDLOG_LOGGER_CRITICAL(detail::tcp_logger(),
                "RAND_bytes failed for ISN generation — cannot establish "
                "secure TCP connection");
            return 0;
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

// std::formatter specialization for TcpSession<>::Stats (default ReorderSlots=8).
// Non-default instantiations share the same Stats layout; call dump() directly.
template <>
struct std::formatter<eph::dpdk::TcpSession<>::Stats> : std::formatter<std::string> {
    auto format(const eph::dpdk::TcpSession<>::Stats& s, auto& ctx) const {
        return std::formatter<std::string>::format(s.dump(), ctx);
    }
};
