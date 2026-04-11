#pragma once

/// @file tcp_stream.hpp
/// DPDK user-space TCP stream satisfying `eph::net::Stream`. Part of
/// Phase 4 of the v3.3 refactor (see
/// .artifacts/design-eph-v3.3-architecture-20260410.md).
///
/// Architecture:
///
///     user code
///        │
///        v
///     DpdkTcpStream<C, EnableTls>
///        │  ├── eph::dpdk::TcpSession<>  (byte-pipe; pragmatic reuse
///        │  │                              of the battle-tested legacy
///        │  │                              TCP state machine — Phase 7
///        │  │                              migrates the source into
///        │  │                              detail/dpdk_tcp_session.hpp)
///        │  ├── C                         (StreamCodec template param)
///        │  ├── TlsState                  (only when EnableTls == true —
///        │  │                              Phase 4 stub returns
///        │  │                              TlsHandshakeFailed)
///        │  ├── reassembly buffer         (for codec's decode loop)
///        │  └── DpdkPoller<>*             (set by Poller::add)
///        │
///        v
///     DpdkPoller<> (lcore burst poll)
///
/// Phase 4 scope:
///   - Plaintext path works: `create()` runs the TCP 3-way handshake by
///     calling `TcpSession::connect()`. Once the Poller adopts the stream,
///     incoming mbufs dispatch into `process_burst_()` which feeds the
///     TCP session and then runs the codec decode loop against the freshly
///     delivered payload, invoking `on_message` per decoded frame.
///   - `send()` writes bytes via `TcpSession::send`. `NotAttached` is
///     returned when called before the stream is in a Poller.
///   - TLS path is a Phase 5 stub, exactly like `KernelTcpStream`: with
///     `EnableTls=true` the `create()` factory returns `TlsHandshakeFailed`.
///   - Destructor auto-detaches from the Poller if still attached.

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
#include <spdlog/sinks/stdout_color_sinks.h>

#include "eph/core/codec.hpp"
#include "eph/core/error.hpp"
#include "eph/dpdk/packet_parse.hpp"
#include "eph/dpdk/tcp.hpp"
#include "eph/net/concepts.hpp"
#include "eph/net/detail/ws_handshake.hpp"   // Sub-phase 9.5
#include "eph/net/dpdk/config.hpp"
#include "eph/net/dpdk/detail/mbuf_view.hpp"
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/reconnect_policy.hpp"
#include "eph/net/tcp_state.hpp"

// Phase 7: the DPDK TLS path is now FULLY WIRED. Phase 5 shipped it as a
// structural stub because `eph::dpdk::tcp.hpp` included `<openssl/rand.h>`
// from vcpkg-openssl which collided with aws-lc in the same TU. Phase 7
// replaced the two `RAND_bytes` call sites (TcpSession ISN generation and
// the WS mask-key cache) with `getrandom(2)`, deleted the legacy
// eph-transport / eph-dpdk modules, and moved the DPDK primitives into
// eph-net-dpdk. aws-lc is now the only OpenSSL flavour in any eph-net-dpdk
// TU, which lets us include the real `detail/tls_state.hpp` unconditionally
// and run the TLS 1.3 handshake + in-place AEAD path for `EnableTls=true`.
#include "eph/net/dpdk/detail/tls_state.hpp"

