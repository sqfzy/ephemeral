#pragma once

/// @file tcp_stream.hpp
/// DPDK user-space TCP stream satisfying `eph::net::Stream`.
///
/// Architecture:
///
///     user code
///        │
///        v
///     DpdkTcpStream<C, EnableTls>
///        │  ├── eph::dpdk::TcpSession<>  (byte-pipe; reuse of the
///        │  │                              battle-tested TCP state machine)
///        │  ├── C                         (StreamCodec template param)
///        │  ├── TlsState                  (only when EnableTls == true)
///        │  ├── reassembly buffer         (for codec's decode loop)
///        │  └── DpdkPoller<>*             (set by Poller::add)
///        │
///        v
///     DpdkPoller<> (lcore burst poll)

#include <array>
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

#include <rte_mbuf.h>

#include <spdlog/spdlog.h>

#include "eph/core/detail/logger.hpp"
#include "eph/core/codec.hpp"
#include "eph/core/error.hpp"
#include "eph/dpdk/packet_parse.hpp"
#include "eph/dpdk/tcp.hpp"
#include "eph/net/concepts.hpp"
#include "eph/net/detail/ws_handshake.hpp"   // WS HTTP handshake
#include "eph/net/dpdk/config.hpp"
#include "eph/net/dpdk/detail/mbuf_view.hpp"
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/tcp_state.hpp"

// The DPDK TLS path uses aws-lc exclusively (no vcpkg-openssl). ISN
// generation and WS mask-key use `getrandom(2)` so there is no OpenSSL
// symbol conflict. `detail/tls_state.hpp` provides the TLS 1.3 handshake
// and in-place AEAD for `EnableTls=true`.
#include "eph/net/dpdk/detail/tls_state.hpp"

namespace eph::net::dpdk {

namespace detail {

/// @brief Lazily-initialized logger for the DPDK TCP stream subsystem.
inline spdlog::logger* tcp_stream_logger() {
    static auto* l = ::eph::core::detail::make_logger("net.dpdk.tcp_stream");
    return l;
}

// TlsState (with aws-lc-backed in-place AEAD) lives in
// `detail/tls_state.hpp`.

/// @brief Reassembly buffer for the codec decode loop. Bytes dispatched
///        in from the Poller append here, then the codec drains them
///        incrementally. Implemented as a simple std::vector<uint8_t>
///        with a front cursor.
class ReasmBuffer {
public:
    explicit ReasmBuffer(std::size_t cap = 256 * 1024) { buf_.resize(cap); }

    [[nodiscard]] std::size_t readable() const noexcept { return tail_ - head_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return buf_.size(); }
    [[nodiscard]] std::size_t writable_capacity() const noexcept {
        return buf_.size() - tail_;
    }
    [[nodiscard]] const uint8_t* read_ptr() const noexcept {
        return buf_.data() + head_;
    }
    [[nodiscard]] uint8_t* writable_ptr() noexcept { return buf_.data() + tail_; }

    void commit_write(std::size_t n) noexcept { tail_ += n; }
    void consume(std::size_t n) noexcept {
        if (n >= readable()) {
            head_ = tail_;  // clamp — never push head_ past tail_
        } else {
            head_ += n;
        }
    }
    void compact() noexcept {
        if (head_ == 0) return;
        if (readable() == 0) {
            head_ = tail_ = 0;
            return;
        }
        std::memmove(buf_.data(), buf_.data() + head_, readable());
        tail_ -= head_;
        head_  = 0;
    }
    /// @brief Append `n` bytes from `src`, compacting first if needed.
    /// @return true on success, false if there is not enough room even
    ///         after compaction.
    [[nodiscard]] bool append(const uint8_t* src, std::size_t n) noexcept {
        if (writable_capacity() < n) {
            compact();
            if (writable_capacity() < n) return false;
        }
        std::memcpy(writable_ptr(), src, n);
        commit_write(n);
        return true;
    }

private:
    std::vector<uint8_t> buf_;
    std::size_t          head_{0};
    std::size_t          tail_{0};
};

// ---------------------------------------------------------------------------
// WS-handshake ByteSink adapters
// ---------------------------------------------------------------------------
//
// Mirrors the kernel variants: the DPDK byte pipe is `eph::dpdk::TcpSession<>`
// and its `send` + `poll_rx` return legacy string-typed errors, so the
// adapters translate into `core::ErrorInfo` before handing the bytes to
// `eph::net::detail::perform_ws_handshake`.
//
// Send path:
//   * TcpSession::send has an MSS cap — we chunk larger payloads on its
//     behalf (the handshake request is typically under one MSS so this
//     loop normally iterates once).
//
// Recv path:
//   * TcpSession::poll_rx drives a single rte_eth_rx_burst and dispatches
//     each payload segment via a callback. We accumulate segments into a
//     per-sink scratch vector and drain as many complete TLS records /
//     HTTP bytes as possible before returning.
//
// For the TLS variant we additionally run `tls.process_records_in_place`
// on the decoded ciphertext to produce plaintext staging before serving
// the handshake reader.

class PlainDpdkWsSink {
public:
    explicit PlainDpdkWsSink(::eph::dpdk::TcpSession<>* sess) noexcept
        : sess_(sess) {}

