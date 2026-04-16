#pragma once

/// @file tcp_stream.hpp
/// Epoll-backed TCP stream satisfying the `eph::net::Stream` concept.
///
/// Architecture:
///
///     user code
///        │
///        v
///     KernelTcpStream<C, EnableTls>
///        │  ├── detail::ByteSocket    (raw non-blocking fd)
///        │  ├── C                     (StreamCodec template param)
///        │  ├── detail::ReassemblyBuffer (RX staging)
///        │  ├── TlsState              (only when EnableTls == true;
///        │  │                           std::monostate otherwise)
///        │  └── KernelPoller*         (set by Poller::add, cleared on remove)
///        │
///        v
///     KernelPoller (epoll)

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "eph/core/detail/logger.hpp"
#include "eph/core/codec.hpp"
#include "eph/core/error.hpp"
#include "eph/net/concepts.hpp"
#include "eph/net/detail/http_connect.hpp"   // HTTP CONNECT proxy
#include "eph/net/detail/ws_handshake.hpp"   // WS HTTP handshake
#include "eph/net/kernel/config.hpp"
#include "eph/net/kernel/detail/byte_socket.hpp"
#include "eph/net/kernel/detail/reassembly_buffer.hpp"
#include "eph/net/kernel/detail/span_view.hpp"
#include "eph/net/kernel/detail/tls_state.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/tcp_state.hpp"

namespace eph::net::kernel {

namespace detail {

/// @brief Lazily-initialized logger for the kernel TcpStream subsystem.
inline spdlog::logger* tcp_stream_logger() {
    static auto* l = ::eph::core::detail::make_logger("net.kernel.tcp_stream");
    return l;
}

// ---------------------------------------------------------------------------
// Scratch buffer sizing constants (batch3-round5 LOW-1)
// ---------------------------------------------------------------------------

/// @brief TlsWsSink::recv ciphertext scratch window used during the
///        WebSocket handshake over a TLS tunnel. 4 KiB is one mmap page
///        and fits the largest handshake fragment any real broker emits
///        (typical: <1.5 KiB). Too big wastes stack; too small would
///        require multiple recv calls per TLS record.
inline constexpr std::size_t kTlsWsSinkRxScratchBytes = 4096;

/// @brief Sink-less drain buffer inside `KernelTcpStream::poll_once_`.
///        When a stream is attached without an `on_message` callback we
///        still drain the socket to avoid stalling the peer, but the
///        bytes are discarded — a 4 KiB stack buffer is enough to clear
///        one epoll wakeup's worth of traffic in a single recv().
inline constexpr std::size_t kNoSinkDrainBytes = 4096;

/// @brief Stack-allocated OutputBuffer for the codec auto-response path
///        (e.g. WS pong, close ack). 1 KiB is generous for the control
///        frames the WsCodec emits.
inline constexpr std::size_t kCodecAutoResponseBytes = 1024;

// TlsState is defined in detail/tls_state.hpp (aws-lc AEAD machinery).

// ---------------------------------------------------------------------------
// WS-handshake ByteSink adapters
// ---------------------------------------------------------------------------
//
// `eph::net::detail::perform_ws_handshake<ByteSink>` is duck-typed on an
// object providing:
//     std::expected<size_t, core::ErrorInfo> send(std::span<const uint8_t>);
//     std::expected<size_t, core::ErrorInfo> recv(uint8_t*, size_t);
//
// The kernel stream has two different byte pipes at handshake time:
//
//   1. Plaintext: raw ByteSocket — `send`/`recv` already match the shape.
//      A trivial reference wrapper is used so the template parameter type
//      is consistent between the two branches.
//
//   2. TLS-wrapped: ByteSocket + TlsState. We must encrypt outbound bytes
//      into TLS records before `sock.send`, and on the inbound side we
//      must loop `sock.recv` + `tls.process_records` until at least one
//      plaintext byte becomes available. The `TlsWsSink` adapter owns a
//      scratch vector for the encrypt path and a small plaintext staging
//      vector for the decrypt path (both persist for the lifetime of the
//      handshake, which is a single-digit-milliseconds window).

struct PlainWsSink {
    ByteSocket* sock;

    [[nodiscard]] std::expected<std::size_t, ::eph::core::ErrorInfo>
    send(std::span<const uint8_t> data) noexcept {
        return sock->send(data);
    }

    [[nodiscard]] std::expected<std::size_t, ::eph::core::ErrorInfo>
    recv(uint8_t* buf, std::size_t cap) noexcept {
        return sock->recv(buf, cap);
    }
};

struct TlsWsSink {
    ByteSocket*    sock;
    TlsState*      tls;

    // Scratch for outbound TLS records (reused across multiple send() calls).
    std::vector<uint8_t> tx_scratch{};

    // Inbound plaintext staging: accumulates decrypted bytes so that
    // `recv()` can return them incrementally while the underlying socket
    // may deliver multiple TLS records per recv(2) call.
    std::vector<uint8_t> rx_plain{};
    std::size_t          rx_plain_off{0};

    // Ciphertext accumulator: a TLS record may arrive in fragments across
    // multiple recv(2) calls, so we must hold partial records between calls.
    std::vector<uint8_t> rx_cipher{};

