#pragma once

/// @file tcp_stream.hpp
/// Epoll-backed TCP stream satisfying the `eph::net::Stream` concept.
///
/// Part of Phase 3 of the v3.3 refactor (see
/// .artifacts/design-eph-v3.3-architecture-20260410.md).
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
///
/// Phase 3 scope:
///   - **Plaintext path is fully functional** (EnableTls == false).
///   - **EnableTls == true** compiles and the class name exists, but the
///     TLS state is a stub: `create()` with TLS enabled will return
///     `TlsHandshakeFailed` until Phase 5 wires the real aws-lc TLS record
///     machinery. The design doc permits this staging so Phase 3 can land
///     without dragging in the entire `eph-transport/detail/tls_*` stack.
///   - **WebSocket upgrade** is likewise Phase 5: the stream delivers raw
///     bytes to the codec via `poll_once_`, and the codec (`WsCodec`) is
///     responsible for emitting / consuming frame structure. The HTTP
///     upgrade handshake itself is orthogonal and will slot in alongside
///     TLS handshake in Phase 5.
///
/// What IS covered by Phase 3:
///   - Sync connect with timeout.
///   - `poll_once_()` drains the fd via `recv` → codec decode → on_message.
///   - `send()` writes bytes directly via the ByteSocket (no codec encode —
///     the caller is responsible for supplying wire-format frames, matching
///     the design doc's send signature of `std::span<const uint8_t>`).
///   - `NotAttached` error is returned when `send` is called before the
///     stream is attached to a Poller.
///   - Destructor auto-detaches from the Poller if still attached, so that
///     the Poller's `entries_` does not get a dangling pointer.
///   - Concept conformance static_asserts for the common instantiations.

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
#include <spdlog/sinks/stdout_color_sinks.h>

#include "eph/core/codec.hpp"
#include "eph/core/error.hpp"
#include "eph/net/concepts.hpp"
#include "eph/net/kernel/config.hpp"
#include "eph/net/kernel/detail/byte_socket.hpp"
#include "eph/net/kernel/detail/reassembly_buffer.hpp"
#include "eph/net/kernel/detail/span_view.hpp"
#include "eph/net/kernel/detail/tls_state.hpp"  // Phase 5: real TLS state
#include "eph/net/kernel/poller.hpp"
#include "eph/net/reconnect_policy.hpp"
#include "eph/net/tcp_state.hpp"

namespace eph::net::kernel {

namespace detail {

/// @brief Lazily-initialized logger for the kernel TcpStream subsystem.
inline spdlog::logger* tcp_stream_logger() {
    static auto* l = [] {
        auto lg = spdlog::get("net.kernel.tcp_stream");
        if (!lg) {
            try {
                lg = spdlog::stdout_color_mt("net.kernel.tcp_stream");
            } catch (const spdlog::spdlog_ex&) {
                lg = spdlog::get("net.kernel.tcp_stream");
            }
        }
        return lg.get();
    }();
    return l;
}

// Phase 5: TlsState is now defined in detail/tls_state.hpp (real aws-lc
// AEAD machinery, replacing the Phase 3 stub).

} // namespace detail

// ---------------------------------------------------------------------------
// KernelTcpStream
// ---------------------------------------------------------------------------

/// @brief Stream impl backed by a non-blocking AF_INET socket + epoll.
///
/// @tparam C          StreamCodec implementation (duck-typed per the
///                    `eph::core::StreamCodec` concept).
/// @tparam EnableTls  When true, the instance carries TLS session state
///                    and `create()` runs the handshake before returning.
///                    Phase 3 staging: with `EnableTls=true` the handshake
///                    currently returns `TlsHandshakeFailed`. Phase 5
///                    wires the real session.
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

        // Allocate first so the ByteSocket lives inside the returned object
        // and the ReassemblyBuffer's vector is not copied around.
        auto stream = std::unique_ptr<KernelTcpStream>(
            new KernelTcpStream(std::move(cfg)));

        auto cr = stream->sock_.connect(stream->cfg_.remote,
                                        stream->cfg_.connect_timeout);
        if (!cr) {
            SPDLOG_LOGGER_WARN(log,
                "KernelTcpStream::create: connect failed: {}", cr.error().detail);
            return std::unexpected(cr.error());
        }

        if (stream->cfg_.tcp_nodelay) {
            (void)stream->sock_.set_no_delay(true);
        }

        if constexpr (EnableTls) {
            // Phase 5: real TLS 1.3 handshake via aws-lc, driven through a
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
            stream->state_ = TcpState::Established;
            SPDLOG_LOGGER_INFO(log,
                "KernelTcpStream::create: TLS up fd={} remote={}",
                stream->sock_.fd(), stream->cfg_.remote.to_string());
            return stream;
        } else {
            stream->state_ = TcpState::Established;
            SPDLOG_LOGGER_INFO(log,
                "KernelTcpStream::create: connected fd={} remote={}",
                stream->sock_.fd(), stream->cfg_.remote.to_string());
            return stream;
        }
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
        // Phase 5: when TLS is enabled, encrypt the bytes into one or more
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
    [[nodiscard]] std::expected<void, core::ErrorInfo>
    close_gracefully() noexcept {
        if (sock_.fd() >= 0) {
            ::shutdown(sock_.fd(), SHUT_WR);
        }
        state_ = TcpState::FinWait1;
        SPDLOG_LOGGER_DEBUG(detail::tcp_stream_logger(),
            "KernelTcpStream::close_gracefully fd={}", sock_.fd());
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
            uint8_t sink[4096];
            auto r = sock_.recv(sink, sizeof(sink));
            if (!r && r.error().code == core::Error::Disconnected) {
                state_ = TcpState::Closed;
            }
            return 0;
        }