    [[nodiscard]] std::expected<std::size_t, ::eph::core::ErrorInfo>
    send(std::span<const uint8_t> data) noexcept {
        const std::size_t mss = sess_->mss();
        std::size_t off = 0;
        while (off < data.size()) {
            const std::size_t chunk =
                std::min<std::size_t>(mss, data.size() - off);
            auto r = sess_->send(data.data() + off, chunk);
            if (!r) {
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::Disconnected,
                    "PlainDpdkWsSink::send: TcpSession::send failed"});
            }
            if (*r == 0) {
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::BufferFull,
                    "PlainDpdkWsSink::send: TcpSession::send returned 0 bytes"});
            }
            off += *r;
        }
        return data.size();
    }

    [[nodiscard]] std::expected<std::size_t, ::eph::core::ErrorInfo>
    recv(uint8_t* buf, std::size_t cap) noexcept {
        // Drain any bytes still staged from a previous poll_rx burst.
        if (staged_off_ < staged_.size()) {
            const std::size_t n =
                std::min(cap, staged_.size() - staged_off_);
            std::memcpy(buf, staged_.data() + staged_off_, n);
            staged_off_ += n;
            if (staged_off_ == staged_.size()) {
                staged_.clear();
                staged_off_ = 0;
            }
            return n;
        }
        // Pull one burst; accept multiple recv loops if the burst was empty.
        for (int iter = 0; iter < 16; ++iter) {
            auto r = sess_->poll_rx(
                [this](const uint8_t* p, uint16_t len) {
                    staged_.insert(staged_.end(), p, p + len);
                });
            if (!r) {
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::Disconnected,
                    "PlainDpdkWsSink::recv: TcpSession::poll_rx failed"});
            }
            if (!staged_.empty()) {
                const std::size_t n = std::min(cap, staged_.size());
                std::memcpy(buf, staged_.data(), n);
                staged_off_ = n;
                if (staged_off_ == staged_.size()) {
                    staged_.clear();
                    staged_off_ = 0;
                }
                return n;
            }
            // Empty burst — tiny pause avoided; rely on the caller's
            // outer deadline (WouldBlock triggers a retry there).
        }
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::WouldBlock,
            "PlainDpdkWsSink::recv: no data after bounded retry"});
    }

private:
    ::eph::dpdk::TcpSession<>* sess_{nullptr};
    std::vector<uint8_t>        staged_{};
    std::size_t                 staged_off_{0};
};

class TlsDpdkWsSink {
public:
    TlsDpdkWsSink(::eph::dpdk::TcpSession<>* sess, TlsState* tls) noexcept
        : sess_(sess), tls_(tls) {}

    [[nodiscard]] std::expected<std::size_t, ::eph::core::ErrorInfo>
    send(std::span<const uint8_t> data) noexcept {
        tx_scratch_.clear();
        auto enc = tls_->encrypt_for_send(data.data(), data.size(), tx_scratch_);
        if (!enc) return std::unexpected(enc.error());

        const std::size_t mss = sess_->mss();
        std::size_t off = 0;
        while (off < tx_scratch_.size()) {
            const std::size_t chunk =
                std::min<std::size_t>(mss, tx_scratch_.size() - off);
            auto r = sess_->send(tx_scratch_.data() + off, chunk);
            if (!r) {
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::Disconnected,
                    "TlsDpdkWsSink::send: TcpSession::send failed"});
            }
            if (*r == 0) {
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::BufferFull,
                    "TlsDpdkWsSink::send: TcpSession::send returned 0 bytes"});
            }
            off += *r;
        }
        return data.size();
    }

    [[nodiscard]] std::expected<std::size_t, ::eph::core::ErrorInfo>
    recv(uint8_t* buf, std::size_t cap) noexcept {
        // Serve from plaintext stage first.
        if (plain_off_ < plain_.size()) {
            const std::size_t n = std::min(cap, plain_.size() - plain_off_);
            std::memcpy(buf, plain_.data() + plain_off_, n);
            plain_off_ += n;
            if (plain_off_ == plain_.size()) {
                plain_.clear();
                plain_off_ = 0;
            }
            return n;
        }

        for (int iter = 0; iter < 16; ++iter) {
            auto r = sess_->poll_rx(
                [this](const uint8_t* p, uint16_t len) {
                    cipher_.insert(cipher_.end(), p, p + len);
                });
            if (!r) {
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::Disconnected,
                    "TlsDpdkWsSink::recv: TcpSession::poll_rx failed"});
            }
            if (cipher_.empty()) continue;

            // In-place decrypt over a mutable copy: we need a contiguous
            // buffer we can write through. plain_ accumulates the emitted
            // plaintext slices copied out of the in-place buffer.
            auto cr = tls_->process_records_in_place(
                cipher_.data(), cipher_.size(),
                [this](uint8_t* p, std::size_t len) {
                    plain_.insert(plain_.end(), p, p + len);
                });
            if (!cr) return std::unexpected(cr.error());
            if (*cr > 0) {
                cipher_.erase(cipher_.begin(), cipher_.begin() + *cr);
            }

            if (!plain_.empty()) {
                const std::size_t n = std::min(cap, plain_.size());
                std::memcpy(buf, plain_.data(), n);
                plain_off_ = n;
                if (plain_off_ == plain_.size()) {
                    plain_.clear();
                    plain_off_ = 0;
                }
                return n;
            }
        }
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::WouldBlock,
            "TlsDpdkWsSink::recv: no plaintext after bounded retry"});
    }