    [[nodiscard]] std::expected<std::size_t, ::eph::core::ErrorInfo>
    send(std::span<const uint8_t> data) noexcept {
        tx_scratch.clear();
        auto enc = tls->encrypt_for_send(data.data(), data.size(), tx_scratch);
        if (!enc) return std::unexpected(enc.error());
        // Drain the encrypted payload to the wire. The wrapper returns
        // "plaintext byte count" so the caller's byte accounting stays
        // plaintext-relative (mirrors KernelTcpStream::send's contract).
        std::size_t off = 0;
        while (off < tx_scratch.size()) {
            auto sr = sock->send(
                std::span<const uint8_t>(tx_scratch.data() + off,
                                          tx_scratch.size() - off));
            if (!sr) {
                if (sr.error().code == ::eph::core::Error::WouldBlock) continue;
                return std::unexpected(sr.error());
            }
            if (*sr == 0) {
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::BufferFull,
                    "TlsByteSocket::send: socket returned 0 bytes"});
            }
            off += *sr;
        }
        return data.size();
    }

    [[nodiscard]] std::expected<std::size_t, ::eph::core::ErrorInfo>
    recv(uint8_t* buf, std::size_t cap) noexcept {
        // Fast path: any staged plaintext from a previous call?
        if (rx_plain_off < rx_plain.size()) {
            const std::size_t avail = rx_plain.size() - rx_plain_off;
            const std::size_t n     = std::min(cap, avail);
            std::memcpy(buf, rx_plain.data() + rx_plain_off, n);
            rx_plain_off += n;
            if (rx_plain_off == rx_plain.size()) {
                rx_plain.clear();
                rx_plain_off = 0;
            }
            return n;
        }

        // No plaintext staged — pull ciphertext from the socket, decrypt
        // as many complete records as possible, then serve the staged
        // plaintext.
        //
        // We make a single recv attempt and propagate WouldBlock up to the
        // handshake driver, which owns the outer deadline loop. A prior
        // version of this function ran an 8-iteration "bounded retry" but
        // the very first unconditional propagation of ByteSocket errors
        // (any error, including WouldBlock) short-circuited the loop — it
        // was dead code that only confused readers. Keep the semantics
        // but collapse the structure.
        uint8_t tmp[kTlsWsSinkRxScratchBytes];
        auto rr = sock->recv(tmp, sizeof(tmp));
        if (!rr) {
            // Propagate WouldBlock (the handshake driver retries against
            // its own timeout) and real errors verbatim.
            return std::unexpected(rr.error());
        }
        if (*rr == 0) {
            // Spurious — ByteSocket::recv never returns 0 per its contract
            // — but be defensive and signal WouldBlock so the handshake
            // driver re-enters without treating this as a hard error.
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::WouldBlock,
                "TlsWsSink::recv: ByteSocket::recv returned 0"});
        }
        rx_cipher.insert(rx_cipher.end(), tmp, tmp + *rr);

        // Decrypt complete records.
        auto cr = tls->process_records(rx_cipher.data(), rx_cipher.size(),
                                        rx_plain);
        if (!cr) return std::unexpected(cr.error());
        // Drop consumed ciphertext; partial record (if any) stays.
        if (*cr > 0) {
            rx_cipher.erase(rx_cipher.begin(),
                             rx_cipher.begin() + *cr);
        }

        if (!rx_plain.empty()) {
            const std::size_t n = std::min(cap, rx_plain.size());
            std::memcpy(buf, rx_plain.data(), n);
            rx_plain_off = n;
            if (rx_plain_off == rx_plain.size()) {
                rx_plain.clear();
                rx_plain_off = 0;
            }
            return n;
        }

        // No full record buffered yet — tell the handshake driver to
        // re-poll. (The ByteSocket recv succeeded but the bytes were
        // only part of a TLS record, so we have nothing plaintext to
        // deliver this call.)
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::WouldBlock,
            "TlsWsSink::recv: no plaintext record yet"});
    }
};

} // namespace detail

// ---------------------------------------------------------------------------
// KernelTcpStream
// ---------------------------------------------------------------------------

/// @brief Stream impl backed by a non-blocking AF_INET socket + epoll.
///
/// @tparam C          StreamCodec implementation (duck-typed per the
///                    `eph::core::StreamCodec` concept).
/// @tparam EnableTls  When true, the instance carries TLS session state
///                    and `create()` runs the TLS 1.3 handshake via aws-lc
///                    before returning.
template <class C, bool EnableTls = true>
class KernelTcpStream {
public:
    // ── Associated types (Stream concept) ─────────────────────────────────

    using CodecType  = C;
    using PacketView = detail::SpanView;
    using OnMessage  = std::function<void(const uint8_t*, uint16_t)>;

    // ── Factory ──────────────────────────────────────────────────────────

