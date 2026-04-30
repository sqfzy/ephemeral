#pragma once

/// @file udp_socket.hpp
/// DPDK UDP datagram socket satisfying `eph::net::Datagram`.
///
/// RX checksum validation: gated by PlatformConfig::enable_rx_checksum_offload.
/// When the port is configured with NIC RX checksum offload, mbufs carry
/// `RTE_MBUF_F_RX_{IP,L4}_CKSUM_{GOOD,BAD,UNKNOWN,NONE}` flags. `process_burst_`
/// drops BAD-flagged packets before parse / codec dispatch, counting them
/// in `StreamMetric::kRxBadChecksum`. UNKNOWN / NONE / GOOD are accepted
/// (best-effort — some PMDs emit UNKNOWN on tunnel / VLAN paths even when
/// offload is active). No software fallback by design (HFT budget).
///
/// Architecture:
///
///     user code
///        │
///        v
///     DpdkUdpSocket<C>
///        │  ├── eph::dpdk::UdpSender     (TX path — fixed-peer template)
///        │  ├── C                         (DatagramCodec template param)
///        │  ├── registered 5-tuple        (for Poller routing)
///        │  └── DpdkPoller<>*             (set by Poller::add)
///        │
///        v
///     DpdkPoller<> (lcore burst poll)
///
/// TX uses `UdpSender` for header-precomputed fixed-destination sends.
/// `send_to(data, dst)` is only valid when `dst` matches the configured
/// peer (the underlying template is fixed-peer). RX dispatch comes in via
/// `process_burst_` from DpdkPoller, which parses the UDP packet, extracts
/// the payload, and invokes the codec decode loop.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include <rte_ethdev.h>
#include <rte_mbuf.h>

#include <spdlog/spdlog.h>

#include "eph/core/detail/logger.hpp"
#include "eph/core/codec.hpp"
#include "eph/core/error.hpp"
#include "eph/dpdk/multicast.hpp"   // multicast_mac_from_ip helper
#include "eph/dpdk/packet_parse.hpp"
#include "eph/dpdk/platform.hpp"
#include "eph/dpdk/udp.hpp"
#include "eph/net/concepts.hpp"
#include "eph/net/dpdk/config.hpp"
#include "eph/net/dpdk/detail/mbuf_view.hpp"
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/net/stream_metrics.hpp"
#include "eph/net/stream_snapshot.hpp"

namespace eph::net::dpdk {

namespace detail {

/// @brief Lazily-initialized logger for the DPDK UDP socket subsystem.
inline spdlog::logger* udp_socket_logger() {
    static auto* l = ::eph::core::detail::make_logger("net.dpdk.udp_socket");
    return l;
}

/// @brief Stack scratch size for the datagram-codec auto-response
///        `OutputBuffer` sink. Sized to hold a single max-sized UDP
///        response a codec might synthesize (e.g. MoldUDP64 request-retrans
///        reply). Matches `tcp_stream::detail::kCodecAutoResponseBytes` for
///        parity so both backends treat the auto-response budget the same.
///        Named separately from the tcp_stream constant to keep the two
///        headers mutually independent — either can be included alone.
inline constexpr std::size_t kDatagramAutoResponseBytes = 1024;

} // namespace detail

// ---------------------------------------------------------------------------
// DpdkUdpSocket
// ---------------------------------------------------------------------------

/// @brief Datagram impl backed by `eph::dpdk::UdpSender` (TX) + direct
///        mbuf parse on RX, dispatched by `DpdkPoller<>`.
///
/// @tparam C  DatagramCodec implementation (duck-typed per the
///            `eph::core::DatagramCodec` concept).
template <class C>
class DpdkUdpSocket {
public:
    // ── Associated types (Datagram concept) ──────────────────────────────

    using CodecType   = C;
    using PacketView  = detail::MbufView;
    /// @brief Datagram sink: invoked once per decoded datagram. The span
    ///        carries application-layer bytes post-codec; `peer` is the
    ///        source address the datagram arrived from.
    using OnDatagram  = std::function<void(std::span<const uint8_t>,
                                            const SocketAddr&)>;

    // ── Factory ──────────────────────────────────────────────────────────

    [[nodiscard]] static std::expected<std::unique_ptr<DpdkUdpSocket>, core::ErrorInfo>
    create(UdpConfig cfg) noexcept {
        auto* log = detail::udp_socket_logger();
        SPDLOG_LOGGER_DEBUG(log,
            "DpdkUdpSocket::create: src=0x{:08x}:{} dst=0x{:08x}:{}",
            cfg.legacy.src_ip, cfg.legacy.src_port,
            cfg.legacy.dst_ip, cfg.legacy.dst_port);

        // Validate the legacy UdpConfig first.
        auto verr = cfg.legacy.validate();
        if (!verr.empty()) {
            SPDLOG_LOGGER_WARN(log,
                "DpdkUdpSocket::create: legacy validate failed: {}", verr);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkUdpSocket::create: UdpConfig invalid"});
        }

        auto sender_r = ::eph::dpdk::UdpSender::create(cfg.legacy);
        if (!sender_r) {
            SPDLOG_LOGGER_WARN(log,
                "DpdkUdpSocket::create: UdpSender::create failed: {}",
                sender_r.error());
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkUdpSocket::create: UdpSender::create failed"});
        }

        auto sock = std::unique_ptr<DpdkUdpSocket>(
            new DpdkUdpSocket(std::move(cfg), std::move(*sender_r)));
        SPDLOG_LOGGER_INFO(log,
            "DpdkUdpSocket::create: ready, peer=0x{:08x}:{}",
            sock->cfg_.legacy.dst_ip, sock->cfg_.legacy.dst_port);
        return sock;
    }

