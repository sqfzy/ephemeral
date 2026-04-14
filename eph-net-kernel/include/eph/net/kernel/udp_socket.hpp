#pragma once

/// @file udp_socket.hpp
/// Epoll-backed UDP datagram socket satisfying the `eph::net::Datagram`
/// concept.
///
/// Features:
///   - AF_INET SOCK_DGRAM with bind(), optional SO_REUSEADDR, non-blocking.
///   - `send_to(span, dst)` — unconnected mode single-datagram send.
///   - `join_multicast(group)` / `leave_multicast(group)` via
///     IP_ADD_MEMBERSHIP / IP_DROP_MEMBERSHIP on INADDR_ANY.
///   - `connect_to(peer)` — switches to connected-UDP mode for kernel-
///     level source filtering.
///   - `poll_once_()` drains one recvmsg into the codec; one datagram per
///     call is sufficient for epoll level-triggered operation.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "eph/core/detail/logger.hpp"
#include "eph/core/codec.hpp"
#include "eph/core/error.hpp"
#include "eph/net/concepts.hpp"
#include "eph/net/kernel/config.hpp"
#include "eph/net/kernel/detail/span_view.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/socket_addr.hpp"

namespace eph::net::kernel {

namespace detail {

/// @brief Lazily-initialized logger for the kernel UDP socket subsystem.
inline spdlog::logger* udp_socket_logger() {
    static auto* l = ::eph::core::detail::make_logger("net.kernel.udp_socket");
    return l;
}

} // namespace detail

// ---------------------------------------------------------------------------
// KernelUdpSocket
// ---------------------------------------------------------------------------

/// @brief Datagram socket backed by non-blocking AF_INET SOCK_DGRAM fd.
template <class C>
class KernelUdpSocket {
public:
    using CodecType   = C;
    using PacketView  = detail::SpanView;
    using OnDatagram  = std::function<void(const uint8_t*, uint16_t,
                                           const SocketAddr&)>;

    // ── Factory ──────────────────────────────────────────────────────────

    [[nodiscard]] static std::expected<std::unique_ptr<KernelUdpSocket>, core::ErrorInfo>
    create(UdpConfig cfg) noexcept {
        auto* log = detail::udp_socket_logger();
        SPDLOG_LOGGER_DEBUG(log,
            "KernelUdpSocket::create: bind={}", cfg.bind.to_string());

        const int s = ::socket(AF_INET,
                               SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                               IPPROTO_UDP);
        if (s < 0) {
            SPDLOG_LOGGER_ERROR(log,
                "KernelUdpSocket::create: socket() failed: {}",
                std::strerror(errno));
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "KernelUdpSocket::create: socket() failed"});
        }

