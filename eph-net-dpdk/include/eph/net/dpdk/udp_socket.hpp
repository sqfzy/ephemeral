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
///        │  ├── registered 4-tuple        (for Poller routing)
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
#include "eph/dpdk/udp.hpp"
#include "eph/net/concepts.hpp"
#include "eph/net/dpdk/config.hpp"
#include "eph/net/dpdk/detail/mbuf_view.hpp"
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/socket_addr.hpp"

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
    using OnDatagram  = std::function<void(const uint8_t*, uint16_t,
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
    send_to(std::span<const uint8_t> data, const SocketAddr& dst) noexcept {
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
        if (data.size() > 0xFFFFu) {
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkUdpSocket::send_to: payload exceeds max UDP size (65535)"});
        }
        if (!sender_.send(data.data(),
                           static_cast<uint16_t>(data.size()))) {
            return std::unexpected(core::ErrorInfo{
                core::Error::BufferFull,
                "DpdkUdpSocket::send_to: UdpSender::send failed"});
        }
        return data.size();
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
    ///        packets by 4-tuple, so updating the configured peer is
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

    /// @brief Supplies the registered 4-tuple to the Poller at add-time.
    void tuple_for_poller_(uint32_t* src_ip, uint32_t* dst_ip,
                            uint16_t* src_port, uint16_t* dst_port) noexcept {
        *src_ip   = cfg_.legacy.src_ip;
        *dst_ip   = cfg_.legacy.dst_ip;
        *src_port = cfg_.legacy.src_port;
        *dst_port = cfg_.legacy.dst_port;
    }

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

            uint8_t            scratch[detail::kDatagramAutoResponseBytes];
            core::OutputBuffer out_sink(scratch, sizeof(scratch));

            auto sink = [&](auto&& frame) {
                if (frame.size() > 0 && on_datagram) {
                    if (saturate_u16_clamps(frame.size()) && !trunc_warned_) {
                        SPDLOG_LOGGER_WARN(detail::udp_socket_logger(),
                            "DpdkUdpSocket::process_burst_: datagram frame "
                            "size {} > 0xFFFF; on_datagram length clamped to "
                            "0xFFFF (warn-once per socket)",
                            frame.size());
                        trunc_warned_ = true;
                    }
                    on_datagram(frame.data(), saturate_u16(frame.size()), src_addr);
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
    /// Per-socket multicast MAC list (max 8 groups, matches the legacy
    /// kMaxMulticastGroups). Pushed to the NIC via
    /// `rte_eth_dev_set_mc_addr_list` on join / leave.
    std::array<rte_ether_addr, 8>          mcast_macs_{};
    std::size_t                            mcast_count_{0};
    /// connect_to() bookkeeping — when set, packets from non-matching
    /// peers are filtered by the Poller's tuple dispatch.
    SocketAddr                             connected_peer_{};
    bool                                   connected_{false};
    /// Warn-once latch for `saturate_u16` clamping during on_datagram dispatch.
    /// See batch3-round1 MEDIUM-1.
    bool                                   trunc_warned_{false};
};

} // namespace eph::net::dpdk