    /// @brief TD-2 injection hook. `create_and_attach` copies the
    /// effective Platform strict flag into the socket so the hot path
    /// can branch on a cheap stack-local `bool`. Not for user code.
    void set_strict_rx_checksum_(bool v) noexcept { strict_rx_cksum_ = v; }

    /// @brief Turnkey factory: create a UDP socket and attach it to the
    /// per-queue Poller already registered with `platform`. Mirrors
    /// `DpdkTcpStream::create_and_attach`. UDP has no connect handshake,
    /// so the RSS+pin path simply rebinds the src_port in `cfg.legacy`
    /// before sender construction.
    [[nodiscard]] static std::expected<std::unique_ptr<DpdkUdpSocket>, core::ErrorInfo>
    create_and_attach(UdpConfig cfg, ::eph::dpdk::Platform& platform) noexcept {
        auto* log = detail::udp_socket_logger();

        const auto mode = platform.dispatch_mode();
        const uint16_t nb_q = platform.nb_rx_queues();
        if (nb_q == 0) {
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "create_and_attach: Platform has 0 RX queues"});
        }

        uint16_t target_qid = 0;

        if (mode == ::eph::net::dpdk::RxDispatchMode::Software) {
            if (cfg.pin_to_queue && *cfg.pin_to_queue != 0) {
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "create_and_attach: pin_to_queue != 0 in Software mode"});
            }
            target_qid = 0;
        } else if (mode == ::eph::net::dpdk::RxDispatchMode::RssPartitioned) {
            // Mirror of tcp_stream.hpp's RSS-aware fix (reshape/
            // rss-aware-connect): always engineer src_port via
            // find_src_port_for_queue so the inbound reply
            // Toeplitz-hashes onto a queue this process owns.
            // Pre-fix the no-pin branch used predict_rss_queue on
            // the caller's random src_port and then required a
            // poller on whichever queue the hash happened to land —
            // which under autojoin would frequently be a queue
            // owned by a different peer process (manifested as
            // "no Poller registered for target queue" at attach
            // time). Single-process callers see no behavioural
            // change because all queues belong to one Platform.
            if (cfg.pin_to_queue) {
                const uint16_t want = *cfg.pin_to_queue;
                if (want >= nb_q) {
                    return std::unexpected(core::ErrorInfo{
                        core::Error::InvalidConfig,
                        "create_and_attach: pin_to_queue >= nb_rx_queues"});
                }
                target_qid = want;
            } else {
                static std::atomic<uint16_t> rr_counter{0};
                const auto [qlo, qhi] = platform.effective_rx_queue_range();
                if (qhi <= qlo) {
                    return std::unexpected(core::ErrorInfo{
                        core::Error::InvalidConfig,
                        "create_and_attach: empty effective_rx_queue_range "
                        "(Platform moved-from or misconfigured)"});
                }
                const uint16_t qrange = qhi - qlo;
                target_qid = qlo + (rr_counter.fetch_add(1,
                                std::memory_order_relaxed) % qrange);
            }

            // RSS input "src" is the REMOTE end on the inbound reply
            // (peer→local). find_src_port_for_queue puts the local sp
            // in the dst_port slot. self_port_range narrows the search
            // to this peer's autojoin-derived window when present.
            const auto pr = platform.self_port_range();
            const uint16_t port_lo_arg =
                pr ? static_cast<uint16_t>(pr->first)
                   : uint16_t{32768};
            const uint16_t port_hi_arg =
                pr ? static_cast<uint16_t>(pr->second - 1)
                   : uint16_t{60999};
            auto sp = ::eph::net::dpdk::find_src_port_for_queue(
                platform.port_id(), target_qid,
                /*remote_ip=*/  cfg.legacy.dst_ip,
                /*remote_port=*/cfg.legacy.dst_port,
                /*local_ip=*/   cfg.legacy.src_ip,
                port_lo_arg, port_hi_arg);
            if (!sp) {
                SPDLOG_LOGGER_WARN(log,
                    "create_and_attach: find_src_port_for_queue({}) failed: {}",
                    target_qid, sp.error());
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "create_and_attach: find_src_port_for_queue exhausted"});
            }
            cfg.legacy.src_port = *sp;
            // TX queue alignment: same family as RX. UdpConfig has no
            // rx_queue_id field by design (Poller owns the RX queue);
            // see the long comment removed alongside this rewrite.
            cfg.legacy.tx_queue_id = target_qid;
            SPDLOG_LOGGER_INFO(log,
                "create_and_attach: RSS-aware → src_port={} hashes to queue={} (pin={})",
                *sp, target_qid, cfg.pin_to_queue.has_value());
        } else {  // FlowDirector
            if (cfg.pin_to_queue && *cfg.pin_to_queue >= nb_q) {
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "create_and_attach: pin_to_queue >= nb_rx_queues"});
            }
            // UDP has no handshake, so the FD-mode handshake-race that
            // bites tcp_stream.hpp's FD branch (see its KNOWN LIMITATION
            // comment) is moot here for connection establishment. But the
            // post-attach steady-state is still subject to the same shape:
            // the rte_flow rule is installed AFTER create() returns, so any
            // datagram arriving in the brief window between create() and
            // install_flow_rule() lands wherever default RSS routes it,
            // not target_qid. For UDP this manifests as a small initial
            // burst going to the wrong queue and being silently dropped by
            // the per-queue Poller's 5-tuple demux. Acceptable for the
            // typical UDP use case (multicast joins, request/reply) but
            // pathological for low-latency one-shot datagrams. See the
            // tcp_stream.hpp comment for the full rationale and proposed
            // fix shape.
            //
            // Default queue selection: round-robin via a static counter.
            // Range-aware so multi-process setups can partition queues via
            // PlatformConfig::rx_queue_range; single-process default
            // `{0, 0}` resolves to `[0, nb_rx_queues)` which matches the
            // prior `% nb_q` behavior byte-for-byte. `validate_config`
            // guarantees `qlo < qhi`, so no runtime fallback is needed.
            if (cfg.pin_to_queue) {
                target_qid = *cfg.pin_to_queue;
            } else {
                static std::atomic<uint16_t> rr_counter{0};
                const auto [qlo, qhi] = platform.effective_rx_queue_range();
                // Defense in depth: empty range (e.g. moved-from Platform
                // returns {0, 0}) would make `% qrange` UB. Mirror the
                // TCP path's guard so both protocols fail with the same
                // explicit error rather than crash silently.
                if (qhi <= qlo) {
                    return std::unexpected(core::ErrorInfo{
                        core::Error::InvalidConfig,
                        "create_and_attach: empty effective_rx_queue_range "
                        "(Platform moved-from or misconfigured)"});
                }
                const uint16_t qrange = qhi - qlo;
                target_qid = qlo + (rr_counter.fetch_add(1,
                                std::memory_order_relaxed) % qrange);
            }
        }

        // ── Optional: resolve per-lcore mempool hint ─────────────────────────
        //
        // When `cfg.pool_lcore_hint >= 0`, override `cfg.legacy.pool` with
        // the Platform's per-lcore pool for that lcore id. NUMA-aware
        // alloc path (T2.9). When the hint is -1 (default) we leave
        // `cfg.legacy.pool` untouched.
        if (cfg.pool_lcore_hint >= 0) {
            const auto lcore_id =
                static_cast<uint16_t>(cfg.pool_lcore_hint);
            auto* p = platform.pool_for_lcore(lcore_id);
            if (p == nullptr) {
                SPDLOG_LOGGER_WARN(log,
                    "create_and_attach: pool_for_lcore({}) returned nullptr "
                    "(per_lcore_pools may be 0 with non-zero hint, or hint "
                    "exceeds per_lcore_pools)", lcore_id);
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "create_and_attach: pool_for_lcore lookup returned nullptr"});
            }
            cfg.legacy.pool = p;
        }

        auto sr = create(std::move(cfg));
        if (!sr) return std::unexpected(sr.error());
        auto sock = std::move(*sr);

        // TD-2: propagate effective strict mode from Platform. Only set
        // here (not in plain create()) because create() has no Platform
        // reference; unattached sockets stay in non-strict best-effort.
        sock->set_strict_rx_checksum_(platform.strict_rx_checksum());

        // Attach BEFORE installing the flow rule (see tcp_stream.hpp's
        // create_and_attach for the race-window rationale).
        auto* poller = platform.poller_for_queue(target_qid);
        if (poller == nullptr) {
            return std::unexpected(core::ErrorInfo{
                core::Error::NotAttached,
                "create_and_attach: no Poller registered for target queue"});
        }
        auto add_r = poller->add(sock.get());
        if (!add_r) return std::unexpected(add_r.error());

        if (mode == ::eph::net::dpdk::RxDispatchMode::FlowDirector) {
            ::eph::dpdk::net::ConnectionTuple ft{
                .src_ip   = sock->cfg_.legacy.src_ip,
                .dst_ip   = sock->cfg_.legacy.dst_ip,
                .src_port = sock->cfg_.legacy.src_port,
                .dst_port = sock->cfg_.legacy.dst_port};
            auto rule = ::eph::net::dpdk::install_flow_rule(
                platform.port_id(), target_qid, ft,
                ::eph::net::dpdk::FlowProtocol::Udp);
            // Same try-secondary-then-fallback as DpdkTcpStream — see
            // tcp_stream.hpp for the full rationale.
            if (!rule && platform.is_secondary() &&
                platform.has_mp_topology()) {
                SPDLOG_LOGGER_WARN(log,
                    "create_and_attach: local rte_flow_create rejected "
                    "({}); trying eph_fd_install IPC fallback",
                    rule.error());
                rule = ::eph::net::dpdk::try_install_flow_rule_via_ipc(
                    platform.port_id(), target_qid, ft,
                    ::eph::net::dpdk::FlowProtocol::Udp);
            }
            if (!rule) {
                SPDLOG_LOGGER_WARN(log,
                    "create_and_attach: install_flow_rule failed: {}",
                    rule.error());
                (void)poller->remove(sock.get());
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "create_and_attach: install_flow_rule failed"});
            }
            // Move into the socket so ~FlowRule cleans up at teardown.
            sock->flow_rule_.emplace(std::move(*rule));
        }

        SPDLOG_LOGGER_INFO(log,
            "create_and_attach: UDP socket attached → port={}, queue={}, mode={}",
            platform.port_id(), target_qid,
            ::eph::net::dpdk::rx_dispatch_mode_name(mode));
        return sock;
    }

    ~DpdkUdpSocket() {
        // See the symmetric rationale in ~DpdkTcpStream — a failing remove
        // here signals a Poller/Socket lifecycle mismatch and should not
        // be silently swallowed.
        if (attached_to_ != nullptr) {
            SPDLOG_LOGGER_DEBUG(detail::udp_socket_logger(),
                "~DpdkUdpSocket: auto-detach");
            auto r = attached_to_->remove(this);
            if (!r) {
                SPDLOG_LOGGER_WARN(detail::udp_socket_logger(),
                    "~DpdkUdpSocket: auto-detach failed: {} — possible "
                    "Poller/Socket lifecycle mismatch",
                    r.error().detail ? r.error().detail : "unknown");
            }
        }
        // Symmetric with `~MulticastReceiver::leave_all_groups()`: any
        // multicast MAC filters this socket pushed to the NIC via
        // `join_multicast()` must be torn down here. Without this the
        // filters survive socket destruction; the next process / user
        // joining on the same port faces stale state, and after enough
        // socket churn the NIC's finite mcast filter table (commonly
        // 8-16 slots on AWS ENA) is exhausted.
        if (mcast_count_ > 0) {
            SPDLOG_LOGGER_DEBUG(detail::udp_socket_logger(),
                "~DpdkUdpSocket: clearing {} multicast MAC filter(s)",
                mcast_count_);
            mcast_count_ = 0;
            // Wipe the slot bytes so `mcast_macs_` no longer reports
            // stale MACs to anything that might inspect it (defensive —
            // the dtor is the last reader, but match the join/leave
            // pattern that zeroes evicted slots).
            for (auto& m : mcast_macs_) m = rte_ether_addr{};
            // Best effort — return-value ignored. We're in a dtor and
            // a failing teardown is logged inside apply_mcast_list_()
            // via its WARN path; no caller could act on a dtor return.
            (void)apply_mcast_list_();
        }
    }

    DpdkUdpSocket(const DpdkUdpSocket&)            = delete;
    DpdkUdpSocket& operator=(const DpdkUdpSocket&) = delete;
    DpdkUdpSocket(DpdkUdpSocket&&)                 = delete;
    DpdkUdpSocket& operator=(DpdkUdpSocket&&)      = delete;

    // ── Public fields (Datagram concept) ─────────────────────────────────

    OnDatagram on_datagram;

    // ── Datagram concept API ─────────────────────────────────────────────

    [[nodiscard]] std::expected<std::size_t, core::ErrorInfo>
    send_to(std::span<const uint8_t> app_payload, const SocketAddr& dst) noexcept {
        if (attached_to_ == nullptr) {
            return std::unexpected(core::ErrorInfo{
                core::Error::NotAttached,
                "DpdkUdpSocket::send_to called before attach"});
        }
        // Legacy UdpSender is fixed-peer; reject dst mismatches so user
        // code gets a clear error instead of silent misdelivery.
        if (dst.ip.to_be32() != cfg_.legacy.dst_ip ||
            dst.port         != cfg_.legacy.dst_port) {
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkUdpSocket::send_to: dst does not match configured peer "
                "(fixed-peer UdpSender)"});
        }
        // UDP on IPv4 is capped not by the raw UDP length field
        // (uint16_t) but by the enclosing IPv4 total_length field
        // (also uint16_t), which must include 20 bytes of IP header +
        // 8 bytes of UDP header + payload. The real maximum payload
        // is therefore 65535 - 28 = 65507 bytes. Add the Ethernet
        // header (14 bytes) and the frame layout itself caps at
        // 65535 - 42 = 65493 bytes — which is what `UdpPacketTemplate::
        // fill` enforces downstream. Reject at the API boundary with
        // an InvalidConfig instead of letting the user discover a
        // BufferFull from the template. The 42-byte constant matches
        // `eph::dpdk::net::kUdpAllHeadersLen`.
        static constexpr std::size_t kMaxUdpPayload =
            0xFFFFu - ::eph::dpdk::net::kUdpAllHeadersLen;
        if (app_payload.size() > kMaxUdpPayload) {
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkUdpSocket::send_to: payload exceeds max UDP-over-IP "
                "frame size (65493 bytes)"});
        }
        if (!sender_.send(app_payload.data(),
                           static_cast<uint16_t>(app_payload.size()))) {
            return std::unexpected(core::ErrorInfo{
                core::Error::BufferFull,
                "DpdkUdpSocket::send_to: UdpSender::send failed"});
        }
        inc_<::eph::net::StreamMetric::kBytesSent>(app_payload.size());
        return app_payload.size();
    }

    /// @brief Subscribe to a multicast group at the NIC MAC-filter layer.
    ///
    /// Derives the multicast Ethernet MAC from the group IP per RFC 1112,
    /// appends it to our per-socket MAC list, and pushes the rebuilt list
    /// to the NIC via `rte_eth_dev_set_mc_addr_list`. Per-socket
    /// connect_to / 5-tuple steering is handled separately by
    /// `connect_to()`.
    ///
    /// Returns `InvalidConfig` if `group.ip` is not in the 224.0.0.0/4
    /// multicast range, or `BufferFull` if the per-socket cap is reached.
    [[nodiscard]] std::expected<void, core::ErrorInfo>
    join_multicast(const SocketAddr& group) noexcept {
        const uint32_t ip_be = group.ip.to_be32();
        if (!::eph::dpdk::is_multicast_ip(ip_be)) {
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkUdpSocket::join_multicast: ip not in 224.0.0.0/4"});
        }
        if (mcast_count_ >= mcast_macs_.size()) {
            return std::unexpected(core::ErrorInfo{
                core::Error::BufferFull,
                "DpdkUdpSocket::join_multicast: too many groups"});
        }
        const rte_ether_addr mac = ::eph::dpdk::multicast_mac_from_ip(ip_be);
        // Idempotent — skip if already joined.
        for (std::size_t i = 0; i < mcast_count_; ++i) {
            if (std::memcmp(&mcast_macs_[i], &mac, sizeof(mac)) == 0) {
                return {};
            }
        }
        // Stage the MAC, then push to the NIC. On a NIC-level failure,
        // roll back the in-process count so it stays in lock-step with
        // what the NIC accepted; otherwise the next join_multicast
        // would treat the failed MAC as already-joined and silently
        // skip pushing a real (different) group later. Same rollback
        // pattern as MulticastReceiver::join_group in multicast.hpp.
        mcast_macs_[mcast_count_++] = mac;
        if (auto r = apply_mcast_list_(); !r) {
            --mcast_count_;
            mcast_macs_[mcast_count_] = rte_ether_addr{};
            return std::unexpected(std::move(r.error()));
        }
        return {};
    }

    [[nodiscard]] std::expected<void, core::ErrorInfo>
    leave_multicast(const SocketAddr& group) noexcept {
        const uint32_t ip_be = group.ip.to_be32();
        if (!::eph::dpdk::is_multicast_ip(ip_be)) {
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkUdpSocket::leave_multicast: ip not in 224.0.0.0/4"});
        }
        const rte_ether_addr mac = ::eph::dpdk::multicast_mac_from_ip(ip_be);
        for (std::size_t i = 0; i < mcast_count_; ++i) {
            if (std::memcmp(&mcast_macs_[i], &mac, sizeof(mac)) == 0) {
                // Snapshot the slot for rollback before compacting:
                // if apply_mcast_list_ rejects (NIC ENOSPC / ENOTSUP /
                // -EAGAIN), the in-process list must continue to claim
                // the group is joined — otherwise a follow-up join of
                // the same group would no-op (the membership check
                // wouldn't find it) while the NIC still has the
                // filter installed, and the operator's view diverges
                // from reality. Symmetric with join_multicast's
                // rollback above.
                const auto saved = mcast_macs_[i];
                const std::size_t leave_idx = i;
                for (std::size_t j = i; j + 1 < mcast_count_; ++j) {
                    mcast_macs_[j] = mcast_macs_[j + 1];
                }
                --mcast_count_;
                mcast_macs_[mcast_count_] = rte_ether_addr{};
                if (auto r = apply_mcast_list_(); !r) {
                    // Re-insert at the original slot. Shift entries
                    // [leave_idx..mcast_count_) one back to make room,
                    // then restore the saved MAC and bump the count.
                    for (std::size_t j = mcast_count_; j > leave_idx; --j) {
                        mcast_macs_[j] = mcast_macs_[j - 1];
                    }
                    mcast_macs_[leave_idx] = saved;
                    ++mcast_count_;
                    return std::unexpected(std::move(r.error()));
                }
                return {};
            }
        }
        // Not joined — be idempotent.
        return {};
    }

    /// @brief Pin per-socket inbound to a specific source 4-tuple via NIC
    ///        flow steering. Until a higher-level FlowSteering manager
    ///        exists in eph-net-dpdk, this records the peer for the
    ///        Poller's per-tuple dispatch — the existing
    ///        `tuple_for_poller_` machinery already routes inbound
    ///        packets by 5-tuple, so updating the configured peer is
    ///        sufficient to redirect inbound dispatch.
    [[nodiscard]] std::expected<void, core::ErrorInfo>
    connect_to(const SocketAddr& peer) noexcept {
        // The legacy UdpSender is fixed-peer (built around a precomputed
        // packet template), so we can't change the TX peer mid-stream.
        // What we CAN do is update the inbound dispatch tuple — the
        // Poller's routing table keys on (src_ip, src_port, dst_ip, dst_port).
        //
        // Reject peer mismatches up front: if `peer` differs from the
        // fixed TX destination, we'd end up sending to cfg.legacy.dst_*
        // but filtering inbound on `peer`, so replies from the real
        // server would be silently dropped and the socket would appear
        // hung. Fail loudly instead.
        if (peer.ip.to_be32() != cfg_.legacy.dst_ip ||
            peer.port         != cfg_.legacy.dst_port) {
            SPDLOG_LOGGER_WARN(detail::udp_socket_logger(),
                "DpdkUdpSocket::connect_to: peer ip_be=0x{:08x}:{} does not "
                "match configured fixed peer ip_be=0x{:08x}:{} — would silently "
                "drop all inbound traffic",
                peer.ip.to_be32(), peer.port,
                cfg_.legacy.dst_ip, cfg_.legacy.dst_port);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkUdpSocket::connect_to: peer does not match configured "
                "fixed peer (UdpSender is precomputed-template, fixed-peer)"});
        }
        connected_peer_ = peer;
        connected_     = true;
        SPDLOG_LOGGER_DEBUG(detail::udp_socket_logger(),
            "DpdkUdpSocket::connect_to: peer set ip_be=0x{:08x} port={}",
            peer.ip.to_be32(), peer.port);
        return {};
    }

    [[nodiscard]] bool is_attached() const noexcept {
        return attached_to_ != nullptr;
    }

    // ── Pollable concept API ─────────────────────────────────────────────

    /// @brief Single-poll driver for concept conformance. DPDK lcore
    ///        dispatch goes via `process_burst_` — this exists purely so
    ///        the `Pollable` concept is satisfied.
    std::size_t poll_once_() noexcept {
        // No-op: the Poller hands mbufs via process_burst_ directly.
        return 0;
    }

    /// @brief Pollable's is_attached hook — forwards to the public
    ///        `is_attached()`. See DpdkTcpStream's matching comment for
    ///        the concept-dispatch rationale.
    [[nodiscard]] bool is_attached_() const noexcept {
        return is_attached();
    }

    /// @brief Pollable native_handle — the UdpSender pointer (distinct
    ///        from a kernel fd).
    [[nodiscard]] void* native_handle() const noexcept {
        return const_cast<void*>(static_cast<const void*>(&sender_));
    }

    // ── Poller-facing friend hooks ───────────────────────────────────────

    void notify_attached_(DpdkPoller<void>* p) noexcept { attached_to_ = p; }
    void notify_detached_() noexcept { attached_to_ = nullptr; }

    /// @brief Supplies the registered 5-tuple to the Poller at add-time.
    ///        `*proto` is set to `kIpProtoUdp` so the poller routing table
    ///        can coexist with TCP Pollables sharing the same 4-tuple.
    void tuple_for_poller_(uint32_t* src_ip, uint32_t* dst_ip,
                            uint16_t* src_port, uint16_t* dst_port,
                            uint8_t*  proto) noexcept {
        *src_ip   = cfg_.legacy.src_ip;
        *dst_ip   = cfg_.legacy.dst_ip;
        *src_port = cfg_.legacy.src_port;
        *dst_port = cfg_.legacy.dst_port;
        *proto    = eph::dpdk::net::kIpProtoUdp;
    }

    /// @brief Per-poll-cycle tick. UDP has no session-level periodic
    ///        work analogous to TCP keepalive, so this is an inline
    ///        no-op — GCC -O2 compiles it out of the Poller's tick
    ///        sweep entirely.
    void on_poll_tick_(uint64_t /*tsc*/) noexcept {}

    /// @brief Hot-path burst dispatch from DpdkPoller. Parses each mbuf
    ///        as a UDP packet, wraps the payload in an MbufView, and
    ///        drives the codec. Must stay noexcept and allocation-free.
    void process_burst_(rte_mbuf** mbufs, uint16_t n,
                         uint64_t rx_tsc) noexcept {
        if (!on_datagram) {
            for (uint16_t i = 0; i < n; ++i) rte_pktmbuf_free(mbufs[i]);
            return;
        }
        // Hot-path RX checksum drop. Two modes, controlled by
        // `strict_rx_cksum_` (set from Platform::strict_rx_checksum() at
        // create_and_attach time — see TD-2):
        //   strict_rx_cksum_ == false (default, best-effort):
        //     drop iff (olf & IP_CKSUM_BAD) | (olf & L4_CKSUM_BAD) set.
        //     UNKNOWN / GOOD / NONE all accepted.
        //   strict_rx_cksum_ == true:
        //     drop iff (olf & IP_CKSUM_MASK) != IP_CKSUM_GOOD
        //           || (olf & L4_CKSUM_MASK) != L4_CKSUM_GOOD.
        //     Only explicitly GOOD packets admitted.
        // In both modes the branch is `[[unlikely]]` because opt-in off
        // keeps BAD/NONE bits clear anyway, and even with opt-in on a
        // healthy colo link reports GOOD > 99.99%.
        //
        // Drop attribution (TD-1): split into kRxIpChecksumBad /
        // kRxL4ChecksumBad by which layer is the problem. Invariant:
        //   kRxBadChecksum == kRxIpChecksumBad + kRxL4ChecksumBad
        // holds in both modes because metric(kRxBadChecksum) reads the
        // sum lazily and we never increment the aggregate directly.
        constexpr uint64_t kRxIpCksumBad  =
            static_cast<uint64_t>(RTE_MBUF_F_RX_IP_CKSUM_BAD);
        constexpr uint64_t kRxL4CksumBad  =
            static_cast<uint64_t>(RTE_MBUF_F_RX_L4_CKSUM_BAD);
        constexpr uint64_t kRxIpCksumMask =
            static_cast<uint64_t>(RTE_MBUF_F_RX_IP_CKSUM_MASK);
        constexpr uint64_t kRxL4CksumMask =
            static_cast<uint64_t>(RTE_MBUF_F_RX_L4_CKSUM_MASK);
        constexpr uint64_t kRxIpCksumGood =
            static_cast<uint64_t>(RTE_MBUF_F_RX_IP_CKSUM_GOOD);
        constexpr uint64_t kRxL4CksumGood =
            static_cast<uint64_t>(RTE_MBUF_F_RX_L4_CKSUM_GOOD);
        const bool strict = strict_rx_cksum_;  // stack-local for codegen
        for (uint16_t i = 0; i < n; ++i) {
            const uint64_t olf = mbufs[i]->ol_flags;
            // TD-6 precision: non-strict must test `(olf & MASK) == BAD`,
            // NOT `(olf & BAD_bit) != 0`. DPDK encodes NONE as
            // `BAD_bit | GOOD_bit`, so the naive bit test also matches
            // NONE — which would false-drop RFC 768 zero-checksum UDP
            // datagrams. Using the mask == BAD equality makes non-strict
            // drop exactly BAD (and nothing else). Strict uses `!= GOOD`
            // so it drops every non-GOOD value (UNKNOWN, BAD, NONE).
            const bool ip_bad = strict
                ? ((olf & kRxIpCksumMask) != kRxIpCksumGood)
                : ((olf & kRxIpCksumMask) == kRxIpCksumBad);
            const bool l4_bad = strict
                ? ((olf & kRxL4CksumMask) != kRxL4CksumGood)
                : ((olf & kRxL4CksumMask) == kRxL4CksumBad);
            if (ip_bad || l4_bad) [[unlikely]] {
                if (ip_bad) inc_<::eph::net::StreamMetric::kRxIpChecksumBad>();
                if (l4_bad) inc_<::eph::net::StreamMetric::kRxL4ChecksumBad>();
                SPDLOG_LOGGER_TRACE(detail::udp_socket_logger(),
                    "process_burst_: drop bad-checksum mbuf ol_flags={:#018x}"
                    " (strict={})", olf, strict);
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }
            auto parsed = ::eph::dpdk::net::parse_udp_packet(mbufs[i]);
            // Accept zero-length payloads per RFC 768 — only reject
            // truly unparseable packets (no UDP header).
            if (!parsed.udp) {
                // Attribute the drop cause: parse_ip_header rejected
                // fragments wholesale, so distinguish "fragment" (operational
                // signal — check DF/MTU) from "other malformed" (catch-all).
                if (::eph::dpdk::net::is_ip_fragment(mbufs[i])) {
                    inc_<::eph::net::StreamMetric::kFragmentRejected>();
                } else {
                    inc_<::eph::net::StreamMetric::kPacketsDropped>();
                }
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }

            SocketAddr src_addr{
                Ipv4Addr::from_be32(parsed.src_ip()),
                parsed.src_port()
            };

            // connect_to() filter — drop packets whose source does not
            // match the configured peer. Attribute to kPacketsDropped —
            // flow-steering misconfiguration is the usual cause.
            if (connected_) {
                if (src_addr.ip.to_be32() != connected_peer_.ip.to_be32() ||
                    src_addr.port         != connected_peer_.port) {
                    inc_<::eph::net::StreamMetric::kPacketsDropped>();
                    rte_pktmbuf_free(mbufs[i]);
                    continue;
                }
            }

            // Wrap the payload in an MbufView. The memory is owned by
            // the mbuf, which stays alive for the entire on_datagram
            // callback scope.
            detail::MbufView view(const_cast<uint8_t*>(parsed.payload),
                                   parsed.payload_len, rx_tsc);
            inc_<::eph::net::StreamMetric::kBytesRecv>(parsed.payload_len);

            uint8_t            scratch[detail::kDatagramAutoResponseBytes];
            core::OutputBuffer out_sink(scratch, sizeof(scratch));

            // Track frames delivered before any decode error so the
            // ERROR log below can report partial success. Mirrors the
            // kernel backend's poll_once_ (`delivered_before_err`) so
            // operators reading either backend's logs see the same
            // diagnostic shape — UDP datagram-loss attribution must
            // not depend on which transport the venue is using.
            std::size_t delivered = 0;
            auto sink = [&](auto&& frame) {
                if (frame.size() > 0 && on_datagram) {
                    on_datagram(std::span<const uint8_t>(
                                    frame.data(), frame.size()),
                                src_addr);
                    inc_<::eph::net::StreamMetric::kFramesDecoded>();
                    ++delivered;
                }
            };
            auto dr = codec_.decode(view, out_sink, sink);

            // Flush any auto-response bytes the codec wrote into
            // `out_sink` back to the source peer. Latent fix symmetric
            // with the kernel backend (5c44e99) and the TCP drain_codec_
            // flush (7401196). No in-tree DatagramCodec currently writes
            // to out_sink, so this is dormant today. Note: `UdpSender`
            // is fixed-peer — send_to only succeeds if `src_addr` matches
            // the configured peer, which `connected_` mode above already
            // enforces. On other (broadcast / multicast) configurations
            // the send_to may be rejected as dst mismatch; we WARN-log
            // and keep going.
            if (out_sink.size() > 0) {
                auto sr = this->send_to(
                    std::span<const uint8_t>(out_sink.data(), out_sink.size()),
                    src_addr);
                if (!sr) {
                    SPDLOG_LOGGER_WARN(detail::udp_socket_logger(),
                        "DpdkUdpSocket::process_burst_: auto-response "
                        "send_to failed ({} bytes to {}): {}",
                        out_sink.size(), src_addr.to_string(),
                        sr.error().detail);
                } else {
                    SPDLOG_LOGGER_DEBUG(detail::udp_socket_logger(),
                        "DpdkUdpSocket::process_burst_: sent {} "
                        "auto-response bytes to {}",
                        out_sink.size(), src_addr.to_string());
                }
            }

            if (!dr) {
                // Elevate to ERROR + include {src, payload_len,
                // delivered_before_err} for parity with KernelUdpSocket
                // (5c44e99 shape). A decode failure on a datagram is
                // not state-corrupting on UDP (per-packet codec, no
                // session) but it IS a market-data-loss event the
                // operator must see. The shared shape lets a venue
                // running both backends grep one query across logs.
                inc_<::eph::net::StreamMetric::kCodecErrors>();
                SPDLOG_LOGGER_ERROR(detail::udp_socket_logger(),
                    "DpdkUdpSocket::process_burst_: decode err={} "
                    "src={} payload_len={} delivered_before_err={}",
                    dr.error().detail, src_addr.to_string(),
                    static_cast<std::size_t>(parsed.payload_len),
                    delivered);
            }
            rte_pktmbuf_free(mbufs[i]);
        }
    }

