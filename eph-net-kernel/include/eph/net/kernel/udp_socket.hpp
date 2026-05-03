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

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <array>
#include <atomic>
#include <functional>
#include <limits>
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
#include "eph/net/stream_metrics.hpp"
#include "eph/net/stream_snapshot.hpp"

namespace eph::net::kernel {

namespace detail {

/// @brief Lazily-initialized logger for the kernel UDP socket subsystem.
inline spdlog::logger* udp_socket_logger() {
    static auto* l = ::eph::core::detail::make_logger("net.kernel.udp_socket");
    return l;
}

/// @brief Stack-allocated OutputBuffer for UDP codec auto-response path.
///        Datagram codecs rarely emit control frames (Mold64 is pure RX)
///        so 64 B is generous for the forseeable consumers. See
///        batch3-round5 LOW-2.
inline constexpr std::size_t kUdpCodecAutoResponseBytes = 64;

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
    /// @brief Datagram sink: invoked once per decoded datagram. The span
    ///        holds application-layer bytes post-codec; `peer` is the
    ///        source address the datagram arrived from.
    using OnDatagram  = std::function<void(std::span<const uint8_t>,
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
        // SO_RCVBUF / SO_SNDBUF take an int — clamp to INT_MAX so an
        // oversized size_t (e.g. 4 GiB+) doesn't truncate to a negative
        // or near-zero value via static_cast and silently neuter the
        // buffer sizing the caller asked for. The kernel cap is governed
        // by sysctl net.core.{r,w}mem_max, which is typically <= a few
        // hundred MiB; INT_MAX is a comfortable upper bound.
        if (cfg.rcv_buf > 0) {
            const std::size_t clamped = std::min<std::size_t>(
                cfg.rcv_buf, static_cast<std::size_t>(std::numeric_limits<int>::max()));
            if (clamped != cfg.rcv_buf) {
                SPDLOG_LOGGER_WARN(log,
                    "KernelUdpSocket::create: rcv_buf={} exceeds INT_MAX, "
                    "clamping to {}", cfg.rcv_buf, clamped);
            }
            int v = static_cast<int>(clamped);
            if (::setsockopt(s, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v)) != 0) {
                SPDLOG_LOGGER_WARN(log,
                    "KernelUdpSocket::create: setsockopt(SO_RCVBUF={}) "
                    "errno={} ({}) — kernel default in effect",
                    clamped, errno, std::strerror(errno));
            }
        }
        if (cfg.snd_buf > 0) {
            const std::size_t clamped = std::min<std::size_t>(
                cfg.snd_buf, static_cast<std::size_t>(std::numeric_limits<int>::max()));
            if (clamped != cfg.snd_buf) {
                SPDLOG_LOGGER_WARN(log,
                    "KernelUdpSocket::create: snd_buf={} exceeds INT_MAX, "
                    "clamping to {}", cfg.snd_buf, clamped);
            }
            int v = static_cast<int>(clamped);
            if (::setsockopt(s, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v)) != 0) {
                SPDLOG_LOGGER_WARN(log,
                    "KernelUdpSocket::create: setsockopt(SO_SNDBUF={}) "
                    "errno={} ({}) — kernel default in effect",
                    clamped, errno, std::strerror(errno));
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

        auto sock = std::unique_ptr<KernelUdpSocket>(new KernelUdpSocket(s, cfg.bind));

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
        // Symmetric with ~DpdkUdpSocket and ~KernelTcpStream: log a WARN
        // when auto-detach fails so a Poller/Socket lifecycle mismatch is
        // visible instead of being silently `(void)`-discarded. Pre-fix
        // the result was dropped on the floor, hiding double-remove and
        // stale-attached_to_ bugs at the kernel-backend tier.
        if (attached_to_ != nullptr) {
            auto r = attached_to_->remove(this);
            if (!r) {
                SPDLOG_LOGGER_WARN(detail::udp_socket_logger(),
                    "~KernelUdpSocket: auto-detach failed: {} — possible "
                    "Poller/Socket lifecycle mismatch",
                    r.error().detail ? r.error().detail : "unknown");
            }
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
    send_to(std::span<const uint8_t> app_payload, const SocketAddr& dst) noexcept {
        if (attached_to_ == nullptr) [[unlikely]] {
            return std::unexpected(core::ErrorInfo{
                core::Error::NotAttached,
                "KernelUdpSocket::send_to called before attach"});
        }
        if (fd_ < 0) [[unlikely]] {
            return std::unexpected(core::ErrorInfo{
                core::Error::Disconnected,
                "KernelUdpSocket::send_to: fd closed"});
        }

        struct sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_port        = ::htons(dst.port);
        sa.sin_addr.s_addr = ::htonl(dst.ip.to_be32());

        const ssize_t n = ::sendto(fd_, app_payload.data(), app_payload.size(),
                                    MSG_NOSIGNAL | MSG_DONTWAIT,
                                    reinterpret_cast<struct sockaddr*>(&sa),
                                    sizeof(sa));
        if (n < 0) [[unlikely]] {
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
        inc_<::eph::net::StreamMetric::kBytesSent>(static_cast<std::uint64_t>(n));
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
            // WARN-log: cold-path guard, matches the WARN below on
            // setsockopt-style failures so callers that drop the
            // expected<> return still see the misuse in the journal.
            SPDLOG_LOGGER_WARN(detail::udp_socket_logger(),
                "KernelUdpSocket::connect_to: fd closed (peer={})",
                peer.to_string());
            return std::unexpected(core::ErrorInfo{
                core::Error::Disconnected,
                "KernelUdpSocket::connect_to: fd closed"});
        }
        // Reject (0.0.0.0, 0) — POSIX semantics differ across kernels and
        // the most common interpretation is "disassociate the connected
        // peer", which silently un-does an earlier connect_to without
        // surfacing the unexpected state. Force the caller to use a real
        // peer; if they want to disassociate, a future explicit
        // `disconnect()` API can carry that intent without overloading
        // the same entry point.
        if (peer.ip == Ipv4Addr{} && peer.port == 0) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::udp_socket_logger(),
                "KernelUdpSocket::connect_to: rejecting (0.0.0.0:0) "
                "peer — caller likely wanted explicit disconnect()");
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "KernelUdpSocket::connect_to: peer (0.0.0.0:0) is not a "
                "valid peer (POSIX semantics on this address effectively "
                "disassociate; use a real address)"});
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
        connected_peer_ = peer;
        connected_      = true;
        return {};
    }