    /// @brief Open a TCP connection, run any enabled handshake, and return
    ///        a ready-to-attach stream. Performs a single sync connect.
    [[nodiscard]] static std::expected<std::unique_ptr<KernelTcpStream>, core::ErrorInfo>
    create(StreamConfig cfg) noexcept {
        auto* log = detail::tcp_stream_logger();
        SPDLOG_LOGGER_DEBUG(log,
            "KernelTcpStream::create: remote={} tls={}",
            cfg.remote.to_string(), EnableTls);

        if (cfg.reasm_capacity == 0) {
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "KernelTcpStream::create: reasm_capacity must be > 0"});
        }

        // Timeout fields must be strictly positive. Previously a zero /
        // negative `connect_timeout` or `ws_timeout` slipped through and
        // emerged from deep inside the handshake as a confusing
        // `Error::Timeout` with no breadcrumb linking it back to the bad
        // config. Surface the cause at the validation boundary instead.
        // See review-audit-net-batch3-round4 MEDIUM-2.
        if (cfg.connect_timeout <= std::chrono::milliseconds::zero()) {
            SPDLOG_LOGGER_WARN(log,
                "KernelTcpStream::create: connect_timeout={}ms must be > 0",
                cfg.connect_timeout.count());
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "KernelTcpStream::create: connect_timeout must be > 0"});
        }
        if (!cfg.ws_path.empty() &&
            cfg.ws_timeout <= std::chrono::milliseconds::zero()) {
            SPDLOG_LOGGER_WARN(log,
                "KernelTcpStream::create: ws_timeout={}ms must be > 0 when "
                "ws_path is non-empty",
                cfg.ws_timeout.count());
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "KernelTcpStream::create: ws_timeout must be > 0"});
        }

        // Validate optional proxy config up-front to avoid constructing
        // (and then immediately tearing down) a ByteSocket on a bad config.
        if (cfg.proxy.has_value()) {
            auto pv = cfg.proxy->validate();
            if (!pv) {
                SPDLOG_LOGGER_WARN(log,
                    "KernelTcpStream::create: ProxyConfig invalid: {}",
                    pv.error().detail);
                return std::unexpected(pv.error());
            }
        }

        // Allocate first so the ByteSocket lives inside the returned object
        // and the ReassemblyBuffer's vector is not copied around.
        auto stream = std::unique_ptr<KernelTcpStream>(
            new KernelTcpStream(std::move(cfg)));

        // ── TCP connect target: proxy (if set) vs direct upstream ────────
        //
        // When a proxy is configured, the initial TCP connect targets the
        // *proxy*, not the ultimate upstream. After the CONNECT handshake
        // succeeds the socket is tunneled to `cfg.remote`, which TLS / WS
        // (and the application) will then see transparently.
        SocketAddr connect_target = stream->cfg_.remote;
        if (stream->cfg_.proxy.has_value()) {
            auto proxy_ip_r = Ipv4Addr::parse(stream->cfg_.proxy->host);
            if (!proxy_ip_r) {
                SPDLOG_LOGGER_WARN(log,
                    "KernelTcpStream::create: ProxyConfig.host='{}' "
                    "must be a dotted-quad IPv4 literal: {}",
                    stream->cfg_.proxy->host, proxy_ip_r.error().detail);
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "KernelTcpStream::create: proxy.host must be IPv4 literal"});
            }
            connect_target = SocketAddr{*proxy_ip_r, stream->cfg_.proxy->port};
            SPDLOG_LOGGER_DEBUG(log,
                "KernelTcpStream::create: routing via proxy {}",
                connect_target.to_string());
        }

        // The local-bind side is *upstream* configuration: even when a
        // proxy is in use, the bind is on our local end of the wire to the
        // proxy. So we always pass `cfg_.local`, regardless of whether the
        // immediate connect_target is the proxy or the upstream itself.
        auto cr = stream->sock_.connect(connect_target,
                                        stream->cfg_.connect_timeout,
                                        stream->cfg_.local);
        if (!cr) {
            SPDLOG_LOGGER_WARN(log,
                "KernelTcpStream::create: connect failed: {}", cr.error().detail);
            // Distinguish proxy-TCP-connect failure from direct connect
            // failure so callers can handle it separately (the plan's
            // Error triad: ProxyConnectFailed vs ConnectFailed).
            if (stream->cfg_.proxy.has_value()) {
                return std::unexpected(core::ErrorInfo{
                    core::Error::ProxyConnectFailed,
                    "KernelTcpStream::create: TCP connect to proxy failed"});
            }
            return std::unexpected(cr.error());
        }

        if (stream->cfg_.tcp_nodelay) {
            (void)stream->sock_.set_no_delay(true);
        }

        // HTTP CONNECT handshake (before TLS). At this point we are
        // TCP-connected to the proxy. Drive the
        // CONNECT handshake over a plain ByteSocket sink; the proxy
        // either returns 200 (tunnel established) or an error status.
        if (stream->cfg_.proxy.has_value()) {
            detail::PlainWsSink plain_sink{&stream->sock_};
            std::vector<uint8_t> connect_leftover;
            auto connect_r = ::eph::net::detail::perform_http_connect(
                plain_sink,
                *stream->cfg_.proxy,
                stream->cfg_.remote.ip.to_string(),
                stream->cfg_.remote.port,
                &connect_leftover);
            if (!connect_r) {
                SPDLOG_LOGGER_WARN(log,
                    "KernelTcpStream::create: CONNECT handshake failed: {}",
                    connect_r.error().detail);
                return std::unexpected(connect_r.error());
            }
            // Rare: proxy sent extra bytes after the 200. For a plaintext
            // post-proxy stream (no TLS, no WS) we seed them into the
            // reasm buffer. For TLS / WS they'd have to survive through
            // aws-lc's BIO, which our TlsState::handshake doesn't expose —
            // so we refuse the stream with a diagnostic rather than
            // silently desync.
            if (!connect_leftover.empty()) {
                if constexpr (EnableTls) {
                    SPDLOG_LOGGER_WARN(log,
                        "KernelTcpStream::create: {}B over-read from proxy "
                        "cannot be threaded through TLS handshake input; "
                        "refusing the stream",
                        connect_leftover.size());
                    return std::unexpected(core::ErrorInfo{
                        core::Error::ProxyHandshakeFailed,
                        "KernelTcpStream::create: proxy over-read before TLS "
                        "is not supported"});
                } else if (!stream->cfg_.ws_path.empty()) {
                    SPDLOG_LOGGER_WARN(log,
                        "KernelTcpStream::create: {}B over-read from proxy "
                        "cannot be threaded through WS handshake input; "
                        "refusing the stream",
                        connect_leftover.size());
                    return std::unexpected(core::ErrorInfo{
                        core::Error::ProxyHandshakeFailed,
                        "KernelTcpStream::create: proxy over-read before WS "
                        "is not supported"});
                } else {
                    // Pure plaintext TCP post-CONNECT: seed into reasm.
                    if (stream->reasm_.writable_capacity() <
                        connect_leftover.size()) {
                        return std::unexpected(core::ErrorInfo{
                            core::Error::BufferFull,
                            "KernelTcpStream::create: proxy leftover exceeds "
                            "reasm capacity"});
                    }
                    std::memcpy(stream->reasm_.writable_ptr(),
                                connect_leftover.data(),
                                connect_leftover.size());
                    stream->reasm_.commit_write(connect_leftover.size());
                    SPDLOG_LOGGER_DEBUG(log,
                        "KernelTcpStream::create: seeded {}B post-CONNECT "
                        "bytes into reasm buffer",
                        connect_leftover.size());
                }
            }
            SPDLOG_LOGGER_INFO(log,
                "KernelTcpStream::create: HTTP CONNECT tunnel established "
                "via {}:{} -> {}",
                stream->cfg_.proxy->host, stream->cfg_.proxy->port,
                stream->cfg_.remote.to_string());
        }

        if constexpr (EnableTls) {
            // TLS 1.3 handshake via aws-lc, driven through a
            // ByteSocketTcpAdapter. After handshake the AEAD context is
            // owned by stream->tls_ and the bare ByteSocket resumes ownership
            // of the fd for the data plane.
            auto h = stream->tls_.handshake(stream->sock_, stream->cfg_.tls);
            if (!h) {
                SPDLOG_LOGGER_WARN(log,
                    "KernelTcpStream::create: TLS handshake failed: {}",
                    h.error().detail);
                return std::unexpected(h.error());
            }
            SPDLOG_LOGGER_INFO(log,
                "KernelTcpStream::create: TLS up fd={} remote={}",
                stream->sock_.fd(), stream->cfg_.remote.to_string());
        }

        // Optional WebSocket HTTP Upgrade. When cfg.ws_path is non-empty,
        // drive the RFC 6455 handshake
        // through either a plaintext (PlainWsSink) or TLS-wrapped
        // (TlsWsSink) byte sink. Any post-handshake bytes that arrived in
        // the same recv(2) as the 101 response are seeded into the
        // reassembly buffer so the codec sees them on the first poll.
        if (!stream->cfg_.ws_path.empty()) {
            // Pick the Host header: prefer an explicit ws_host, fall back
            // to the TLS SNI hostname (for wss://), then to the numeric
            // remote address (for ws://).
            std::string host_storage;
            std::string_view host_sv;
            if (!stream->cfg_.ws_host.empty()) {
                host_sv = stream->cfg_.ws_host;
            } else if constexpr (EnableTls) {
                if (!stream->cfg_.tls.hostname.empty()) {
                    host_sv = stream->cfg_.tls.hostname;
                }
            }
            if (host_sv.empty()) {
                host_storage = stream->cfg_.remote.to_string();
                host_sv      = host_storage;
            }

            std::vector<uint8_t> leftover;
            std::expected<void, core::ErrorInfo> hs_result;
            if constexpr (EnableTls) {
                detail::TlsWsSink sink{&stream->sock_, &stream->tls_};
                hs_result = ::eph::net::detail::perform_ws_handshake(
                    sink, host_sv, stream->cfg_.ws_path,
                    std::span<const ::eph::net::HttpHeader>(
                        stream->cfg_.ws_extra_headers),
                    stream->cfg_.ws_timeout,
                    &leftover);
            } else {
                detail::PlainWsSink sink{&stream->sock_};
                hs_result = ::eph::net::detail::perform_ws_handshake(
                    sink, host_sv, stream->cfg_.ws_path,
                    std::span<const ::eph::net::HttpHeader>(
                        stream->cfg_.ws_extra_headers),
                    stream->cfg_.ws_timeout,
                    &leftover);
            }
            if (!hs_result) {
                SPDLOG_LOGGER_WARN(log,
                    "KernelTcpStream::create: WS handshake failed: {}",
                    hs_result.error().detail);
                return std::unexpected(hs_result.error());
            }

            // Seed any post-handshake over-read into the reasm buffer so
            // the codec sees it on the first poll_once_() call.
            if (!leftover.empty()) {
                if (stream->reasm_.writable_capacity() < leftover.size()) {
                    SPDLOG_LOGGER_WARN(log,
                        "KernelTcpStream::create: reasm cannot hold {}B "
                        "of post-handshake over-read",
                        leftover.size());
                    return std::unexpected(core::ErrorInfo{
                        core::Error::BufferFull,
                        "KernelTcpStream::create: ws leftover exceeds reasm capacity"});
                }
                std::memcpy(stream->reasm_.writable_ptr(),
                            leftover.data(), leftover.size());
                stream->reasm_.commit_write(leftover.size());
                SPDLOG_LOGGER_DEBUG(log,
                    "KernelTcpStream::create: seeded {}B of post-handshake "
                    "bytes into reasm buffer", leftover.size());
            }

            SPDLOG_LOGGER_INFO(log,
                "KernelTcpStream::create: WS upgrade OK path='{}' fd={}",
                stream->cfg_.ws_path, stream->sock_.fd());
        }

        stream->state_ = TcpState::Established;
        if constexpr (!EnableTls) {
            SPDLOG_LOGGER_INFO(log,
                "KernelTcpStream::create: connected fd={} remote={}",
                stream->sock_.fd(), stream->cfg_.remote.to_string());
        }
        return stream;
    }

    ~KernelTcpStream() {
        // If still attached to a Poller, remove ourselves first so the
        // Poller's entries_ does not retain a dangling pointer. The Poller
        // clears our attached_to_ via notify_detached_ during the remove.
        if (attached_to_ != nullptr) {
            SPDLOG_LOGGER_DEBUG(detail::tcp_stream_logger(),
                "~KernelTcpStream: auto-detach fd={}", sock_.fd());
            (void)attached_to_->remove(this);
        }
        // ByteSocket closes automatically on its own destruction.
    }

    KernelTcpStream(const KernelTcpStream&)            = delete;
    KernelTcpStream& operator=(const KernelTcpStream&) = delete;
    KernelTcpStream(KernelTcpStream&&)                 = delete;
    KernelTcpStream& operator=(KernelTcpStream&&)      = delete;

    // ── Public fields (Stream concept) ───────────────────────────────────

    /// @brief Invoked once per decoded frame. Assigned by user code
    ///        before attach; the Poller drives `poll_once_` which calls it.
    OnMessage on_message;

    // ── Stream concept API ───────────────────────────────────────────────

    /// @brief Send `data` bytes to the peer. Fails with `NotAttached` if
    ///        the stream has not been added to a Poller yet.
    [[nodiscard]] std::expected<std::size_t, core::ErrorInfo>
    send(std::span<const uint8_t> data) noexcept {
        if (attached_to_ == nullptr) {
            return std::unexpected(core::ErrorInfo{
                core::Error::NotAttached,
                "KernelTcpStream::send called before attach"});
        }
        if (state_ != TcpState::Established) {
            return std::unexpected(core::ErrorInfo{
                core::Error::Disconnected,
                "KernelTcpStream::send: state != Established"});
        }
        // When TLS is enabled, encrypt the bytes into one or more
        // TLS records before forwarding to the socket. The plaintext API
        // is still bytes-in / bytes-out — the caller has already encoded
        // their frames via `WsCodec::encode` etc.
        if constexpr (EnableTls) {
            tls_send_buf_.clear();
            auto enc = tls_.encrypt_for_send(data.data(), data.size(),
                                              tls_send_buf_);
            if (!enc) {
                return std::unexpected(enc.error());
            }
            auto sr = sock_.send(tls_send_buf_);
            if (!sr) return std::unexpected(sr.error());
            // Return plaintext byte count (the API contract is plaintext-len).
            return data.size();
        } else {
            return sock_.send(data);
        }
    }

    /// @brief Initiate a graceful half-close (shutdown(SHUT_WR) equivalent).
    ///
    /// Error-path policy:
    ///   - ENOTCONN: peer already closed; tolerated silently (DEBUG-logged)
    ///     because the caller's intent ("stop sending") is already achieved.
    ///     State flips to FinWait1 as if the syscall had succeeded.
    ///   - EBADF: our fd is bogus — programming error. WARN-logged and
    ///     reported as Disconnected without touching state_.
    ///   - other errnos: WARN-logged, state unchanged, reported as
    ///     Disconnected. The caller can retry or destroy the stream.
    [[nodiscard]] std::expected<void, core::ErrorInfo>
    close_gracefully() noexcept {
        auto* log = detail::tcp_stream_logger();
        const int fd = sock_.fd();
        if (fd < 0) {
            // Already closed — treat as idempotent success and flip state.
            state_ = TcpState::Closed;
            SPDLOG_LOGGER_DEBUG(log,
                "KernelTcpStream::close_gracefully: fd already closed");
            return {};
        }
        if (::shutdown(fd, SHUT_WR) != 0) {
            const int err = errno;
            if (err == ENOTCONN) {
                // Peer already tore the TCP session down before we got here.
                // Not an error — our half-close intent is fulfilled.
                SPDLOG_LOGGER_DEBUG(log,
                    "KernelTcpStream::close_gracefully fd={} "
                    "peer already closed (ENOTCONN)", fd);
                state_ = TcpState::FinWait1;
                return {};
            }
            SPDLOG_LOGGER_WARN(log,
                "KernelTcpStream::close_gracefully fd={} errno={} ({})",
                fd, err, std::strerror(err));
            return std::unexpected(core::ErrorInfo{
                core::Error::Disconnected,
                "KernelTcpStream::close_gracefully: shutdown(SHUT_WR) failed"});
        }
        state_ = TcpState::FinWait1;
        SPDLOG_LOGGER_DEBUG(log,
            "KernelTcpStream::close_gracefully fd={}", fd);
        return {};
    }

    [[nodiscard]] bool is_attached() const noexcept {
        return attached_to_ != nullptr;
    }

    [[nodiscard]] TcpState state() const noexcept { return state_; }

    [[nodiscard]] int fd() const noexcept { return sock_.fd(); }

    // ── Pollable concept API ─────────────────────────────────────────────
    //
    // These methods are conceptually private to the Poller. They are public
    // because:
    //   1. The `eph::net::Pollable` concept check needs to invoke them on
    //      a non-const T&; a `private` + `friend KernelPoller` combination
    //      would work but makes the concept check at the top of poller.hpp
    //      reach through friendship which C++ concepts do not resolve.
    //   2. Exposing them is harmless — they are noexcept, purely drive I/O,
    //      and the Poller is the only code expected to call them.

    /// @brief Pollable::poll_once_ — drain the socket, decode, dispatch.
    ///
    /// Drains the socket in a single recv() call (non-blocking), feeds the
    /// bytes through the codec until `Ok(None)` is returned, and invokes
    /// `on_message` once per decoded frame. Returns the number of frames
    /// delivered.
    std::size_t poll_once_() noexcept {
        if (state_ != TcpState::Established) return 0;
        if (!on_message) {
            // No sink — still drain to avoid stalling the peer, but skip
            // the decode path (codec.decode would be wasted work).
            //
            // This is a developer-error footgun: a user who forgets to
            // assign `on_message` before `poller->add(stream)` will
            // silently discard every frame. Emit a warn-once diagnostic
            // on the first sink-less drain so the mistake surfaces at
            // runtime instead of becoming silent data loss.
            if (!no_sink_warned_) {
                SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                    "KernelTcpStream::poll_once_: on_message is unset for "
                    "fd={} — discarding inbound bytes (warn-once per stream). "
                    "Assign on_message before attaching to the Poller.",
                    sock_.fd());
                no_sink_warned_ = true;
            }
            uint8_t sink[detail::kNoSinkDrainBytes];
            auto r = sock_.recv(sink, sizeof(sink));
            if (!r && r.error().code == core::Error::Disconnected) {
                state_ = TcpState::Closed;
            }
            return 0;
        }

        // Drain any pre-seeded / carry-over bytes BEFORE recv. This handles
        // the post-WS-handshake / post-HTTP-CONNECT case where `create()`
        // committed over-read bytes into `reasm_` but the kernel socket
        // buffer was already drained by the handshake `recv()`. With EPOLLIN
        // level-triggered, epoll_wait() will not fire until the peer sends
        // more bytes, so those seeded bytes would stall in `reasm_` until
        // some unrelated network packet arrives. Also covers the TLS path
        // where `tls_plain_buf_` may hold leftover plaintext from a prior
        // poll. A drain on an empty buffer is a cheap no-op.
        std::size_t preserved = 0;
        if (reasm_.readable() > 0 || (EnableTls && tls_plain_head_ < tls_plain_buf_.size())) {
            preserved = drain_codec_();
            if (state_ != TcpState::Established) {
                return preserved;
            }
        }

        // Compact front-headroom before the next recv so the tail keeps
        // growing. No-op if head_ == 0.
        reasm_.compact();

        if (reasm_.writable_capacity() == 0) {
            SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                "KernelTcpStream::poll_once_: reasm buffer full; "
                "dropping connection");
            state_ = TcpState::Closed;
            return preserved;
        }

        auto rr = sock_.recv(reasm_.writable_ptr(), reasm_.writable_capacity());
        if (!rr) {
            const auto& err = rr.error();
            if (err.code == core::Error::WouldBlock) {
                // Epoll is level-triggered: returning the already-drained
                // count is fine — the next poll will pick up fresh bytes.
                return preserved;
            }
            if (err.code == core::Error::Disconnected) {
                SPDLOG_LOGGER_INFO(detail::tcp_stream_logger(),
                    "KernelTcpStream::poll_once_: peer closed fd={}",
                    sock_.fd());
                state_ = TcpState::Closed;
                return preserved;
            }
            SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                "KernelTcpStream::poll_once_: recv err={}", err.detail);
            state_ = TcpState::Closed;
            return preserved;
        }
        reasm_.commit_write(*rr);

        return preserved + drain_codec_();
    }

    // ── Poller-facing friend hooks ───────────────────────────────────────

    /// @brief Invoked by `KernelPoller::add` to record our attachment.
    void notify_attached_(KernelPoller* p) noexcept {
        attached_to_ = p;
    }

    /// @brief Invoked by `KernelPoller::remove` and by the Poller destructor.
    void notify_detached_() noexcept {
        attached_to_ = nullptr;
    }

    /// @brief Pollable `is_attached_` — mirrors the public is_attached().
    [[nodiscard]] bool is_attached_() const noexcept {
        return attached_to_ != nullptr;
    }

    /// @brief Pollable `native_handle` — kernel fd reinterpreted as void*.
    [[nodiscard]] void* native_handle() const noexcept {
        return reinterpret_cast<void*>(
            static_cast<std::intptr_t>(sock_.fd()));
    }