        // setsockopt failures here are non-fatal (we continue with kernel
        // defaults) but must be surfaced via WARN logs. Previously the
        // return values were silently dropped, which hid the rejection
        // of rcv/snd buffer sizing — a real issue for market-data
        // consumers that rely on large SO_RCVBUF to absorb burst
        // traffic. See review-audit-net-batch1-round2 MEDIUM-2.
        if (cfg.reuse_addr) {
            int one = 1;
            if (::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
                SPDLOG_LOGGER_WARN(log,
                    "KernelUdpSocket::create: setsockopt(SO_REUSEADDR) "
                    "errno={} ({})", errno, std::strerror(errno));
            }
        }
        if (cfg.rcv_buf > 0) {
            int v = static_cast<int>(cfg.rcv_buf);
            if (::setsockopt(s, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v)) != 0) {
                SPDLOG_LOGGER_WARN(log,
                    "KernelUdpSocket::create: setsockopt(SO_RCVBUF={}) "
                    "errno={} ({}) — kernel default in effect",
                    cfg.rcv_buf, errno, std::strerror(errno));
            }
        }
        if (cfg.snd_buf > 0) {
            int v = static_cast<int>(cfg.snd_buf);
            if (::setsockopt(s, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v)) != 0) {
                SPDLOG_LOGGER_WARN(log,
                    "KernelUdpSocket::create: setsockopt(SO_SNDBUF={}) "
                    "errno={} ({}) — kernel default in effect",
                    cfg.snd_buf, errno, std::strerror(errno));
            }
        }

        struct sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_port        = ::htons(cfg.bind.port);
        sa.sin_addr.s_addr = ::htonl(cfg.bind.ip.to_be32());
        if (::bind(s, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) != 0) {
            const int err = errno;
            ::close(s);
            SPDLOG_LOGGER_ERROR(log,
                "KernelUdpSocket::create: bind() failed: {}", std::strerror(err));
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "KernelUdpSocket::create: bind() failed"});
        }

        auto sock = std::unique_ptr<KernelUdpSocket>(new KernelUdpSocket(s));

        // Optional connected-UDP mode (source filtering at the kernel).
        if (cfg.connect_to.port != 0) {
            auto cr = sock->connect_to(cfg.connect_to);
            if (!cr) return std::unexpected(cr.error());
        }

        SPDLOG_LOGGER_INFO(log,
            "KernelUdpSocket::create: bound fd={} addr={}",
            sock->fd_, cfg.bind.to_string());
        return sock;
    }

    ~KernelUdpSocket() {
        if (attached_to_ != nullptr) {
            (void)attached_to_->remove(this);
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    KernelUdpSocket(const KernelUdpSocket&)            = delete;
    KernelUdpSocket& operator=(const KernelUdpSocket&) = delete;
    KernelUdpSocket(KernelUdpSocket&&)                 = delete;
    KernelUdpSocket& operator=(KernelUdpSocket&&)      = delete;

    // ── Public fields (Datagram concept) ─────────────────────────────────

    OnDatagram on_datagram;

    // ── Datagram concept API ─────────────────────────────────────────────

    [[nodiscard]] std::expected<std::size_t, core::ErrorInfo>
    send_to(std::span<const uint8_t> data, const SocketAddr& dst) noexcept {
        if (attached_to_ == nullptr) {
            return std::unexpected(core::ErrorInfo{
                core::Error::NotAttached,
                "KernelUdpSocket::send_to called before attach"});
        }
        if (fd_ < 0) {
            return std::unexpected(core::ErrorInfo{
                core::Error::Disconnected,
                "KernelUdpSocket::send_to: fd closed"});
        }

        struct sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_port        = ::htons(dst.port);
        sa.sin_addr.s_addr = ::htonl(dst.ip.to_be32());

        const ssize_t n = ::sendto(fd_, data.data(), data.size(),
                                    MSG_NOSIGNAL | MSG_DONTWAIT,
                                    reinterpret_cast<struct sockaddr*>(&sa),
                                    sizeof(sa));
        if (n < 0) {
            const int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK) {
                return std::unexpected(core::ErrorInfo{
                    core::Error::WouldBlock,
                    "KernelUdpSocket::send_to: would block"});
            }
            SPDLOG_LOGGER_WARN(detail::udp_socket_logger(),
                "KernelUdpSocket::send_to: errno {} ({})",
                err, std::strerror(err));
            // Classify by errno so operators can distinguish a bad
            // caller payload (EMSGSIZE / EINVAL) from a dead peer
            // (ECONNREFUSED on connected UDP) from a firewall drop
            // (EPERM / EACCES) or a routing failure (ENETUNREACH /
            // EHOSTUNREACH). Previously every non-EAGAIN errno was
            // lumped into Disconnected, which hid the actionable
            // causes behind a generic "unexpected I/O error" string.
            if (err == EMSGSIZE || err == EINVAL) {
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "KernelUdpSocket::send_to: payload rejected by kernel "
                    "(EMSGSIZE/EINVAL)"});
            }
            if (err == ENOBUFS || err == ENOMEM) {
                return std::unexpected(core::ErrorInfo{
                    core::Error::BufferFull,
                    "KernelUdpSocket::send_to: kernel buffer exhausted "
                    "(ENOBUFS/ENOMEM)"});
            }
            // ECONNREFUSED / ENETUNREACH / EHOSTUNREACH / EPERM etc. all
            // fall through to Disconnected since from the caller's
            // perspective the datagram cannot be delivered and the
            // reconnect policy is the right recovery action.
            return std::unexpected(core::ErrorInfo{
                core::Error::Disconnected,
                "KernelUdpSocket::send_to: sendto failed (see log for errno)"});
        }
        return static_cast<std::size_t>(n);
    }

    [[nodiscard]] std::expected<void, core::ErrorInfo>
    join_multicast(const SocketAddr& group) noexcept {
        return set_membership_(group, IP_ADD_MEMBERSHIP);
    }

    [[nodiscard]] std::expected<void, core::ErrorInfo>
    leave_multicast(const SocketAddr& group) noexcept {
        return set_membership_(group, IP_DROP_MEMBERSHIP);
    }

    /// @brief Switch the socket to connected-UDP mode (kernel filters source).
    [[nodiscard]] std::expected<void, core::ErrorInfo>
    connect_to(const SocketAddr& peer) noexcept {
        if (fd_ < 0) {
            return std::unexpected(core::ErrorInfo{
                core::Error::Disconnected,
                "KernelUdpSocket::connect_to: fd closed"});
        }
        struct sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_port        = ::htons(peer.port);
        sa.sin_addr.s_addr = ::htonl(peer.ip.to_be32());
        if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa))
            != 0) {
            SPDLOG_LOGGER_WARN(detail::udp_socket_logger(),
                "KernelUdpSocket::connect_to: errno {} ({})",
                errno, std::strerror(errno));
            return std::unexpected(core::ErrorInfo{
                core::Error::ConnectFailed,
                "KernelUdpSocket::connect_to: connect() failed"});
        }
        return {};
    }

    [[nodiscard]] bool is_attached() const noexcept {
        return attached_to_ != nullptr;
    }

    [[nodiscard]] int fd() const noexcept { return fd_; }

    // ── Pollable concept API ─────────────────────────────────────────────

    std::size_t poll_once_() noexcept {
        if (fd_ < 0) return 0;

        uint8_t            buf[65536];  // max UDP payload
        struct sockaddr_in src{};
        socklen_t          src_len = sizeof(src);
        const ssize_t n = ::recvfrom(fd_, buf, sizeof(buf), MSG_DONTWAIT,
                                      reinterpret_cast<struct sockaddr*>(&src),
                                      &src_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return 0;
            }
            SPDLOG_LOGGER_WARN(detail::udp_socket_logger(),
                "KernelUdpSocket::poll_once_: recvfrom errno {} ({})",
                errno, std::strerror(errno));
            return 0;
        }
        if (n == 0) {
            // Zero-length UDP datagram is legal; we treat it as "nothing to
            // deliver" because the codec and sink contracts expect >0 bytes.
            return 0;
        }

        SocketAddr src_addr{
            Ipv4Addr::from_be32(::ntohl(src.sin_addr.s_addr)),
            ::ntohs(src.sin_port)
        };

        if (!on_datagram) return 0;

        // Feed the datagram through the codec. The codec may emit 0..N
        // frames via a sink lambda; we forward each to `on_datagram` with
        // the source address.
        detail::SpanView   view(buf, static_cast<std::size_t>(n));
        uint8_t            scratch[64];
        core::OutputBuffer out_sink(scratch, sizeof(scratch));

        std::size_t delivered = 0;
        auto sink = [&](auto&& frame) {
            if (frame.size() > 0 && on_datagram) {
                on_datagram(frame.data(), saturate_u16(frame.size()), src_addr);
                ++delivered;
            }
        };
        auto dr = codec_.decode(view, out_sink, sink);
        if (!dr) {
            SPDLOG_LOGGER_WARN(detail::udp_socket_logger(),
                "KernelUdpSocket::poll_once_: decode err={}", dr.error().detail);
            return delivered;
        }
        return delivered;
    }

    // ── Poller-facing friend hooks ───────────────────────────────────────

    void notify_attached_(KernelPoller* p) noexcept { attached_to_ = p; }
    void notify_detached_() noexcept { attached_to_ = nullptr; }

    [[nodiscard]] bool is_attached_() const noexcept {
        return attached_to_ != nullptr;
    }

    [[nodiscard]] void* native_handle() const noexcept {
        return reinterpret_cast<void*>(static_cast<std::intptr_t>(fd_));
    }

private:
    explicit KernelUdpSocket(int fd) noexcept : fd_(fd) {}

    /// @brief Shared IP_ADD/DROP_MEMBERSHIP helper.
    [[nodiscard]] std::expected<void, core::ErrorInfo>
    set_membership_(const SocketAddr& group, int optname) noexcept {
        if (fd_ < 0) {
            return std::unexpected(core::ErrorInfo{
                core::Error::Disconnected,
                "KernelUdpSocket::set_membership_: fd closed"});
        }
        struct ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = ::htonl(group.ip.to_be32());
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (::setsockopt(fd_, IPPROTO_IP, optname, &mreq, sizeof(mreq)) != 0) {
            SPDLOG_LOGGER_WARN(detail::udp_socket_logger(),
                "KernelUdpSocket::set_membership_: errno {} ({})",
                errno, std::strerror(errno));
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "KernelUdpSocket::set_membership_: setsockopt failed"});
        }
        return {};
    }

    int                      fd_{-1};
    [[no_unique_address]] C  codec_{};
    KernelPoller*            attached_to_{nullptr};
};

} // namespace eph::net::kernel
