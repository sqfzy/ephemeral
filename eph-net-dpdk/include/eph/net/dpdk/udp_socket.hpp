#pragma once

/// @file udp_socket.hpp
/// DPDK UDP datagram socket satisfying `eph::net::Datagram`.
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
            "DpdkUdpSocket::create: src={}:{} dst={}:{}",
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
            "DpdkUdpSocket::create: ready, peer={}:{}",
            sock->cfg_.legacy.dst_ip, sock->cfg_.legacy.dst_port);
        return sock;
    }

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
            if (cfg.pin_to_queue) {
                const uint16_t want = *cfg.pin_to_queue;
                if (want >= nb_q) {
                    return std::unexpected(core::ErrorInfo{
                        core::Error::InvalidConfig,
                        "create_and_attach: pin_to_queue >= nb_rx_queues"});
                }
                auto sp = ::eph::net::dpdk::find_src_port_for_queue(
                    platform.port_id(), want,
                    /*src_ip=*/cfg.legacy.dst_ip,
                    /*dst_ip=*/cfg.legacy.src_ip,
                    /*dst_port=*/cfg.legacy.src_port);
                if (!sp) {
                    return std::unexpected(core::ErrorInfo{
                        core::Error::InvalidConfig,
                        "create_and_attach: find_src_port_for_queue exhausted"});
                }
                cfg.legacy.src_port = *sp;
                target_qid = want;
            } else {
                auto qr = ::eph::net::dpdk::predict_rss_queue(
                    platform.port_id(),
                    /*src_ip=*/cfg.legacy.dst_ip,
                    /*src_port=*/cfg.legacy.dst_port,
                    /*dst_ip=*/cfg.legacy.src_ip,
                    /*dst_port=*/cfg.legacy.src_port);
                if (!qr) {
                    return std::unexpected(core::ErrorInfo{
                        core::Error::InvalidConfig,
                        "create_and_attach: predict_rss_queue failed"});
                }
                target_qid = *qr;
            }
        } else {  // FlowDirector
            if (cfg.pin_to_queue && *cfg.pin_to_queue >= nb_q) {
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "create_and_attach: pin_to_queue >= nb_rx_queues"});
            }
            if (cfg.pin_to_queue) {
                target_qid = *cfg.pin_to_queue;
            } else {
                static std::atomic<uint16_t> rr_counter{0};
                target_qid = rr_counter.fetch_add(1, std::memory_order_relaxed)
                             % nb_q;
            }
        }

        auto sr = create(std::move(cfg));
        if (!sr) return std::unexpected(sr.error());
        auto sock = std::move(*sr);

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
        if (attached_to_ != nullptr) {
            SPDLOG_LOGGER_DEBUG(detail::udp_socket_logger(),
                "~DpdkUdpSocket: auto-detach");
            (void)attached_to_->remove(this);
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
        // UDP maximum payload is 65535 bytes (uint16_t). Reject oversized
        // payloads explicitly rather than silently truncating via cast.
        if (app_payload.size() > 0xFFFFu) {
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkUdpSocket::send_to: payload exceeds max UDP size (65535)"});
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
        mcast_macs_[mcast_count_++] = mac;
        return apply_mcast_list_();
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
                // Compact the array.
                for (std::size_t j = i; j + 1 < mcast_count_; ++j) {
                    mcast_macs_[j] = mcast_macs_[j + 1];
                }
                --mcast_count_;
                return apply_mcast_list_();
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

    [[nodiscard]] bool is_attached_() const noexcept {
        return attached_to_ != nullptr;
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
        for (uint16_t i = 0; i < n; ++i) {
            auto parsed = ::eph::dpdk::net::parse_udp_packet(mbufs[i]);
            // Accept zero-length payloads per RFC 768 — only reject
            // truly unparseable packets (no UDP header).
            if (!parsed.udp) {
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }

            SocketAddr src_addr{
                Ipv4Addr::from_be32(parsed.src_ip()),
                parsed.src_port()
            };

            // connect_to() filter — drop packets whose source does not
            // match the configured peer.
            if (connected_) {
                if (src_addr.ip.to_be32() != connected_peer_.ip.to_be32() ||
                    src_addr.port         != connected_peer_.port) {
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

            auto sink = [&](auto&& frame) {
                if (frame.size() > 0 && on_datagram) {
                    on_datagram(std::span<const uint8_t>(
                                    frame.data(), frame.size()),
                                src_addr);
                    inc_<::eph::net::StreamMetric::kFramesDecoded>();
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
                inc_<::eph::net::StreamMetric::kCodecErrors>();
                SPDLOG_LOGGER_WARN(detail::udp_socket_logger(),
                    "DpdkUdpSocket::process_burst_: codec decode err={}",
                    dr.error().detail);
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
            SPDLOG_LOGGER_WARN(detail::udp_socket_logger(),
                "DpdkUdpSocket::apply_mcast_list_: "
                "rte_eth_dev_set_mc_addr_list failed: rc={} count={} port={}",
                rc, mcast_count_, port);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "rte_eth_dev_set_mc_addr_list failed"});
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
    /// Per-socket multicast MAC list (max 8 groups, matches the legacy
    /// kMaxMulticastGroups). Pushed to the NIC via
    /// `rte_eth_dev_set_mc_addr_list` on join / leave.
    std::array<rte_ether_addr, 8>          mcast_macs_{};
    std::size_t                            mcast_count_{0};
    /// connect_to() bookkeeping — when set, packets from non-matching
    /// peers are filtered by the Poller's tuple dispatch.
    SocketAddr                             connected_peer_{};
    bool                                   connected_{false};

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
        return counters_[static_cast<std::size_t>(m)]
            .v.load(std::memory_order_relaxed);
    }
};

} // namespace eph::net::dpdk