private:
    DpdkUdpSocket(UdpConfig cfg, ::eph::dpdk::UdpSender sender) noexcept
        : cfg_(std::move(cfg)), sender_(std::move(sender)) {}

    /// @brief Push the current `mcast_macs_[0..mcast_count_)` list to the
    ///        NIC via `rte_eth_dev_set_mc_addr_list`. Called from join /
    ///        leave; safe to call when the list is empty (uninstalls).
    [[nodiscard]] std::expected<void, core::ErrorInfo>
    apply_mcast_list_() noexcept {
        const uint16_t port = cfg_.legacy.port_id;
        const int rc = ::rte_eth_dev_set_mc_addr_list(
            port,
            mcast_count_ > 0 ? mcast_macs_.data() : nullptr,
            static_cast<uint32_t>(mcast_count_));
        if (rc != 0) {
            // Carry rte_errno into both the log line and the surfaced
            // error detail. -ENOTSUP / -ENOSPC / -EINVAL all show up
            // here; without rte_errno operators see only "rc=-95".
            // (rte_errno.h is brought in transitively via multicast.hpp.)
            const int err = rte_errno;
            SPDLOG_LOGGER_WARN(detail::udp_socket_logger(),
                "DpdkUdpSocket::apply_mcast_list_: "
                "rte_eth_dev_set_mc_addr_list failed: rc={} count={} port={} "
                "rte_errno={} ({})",
                rc, mcast_count_, port, err, rte_strerror(err));
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "rte_eth_dev_set_mc_addr_list failed (see rte_errno log)"});
        }
        SPDLOG_LOGGER_DEBUG(detail::udp_socket_logger(),
            "DpdkUdpSocket::apply_mcast_list_: count={} port={}",
            mcast_count_, port);
        return {};
    }

    UdpConfig                              cfg_{};
    ::eph::dpdk::UdpSender                 sender_;
    [[no_unique_address]] C                codec_{};
    DpdkPoller<void>*                      attached_to_{nullptr};
    /// @brief RAII handle for the rte_flow rule installed by
    /// `create_and_attach` in FlowDirector mode. See the matching field
    /// on `DpdkTcpStream` for the lifecycle contract.
    std::optional<::eph::net::dpdk::FlowRule> flow_rule_{};
    /// Per-socket multicast MAC list. Capped at
    /// `::eph::dpdk::kMaxMulticastGroups` (defined in multicast.hpp) so both
    /// the legacy MulticastReceiver and this socket agree on the per-feed
    /// subscription ceiling — raising one without the other would silently
    /// diverge. Pushed to the NIC via `rte_eth_dev_set_mc_addr_list` on
    /// join / leave.
    std::array<rte_ether_addr, ::eph::dpdk::kMaxMulticastGroups> mcast_macs_{};
    std::size_t                            mcast_count_{0};
    /// connect_to() bookkeeping — when set, packets from non-matching
    /// peers are filtered by the Poller's tuple dispatch.
    SocketAddr                             connected_peer_{};
    bool                                   connected_{false};
    /// TD-2 strict RX checksum mode. Off by default. `create_and_attach`
    /// injects the effective `Platform::strict_rx_checksum()` value;
    /// plain `create()` leaves it at the safe best-effort default.
    bool                                   strict_rx_cksum_{false};

    // ── Hot-path metric counters (pull model — see stream_metrics.hpp) ──
    //
    // UDP backend N/A entries: kReasmOverflows, kCodecErrors (TLS-only
    // metric N/A here), kTlsCrossRecordFrames — all stay at 0.

    struct alignas(64) Counter { std::atomic<std::uint64_t> v{0}; };

    std::array<Counter,
               static_cast<std::size_t>(::eph::net::StreamMetric::kCount)>
        counters_{};

    template <::eph::net::StreamMetric M>
    void inc_(std::uint64_t n = 1) noexcept {
        counters_[static_cast<std::size_t>(M)]
            .v.fetch_add(n, std::memory_order_relaxed);
    }