namespace eph::net::dpdk {

namespace detail {

/// @brief Lazily-initialized logger for the DPDK TCP stream subsystem.
inline spdlog::logger* tcp_stream_logger() {
    static auto* l = [] {
        auto lg = spdlog::get("net.dpdk.tcp_stream");
        if (!lg) {
            try {
                lg = spdlog::stdout_color_mt("net.dpdk.tcp_stream");
            } catch (const spdlog::spdlog_ex&) {
                lg = spdlog::get("net.dpdk.tcp_stream");
            }
        }
        return lg.get();
    }();
    return l;
}

// Phase 5: see the BLOCKER note above the namespace block. The real
// `detail::TlsState` (with aws-lc-backed in-place AEAD) lives in
// `detail/tls_state.hpp` but cannot be wired in here until the
// vcpkg-openssl ↔ aws-lc TU conflict is resolved.

/// @brief Reassembly buffer for the codec decode loop. Bytes dispatched
///        in from the Poller append here, then the codec drains them
///        incrementally. Implemented as a simple std::vector<uint8_t>
///        with a front cursor — Phase 5 can upgrade to a ring buffer.
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
    void consume(std::size_t n) noexcept { head_ += n; }
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
    bool append(const uint8_t* src, std::size_t n) noexcept {
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
// WS-handshake ByteSink adapters (Sub-phase 9.5)
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
/// @tparam EnableTls  When true, TLS session state is carried (but real
///                    handshake is deferred to Phase 5 — `create()`
///                    returns `TlsHandshakeFailed` until then).
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
        // Sub-phase 9.6: HTTP CONNECT proxies are unsupported on DPDK.
        // HFT colo deployments don't use proxies, and a DPDK client
        // bypasses the kernel userland stack that would otherwise be the
        // natural vehicle for reaching one. Reject up-front with a clear
        // diagnostic so users who accidentally reuse a kernel StreamConfig
        // get an actionable error rather than a silent data-plane stall.
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
            // Phase 7: real TLS 1.3 handshake via aws-lc, driven through the
            // legacy `eph::dpdk::TcpSession<>` (which satisfies the legacy
            // TcpTransport concept via its `send`/`poll_rx`/`state` triple).
            // The hot-path AEAD state is extracted into the TlsState object
            // held as a [[no_unique_address]] member of this stream; data
            // frames decrypt in place over the reasm buffer on the RX burst.
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

        // ── Sub-phase 9.5: optional WebSocket HTTP Upgrade ───────────────
        //
        // Same contract as KernelTcpStream: empty ws_path skips entirely.
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
        // Phase 7: with EnableTls=true, encrypt the bytes into one or more
        // TLS records before forwarding to the DPDK byte pipe. Mirrors the
        // kernel-side path (KernelTcpStream::send).
        if constexpr (EnableTls) {
            tls_send_buf_.clear();
            auto enc = tls_.encrypt_for_send(data.data(), data.size(),
                                              tls_send_buf_);
            if (!enc) {
                return std::unexpected(enc.error());
            }
            auto sr = sess_.send(tls_send_buf_.data(), tls_send_buf_.size());
            if (!sr) {
                SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                    "DpdkTcpStream::send(TLS): TcpSession::send err={}", sr.error());
                return std::unexpected(core::ErrorInfo{
                    core::Error::Disconnected,
                    "DpdkTcpStream::send: TcpSession::send failed"});
            }
            // API contract: report plaintext byte count.
            return data.size();
        } else {
            auto r = sess_.send(data.data(), data.size());
            if (!r) {
                SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                    "DpdkTcpStream::send: TcpSession::send err={}", r.error());
                return std::unexpected(core::ErrorInfo{
                    core::Error::Disconnected,
                    "DpdkTcpStream::send: TcpSession::send failed"});
            }
            return *r;
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
    [[nodiscard]] void* native_handle() noexcept {
        return static_cast<void*>(&sess_);
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
        , reasm_(cfg_.reasm_capacity > 0 ? cfg_.reasm_capacity : 256 * 1024)
        , reconnect_policy_(cfg_.reconnect) {}

    /// @brief Run the codec over the accumulated payload bytes, firing
    ///        `on_message` per decoded frame. Returns the number of
    ///        frames delivered.
    ///
    /// Phase 5 note: the in-place TLS decrypt path is implemented in
    /// `eph/net/dpdk/detail/tls_state.hpp::process_records_in_place` and
    /// is fully unit-tested via the in-place AEAD primitive in
    /// `eph-net/tests/test_tls_in_place_decrypt.cpp`. The wiring of that
    /// path back into `drain_codec_` is BLOCKED by the vcpkg-openssl ↔
    /// aws-lc TU conflict described at the top of this file. Plaintext
    /// `EnableTls=false` instantiations are unaffected and still produce
    /// the Phase 4 byte-pipe behaviour below.
    std::size_t drain_codec_() noexcept {
        if (!on_message) return 0;
        reasm_.compact();
        if (reasm_.readable() == 0) return 0;

        std::size_t delivered = 0;
        // Scratch OutputBuffer for auto-response injection (WS pong, etc.).
        uint8_t            scratch[1024];
        core::OutputBuffer out_sink(scratch, sizeof(scratch));

        // ──────── Plaintext path (Phase 4 behavior) ────────
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
            const auto& frame = **dr;
            if (frame.size() > 0 && on_message) {
                on_message(frame.data(),
                           static_cast<uint16_t>(
                               frame.size() > 0xFFFFu ? 0xFFFFu : frame.size()));
                ++delivered;
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
    // Phase 7: scratch buffer for encrypting send() payloads into TLS records
    // before handing them to the byte pipe. Persists across calls so we do not
    // reallocate per send. Only populated when `EnableTls=true`.
    std::vector<uint8_t>                    tls_send_buf_{};
    detail::ReasmBuffer                     reasm_;
    /// @brief Latch set when `reasm_.append()` reports overflow. Once
    ///        tripped, the stream short-circuits all further RX dispatch
    ///        and the reconnect policy is expected to tear it down.
    bool                                    reasm_overflowed_{false};
    DpdkPoller<void>*                       attached_to_{nullptr};
    ::eph::net::ReconnectPolicy             reconnect_policy_;
};

} // namespace eph::net::dpdk