private:
    // ── Construction ─────────────────────────────────────────────────────

    explicit KernelTcpStream(StreamConfig cfg)
        : cfg_(std::move(cfg)),
          reasm_(cfg_.reasm_capacity) {}

    // ── Codec drain loop ─────────────────────────────────────────────────

    /// @brief Feed buffered bytes through the codec until it returns
    ///        `Ok(None)`, firing `on_message` per decoded frame.
    ///
    /// When TLS is enabled the reasm buffer holds ciphertext (raw TLS
    /// records). We decrypt complete records into `tls_plain_buf_` and run
    /// the codec over the plaintext. Partial records stay in the reasm
    /// buffer for the next poll.
    std::size_t drain_codec_() noexcept {
        std::size_t delivered = 0;
        // Scratch region backing the per-iteration `out_sink` below. Reused
        // across iterations via a fresh `OutputBuffer` each time — the
        // previous iteration's bytes are already flushed to the peer at the
        // bottom of the loop, so reusing the same storage is safe.
        uint8_t scratch[detail::kCodecAutoResponseBytes];

        if constexpr (EnableTls) {
            // 1) Decrypt as many complete TLS records as possible.
            //    process_records appends plaintext to the tail of
            //    tls_plain_buf_; any carry-over from a previous poll lives
            //    at [tls_plain_head_, tls_plain_buf_.size()) and is
            //    preserved below.
            auto cr = tls_.process_records(reasm_.read_ptr(),
                                            reasm_.readable(),
                                            tls_plain_buf_);
            if (!cr) {
                SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                    "KernelTcpStream::drain_codec_: TLS process_records "
                    "failed: {}", cr.error().detail);
                state_ = TcpState::Closed;
                return 0;
            }
            reasm_.consume(*cr);

            // 2) Run the codec over the decrypted plaintext window.
            //    Walk `tls_plain_head_` forward rather than erasing from
            //    the front — the earlier std::vector::erase(begin,
            //    begin+plain_off) shifted the entire remaining tail every
            //    poll and was the hottest allocation-adjacent op in the
            //    TLS RX path (batch2-round2 MED-1).
            while (tls_plain_head_ < tls_plain_buf_.size()) {
                const std::size_t before =
                    tls_plain_buf_.size() - tls_plain_head_;
                detail::SpanView view(
                    tls_plain_buf_.data() + tls_plain_head_, before);

                // Per-iteration sink: bounded to one control-frame worth
                // of auto-response (pong / close-ack). Flush it before
                // branching on `dr` so a close-ack written alongside
                // WsCloseReceived reaches the wire before state_ flips.
                core::OutputBuffer out_sink(scratch, sizeof(scratch));
                auto dr = codec_.decode(view, out_sink);
                if (out_sink.size() > 0) {
                    auto sr = this->send(std::span<const uint8_t>(
                        out_sink.data(), out_sink.size()));
                    if (!sr) {
                        SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                            "KernelTcpStream::drain_codec_(TLS): "
                            "auto-response send failed ({} bytes): {}",
                            out_sink.size(), sr.error().detail);
                    } else {
                        SPDLOG_LOGGER_DEBUG(detail::tcp_stream_logger(),
                            "KernelTcpStream::drain_codec_(TLS): sent {} "
                            "auto-response bytes", out_sink.size());
                    }
                }
                if (!dr) {
                    SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                        "KernelTcpStream::drain_codec_: decode error (TLS): {}",
                        dr.error().detail);
                    state_ = TcpState::Closed;
                    tls_plain_buf_.clear();
                    tls_plain_head_ = 0;
                    return delivered;
                }
                const std::size_t consumed = before - view.length();
                tls_plain_head_ += consumed;

                if (!dr->has_value()) {
                    // Ok(None) can mean either "codec auto-handled a
                    // control frame and consumed bytes" (e.g. WsCodec
                    // absorbing a ping) or "need more bytes". Only the
                    // latter justifies a break; the former should keep
                    // draining so back-to-back control frames in one
                    // recv() burst are all processed before we yield
                    // back to the epoll loop. A burst of server pings
                    // that stopped at the first one would otherwise
                    // stall until the next level-triggered wakeup.
                    if (consumed == 0) break;
                    continue;
                }

                // Guard against infinite loop: codec returned a frame
                // but did not advance the view.
                if (consumed == 0) {
                    SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                        "KernelTcpStream::drain_codec_(TLS): codec returned frame "
                        "but consumed 0 bytes — breaking to avoid infinite loop");
                    break;
                }

                const auto& frame = **dr;
                if (frame.size() > 0) {
                    if (saturate_u16_clamps(frame.size()) && !trunc_warned_) {
                        SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                            "KernelTcpStream::drain_codec_(TLS): frame size "
                            "{} > 0xFFFF; on_message length is clamped to "
                            "0xFFFF (warn-once per stream)",
                            frame.size());
                        trunc_warned_ = true;
                    }
                    on_message(frame.data(), saturate_u16(frame.size()));
                    ++delivered;
                }
            }
            // Compaction policy:
            //   - Fully consumed: O(1) reset to reuse capacity.
            //   - Partially consumed AND dead-head is large relative to
            //     the live tail: shift the live tail to the front so the
            //     vector's live window does not grow without bound across
            //     many polls of a slow codec. Threshold chosen such that
            //     the amortized cost stays O(1) per byte (we shift only
            //     when dead bytes >= live bytes, so each byte is shifted
            //     at most log-times over its lifetime).
            //   - Partially consumed AND dead-head is small: leave in
            //     place — the next poll will extend the tail with new
            //     plaintext and the codec will likely drain it.
            if (tls_plain_head_ == tls_plain_buf_.size()) {
                tls_plain_buf_.clear();
                tls_plain_head_ = 0;
            } else if (tls_plain_head_ > 0 &&
                       tls_plain_head_ >=
                           tls_plain_buf_.size() - tls_plain_head_) {
                const std::size_t live =
                    tls_plain_buf_.size() - tls_plain_head_;
                std::memmove(tls_plain_buf_.data(),
                             tls_plain_buf_.data() + tls_plain_head_, live);
                tls_plain_buf_.resize(live);
                tls_plain_head_ = 0;
            }
            return delivered;
        }

        // Plaintext path.
        while (reasm_.readable() > 0) {
            const std::size_t before = reasm_.readable();
            detail::SpanView view(reasm_.read_ptr(), before);

            // Per-iteration sink; flushed before any branch on `dr`
            // so close-acks written on WsCloseReceived reach the wire
            // before state_ flips.
            core::OutputBuffer out_sink(scratch, sizeof(scratch));
            auto dr = codec_.decode(view, out_sink);
            if (out_sink.size() > 0) {
                auto sr = this->send(std::span<const uint8_t>(
                    out_sink.data(), out_sink.size()));
                if (!sr) {
                    SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                        "KernelTcpStream::drain_codec_: "
                        "auto-response send failed ({} bytes): {}",
                        out_sink.size(), sr.error().detail);
                } else {
                    SPDLOG_LOGGER_DEBUG(detail::tcp_stream_logger(),
                        "KernelTcpStream::drain_codec_: sent {} "
                        "auto-response bytes", out_sink.size());
                }
            }
            if (!dr) {
                SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                    "KernelTcpStream::drain_codec_: decode error: {}",
                    dr.error().detail);
                state_ = TcpState::Closed;
                break;
            }
            const std::size_t consumed = before - view.length();
            reasm_.consume(consumed);

            if (!dr->has_value()) {
                // See TLS branch for the rationale: Ok(None) + consumed>0
                // means the codec auto-handled a control frame and we
                // should keep draining. Only consumed==0 is "need more
                // bytes" and warrants a break.
                if (consumed == 0) break;
                continue;
            }
            // Guard against infinite loop: codec returned a frame
            // but did not advance the view.
            if (consumed == 0) {
                SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                    "KernelTcpStream::drain_codec_: codec returned frame "
                    "but consumed 0 bytes — breaking to avoid infinite loop");
                break;
            }
            const auto& frame = **dr;
            if (frame.size() > 0) {
                if (saturate_u16_clamps(frame.size()) && !trunc_warned_) {
                    SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                        "KernelTcpStream::drain_codec_: frame size {} > "
                        "0xFFFF; on_message length is clamped to 0xFFFF "
                        "(warn-once per stream)",
                        frame.size());
                    trunc_warned_ = true;
                }
                on_message(frame.data(), saturate_u16(frame.size()));
                ++delivered;
            }
        }
        return delivered;
    }

    // ── Data members ─────────────────────────────────────────────────────

    StreamConfig                cfg_{};
    detail::ByteSocket          sock_{};
    [[no_unique_address]] C     codec_{};
    [[no_unique_address]] std::conditional_t<EnableTls,
                                              detail::TlsState,
                                              std::monostate> tls_{};
    detail::ReassemblyBuffer    reasm_;
    // TLS plaintext staging buffer (only used when EnableTls=true). Lives
    // here so its capacity can amortize across many polls. The empty-base
    // size penalty for the plaintext path is one std::vector<uint8_t> —
    // 24 bytes.
    //
    // Compacted via a head-index (`tls_plain_head_`) rather than
    // `erase(begin, begin + n)` so that post-decode cleanup is O(1)
    // instead of O(remaining-bytes) — see batch2-round2 MED-1. When the
    // head reaches the size the vector and index are both reset to zero
    // so capacity is reused across polls.
    std::vector<uint8_t>        tls_plain_buf_{};
    std::size_t                 tls_plain_head_{0};
    /// TLS encrypt staging — sized per-call so we don't realloc.
    std::vector<uint8_t>        tls_send_buf_{};
    KernelPoller*               attached_to_{nullptr};
    TcpState                    state_{TcpState::Closed};
    /// Warn-once latch for `saturate_u16` clamping during on_message dispatch.
    /// See batch3-round1 MEDIUM-1: silent truncation of frames >64 KiB.
    bool                        trunc_warned_{false};
    /// Warn-once latch for the no-on_message drain path (MEDIUM-2). Set on
    /// the first poll that finds `on_message` unset so operators see the
    /// misconfiguration surface once per stream instead of never.
    bool                        no_sink_warned_{false};
};

} // namespace eph::net::kernel