public:
    [[nodiscard]] std::uint64_t metric(::eph::net::StreamMetric m) const noexcept {
        // Defensive bounds check — see DpdkTcpStream::metric for rationale.
        if (static_cast<std::size_t>(m) >=
            static_cast<std::size_t>(::eph::net::StreamMetric::kCount)) {
            return 0;
        }
        // kRxBadChecksum is the deprecated-in-place aggregate of the two
        // split counters (TD-1). Compute on-demand so we never maintain
        // a third atomic for the same event. Invariant:
        //     metric(kRxBadChecksum) == metric(kRxIpChecksumBad)
        //                             + metric(kRxL4ChecksumBad)
        if (m == ::eph::net::StreamMetric::kRxBadChecksum) {
            return counters_[static_cast<std::size_t>(
                                ::eph::net::StreamMetric::kRxIpChecksumBad)]
                       .v.load(std::memory_order_relaxed) +
                   counters_[static_cast<std::size_t>(
                                ::eph::net::StreamMetric::kRxL4ChecksumBad)]
                       .v.load(std::memory_order_relaxed);
        }
        return counters_[static_cast<std::size_t>(m)]
            .v.load(std::memory_order_relaxed);
    }

    /// @brief Post-create socket state snapshot.
    /// @see eph::net::StreamSnapshot for field semantics.
    /// @note UDP has no TCP / TLS / WS state — those sub-structs stay
    ///       inert (`enabled=false`). DPDK UDP also has no RX queue at
    ///       config time (RX dispatch is the Poller's job per
    ///       `PollerConfig::rx_queue_id`); `dpdk.rx_queue` therefore
    ///       reports 0 and consumers should query
    ///       `Platform::dispatch_mode()` for the broader picture.
    [[nodiscard]] ::eph::net::StreamSnapshot snapshot() const noexcept {
        ::eph::net::StreamSnapshot s{};
        s.endpoint.src_ip   = cfg_.legacy.src_ip;
        s.endpoint.src_port = cfg_.legacy.src_port;
        s.endpoint.dst_ip   = cfg_.legacy.dst_ip;
        s.endpoint.dst_port = cfg_.legacy.dst_port;
        // UDP never reverse-picks src_port; src_port_rewritten stays false.

        // tcp.* / tls.* / ws.* / keepalive.* all stay default (enabled=false).

        s.dpdk.rx_queue                 = 0;  // RX is Poller-driven; no per-socket binding
        s.dpdk.tx_queue                 = cfg_.legacy.tx_queue_id;
        s.dpdk.pool_lcore_hint_resolved = cfg_.pool_lcore_hint;
        if (flow_rule_ && flow_rule_->valid()) {
            s.dpdk.flow_rule_handle = flow_rule_->opaque_handle_id();
        }
        return s;
    }
};

} // namespace eph::net::dpdk