        // Compact front-headroom before the next recv so the tail keeps
        // growing. No-op if head_ == 0.
        reasm_.compact();

        if (reasm_.writable_capacity() == 0) {
            SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                "KernelTcpStream::poll_once_: reasm buffer full; "
                "dropping connection");
            state_ = TcpState::Closed;
            return 0;
        }

        auto rr = sock_.recv(reasm_.writable_ptr(), reasm_.writable_capacity());
        if (!rr) {
            const auto& err = rr.error();
            if (err.code == core::Error::WouldBlock) {
                // Epoll is level-triggered: returning 0 is fine.
                return 0;
            }
            if (err.code == core::Error::Disconnected) {
                SPDLOG_LOGGER_INFO(detail::tcp_stream_logger(),
                    "KernelTcpStream::poll_once_: peer closed fd={}",
                    sock_.fd());
                state_ = TcpState::Closed;
                return 0;
            }
            SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                "KernelTcpStream::poll_once_: recv err={}", err.detail);
            state_ = TcpState::Closed;
            return 0;
        }
        reasm_.commit_write(*rr);

        return drain_codec_();
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
    [[nodiscard]] void* native_handle() noexcept {
        return reinterpret_cast<void*>(
            static_cast<std::intptr_t>(sock_.fd()));
    }

private:
    // ── Construction ─────────────────────────────────────────────────────

    explicit KernelTcpStream(StreamConfig cfg)
        : cfg_(std::move(cfg)),
          reasm_(cfg_.reasm_capacity),
          reconnect_policy_(cfg_.reconnect) {}

    // ── Codec drain loop ─────────────────────────────────────────────────

    /// @brief Feed buffered bytes through the codec until it returns
    ///        `Ok(None)`, firing `on_message` per decoded frame.
    ///
    /// Phase 5: when TLS is enabled the reasm buffer holds ciphertext
    /// (raw TLS records). We decrypt complete records into `tls_plain_buf_`
    /// and run the codec over the plaintext. Partial records stay in the
    /// reasm buffer for the next poll.
    std::size_t drain_codec_() noexcept {
        std::size_t delivered = 0;
        // Scratch OutputBuffer for auto-response injection. The auto-
        // responses are queued for the next send() call (kernel-side
        // we don't yet plumb the sink back into the TX path automatically;
        // a Phase 6 cleanup will hook that into the codec contract).
        uint8_t          scratch[1024];
        core::OutputBuffer out_sink(scratch, sizeof(scratch));

        if constexpr (EnableTls) {
            // 1) Decrypt as many complete TLS records as possible.
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
            std::size_t plain_off = 0;
            while (plain_off < tls_plain_buf_.size()) {
                const std::size_t before = tls_plain_buf_.size() - plain_off;
                detail::SpanView view(tls_plain_buf_.data() + plain_off, before);

                auto dr = codec_.decode(view, out_sink);
                if (!dr) {
                    SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                        "KernelTcpStream::drain_codec_: decode error (TLS): {}",
                        dr.error().detail);
                    state_ = TcpState::Closed;
                    tls_plain_buf_.clear();
                    return delivered;
                }
                const std::size_t consumed = before - view.length();
                plain_off += consumed;

                if (!dr->has_value()) break;  // need more plaintext

                const auto& frame = **dr;
                if (frame.size() > 0) {
                    on_message(frame.data(),
                               static_cast<uint16_t>(
                                   frame.size() > 0xFFFFu ? 0xFFFFu : frame.size()));
                    ++delivered;
                }
            }
            // Compact the plaintext buffer: drop everything we consumed.
            if (plain_off > 0) {
                tls_plain_buf_.erase(tls_plain_buf_.begin(),
                                      tls_plain_buf_.begin() + plain_off);
            }
            return delivered;
        }

        // Plaintext path — original Phase 3 behavior.
        while (reasm_.readable() > 0) {
            const std::size_t before = reasm_.readable();
            detail::SpanView view(reasm_.read_ptr(), before);

            auto dr = codec_.decode(view, out_sink);
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
                break;
            }
            const auto& frame = **dr;
            if (frame.size() > 0) {
                on_message(frame.data(),
                           static_cast<uint16_t>(
                               frame.size() > 0xFFFFu ? 0xFFFFu : frame.size()));
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
    // 24 bytes. Phase 6 may swap this for a SPSC ring buffer if profiling
    // calls for it.
    std::vector<uint8_t>        tls_plain_buf_{};
    /// TLS encrypt staging — sized per-call so we don't realloc.
    std::vector<uint8_t>        tls_send_buf_{};
    KernelPoller*               attached_to_{nullptr};
    ReconnectPolicy             reconnect_policy_;
    TcpState                    state_{TcpState::Closed};
};

} // namespace eph::net::kernel
