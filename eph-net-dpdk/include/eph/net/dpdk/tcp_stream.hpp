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
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
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
#include "eph/dpdk/platform.hpp"
#include "eph/dpdk/tcp.hpp"
#include "eph/net/concepts.hpp"
#include "eph/net/detail/ws_handshake.hpp"   // WS HTTP handshake
#include "eph/net/dpdk/config.hpp"
#include "eph/net/dpdk/detail/mbuf_view.hpp"
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/stream_metrics.hpp"
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

/// @brief Stack scratch size for the codec auto-response `OutputBuffer`
///        sink. Sized to hold a single max-sized WS control frame (pong
///        or close-ack). 1 KB matches the kernel backend's
///        `kCodecAutoResponseBytes` for parity.
inline constexpr std::size_t kCodecAutoResponseBytes = 1024;

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
            // `plaintext_chunk` here: PlainDpdkWsSink path — no TLS, so the
            // TCP payload IS already the plaintext handshake bytes.
            auto r = sess_->poll_rx(
                [this](const uint8_t* plaintext_chunk, uint16_t len) {
                    staged_.insert(staged_.end(),
                                   plaintext_chunk, plaintext_chunk + len);
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
            // `ciphertext_chunk`: TLS path — TCP payload is an encrypted
            // TLS record, not yet decrypted. Accumulated in `cipher_` until
            // a full record can be decrypted in place below.
            auto r = sess_->poll_rx(
                [this](const uint8_t* ciphertext_chunk, uint16_t len) {
                    cipher_.insert(cipher_.end(),
                                   ciphertext_chunk, ciphertext_chunk + len);
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
            // `plaintext_chunk`: post-AEAD, ready for codec/handshake consumer.
            auto cr = tls_->process_records_in_place(
                cipher_.data(), cipher_.size(),
                [this](uint8_t* plaintext_chunk, std::size_t len) {
                    plain_.insert(plain_.end(),
                                  plaintext_chunk, plaintext_chunk + len);
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
    /// @brief Frame sink: invoked once per decoded application frame.
    ///        The span carries application-layer plaintext — already
    ///        post-codec and, if EnableTls=true, post-AEAD-decrypt in the
    ///        mbuf payload.
    using OnMessage  = std::function<void(std::span<const uint8_t>)>;

    // ── Factory ──────────────────────────────────────────────────────────

    /// @brief Poller-aware factory. Auto-allocates a conflict-free
    ///        source port via `poller.pick_src_port()` when
    ///        `cfg.legacy.tuple.src_port == 0`, then delegates to the
    ///        single-argument `create` overload.
    ///
    /// This is the recommended entry point when a `DpdkPoller` is
    /// available at construction time: it closes the "picked a stale
    /// port still in TIME_WAIT on our side" loophole that bites
    /// high-frequency reconnect workloads, because `pick_src_port` scans
    /// the currently registered 5-tuples and uses a random-start probe
    /// across the whole ephemeral range.
    ///
    /// The strict `create(cfg)` overload below is preserved for the
    /// direct-construction path (tests, users that manage ports
    /// themselves): it still rejects `src_port == 0` via
    /// `TcpConfig::validate()`.
    [[nodiscard]] static std::expected<std::unique_ptr<DpdkTcpStream>, core::ErrorInfo>
    create(StreamConfig cfg, DpdkPoller<void>& poller) noexcept {
        auto* log = detail::tcp_stream_logger();
        if (cfg.legacy.tuple.src_port == 0) {
            if (cfg.legacy.tuple.src_ip == 0 ||
                cfg.legacy.tuple.dst_ip == 0 ||
                cfg.legacy.tuple.dst_port == 0) {
                SPDLOG_LOGGER_WARN(log,
                    "DpdkTcpStream::create(poller): src_ip/dst_ip/dst_port "
                    "must be set before auto-allocating src_port "
                    "(src={:x} dst={:x}:{})",
                    cfg.legacy.tuple.src_ip, cfg.legacy.tuple.dst_ip,
                    cfg.legacy.tuple.dst_port);
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "DpdkTcpStream::create: src_ip/dst_ip/dst_port required "
                    "before auto-allocating src_port"});
            }
            auto port = poller.pick_src_port(
                cfg.legacy.tuple.src_ip,
                cfg.legacy.tuple.dst_ip,
                cfg.legacy.tuple.dst_port);
            if (!port) {
                SPDLOG_LOGGER_WARN(log,
                    "DpdkTcpStream::create: pick_src_port failed: {}",
                    port.error().detail);
                return std::unexpected(port.error());
            }
            cfg.legacy.tuple.src_port = *port;
            SPDLOG_LOGGER_INFO(log,
                "DpdkTcpStream::create: auto-allocated src_port={}", *port);
        }
        auto result = create(std::move(cfg));
        if (result) {
            // Register this stream as the ICMP Frag Needed dispatch
            // target. The callback matches the embedded 4-tuple against
            // our own and forwards the next-hop MTU to the session.
            // Single-callback limitation: the last create(cfg, poller)
            // wins. Multi-stream users sharing one Poller must build
            // their own dispatcher (no silent misrouting — the callback
            // compares tuples and ignores mismatches).
            poller.set_icmp_callback(&DpdkTcpStream::on_icmp_frag_needed_trampoline_,
                                      result->get());
        }
        return result;
    }

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
        // to an ErrorInfo so the error contract holds.
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
        // Validate timeout values up front so a caller passing zero or
        // negative `connect_timeout` / `ws_timeout` fails at the config
        // boundary instead of emerging much later as a cryptic
        // `Error::Timeout`. Parity with the kernel backend fix (MED-2 /
        // commit 7aa19b6) so the two backends reject identical bad configs.
        if (cfg.connect_timeout <= std::chrono::milliseconds::zero()) {
            SPDLOG_LOGGER_WARN(log,
                "DpdkTcpStream::create: connect_timeout={}ms must be > 0",
                cfg.connect_timeout.count());
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkTcpStream::create: connect_timeout must be > 0"});
        }
        if (!cfg.ws_path.empty() &&
            cfg.ws_timeout <= std::chrono::milliseconds::zero()) {
            SPDLOG_LOGGER_WARN(log,
                "DpdkTcpStream::create: ws_timeout={}ms must be > 0 when "
                "ws_path is non-empty",
                cfg.ws_timeout.count());
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkTcpStream::create: ws_timeout must be > 0"});
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

    /// @brief Turnkey factory: create a stream and attach it to the
    /// per-queue Poller already registered with `platform`. Honours
    /// `cfg.pin_to_queue` and the platform's `dispatch_mode()` to pick
    /// the correct RX queue (and, in RSS+pin mode, to rebind the
    /// ephemeral src_port so the connection's hash lands on the target
    /// queue). Pre-conditions:
    ///   * the relevant `DpdkPoller<>` is already registered with
    ///     `platform.register_poller(qid, poller)` for every queue this
    ///     stream might land on;
    ///   * `cfg.legacy.tuple.dst_ip` / `dst_port` carry the remote
    ///     endpoint, `src_ip` is the local ip, and `src_port` is either
    ///     a pre-chosen ephemeral port or 0 (the helper rebinds it in
    ///     the RSS+pin case anyway).
    /// On success the returned `unique_ptr` already has its `attached_to_`
    /// set; the caller may immediately start polling on the matching
    /// `DpdkPoller`'s lcore loop.
    [[nodiscard]] static std::expected<std::unique_ptr<DpdkTcpStream>, core::ErrorInfo>
    create_and_attach(StreamConfig cfg, ::eph::dpdk::Platform& platform) noexcept {
        auto* log = detail::tcp_stream_logger();

        const auto mode = platform.dispatch_mode();
        const uint16_t nb_q = platform.nb_rx_queues();
        if (nb_q == 0) {
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "create_and_attach: Platform has 0 RX queues "
                "(moved-from or never created)"});
        }

        // ── Phase 1: pre-create — pick target queue, possibly rebind src_port ──
        // RxDispatchMode::RssPartitioned with an explicit pin requires us to
        // search the ephemeral src_port range for one whose Toeplitz hash
        // lands on the target queue, AND apply that src_port to cfg before
        // connect() runs. Other modes can defer queue selection to phase 2.
        uint16_t target_qid = 0;
        bool defer_queue_selection = false;

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
                // RSS input "src" is the REMOTE end (incoming packets have
                // peer→local direction), so we feed the helper with
                // (remote_ip=dst_ip, local_ip=src_ip, local_port=src_port).
                const auto& t = cfg.legacy.tuple;
                auto sp = ::eph::net::dpdk::find_src_port_for_queue(
                    platform.port_id(), want,
                    /*src_ip=*/t.dst_ip,
                    /*dst_ip=*/t.src_ip,
                    /*dst_port=*/t.src_port);
                if (!sp) {
                    SPDLOG_LOGGER_WARN(log,
                        "create_and_attach: find_src_port_for_queue({}) failed: {}",
                        want, sp.error());
                    return std::unexpected(core::ErrorInfo{
                        core::Error::InvalidConfig,
                        "create_and_attach: find_src_port_for_queue exhausted"});
                }
                cfg.legacy.tuple.src_port = *sp;
                target_qid = want;
                SPDLOG_LOGGER_INFO(log,
                    "create_and_attach: RSS pin → src_port={} hashes to queue={}",
                    *sp, want);
            } else {
                // Will compute target_qid from the FINAL 5-tuple after create().
                defer_queue_selection = true;
            }
        } else {  // FlowDirector
            if (cfg.pin_to_queue && *cfg.pin_to_queue >= nb_q) {
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "create_and_attach: pin_to_queue >= nb_rx_queues"});
            }
            // Default: round-robin via a static counter. Atomic so concurrent
            // create_and_attach calls from different threads don't all map to
            // queue 0. Bounded modulo nb_q at decode time.
            if (cfg.pin_to_queue) {
                target_qid = *cfg.pin_to_queue;
            } else {
                static std::atomic<uint16_t> rr_counter{0};
                target_qid = rr_counter.fetch_add(1, std::memory_order_relaxed)
                             % nb_q;
            }
        }

        // ── Phase 2: actual stream construction (TCP handshake + TLS + WS) ──
        auto sr = create(std::move(cfg));
        if (!sr) return std::unexpected(sr.error());
        auto stream = std::move(*sr);

        // Resolve queue id from the final post-connect 5-tuple if we deferred.
        if (defer_queue_selection) {
            const auto& t = stream->cfg_.legacy.tuple;
            auto qr = ::eph::net::dpdk::predict_rss_queue(
                platform.port_id(),
                /*src_ip=*/t.dst_ip,
                /*src_port=*/t.dst_port,
                /*dst_ip=*/t.src_ip,
                /*dst_port=*/t.src_port);
            if (!qr) {
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "create_and_attach: predict_rss_queue failed"});
            }
            target_qid = *qr;
            SPDLOG_LOGGER_INFO(log,
                "create_and_attach: RSS auto → queue={}", target_qid);
        }

        // Attach the Pollable BEFORE installing any flow rule, so the
        // moment the NIC starts steering matching packets to target_qid
        // (post install_flow_rule), the Pollable is already there to
        // demux them. Reverse order would open a race window where
        // packets land in target_qid's burst loop with no matching
        // 5-tuple → silent drop.
        auto* poller = platform.poller_for_queue(target_qid);
        if (poller == nullptr) {
            return std::unexpected(core::ErrorInfo{
                core::Error::NotAttached,
                "create_and_attach: no Poller registered for target queue"});
        }
        auto add_r = poller->add(stream.get());
        if (!add_r) return std::unexpected(add_r.error());

        // FlowDirector: install rte_flow rule steering this 5-tuple to qid
        // and move it into the stream so RAII destruction (~FlowRule →
        // rte_flow_destroy) cleans up at stream teardown.
        if (mode == ::eph::net::dpdk::RxDispatchMode::FlowDirector) {
            const auto& t = stream->cfg_.legacy.tuple;
            ::eph::dpdk::net::ConnectionTuple fl_tuple{
                .src_ip   = t.src_ip,   .dst_ip   = t.dst_ip,
                .src_port = t.src_port, .dst_port = t.dst_port};
            auto rule = ::eph::net::dpdk::install_flow_rule(
                platform.port_id(), target_qid, fl_tuple,
                ::eph::net::dpdk::FlowProtocol::Tcp);
            if (!rule) {
                SPDLOG_LOGGER_WARN(log,
                    "create_and_attach: install_flow_rule failed: {}",
                    rule.error());
                // Roll back the attach so the stream isn't half-registered.
                (void)poller->remove(stream.get());
                // RAII unwinds the rest: ~unique_ptr<DpdkTcpStream> →
                // ~optional<FlowRule> (still empty) → ~TcpSession.
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "create_and_attach: install_flow_rule failed"});
            }
            stream->flow_rule_.emplace(std::move(*rule));
        }

        SPDLOG_LOGGER_INFO(log,
            "create_and_attach: TCP stream attached → port={}, queue={}, mode={}",
            platform.port_id(), target_qid,
            ::eph::net::dpdk::rx_dispatch_mode_name(mode));
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

    /// @brief Send `app_payload` bytes to the peer. `app_payload` holds
    ///        plaintext application-layer bytes (post-codec); when
    ///        EnableTls=true it is encrypted into TLS records
    ///        transparently before handing to the DPDK byte pipe.
    [[nodiscard]] std::expected<std::size_t, core::ErrorInfo>
    send(std::span<const uint8_t> app_payload) noexcept {
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
            auto enc = tls_.encrypt_for_send(app_payload.data(),
                                              app_payload.size(),
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
            inc_<::eph::net::StreamMetric::kBytesSent>(app_payload.size());
            // API contract: report plaintext byte count.
            return app_payload.size();
        } else {
            // Plaintext path. `TcpSession::send` rejects payloads
            // larger than MSS (eph-net-dpdk/include/eph/dpdk/tcp.hpp:646),
            // so we must chunk here ourselves. Mirrors the TLS branch
            // above and the WS handshake sinks.
            //
            // Loop until every byte is accepted so the public Stream
            // contract is contractually all-or-nothing: on success the
            // returned count equals `app_payload.size()`. Any session-layer
            // error while draining the loop surfaces as Disconnected,
            // exactly as the pre-fix code path did on a single-shot
            // failure.
            const std::size_t mss = sess_.mss();
            std::size_t off = 0;
            while (off < app_payload.size()) {
                const std::size_t chunk =
                    std::min(mss, app_payload.size() - off);
                auto sr = sess_.send(app_payload.data() + off, chunk);
                if (!sr) {
                    SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                        "DpdkTcpStream::send: TcpSession::send err={} "
                        "(off={}/{}, chunk={}, mss={})",
                        sr.error(), off, app_payload.size(), chunk, mss);
                    return std::unexpected(core::ErrorInfo{
                        core::Error::Disconnected,
                        "DpdkTcpStream::send: TcpSession::send failed"});
                }
                if (*sr == 0) {
                    SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                        "DpdkTcpStream::send: TcpSession::send returned 0 "
                        "bytes (off={}/{}, chunk={})",
                        off, app_payload.size(), chunk);
                    return std::unexpected(core::ErrorInfo{
                        core::Error::BufferFull,
                        "DpdkTcpStream::send: TcpSession::send returned 0"});
                }
                off += *sr;
            }
            inc_<::eph::net::StreamMetric::kBytesSent>(app_payload.size());
            return app_payload.size();
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
        // Drive the keepalive tick on every poll cycle regardless of
        // whether bytes arrived. An idle, established connection has no
        // packets flowing but still needs its liveness probes to emit.
        sess_.tick_keepalive(::eph::utils::TSC::now());
        if (!sess_.is_established()) return 0;
        if (reasm_overflowed_) return 0;
        // `rx_chunk`: TCP payload bytes emerging from the session-layer
        // reassembly. Semantically plaintext when EnableTls=false; when
        // EnableTls=true these are TLS ciphertext records that the
        // drain_codec_ path below will decrypt in place.
        auto r = sess_.poll_rx([this](const uint8_t* rx_chunk, uint16_t len) {
            if (reasm_overflowed_) return;
            if (!this->reasm_.append(rx_chunk, len)) {
                // Silent drop is a reliability bug in HFT — flip the
                // byte pipe into Closed via RST so the reconnect policy
                // takes over on the next scheduler tick.
                SPDLOG_LOGGER_ERROR(detail::tcp_stream_logger(),
                    "DpdkTcpStream::poll_once_: reasm buffer overflow "
                    "cap={} need={} readable={} — forcing reset",
                    reasm_.capacity(), static_cast<std::size_t>(len),
                    reasm_.readable());
                inc_<::eph::net::StreamMetric::kReasmOverflows>();
                reasm_overflowed_ = true;
                sess_.reset();
                return;
            }
            inc_<::eph::net::StreamMetric::kBytesRecv>(len);
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

    // ── Observability (StreamMetric pull model) ──────────────────────────
    //
    // See eph/net/stream_metrics.hpp. All 6 metrics are wired on this
    // backend, including the DPDK-only kTlsCrossRecordFrames in the TLS
    // drain_codec_ slow path.

    [[nodiscard]] std::uint64_t metric(::eph::net::StreamMetric m) const noexcept {
        using SM = ::eph::net::StreamMetric;
        // TCP / ICMP session-level counters live on TcpSession::Stats and
        // are updated in-situ on the hot path; reading them lazily here
        // avoids maintaining a second parallel atomic counter plus the
        // extra lock-add on every event.
        switch (m) {
            case SM::kTcpResetsReceived:
                return sess_.tcp_stats().resets_received;
            case SM::kTcpOutOfOrderSegments:
                return sess_.tcp_stats().out_of_order;
            case SM::kTcpReorderBufferHits:
                return sess_.tcp_stats().reorder_hits;
            case SM::kTcpReorderBufferOverflows:
                return sess_.tcp_stats().reorder_overflows;
            case SM::kTcpKeepaliveProbesSent:
                return sess_.tcp_stats().keepalive_probes_sent;
            case SM::kTcpMssNegotiationApplied:
                return sess_.tcp_stats().mss_negotiations_applied;
            case SM::kIcmpFragNeededReceived:
                return sess_.tcp_stats().icmp_frag_needed_received;
            default:
                break;
        }
        return counters_[static_cast<std::size_t>(m)]
            .v.load(std::memory_order_relaxed);
    }

    // ── Poller-facing friend hooks ───────────────────────────────────────

    /// @brief Invoked by `DpdkPoller::add` after the routing entry is
    ///        built. Establishes back-pointer so destructor can auto-detach.
    void notify_attached_(DpdkPoller<void>* p) noexcept { attached_to_ = p; }

    /// @brief Invoked by `DpdkPoller::remove` and `~DpdkPoller`.
    void notify_detached_() noexcept { attached_to_ = nullptr; }

    /// @brief Supplies the registered 5-tuple to the Poller at add-time.
    ///        `*proto` is set to `kIpProtoTcp` so the poller routing table
    ///        can coexist with UDP Pollables sharing the same 4-tuple.
    void tuple_for_poller_(uint32_t* src_ip, uint32_t* dst_ip,
                            uint16_t* src_port, uint16_t* dst_port,
                            uint8_t*  proto) noexcept {
        const auto& t = cfg_.legacy.tuple;
        *src_ip   = t.src_ip;
        *dst_ip   = t.dst_ip;
        *src_port = t.src_port;
        *dst_port = t.dst_port;
        *proto    = eph::dpdk::net::kIpProtoTcp;
    }

    /// @brief ICMP Frag Needed trampoline registered with the Poller.
    ///        Matches the embedded 4-tuple + protocol against our own
    ///        and, on match, forwards `next_hop_mtu` to the session.
    ///        noexcept so a mis-dispatch can never unwind through the
    ///        Poller's hot path.
    static void on_icmp_frag_needed_trampoline_(
        void* user,
        uint32_t embedded_src_ip, uint32_t embedded_dst_ip,
        uint16_t embedded_src_port, uint16_t embedded_dst_port,
        uint8_t  embedded_proto,
        uint16_t next_hop_mtu) noexcept {
        auto* self = static_cast<DpdkTcpStream*>(user);
        if (self == nullptr) return;
        // The embedded headers carry OUR original packet verbatim: so
        // embedded src/dst equal our cfg.tuple src/dst (not swapped).
        // Protocol must be TCP.
        if (embedded_proto != eph::dpdk::net::kIpProtoTcp) return;
        const auto& t = self->cfg_.legacy.tuple;
        if (embedded_src_ip != t.src_ip ||
            embedded_dst_ip != t.dst_ip ||
            embedded_src_port != t.src_port ||
            embedded_dst_port != t.dst_port) {
            return;
        }
        self->sess_.on_icmp_frag_needed(next_hop_mtu);
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
        // `rx_chunk`: same semantic as poll_once_'s lambda — TCP payload
        // bytes, plaintext if EnableTls=false, ciphertext otherwise.
        auto r = sess_.process_rx(mbufs, n,
            [this](const uint8_t* rx_chunk, uint16_t len) {
                if (reasm_overflowed_) return;
                if (!this->reasm_.append(rx_chunk, len)) {
                    inc_<::eph::net::StreamMetric::kReasmOverflows>();
                    SPDLOG_LOGGER_ERROR(detail::tcp_stream_logger(),
                        "DpdkTcpStream::process_burst_: reasm buffer overflow "
                        "cap={} need={} readable={} — forcing reset",
                        reasm_.capacity(), static_cast<std::size_t>(len),
                        reasm_.readable());
                    reasm_overflowed_ = true;
                    sess_.reset();
                    return;
                }
                inc_<::eph::net::StreamMetric::kBytesRecv>(len);
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
        // Scratch region backing the per-iteration `out_sink` below. Reused
        // across iterations via a fresh `OutputBuffer` each time — the
        // previous iteration's bytes are already flushed to the peer via
        // this->send() (which goes through the TLS encrypt path when
        // EnableTls=true), so reusing the same storage is safe.
        uint8_t scratch[detail::kCodecAutoResponseBytes];

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
                // `plaintext_chunk`: one TLS record's payload after AEAD
                // decrypt-in-place. Ready to feed the application codec.
                [&](uint8_t* plaintext_chunk, std::size_t plaintext_len) {
                    // Stop feeding further records once the codec has
                    // signalled an unrecoverable error — any subsequent
                    // record would just re-trigger the same failure.
                    if (codec_err_latched) return;

                    uint8_t*    feed_ptr;
                    std::size_t feed_len;
                    if (tls_codec_pending_.empty()) {
                        feed_ptr = plaintext_chunk;      // zero-copy fast path
                        feed_len = plaintext_len;
                    } else {
                        // Slow path: a WS frame's payload spans this TLS
                        // record + the previous one's tail. Force a memcpy
                        // into pending so the codec sees a contiguous view.
                        // Counted so operators can spot config drift in the
                        // upstream's TLS write strategy (e.g. record cap
                        // shrunk, app frames suddenly larger). Typical
                        // production traffic should keep this near zero.
                        inc_<::eph::net::StreamMetric::kTlsCrossRecordFrames>();
                        tls_codec_pending_.insert(
                            tls_codec_pending_.end(),
                            plaintext_chunk, plaintext_chunk + plaintext_len);
                        feed_ptr = tls_codec_pending_.data();
                        feed_len = tls_codec_pending_.size();
                    }

                    detail::MbufView view(feed_ptr, feed_len, /*arrival_tsc*/ 0);
                    while (view.length() > 0) {
                        const std::size_t before = view.length();
                        // Per-iteration sink; flushed via this->send()
                        // below (before branching on `dr`) so close-acks
                        // on WsCloseReceived reach the wire prior to any
                        // session teardown. `send()` re-enters the TLS
                        // encrypt path (tls_send_buf_ is a separate member
                        // from tls_codec_pending_) which is safe while
                        // we are still iterating over decrypted RX
                        // records here.
                        core::OutputBuffer out_sink(scratch,
                                                     sizeof(scratch));
                        auto dr = codec_.decode(view, out_sink);
                        if (out_sink.size() > 0) {
                            auto sr = this->send(std::span<const uint8_t>(
                                out_sink.data(), out_sink.size()));
                            if (!sr) {
                                SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                                    "DpdkTcpStream::drain_codec_(TLS): "
                                    "auto-response send failed ({} bytes): {}",
                                    out_sink.size(), sr.error().detail);
                            } else {
                                SPDLOG_LOGGER_DEBUG(detail::tcp_stream_logger(),
                                    "DpdkTcpStream::drain_codec_(TLS): "
                                    "sent {} auto-response bytes",
                                    out_sink.size());
                            }
                        }
                        if (!dr) {
                            inc_<::eph::net::StreamMetric::kCodecErrors>();
                            SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                                "DpdkTcpStream::drain_codec_(TLS): decode err={}",
                                dr.error().detail);
                            tls_codec_pending_.clear();
                            codec_err_latched = true;
                            return;
                        }
                        if (!dr->has_value()) {
                            // Ok(None) + consumed>0 means the codec
                            // auto-handled a control frame; keep draining.
                            // Only consumed==0 justifies break.
                            if (view.length() == before) break;
                            continue;
                        }
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
                            on_message(std::span<const uint8_t>(
                                frame.data(), frame.size()));
                            inc_<::eph::net::StreamMetric::kFramesDecoded>();
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

                // Per-iteration sink; flushed before branching on `dr`
                // so close-acks written alongside WsCloseReceived reach
                // the wire before we teardown session state.
                core::OutputBuffer out_sink(scratch, sizeof(scratch));
                auto dr = codec_.decode(view, out_sink);
                if (out_sink.size() > 0) {
                    auto sr = this->send(std::span<const uint8_t>(
                        out_sink.data(), out_sink.size()));
                    if (!sr) {
                        SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                            "DpdkTcpStream::drain_codec_: "
                            "auto-response send failed ({} bytes): {}",
                            out_sink.size(), sr.error().detail);
                    } else {
                        SPDLOG_LOGGER_DEBUG(detail::tcp_stream_logger(),
                            "DpdkTcpStream::drain_codec_: sent {} "
                            "auto-response bytes", out_sink.size());
                    }
                }
                if (!dr) {
                    inc_<::eph::net::StreamMetric::kCodecErrors>();
                    SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                        "DpdkTcpStream::drain_codec_: decode err={}",
                        dr.error().detail);
                    break;
                }
                const std::size_t consumed = before - view.length();
                reasm_.consume(consumed);

                if (!dr->has_value()) {
                    // Ok(None) + consumed>0 means the codec auto-handled
                    // a control frame and we should keep draining.
                    // Only consumed==0 is "need more bytes".
                    if (consumed == 0) break;
                    continue;
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
                    on_message(std::span<const uint8_t>(frame.data(),
                                                        frame.size()));
                    inc_<::eph::net::StreamMetric::kFramesDecoded>();
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
    /// @brief RAII handle for the rte_flow rule installed by
    /// `create_and_attach` in FlowDirector mode (engaged only on NICs
    /// where `Platform::dispatch_mode() == FlowDirector`). When the
    /// stream is destroyed the rule is automatically removed from the
    /// NIC via `~FlowRule` → `rte_flow_destroy`. Empty in Software /
    /// RssPartitioned mode.
    std::optional<::eph::net::dpdk::FlowRule> flow_rule_{};

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
};

} // namespace eph::net::dpdk