private:
    ::eph::dpdk::TcpSession<>* sess_{nullptr};
    TlsState*                   tls_{nullptr};
    std::vector<uint8_t>        tx_scratch_{};
    std::vector<uint8_t>        cipher_{};
    std::vector<uint8_t>        plain_{};
    std::size_t                 plain_off_{0};
};

} // namespace detail

// Forward declaration of the specialized poller so the friend decl below
// can reference it without circular-include headache.
template <class P>
class DpdkPoller;

// ---------------------------------------------------------------------------
// DpdkTcpStream
// ---------------------------------------------------------------------------

/// @brief Stream impl backed by `eph::dpdk::TcpSession<>` and dispatched
///        by `DpdkPoller<>` on a lcore burst loop.
///
/// @tparam C          StreamCodec implementation (duck-typed per the
///                    `eph::core::StreamCodec` concept).
/// @tparam EnableTls  When true, TLS session state is carried and
///                    `create()` runs the TLS 1.3 handshake via aws-lc.
template <class C, bool EnableTls = true>
class DpdkTcpStream {
public:
    // ── Associated types (Stream concept) ────────────────────────────────

    using CodecType  = C;
    using PacketView = detail::MbufView;
    using OnMessage  = std::function<void(const uint8_t*, uint16_t)>;

    // ── Factory ──────────────────────────────────────────────────────────

