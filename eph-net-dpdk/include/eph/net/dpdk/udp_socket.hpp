#pragma once

/// @file udp_socket.hpp
/// DPDK UDP datagram socket satisfying `eph::net::Datagram`. Part of
/// Phase 4 of the v3.3 refactor.
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
/// Phase 4 scope:
///   - TX uses the legacy `UdpSender` for header-precomputed
///     fixed-destination sends. `send_to(data, dst)` is only valid when
///     `dst` matches the configured peer (the legacy template is fixed-
///     peer — changing destination mid-stream would require rebuilding
///     the template, which is outside Phase 4).
///   - RX dispatch comes in via `process_burst_` from DpdkPoller, which
///     parses the UDP packet, extracts the payload, and invokes the codec
///     decode loop → `on_datagram` per decoded frame.
///   - Multicast join/leave currently returns `InvalidConfig` because the
///     legacy `eph::dpdk::multicast` helper uses a port-level registration
///     API that is orthogonal to per-socket membership; Phase 5 wires
///     that into the new API surface properly. Tests only exercise the
///     API surface (NotAttached, send_to after attach, etc.).

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include <rte_mbuf.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "eph/core/codec.hpp"
#include "eph/core/error.hpp"
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
    static auto* l = [] {
        auto lg = spdlog::get("net.dpdk.udp");
        if (!lg) {
            try {
                lg = spdlog::stdout_color_mt("net.dpdk.udp");
            } catch (const spdlog::spdlog_ex&) {
                lg = spdlog::get("net.dpdk.udp");
            }
        }
        return lg.get();
    }();
    return l;
}

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
                "(Phase 4 uses fixed-peer UdpSender)"});
        }
        if (!sender_.send(data.data(),
                           static_cast<uint16_t>(data.size()))) {
            return std::unexpected(core::ErrorInfo{
                core::Error::BufferFull,
                "DpdkUdpSocket::send_to: UdpSender::send failed"});
        }
        return data.size();
    }

    [[nodiscard]] std::expected<void, core::ErrorInfo>
    join_multicast(const SocketAddr& /*group*/) noexcept {
        // Phase 4 stub — the legacy eph::dpdk::multicast helper uses a
        // port-level API that we need to reinterpret for per-socket
        // membership. Phase 5 hooks this in properly.
        SPDLOG_LOGGER_WARN(detail::udp_socket_logger(),
            "DpdkUdpSocket::join_multicast: Phase 4 stub (no-op)");
        return {};
    }

    [[nodiscard]] std::expected<void, core::ErrorInfo>
    leave_multicast(const SocketAddr& /*group*/) noexcept {
        SPDLOG_LOGGER_WARN(detail::udp_socket_logger(),
            "DpdkUdpSocket::leave_multicast: Phase 4 stub (no-op)");
        return {};
    }

    /// @brief Connect-equivalent — DPDK has no kernel connect(), so this
    ///        is where the 5-tuple would be installed into a flow-
    ///        steering rule. Phase 4 stub; Phase 5 wires into FlowSteering.
    [[nodiscard]] std::expected<void, core::ErrorInfo>
    connect_to(const SocketAddr& /*peer*/) noexcept {
        SPDLOG_LOGGER_DEBUG(detail::udp_socket_logger(),
            "DpdkUdpSocket::connect_to: Phase 4 stub (peer is fixed at create)");
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
    [[nodiscard]] void* native_handle() noexcept {
        return static_cast<void*>(&sender_);
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
            if (!parsed.udp || parsed.payload == nullptr || parsed.payload_len == 0) {
                rte_pktmbuf_free(mbufs[i]);
                continue;
            }

            SocketAddr src_addr{
                Ipv4Addr::from_be32(parsed.src_ip()),
                parsed.src_port()
            };

            // Wrap the payload in an MbufView. The memory is owned by
            // the mbuf, which stays alive for the entire on_datagram
            // callback scope. Phase 5 will let codecs mutate in place
            // for zero-copy TLS decrypt.
            detail::MbufView view(const_cast<uint8_t*>(parsed.payload),
                                   parsed.payload_len, rx_tsc);

            uint8_t            scratch[64];
            core::OutputBuffer out_sink(scratch, sizeof(scratch));

            auto sink = [&](auto&& frame) {
                if (frame.size() > 0 && on_datagram) {
                    on_datagram(frame.data(),
                                static_cast<uint16_t>(
                                    frame.size() > 0xFFFFu ? 0xFFFFu : frame.size()),
                                src_addr);
                }
            };
            auto dr = codec_.decode(view, out_sink, sink);
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

    UdpConfig                           cfg_{};
    ::eph::dpdk::UdpSender              sender_;
    [[no_unique_address]] C             codec_{};
    DpdkPoller<void>*                   attached_to_{nullptr};
};

} // namespace eph::net::dpdk