    /// @brief Post-create socket state snapshot.
    /// @see eph::net::StreamSnapshot for field semantics.
    /// @note If `connect_to()` was invoked, `endpoint.dst_*` reflects the
    ///       connected peer; otherwise it stays at zero (unconnected UDP
    ///       has no fixed destination — `send_to` carries dst per call).
    [[nodiscard]] ::eph::net::StreamSnapshot snapshot() const noexcept {
        ::eph::net::StreamSnapshot s{};
        s.endpoint.src_ip   = bind_addr_.ip.to_be32();
        s.endpoint.src_port = bind_addr_.port;
        if (connected_) {
            s.endpoint.dst_ip   = connected_peer_.ip.to_be32();
            s.endpoint.dst_port = connected_peer_.port;
        }
        // tcp / tls / ws / keepalive / dpdk all stay default (enabled=false).
        return s;
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
        inc_<::eph::net::StreamMetric::kBytesRecv>(static_cast<std::uint64_t>(n));

        if (!on_datagram) return 0;

        // Feed the datagram through the codec. The codec may emit 0..N
        // frames via a sink lambda; we forward each to `on_datagram` with
        // the source address.
        detail::SpanView   view(buf, static_cast<std::size_t>(n));
        uint8_t            scratch[detail::kUdpCodecAutoResponseBytes];
        core::OutputBuffer out_sink(scratch, sizeof(scratch));

        std::size_t delivered = 0;
        auto sink = [&](auto&& frame) {
            if (frame.size() > 0 && on_datagram) {
                on_datagram(std::span<const uint8_t>(frame.data(),
                                                     frame.size()),
                            src_addr);
                inc_<::eph::net::StreamMetric::kFramesDecoded>();
                ++delivered;
            }
        };
        auto dr = codec_.decode(view, out_sink, sink);

        // Flush any auto-response bytes the codec wrote into `out_sink`
        // back to the source peer (NAK, ACK, …). No in-tree DatagramCodec
        // currently exercises this path — Mold64 and RawDatagramCodec are
        // pure RX — but without this flush, a future auto-responding
        // codec would silently drop its own emissions (same class of bug
        // as the TCP drain_codec_ drop fixed in 9bbc163 / 7401196). WARN
        // on failure and continue; UDP has no session state to reset.
        if (out_sink.size() > 0) {
            auto sr = this->send_to(
                std::span<const uint8_t>(out_sink.data(), out_sink.size()),
                src_addr);
            if (!sr) {
                SPDLOG_LOGGER_WARN(detail::udp_socket_logger(),
                    "KernelUdpSocket::poll_once_: auto-response send_to "
                    "failed ({} bytes to {}): {}",
                    out_sink.size(), src_addr.to_string(),
                    sr.error().detail);
            } else {
                SPDLOG_LOGGER_DEBUG(detail::udp_socket_logger(),
                    "KernelUdpSocket::poll_once_: sent {} auto-response "
                    "bytes to {}",
                    out_sink.size(), src_addr.to_string());
            }
        }

        if (!dr) {
            // Elevate to ERROR: a decode failure on one datagram is not
            // state-corrupting for UDP (per-packet codec, stateless in
            // practice), but it IS a market-data-loss event that operators
            // must see. Include the source address and payload length so
            // the offending peer is identifiable from the log.
            inc_<::eph::net::StreamMetric::kCodecErrors>();
            SPDLOG_LOGGER_ERROR(detail::udp_socket_logger(),
                "KernelUdpSocket::poll_once_: decode err={} "
                "src={} payload_len={} delivered_before_err={}",
                dr.error().detail, src_addr.to_string(),
                static_cast<std::size_t>(n), delivered);
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

    // ── Observability (StreamMetric pull model) ──────────────────────────
    //
    // See eph/net/stream_metrics.hpp. UDP backends do not maintain a
    // reasm buffer or a TLS state, so kReasmOverflows and
    // kTlsCrossRecordFrames stay at 0. kCodecErrors IS active here:
    // poll_once_ above bumps it on every per-datagram decode failure
    // (DatagramCodec::decode returning std::unexpected); see also the
    // KernelUdpSocket::poll_once_ ERROR-log call site for the diagnostic
    // shape (src + payload_len + delivered_before_err).

    [[nodiscard]] std::uint64_t metric(::eph::net::StreamMetric m) const noexcept {
        // Defensive bounds check — see DpdkTcpStream::metric for rationale.
        if (static_cast<std::size_t>(m) >=
            static_cast<std::size_t>(::eph::net::StreamMetric::kCount)) {
            return 0;
        }
        return counters_[static_cast<std::size_t>(m)]
            .v.load(std::memory_order_relaxed);
    }

private:
    explicit KernelUdpSocket(int fd, SocketAddr bind_addr) noexcept
        : fd_(fd), bind_addr_(bind_addr) {}

    // ── Hot-path metric counters (pull model — see stream_metrics.hpp) ──

    struct alignas(64) Counter { std::atomic<std::uint64_t> v{0}; };

    std::array<Counter,
               static_cast<std::size_t>(::eph::net::StreamMetric::kCount)>
        counters_{};

    template <::eph::net::StreamMetric M>
    void inc_(std::uint64_t n = 1) noexcept {
        counters_[static_cast<std::size_t>(M)]
            .v.fetch_add(n, std::memory_order_relaxed);
    }

    /// @brief Shared IP_ADD/DROP_MEMBERSHIP helper.
    [[nodiscard]] std::expected<void, core::ErrorInfo>
    set_membership_(const SocketAddr& group, int optname) noexcept {
        if (fd_ < 0) {
            // WARN-log: cold-path guard. Matches the existing
            // setsockopt-failure WARN below so a caller that drops
            // the expected<> return still has signal in the log.
            SPDLOG_LOGGER_WARN(detail::udp_socket_logger(),
                "KernelUdpSocket::set_membership_: fd closed "
                "(group={} optname={})",
                group.to_string(), optname);
            return std::unexpected(core::ErrorInfo{
                core::Error::Disconnected,
                "KernelUdpSocket::set_membership_: fd closed"});
        }
        // Validate the address is actually multicast (224.0.0.0/4, i.e.
        // top-4-bits == 0xE) BEFORE invoking setsockopt. The kernel
        // rejects non-multicast addresses with EINVAL, but the resulting
        // diagnostic ("setsockopt failed errno=22 Invalid argument") is
        // opaque — operators have to consult RFC 1112 to recognise
        // 192.168.x.y as a class-C unicast that simply can't be joined.
        // Surfacing the pre-check inline turns the misuse into an
        // actionable error string at the same call site.
        const uint32_t ip_he = group.ip.to_be32();
        if ((ip_he & 0xF0000000u) != 0xE0000000u) {
            SPDLOG_LOGGER_WARN(detail::udp_socket_logger(),
                "KernelUdpSocket::set_membership_: group={} is not in "
                "224.0.0.0/4 (IPv4 multicast range) — kernel would reject "
                "with EINVAL",
                group.to_string());
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "KernelUdpSocket::set_membership_: group is not in "
                "224.0.0.0/4 (not an IPv4 multicast address)"});
        }
        struct ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = ::htonl(ip_he);
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
    /// @brief Snapshot bookkeeping: the bind address the socket was
    ///        created with. Surfaced via `snapshot().endpoint.src_*`.
    SocketAddr               bind_addr_{};
    /// @brief Snapshot bookkeeping: the peer address `connect_to()`
    ///        was called with (if it was). Surfaced via
    ///        `snapshot().endpoint.dst_*` when `connected_` is true.
    SocketAddr               connected_peer_{};
    bool                     connected_{false};
};

} // namespace eph::net::kernel