    [[nodiscard]] static std::expected<std::unique_ptr<DpdkTcpStream>, core::ErrorInfo>
    create(StreamConfig cfg) noexcept {
        auto* log = detail::tcp_stream_logger();
        SPDLOG_LOGGER_DEBUG(log,
            "DpdkTcpStream::create: tls={} src={}:{} dst={}:{}", EnableTls,
            cfg.legacy.tuple.src_ip, cfg.legacy.tuple.src_port,
            cfg.legacy.tuple.dst_ip, cfg.legacy.tuple.dst_port);

        // Validate the legacy TcpConfig first — it carries all the DPDK
        // wiring (port/queue/MAC/mempool-adjacent parameters) and knows
        // exactly which invariants it needs. We convert the string error
        // to an ErrorInfo so the v3.3 error contract holds.
        auto verr = cfg.legacy.validate();
        if (!verr.empty()) {
            SPDLOG_LOGGER_WARN(log,
                "DpdkTcpStream::create: legacy validate failed: {}", verr);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkTcpStream::create: TcpConfig invalid"});
        }
        if (cfg.pool == nullptr) {
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkTcpStream::create: pool must not be null"});
        }
        // HTTP CONNECT proxies are unsupported on DPDK. HFT colo
        // deployments don't use proxies, and a DPDK client bypasses the
        // kernel userland stack that a proxy would be reachable through.
        // Reject up-front with a clear diagnostic so users who accidentally
        // reuse a kernel StreamConfig get an actionable error.
        if (cfg.proxy.has_value()) {
            SPDLOG_LOGGER_WARN(log,
                "DpdkTcpStream::create: HTTP CONNECT proxy not supported "
                "on DPDK backend (host={}, port={})",
                cfg.proxy->host, cfg.proxy->port);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkTcpStream::create: HTTP CONNECT proxy not supported "
                "on DPDK backend"});
        }

        // Construct the stream first so the TcpSession is rooted inside
        // its final storage location (TcpSession is move-constructible
        // but we prefer to keep the pointer stable for friend-hook thunks).
        auto stream = std::unique_ptr<DpdkTcpStream>(
            new DpdkTcpStream(std::move(cfg)));

        // Run the TCP 3-way handshake synchronously. TcpSession::connect
        // polls the NIC itself, which is safe because we are not yet
        // attached to any Poller — we own the queue exclusively for the
        // duration of the handshake.
        auto cr = stream->sess_.connect(stream->cfg_.connect_timeout);
        if (!cr) {
            SPDLOG_LOGGER_WARN(log,
                "DpdkTcpStream::create: TcpSession::connect failed: {}",
                cr.error());
            return std::unexpected(core::ErrorInfo{
                core::Error::ConnectFailed,
                "DpdkTcpStream::create: TcpSession::connect failed"});
        }

        if constexpr (EnableTls) {
            // TLS 1.3 handshake via aws-lc, driven through TcpSession<>
            // (which satisfies the TcpTransport concept via its
            // `send`/`poll_rx`/`state` triple). The hot-path AEAD state is
            // extracted into the TlsState object held as a
            // [[no_unique_address]] member of this stream; data frames
            // decrypt in place over the reasm buffer on the RX burst.
            auto h = stream->tls_.handshake(stream->sess_, stream->cfg_.tls);
            if (!h) {
                SPDLOG_LOGGER_WARN(log,
                    "DpdkTcpStream::create: TLS handshake failed: {}",
                    h.error().detail);
                return std::unexpected(h.error());
            }
            SPDLOG_LOGGER_INFO(log,
                "DpdkTcpStream::create: TLS 1.3 handshake complete");
        }

        // Optional WebSocket HTTP Upgrade. Same contract as
        // KernelTcpStream: empty ws_path skips entirely.
        if (!stream->cfg_.ws_path.empty()) {
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
                // Fall back to a synthesized IP:port from the 4-tuple.
                // dst_ip is stored in network byte order in TcpConfig.
                const auto& t = stream->cfg_.legacy.tuple;
                uint32_t ip_be = t.dst_ip;
                host_storage =
                    std::to_string((ip_be >>  0) & 0xFFu) + "." +
                    std::to_string((ip_be >>  8) & 0xFFu) + "." +
                    std::to_string((ip_be >> 16) & 0xFFu) + "." +
                    std::to_string((ip_be >> 24) & 0xFFu) + ":" +
                    std::to_string(t.dst_port);
                host_sv = host_storage;
            }

            std::vector<uint8_t> leftover;
            std::expected<void, core::ErrorInfo> hs_result;
            if constexpr (EnableTls) {
                detail::TlsDpdkWsSink sink(&stream->sess_, &stream->tls_);
                hs_result = ::eph::net::detail::perform_ws_handshake(
                    sink, host_sv, stream->cfg_.ws_path,
                    std::span<const ::eph::net::HttpHeader>(
                        stream->cfg_.ws_extra_headers),
                    stream->cfg_.ws_timeout,
                    &leftover);
            } else {
                detail::PlainDpdkWsSink sink(&stream->sess_);
                hs_result = ::eph::net::detail::perform_ws_handshake(
                    sink, host_sv, stream->cfg_.ws_path,
                    std::span<const ::eph::net::HttpHeader>(
                        stream->cfg_.ws_extra_headers),
                    stream->cfg_.ws_timeout,
                    &leftover);
            }
            if (!hs_result) {
                SPDLOG_LOGGER_WARN(log,
                    "DpdkTcpStream::create: WS handshake failed: {}",
                    hs_result.error().detail);
                return std::unexpected(hs_result.error());
            }
            if (!leftover.empty()) {
                if (!stream->reasm_.append(leftover.data(), leftover.size())) {
                    SPDLOG_LOGGER_WARN(log,
                        "DpdkTcpStream::create: reasm append {}B leftover failed",
                        leftover.size());
                    return std::unexpected(core::ErrorInfo{
                        core::Error::BufferFull,
                        "DpdkTcpStream::create: ws leftover exceeds reasm capacity"});
                }
                SPDLOG_LOGGER_DEBUG(log,
                    "DpdkTcpStream::create: seeded {}B post-handshake bytes "
                    "into reasm buffer", leftover.size());
            }
            SPDLOG_LOGGER_INFO(log,
                "DpdkTcpStream::create: WS upgrade OK path='{}'",
                stream->cfg_.ws_path);
        }

        SPDLOG_LOGGER_INFO(log,
            "DpdkTcpStream::create: connected src=0x{:08x}:{} -> dst=0x{:08x}:{}",
            stream->cfg_.legacy.tuple.src_ip, stream->cfg_.legacy.tuple.src_port,
            stream->cfg_.legacy.tuple.dst_ip, stream->cfg_.legacy.tuple.dst_port);
        return stream;
    }

    ~DpdkTcpStream() {
        // If still attached to a Poller, remove ourselves first so the
        // Poller's entries_ does not retain a dangling pointer.
        if (attached_to_ != nullptr) {
            SPDLOG_LOGGER_DEBUG(detail::tcp_stream_logger(),
                "~DpdkTcpStream: auto-detach");
            (void)attached_to_->remove(this);
        }
    }

    DpdkTcpStream(const DpdkTcpStream&)            = delete;
    DpdkTcpStream& operator=(const DpdkTcpStream&) = delete;
    DpdkTcpStream(DpdkTcpStream&&)                 = delete;
    DpdkTcpStream& operator=(DpdkTcpStream&&)      = delete;

    // ── Public fields (Stream concept) ───────────────────────────────────

    /// @brief Invoked once per decoded frame. Set by user before attach.
    OnMessage on_message;

    // ── Stream concept API ───────────────────────────────────────────────

    [[nodiscard]] std::expected<std::size_t, core::ErrorInfo>
    send(std::span<const uint8_t> data) noexcept {
        if (attached_to_ == nullptr) {
            return std::unexpected(core::ErrorInfo{
                core::Error::NotAttached,
                "DpdkTcpStream::send called before attach"});
        }
        if (!sess_.is_established()) {
            return std::unexpected(core::ErrorInfo{
                core::Error::Disconnected,
                "DpdkTcpStream::send: session not Established"});
        }
        // With EnableTls=true, encrypt the bytes into one or more TLS
        // records before forwarding to the DPDK byte pipe. Mirrors the
        // kernel-side path (KernelTcpStream::send).
        if constexpr (EnableTls) {
            tls_send_buf_.clear();
            auto enc = tls_.encrypt_for_send(data.data(), data.size(),
                                              tls_send_buf_);
            if (!enc) {
                return std::unexpected(enc.error());
            }
            // The encrypted buffer may exceed MSS (TLS record overhead +
            // plaintext), so chunk by MSS before handing to the session.
            const std::size_t mss = sess_.mss();
            std::size_t off = 0;
            while (off < tls_send_buf_.size()) {
                const std::size_t chunk =
                    std::min(mss, tls_send_buf_.size() - off);
                auto sr = sess_.send(tls_send_buf_.data() + off, chunk);
                if (!sr) {
                    SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                        "DpdkTcpStream::send(TLS): TcpSession::send err={}", sr.error());
                    return std::unexpected(core::ErrorInfo{
                        core::Error::Disconnected,
                        "DpdkTcpStream::send: TcpSession::send failed"});
                }
                if (*sr == 0) {
                    SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                        "DpdkTcpStream::send(TLS): TcpSession::send returned 0 bytes");
                    return std::unexpected(core::ErrorInfo{
                        core::Error::BufferFull,
                        "DpdkTcpStream::send: TcpSession::send returned 0"});
                }
                off += *sr;
            }
            // API contract: report plaintext byte count.
            return data.size();
        } else {
            // Plaintext path. `TcpSession::send` rejects payloads
            // larger than MSS (eph-net-dpdk/include/eph/dpdk/tcp.hpp:646),
            // so we must chunk here ourselves. Mirrors the TLS branch
            // above and the WS handshake sinks.
            //
            // Loop until every byte is accepted so the public Stream
            // contract is contractually all-or-nothing: on success the
            // returned count equals `data.size()`. Any session-layer
            // error while draining the loop surfaces as Disconnected,
            // exactly as the pre-fix code path did on a single-shot
            // failure.
            const std::size_t mss = sess_.mss();
            std::size_t off = 0;
            while (off < data.size()) {
                const std::size_t chunk =
                    std::min(mss, data.size() - off);
                auto sr = sess_.send(data.data() + off, chunk);
                if (!sr) {
                    SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                        "DpdkTcpStream::send: TcpSession::send err={} "
                        "(off={}/{}, chunk={}, mss={})",
                        sr.error(), off, data.size(), chunk, mss);
                    return std::unexpected(core::ErrorInfo{
                        core::Error::Disconnected,
                        "DpdkTcpStream::send: TcpSession::send failed"});
                }
                if (*sr == 0) {
                    SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                        "DpdkTcpStream::send: TcpSession::send returned 0 "
                        "bytes (off={}/{}, chunk={})",
                        off, data.size(), chunk);
                    return std::unexpected(core::ErrorInfo{
                        core::Error::BufferFull,
                        "DpdkTcpStream::send: TcpSession::send returned 0"});
                }
                off += *sr;
            }
            return data.size();
        }
    }

    [[nodiscard]] std::expected<void, core::ErrorInfo>
    close_gracefully() noexcept {
        auto r = sess_.close();
        if (!r) {
            SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                "DpdkTcpStream::close_gracefully: TcpSession::close err={}",
                r.error());
            return std::unexpected(core::ErrorInfo{
                core::Error::Disconnected,
                "DpdkTcpStream::close_gracefully: TcpSession::close failed"});
        }
        return {};
    }

    [[nodiscard]] bool is_attached() const noexcept {
        return attached_to_ != nullptr;
    }

    [[nodiscard]] ::eph::net::TcpState state() const noexcept {
        return sess_.state();
    }

    // ── Pollable concept API ─────────────────────────────────────────────
    //
    // These methods are conceptually private to the Poller but exposed
    // because the `eph::net::Pollable` concept check needs to invoke them
    // on a non-const T& — same rationale as KernelTcpStream.

    /// @brief Single-poll driver for concept conformance. The DPDK lcore
    ///        path never calls this directly (dispatch goes via
    ///        `process_burst_`), but the concept check requires a
    ///        `poll_once_` member returning `size_t`. We forward to
    ///        `sess_.poll_rx` which runs one burst against the
    ///        session-local NIC driver loop.
    std::size_t poll_once_() noexcept {
        if (!sess_.is_established()) return 0;
        if (reasm_overflowed_) return 0;
        auto r = sess_.poll_rx([this](const uint8_t* data, uint16_t len) {
            if (reasm_overflowed_) return;
            if (!this->reasm_.append(data, len)) {
                // Silent drop is a reliability bug in HFT — flip the
                // byte pipe into Closed via RST so the reconnect policy
                // takes over on the next scheduler tick.
                SPDLOG_LOGGER_ERROR(detail::tcp_stream_logger(),
                    "DpdkTcpStream::poll_once_: reasm buffer overflow "
                    "cap={} need={} readable={} — forcing reset",
                    reasm_.capacity(), static_cast<std::size_t>(len),
                    reasm_.readable());
                reasm_overflowed_ = true;
                sess_.reset();
            }
        });
        if (!r) {
            SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                "DpdkTcpStream::poll_once_: poll_rx err={}", r.error());
            return 0;
        }
        if (reasm_overflowed_) return 0;
        return drain_codec_();
    }

    [[nodiscard]] bool is_attached_() const noexcept {
        return attached_to_ != nullptr;
    }

    /// @brief Pollable native_handle — returns the TcpSession pointer as
    ///        a generic void* (distinct from kernel fd semantics).
    [[nodiscard]] void* native_handle() const noexcept {
        return const_cast<void*>(static_cast<const void*>(&sess_));
    }

    // ── Poller-facing friend hooks ───────────────────────────────────────

    /// @brief Invoked by `DpdkPoller::add` after the routing entry is
    ///        built. Establishes back-pointer so destructor can auto-detach.
    void notify_attached_(DpdkPoller<void>* p) noexcept { attached_to_ = p; }

    /// @brief Invoked by `DpdkPoller::remove` and `~DpdkPoller`.
    void notify_detached_() noexcept { attached_to_ = nullptr; }

    /// @brief Supplies the registered 4-tuple to the Poller at add-time.
    void tuple_for_poller_(uint32_t* src_ip, uint32_t* dst_ip,
                            uint16_t* src_port, uint16_t* dst_port) noexcept {
        const auto& t = cfg_.legacy.tuple;
        *src_ip   = t.src_ip;
        *dst_ip   = t.dst_ip;
        *src_port = t.src_port;
        *dst_port = t.dst_port;
    }

    /// @brief Hot-path burst dispatch entry point called by DpdkPoller.
    ///        Feeds the mbuf(s) into the TCP session, then drains the
    ///        reassembly buffer through the codec.
    void process_burst_(rte_mbuf** mbufs, uint16_t n,
                         uint64_t rx_tsc) noexcept {
        if (!sess_.is_established() || reasm_overflowed_) {
            for (uint16_t i = 0; i < n; ++i) rte_pktmbuf_free(mbufs[i]);
            return;
        }
        sess_.set_last_rx_burst_tsc(rx_tsc);
        // Feed the mbufs into the TCP state machine. `process_rx` consumes
        // the mbufs (frees them internally) and invokes the callback once
        // per decoded TCP payload segment. On reasm overflow we latch
        // `reasm_overflowed_` so any subsequent callback from the same
        // burst skips touching the (full) buffer, and reset the session
        // so the reconnect loop can take over.
        auto r = sess_.process_rx(mbufs, n,
            [this](const uint8_t* data, uint16_t len) {
                if (reasm_overflowed_) return;
                if (!this->reasm_.append(data, len)) {
                    SPDLOG_LOGGER_ERROR(detail::tcp_stream_logger(),
                        "DpdkTcpStream::process_burst_: reasm buffer overflow "
                        "cap={} need={} readable={} — forcing reset",
                        reasm_.capacity(), static_cast<std::size_t>(len),
                        reasm_.readable());
                    reasm_overflowed_ = true;
                    sess_.reset();
                }
            });
        if (!r) {
            SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                "DpdkTcpStream::process_burst_: process_rx err={}", r.error());
            return;
        }
        if (reasm_overflowed_) return;
        sess_.flush_pending_ack();
        (void)drain_codec_();
    }

private:
    explicit DpdkTcpStream(StreamConfig cfg)
        : cfg_(std::move(cfg))
        , sess_(cfg_.legacy, cfg_.pool)
        , reasm_(cfg_.reasm_capacity > 0 ? cfg_.reasm_capacity : 256 * 1024) {}

    /// @brief Run the codec over the accumulated payload bytes, firing
    ///        `on_message` per decoded frame. Returns the number of
    ///        frames delivered.
    ///
    /// The in-place TLS decrypt path is implemented in
    /// `eph/net/dpdk/detail/tls_state.hpp::process_records_in_place`.
    /// Plaintext `EnableTls=false` instantiations use the plain byte-pipe
    /// path below.
    std::size_t drain_codec_() noexcept {
        if (!on_message) return 0;
        reasm_.compact();
        if (reasm_.readable() == 0) return 0;

        std::size_t delivered = 0;
        // Scratch OutputBuffer for auto-response injection (WS pong, etc.).
        uint8_t            scratch[1024];
        core::OutputBuffer out_sink(scratch, sizeof(scratch));

        if constexpr (EnableTls) {
            // ──────── TLS decrypt-in-place path ────────
            // The reasm_ buffer contains raw TLS records (ciphertext).
            // Decrypt them in-place, then feed the plaintext slices to the
            // codec. process_records_in_place() overwrites each record's
            // payload with its plaintext (the AEAD output buffer aliases the
            // input), so the codec reads from the same memory — zero extra
            // copies beyond the session→reasm copy.
            // Per-record emit with cross-record carry-over: when a WS
            // frame's payload spans a TLS record boundary, the tail of
            // the first record is saved into tls_codec_pending_ and
            // prepended to the next record's plaintext before feeding
            // the codec. Fast path (pending empty, frame fits) feeds
            // the codec directly over the in-place decrypted bytes —
            // zero copy.
            // `codec_err_latched` captures a codec protocol error surfaced
            // inside the per-record callback below. The callback cannot
            // propagate errors directly (process_records_in_place takes a
            // void lambda) so we latch here and tear the session down after
            // process_records_in_place returns. This mirrors the existing
            // `reasm_overflowed_` escalation for AEAD failures — without it,
            // a WS protocol violation would only WARN-log on every
            // subsequent poll forever instead of handing back to the
            // reconnect loop (batch2-round5 MED-1).
            bool codec_err_latched = false;
            auto dec_r = tls_.process_records_in_place(
                const_cast<uint8_t*>(reasm_.read_ptr()),
                reasm_.readable(),
                [&](uint8_t* chunk, std::size_t chunk_len) {
                    // Stop feeding further records once the codec has
                    // signalled an unrecoverable error — any subsequent
                    // record would just re-trigger the same failure.
                    if (codec_err_latched) return;

                    uint8_t*    feed_ptr;
                    std::size_t feed_len;
                    if (tls_codec_pending_.empty()) {
                        feed_ptr = chunk;                // zero-copy fast path
                        feed_len = chunk_len;
                    } else {
                        tls_codec_pending_.insert(tls_codec_pending_.end(),
                                                   chunk, chunk + chunk_len);
                        feed_ptr = tls_codec_pending_.data();
                        feed_len = tls_codec_pending_.size();
                    }

                    detail::MbufView view(feed_ptr, feed_len, /*arrival_tsc*/ 0);
                    while (view.length() > 0) {
                        const std::size_t before = view.length();
                        auto dr = codec_.decode(view, out_sink);
                        if (!dr) {
                            SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                                "DpdkTcpStream::drain_codec_(TLS): decode err={}",
                                dr.error().detail);
                            tls_codec_pending_.clear();
                            codec_err_latched = true;
                            return;
                        }
                        if (!dr->has_value()) break;
                        // Guard against infinite loop: codec returned a frame
                        // but did not advance the view.
                        if (view.length() == before) {
                            SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                                "DpdkTcpStream::drain_codec_(TLS): codec returned frame "
                                "but consumed 0 bytes — breaking to avoid infinite loop");
                            break;
                        }
                        const auto& frame = **dr;
                        if (frame.size() > 0 && on_message) {
                            if (saturate_u16_clamps(frame.size()) && !trunc_warned_) {
                                SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                                    "DpdkTcpStream::drain_codec_(TLS): frame size "
                                    "{} > 0xFFFF; on_message length is clamped "
                                    "to 0xFFFF (warn-once per stream)",
                                    frame.size());
                                trunc_warned_ = true;
                            }
                            on_message(frame.data(), saturate_u16(frame.size()));
                            ++delivered;
                        }
                    }

                    // Save any unconsumed tail (incomplete frame) for the
                    // next emit. When `pending` was non-empty on entry,
                    // view.data() points INTO pending itself — calling
                    // `pending.assign(ptr_into_pending, ...)` would be
                    // UB (std::vector::assign explicitly forbids iterators
                    // into *this). Construct a fresh vector from the tail
                    // bytes first, then move-assign — the temporary owns
                    // its own allocation so there is no aliasing.
                    if (view.length() > 0) {
                        tls_codec_pending_ = std::vector<uint8_t>(
                            view.data(), view.data() + view.length());
                    } else {
                        tls_codec_pending_.clear();
                    }
                });
            if (!dec_r) {
                SPDLOG_LOGGER_ERROR(detail::tcp_stream_logger(),
                    "DpdkTcpStream::drain_codec_(TLS): decrypt err={} "
                    "— forcing reset to prevent re-processing corrupt data",
                    dec_r.error().detail);
                // A hard AEAD failure (bad MAC, bad header) means the
                // buffer contains unrecoverable data. If we consume nothing,
                // the same corrupt bytes will be re-processed on every
                // subsequent poll, causing infinite error loops. Force a
                // reset so the reconnect policy can establish a fresh
                // TLS session.
                reasm_overflowed_ = true;
                sess_.reset();
            } else {
                reasm_.consume(*dec_r);
                if (codec_err_latched) {
                    // Codec protocol violation (malformed WS frame, oversized
                    // message, etc.) — escalate the same way AEAD failures
                    // do: latch reasm_overflowed_ and reset the session so
                    // the reconnect policy can spin up a fresh one.
                    SPDLOG_LOGGER_ERROR(detail::tcp_stream_logger(),
                        "DpdkTcpStream::drain_codec_(TLS): codec err latched "
                        "— forcing session reset");
                    tls_codec_pending_.clear();
                    reasm_overflowed_ = true;
                    sess_.reset();
                }
            }
        } else {
            // ──────── Plaintext path ────────
            while (reasm_.readable() > 0) {
                const std::size_t before = reasm_.readable();
                detail::MbufView view(const_cast<uint8_t*>(reasm_.read_ptr()),
                                       before, /*arrival_tsc*/ 0);

                auto dr = codec_.decode(view, out_sink);
                if (!dr) {
                    SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                        "DpdkTcpStream::drain_codec_: decode err={}",
                        dr.error().detail);
                    break;
                }
                const std::size_t consumed = before - view.length();
                reasm_.consume(consumed);

                if (!dr->has_value()) {
                    break;
                }
                // Guard against infinite loop: if the codec returned a frame
                // but did not advance the view, we cannot make progress.
                // Break to avoid spinning forever on malformed codec output.
                if (consumed == 0) {
                    SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                        "DpdkTcpStream::drain_codec_: codec returned frame "
                        "but consumed 0 bytes — breaking to avoid infinite loop");
                    break;
                }
                const auto& frame = **dr;
                if (frame.size() > 0 && on_message) {
                    if (saturate_u16_clamps(frame.size()) && !trunc_warned_) {
                        SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                            "DpdkTcpStream::drain_codec_: frame size {} > "
                            "0xFFFF; on_message length is clamped to 0xFFFF "
                            "(warn-once per stream)",
                            frame.size());
                        trunc_warned_ = true;
                    }
                    on_message(frame.data(), saturate_u16(frame.size()));
                    ++delivered;
                }
            }
        }
        return delivered;
    }

    // ── Data members ─────────────────────────────────────────────────────

    StreamConfig                            cfg_{};
    ::eph::dpdk::TcpSession<>               sess_;
    [[no_unique_address]] C                 codec_{};
    [[no_unique_address]] std::conditional_t<EnableTls,
                                              detail::TlsState,
                                              std::monostate> tls_{};
    // Scratch buffer for encrypting send() payloads into TLS records
    // before handing them to the byte pipe. Persists across calls so we do not
    // reallocate per send. Only populated when `EnableTls=true`.
    std::vector<uint8_t>                    tls_send_buf_{};
    // Cross-record carry-over for the TLS codec drain path. Holds the
    // unconsumed tail from a previous emit when a WS frame's payload
    // spans a TLS record boundary; prepended to the next record's
    // plaintext before the codec runs again. Empty in the common case
    // (frames aligned to record boundaries) — the fast path feeds the
    // codec directly over the in-place decrypted bytes with zero copy.
    // Only populated when `EnableTls=true`.
    std::vector<uint8_t>                    tls_codec_pending_{};
    detail::ReasmBuffer                     reasm_;
    /// @brief Latch set when `reasm_.append()` reports overflow. Once
    ///        tripped, the stream short-circuits all further RX dispatch;
    ///        caller-side recovery code is expected to tear it down.
    bool                                    reasm_overflowed_{false};
    DpdkPoller<void>*                       attached_to_{nullptr};
    /// Warn-once latch for `saturate_u16` clamping during on_message dispatch.
    /// See batch3-round1 MEDIUM-1.
    bool                                    trunc_warned_{false};
};

} // namespace eph::net::dpdk
