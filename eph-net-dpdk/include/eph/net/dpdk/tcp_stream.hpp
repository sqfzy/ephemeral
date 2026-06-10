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
#include "eph/net/detail/websocket.hpp"      // ws::close_code (close_gracefully)
#include "eph/net/detail/ws_handshake.hpp"   // WS HTTP handshake
#include "eph/net/handshake_phase.hpp"        // HandshakePhase (connect FSM)
#include "eph/net/dpdk/config.hpp"
#include "eph/net/dpdk/detail/daemon_disconnected_hook.hpp"  // T1.1+T1.2 in-flight semantics
#include "eph/net/dpdk/detail/mbuf_view.hpp"
#include "eph/net/dpdk/detail/reasm_buffer.hpp"  // ReasmBuffer (T2.2 partial split)
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/stream_metrics.hpp"
#include "eph/net/stream_snapshot.hpp"
#include "eph/net/tcp_state.hpp"
#include "eph/utils/time.hpp"

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

/// @brief Maximum number of `poll_rx` bursts the WS-handshake sinks will
///        attempt before surfacing `WouldBlock` to the outer handshake
///        deadline loop. Empty bursts are expected early in the handshake
///        while the peer's response still traverses the fabric; a bounded
///        retry here avoids a tight spin, and the caller's deadline makes
///        the aggregate timeout the real control.
inline constexpr int kWsHandshakeRecvBurstRetries = 16;

// TlsState (with aws-lc-backed in-place AEAD) lives in
// `detail/tls_state.hpp`.

// ReasmBuffer was extracted into detail/reasm_buffer.hpp as part of
// T2.2's partial split (2026-05-05) — same `eph::net::dpdk::detail`
// namespace, same symbol, no client-visible change. The include lives
// at the top of this file alongside the other detail/ headers.

// ---------------------------------------------------------------------------
// WS-handshake ByteSink adapters
// ---------------------------------------------------------------------------
//
// Mirrors the kernel variants: the DPDK byte pipe is `eph::dpdk::TcpSession<>`
// whose `send` + `poll_rx` return `std::expected<…, core::ErrorInfo>` with
// transport-layer error codes; the adapters re-classify those into the
// handshake-vocabulary codes (`Disconnected` / `BufferFull` / `WouldBlock`)
// that `eph::net::detail::perform_ws_handshake` expects, and pin the
// `detail` field to the adapter's call-site for diagnostics.
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

/// @tparam Session  Test-seam: defaults to the real `::eph::dpdk::TcpSession<>`;
///                  unit tests substitute a fake session that scripts
///                  send / poll_rx outcomes (see tests/fake_ws_session.hpp).
template <class Session = ::eph::dpdk::TcpSession<>>
class PlainDpdkWsSink {
public:
    explicit PlainDpdkWsSink(Session* sess) noexcept
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
        for (int iter = 0; iter < kWsHandshakeRecvBurstRetries; ++iter) {
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
    Session*             sess_{nullptr};
    std::vector<uint8_t> staged_{};
    std::size_t          staged_off_{0};
};

/// @tparam Session  Test-seam, defaults to `::eph::dpdk::TcpSession<>`
/// @tparam Tls      Test-seam, defaults to the real `TlsState`; fakes supply
///                  identity encrypt/decrypt so sink behavior can be exercised
///                  without aws-lc or a real handshake (tests/fake_ws_tls_state.hpp).
template <class Session = ::eph::dpdk::TcpSession<>,
          class Tls     = TlsState>
class TlsDpdkWsSink {
public:
    TlsDpdkWsSink(Session* sess, Tls* tls) noexcept
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

        for (int iter = 0; iter < kWsHandshakeRecvBurstRetries; ++iter) {
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
    Session*             sess_{nullptr};
    Tls*                 tls_{nullptr};
    std::vector<uint8_t> tx_scratch_{};
    std::vector<uint8_t> cipher_{};
    std::vector<uint8_t> plain_{};
    std::size_t          plain_off_{0};
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
    //
    // Two factories are supported:
    //   * `create(cfg)` — strict, requires a pre-chosen `src_port` and
    //     does not attach to any Poller. Used by unit tests and advanced
    //     users that manage their own poller topology.
    //   * `create_and_attach(cfg, platform)` — the recommended production
    //     path: picks a target RX queue, allocates a conflict-free
    //     `src_port` (rebinding to match the RSS hash when necessary),
    //     runs the TCP/TLS/WS handshakes, attaches to the per-queue
    //     Poller and registers the stream as an ICMP Frag Needed target
    //     on the Platform so path-MTU feedback routes correctly in
    //     multi-queue topologies.
    //
    // The older `create(cfg, poller)` overload has been retired: its
    // role (src_port auto-allocation + ICMP wiring) is now a proper
    // subset of `create_and_attach`, and leaving both in was causing
    // users to pick the wrong one.

    [[nodiscard]] static std::expected<std::unique_ptr<DpdkTcpStream>, core::ErrorInfo>
    create(StreamConfig cfg) noexcept {
        auto* log = detail::tcp_stream_logger();
        SPDLOG_LOGGER_DEBUG(log,
            "DpdkTcpStream::create: tls={} src={}:{} dst={}:{}", EnableTls,
            cfg.dpdk.wire.tuple.src_ip,
            cfg.dpdk.wire.tuple.src_port,
            cfg.dpdk.wire.tuple.dst_ip,
            cfg.dpdk.wire.tuple.dst_port);

        // Lower the public KeepaliveConfig into the wire-level TcpConfig
        // BEFORE validation so `wire.validate()` sees the same
        // bytes the PMD will consume. The KeepaliveConfig contract bounds
        // probes to [1, 10] when interval > 0, which is exactly what
        // TcpConfig::validate enforces — but only one of the two layers
        // needs to be the source of truth, and the public one wins.
        if (auto kv = cfg.keepalive.validate(); !kv) {
            SPDLOG_LOGGER_WARN(log,
                "DpdkTcpStream::create: KeepaliveConfig invalid: {}",
                kv.error().detail);
            return std::unexpected(kv.error());
        }
        if (!cfg.keepalive.empty()) {
            cfg.dpdk.wire.keepalive_interval = cfg.keepalive.interval;
            cfg.dpdk.wire.keepalive_probes   = cfg.keepalive.probes;
        }

        // Validate the wire-level TcpConfig — it carries all the DPDK
        // wiring (port/queue/MAC/mempool-adjacent parameters) and knows
        // exactly which invariants it needs. We convert the string error
        // to an ErrorInfo so the error contract holds.
        auto verr = cfg.dpdk.wire.validate();
        if (!verr.empty()) {
            SPDLOG_LOGGER_WARN(log,
                "DpdkTcpStream::create: wire validate failed: {}", verr);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkTcpStream::create: TcpConfig invalid"});
        }
        if (cfg.dpdk.pool == nullptr) {
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkTcpStream::create: pool must not be null"});
        }
        // Validate timeout values up front so a caller passing zero or
        // negative `connect_timeout` fails at the config boundary instead
        // of emerging much later as a cryptic `Error::Timeout`. Parity
        // with the kernel backend (MED-2 / commit 7aa19b6).
        if (cfg.connect_timeout <= std::chrono::milliseconds::zero()) {
            SPDLOG_LOGGER_WARN(log,
                "DpdkTcpStream::create: connect_timeout={}ms must be > 0",
                cfg.connect_timeout.count());
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkTcpStream::create: connect_timeout must be > 0"});
        }
        // Delegate WS sub-config validation. Empty `cfg.ws.path` is the
        // disabled state and always validates; non-empty path requires a
        // strictly positive timeout. See `eph/net/ws_config.hpp`.
        if (auto wv = cfg.ws.validate(); !wv) {
            SPDLOG_LOGGER_WARN(log,
                "DpdkTcpStream::create: WsConfig invalid: {}",
                wv.error().detail);
            return std::unexpected(wv.error());
        }
        // TLS sub-config validation. Mirrors KernelTcpStream::create —
        // catches handshake_timeout <= 0 and client_cert/client_key
        // mismatch before the EAL bring-up path commits resources to a
        // doomed handshake.
        if constexpr (EnableTls) {
            if (auto tv = cfg.tls.validate(); !tv) {
                SPDLOG_LOGGER_WARN(log,
                    "DpdkTcpStream::create: TlsConfig invalid: {}",
                    tv.error().detail);
                return std::unexpected(tv.error());
            }
        }
        // Reasm capacity: 0 silently falls back to 256 KiB (default) —
        // supports the "just use the default" idiom via designated init.
        // Non-zero-but-tiny values (e.g. 512 bytes) would let the stream
        // construct successfully then overflow on the first burst; reject
        // those at config time with an actionable error. The floor must
        // comfortably exceed a single MSS + TLS record overhead so even
        // the smallest reasonable frame can land without a reset.
        static constexpr std::size_t kMinReasmCapacity = 4096;
        if (cfg.reasm_capacity != 0 &&
            cfg.reasm_capacity < kMinReasmCapacity) {
            SPDLOG_LOGGER_WARN(log,
                "DpdkTcpStream::create: reasm_capacity={} bytes is below "
                "floor={} (use 0 for default or >= {}KB)",
                cfg.reasm_capacity, kMinReasmCapacity,
                kMinReasmCapacity / 1024);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkTcpStream::create: reasm_capacity too small"});
        }
        // HTTP CONNECT proxy ghost field was removed from DpdkStreamConfig
        // in T3.19. Users who try to set a proxy now get a compile-time
        // error pointing them to the kernel backend, which is the only
        // place CONNECT is supported.

        // Construct the stream first so the TcpSession is rooted inside
        // its final storage location (TcpSession is move-constructible
        // but we prefer to keep the pointer stable for friend-hook thunks).
        auto stream = std::unique_ptr<DpdkTcpStream>(
            new DpdkTcpStream(std::move(cfg)));

        // ── Issue a NON-BLOCKING connect. The remaining handshake legs
        //    (TLS / WebSocket) are driven later, across poll cycles, by
        //    poll_once_() (single-stream / connect_blocking) or
        //    process_burst_() (multi-stream Poller) via drive_handshake_().
        //    create() never blocks on I/O. ──
        if (auto b = stream->sess_.begin_connect(); !b) {
            SPDLOG_LOGGER_WARN(log,
                "DpdkTcpStream::create: begin_connect failed: {}",
                b.error().detail);
            return std::unexpected(b.error());
        }
        stream->hs_phase_ = ::eph::net::HandshakePhase::TcpConnecting;
        stream->connect_deadline_ =
            std::chrono::steady_clock::now() + stream->cfg_.connect_timeout;
        SPDLOG_LOGGER_DEBUG(log,
            "DpdkTcpStream::create: non-blocking connect initiated "
            "src=0x{:08x}:{} -> dst=0x{:08x}:{} phase={}",
            stream->cfg_.dpdk.wire.tuple.src_ip, stream->cfg_.dpdk.wire.tuple.src_port,
            stream->cfg_.dpdk.wire.tuple.dst_ip, stream->cfg_.dpdk.wire.tuple.dst_port,
            ::eph::net::handshake_phase_name(stream->hs_phase_));
        return stream;
    }

    /// @brief Turnkey factory: create a stream and attach it to the
    /// per-queue Poller already registered with `platform`. Honours
    /// `cfg.dpdk.pin_to_queue` and the platform's `dispatch_mode()` to pick
    /// the correct RX queue (and, in RSS+pin mode, to rebind the
    /// ephemeral src_port so the connection's hash lands on the target
    /// queue). Pre-conditions:
    ///   * the relevant `DpdkPoller<>` is already registered with
    ///     `platform.register_poller(qid, poller)` for every queue this
    ///     stream might land on;
    ///   * `cfg.dpdk.wire.tuple.dst_ip` / `dst_port` carry the remote
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
            SPDLOG_LOGGER_ERROR(log,
                "DpdkTcpStream::create_and_attach: Platform has 0 RX queues "
                "(port_id={}, moved-from or never created)",
                platform.port_id());
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "create_and_attach: Platform has 0 RX queues "
                "(moved-from or never created)"});
        }

        // ── Phase 1: pre-create — pick target queue ──────────────────────────
        // create_and_attach NEVER rewrites src_port: every mode uses the
        // caller's explicit `cfg.dpdk.wire.tuple.src_port` as-is. On ENA the
        // RSS key is a placeholder (prediction at chance), so RSS queue
        // landing is determined empirically — the caller measures a src_port
        // (dpdk_rss_queue_probe --finder) and pins it. `StreamSnapshot::Endpoint::
        // src_port_rewritten` is therefore always false (member default).
        uint16_t target_qid = 0;

        if (mode == ::eph::net::dpdk::RxDispatchMode::Software) {
            if (cfg.dpdk.pin_to_queue && *cfg.dpdk.pin_to_queue != 0) {
                SPDLOG_LOGGER_ERROR(log,
                    "DpdkTcpStream::create_and_attach: pin_to_queue={} != 0 "
                    "in Software dispatch mode (single-queue Platform)",
                    *cfg.dpdk.pin_to_queue);
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "create_and_attach: pin_to_queue != 0 in Software mode"});
            }
            target_qid = 0;
        } else if (mode == ::eph::net::dpdk::RxDispatchMode::RssPartitioned) {
            // RSS queue landing is EMPIRICAL, never predicted: on ENA the
            // readable RSS key is a placeholder that predicts the landing
            // queue at CHANCE (see docs/cpu-no-cross-core.md +
            // .artifacts/experiment-20260601-142315.md). The caller MUST
            // pin_to_queue AND supply an explicit cfg.dpdk.wire.tuple.src_port
            // measured (via tools/dpdk_rss_queue_probe --finder) to land on it.
            if (!cfg.dpdk.pin_to_queue) {
                SPDLOG_LOGGER_ERROR(log,
                    "DpdkTcpStream::create_and_attach: RssPartitioned requires "
                    "pin_to_queue + an explicit measured src_port "
                    "(run dpdk_rss_queue_probe --finder)");
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "create_and_attach: RssPartitioned needs pin_to_queue + "
                    "explicit src_port (queue prediction retired)"});
            }
            const uint16_t want = *cfg.dpdk.pin_to_queue;
            if (want >= nb_q) {
                SPDLOG_LOGGER_ERROR(log,
                    "DpdkTcpStream::create_and_attach: pin_to_queue={} "
                    ">= nb_rx_queues={} (RssPartitioned)", want, nb_q);
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "create_and_attach: pin_to_queue >= nb_rx_queues"});
            }
            if (cfg.dpdk.wire.tuple.src_port == 0) {
                SPDLOG_LOGGER_ERROR(log,
                    "DpdkTcpStream::create_and_attach: RssPartitioned with "
                    "pin_to_queue={} requires an explicit "
                    "cfg.dpdk.wire.tuple.src_port measured to land on that "
                    "queue (run dpdk_rss_queue_probe --finder)",
                    want);
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "create_and_attach: RssPartitioned requires explicit "
                    "cfg.dpdk.wire.tuple.src_port (queue prediction retired)"});
            }
            target_qid = want;
            // Align rx/tx_queue_id with target_qid so the SYN/SYN-ACK/ACK
            // handshake in TStream::create() polls the queue where the
            // caller-measured SYN-ACK will land.
            cfg.dpdk.wire.rx_queue_id = target_qid;
            cfg.dpdk.wire.tx_queue_id = target_qid;
            SPDLOG_LOGGER_INFO(log,
                "create_and_attach: RssPartitioned explicit src_port={} pinned "
                "to queue={} (prediction retired; caller-measured)",
                cfg.dpdk.wire.tuple.src_port, target_qid);
        } else {  // FlowDirector
            if (cfg.dpdk.pin_to_queue && *cfg.dpdk.pin_to_queue >= nb_q) {
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "create_and_attach: pin_to_queue >= nb_rx_queues"});
            }
            // KNOWN LIMITATION (FlowDirector handshake race):
            // Unlike the RssPartitioned branch above — which uses a
            // caller-measured src_port landing on target_qid and aligns
            // cfg.dpdk.wire.{rx,tx}_queue_id = target_qid so the
            // SYN/SYN-ACK/ACK loop in TStream::create() polls the right queue
            // — the FlowDirector branch does NOT touch cfg.dpdk.wire.{rx,tx}_
            // queue_id here. The reason is timing: the rte_flow rule that
            // would steer this 5-tuple to target_qid is only installed AFTER
            // create() returns (post-handshake), so during the handshake the
            // PMD's default RSS routes the SYN-ACK to whichever queue its
            // hash picks — typically NOT target_qid. Setting rx_queue_id to
            // target_qid here would make the handshake poll an empty queue.
            // Today's behaviour: the handshake polls cfg.dpdk.wire.rx_queue_id
            // (caller default, usually 0) and works iff default RSS happens
            // to route the SYN-ACK to that same queue. On NICs that
            // round-robin RSS without a steering rule (e.g. Mellanox, some
            // Intel) this is fragile under multi-stream load. A proper fix
            // requires either (a) installing a transient "steer-to-rx_queue"
            // rule before create() and replacing it after, or (b) installing
            // the final FD rule first and letting create() poll target_qid
            // directly — both are non-trivial and need an FD-capable NIC
            // (Mellanox/Intel) to validate. Tracked as followup; see
            // batch-1 commit d60fe7a2's STATE entry for the RSS-branch
            // counterpart that was fixed there.
            //
            // Default queue selection: round-robin via a static counter.
            // Atomic so concurrent create_and_attach calls from different
            // threads don't all map to queue 0. Range-aware so multi-process
            // (primary+secondary) setups can partition queues via
            // PlatformConfig::rx_queue_range; single-process default `{0, 0}`
            // resolves to `[0, nb_rx_queues)` which matches the prior
            // `% nb_q` behavior byte-for-byte. The `validate_config` step
            // has guaranteed `qlo < qhi` (sentinel `{0,0}` is normalised by
            // `effective_rx_queue_range`), so no runtime fallback is needed
            // here.
            if (cfg.dpdk.pin_to_queue) {
                target_qid = *cfg.dpdk.pin_to_queue;
            } else {
                static std::atomic<uint16_t> rr_counter{0};
                const auto [qlo, qhi] = platform.effective_rx_queue_range();
                // Defense in depth: a moved-from Platform (impl_ == nullptr)
                // returns {0, 0} from effective_rx_queue_range; divide-by-zero
                // in `% qrange` would be UB. validate_config normally rules
                // this out, but a moved-from Platform is reachable via
                // user-after-move and not caught by validation.
                if (qhi <= qlo) {
                    return std::unexpected(core::ErrorInfo{
                        core::Error::InvalidConfig,
                        "create_and_attach: empty effective_rx_queue_range "
                        "(Platform moved-from or misconfigured)"});
                }
                const uint16_t qrange = qhi - qlo;
                target_qid = qlo + (rr_counter.fetch_add(1,
                                std::memory_order_relaxed) % qrange);
            }
        }

        // ── Optional: resolve per-lcore mempool hint ─────────────────────────
        //
        // When `cfg.dpdk.pool_lcore_hint >= 0`, override `cfg.dpdk.pool` with the
        // Platform's per-lcore pool for that lcore id. This is the
        // NUMA-aware allocation path (T2.9). When the hint is -1 (default)
        // we leave `cfg.dpdk.pool` untouched — backwards compatible with every
        // call site that pre-dates per-lcore pools and populates pool by
        // hand from `platform.mempool()`.
        if (cfg.dpdk.pool_lcore_hint >= 0) {
            const auto lcore_id =
                static_cast<uint16_t>(cfg.dpdk.pool_lcore_hint);
            auto* p = platform.pool_for_lcore(lcore_id);
            if (p == nullptr) {
                SPDLOG_LOGGER_WARN(log,
                    "create_and_attach: pool_for_lcore({}) returned nullptr "
                    "(per_lcore_pools may be 0 with non-zero hint, or hint "
                    "exceeds per_lcore_pools)", lcore_id);
                return std::unexpected(core::ErrorInfo{
                    core::Error::InvalidConfig,
                    "create_and_attach: pool_for_lcore lookup returned nullptr"});
            }
            cfg.dpdk.pool = p;
        }

        // ── Phase 2: actual stream construction (TCP handshake + TLS + WS) ──
        auto sr = create(std::move(cfg));
        if (!sr) return std::unexpected(sr.error());
        auto stream = std::move(*sr);

        // src_port_rewritten_ stays false: create_and_attach no longer
        // rewrites src_port (RSS-prediction retired — see Phase 1 note).

        // TD-2: propagate effective strict mode from Platform. Only set
        // here (not in plain create()) because create() has no Platform
        // reference; unattached streams stay in non-strict best-effort.
        stream->set_strict_rx_checksum_(platform.strict_rx_checksum());
        // T1.1+T1.2 wire-up: stash a Platform back-pointer so the burst
        // path's pre-burst is_alive() check can detect daemon-died
        // mid-flight without the application needing an external
        // watchdog. Platform must outlive every attached Stream (same
        // contract the Poller already requires).
        stream->platform_ = &platform;
        // T1.1+T1.2 rx-side wire-up: install the alive-flag address
        // on the Poller so its cycle-boundary check can short-circuit.
        // (Platform::register_poller cannot do this directly because
        // platform.hpp only forward-declares DpdkPoller — same reason
        // the ICMP callback is installed here, not at register time.)
        if (auto* poller = platform.poller_for_queue(target_qid);
            poller != nullptr) {
            poller->set_alive_flag_(platform.alive_flag_addr_());
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
            const auto& t = stream->cfg_.dpdk.wire.tuple;
            ::eph::dpdk::net::ConnectionTuple fl_tuple{
                .src_ip   = t.src_ip,   .dst_ip   = t.dst_ip,
                .src_port = t.src_port, .dst_port = t.dst_port};
            auto rule = ::eph::net::dpdk::install_flow_rule(
                platform.port_id(), target_qid, fl_tuple,
                ::eph::net::dpdk::FlowProtocol::Tcp);
            // Try-secondary-then-fallback: when local rte_flow_create
            // fails AND we're a secondary in mp_topology mode, attempt
            // the IPC fallback to the primary. Some PMDs (notably non-
            // ENA / non-mlx5 / non-i40e) reject rte_flow_create from
            // secondaries — IPC fallback transparently delegates to
            // the primary. Primary-mode failures, or secondary failures
            // with no IPC available, fall through to the original
            // unexpected return.
            if (!rule && platform.is_secondary() &&
                platform.is_multi_process()) {
                SPDLOG_LOGGER_WARN(log,
                    "create_and_attach: local rte_flow_create rejected "
                    "({}); trying eph_fd_install IPC fallback to primary",
                    rule.error());
                rule = ::eph::net::dpdk::try_install_flow_rule_via_ipc(
                    platform.port_id(), target_qid, fl_tuple,
                    ::eph::net::dpdk::FlowProtocol::Tcp);
            }
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

        // Install a Platform-aware ICMP dispatch closure on this
        // Poller. Capturing the registry's `shared_ptr` *by value*
        // gives the Poller a strong ref — if the user's Platform
        // outlives its Poller (the normal order) the ref is released
        // quickly; if the user destroys Platform before the Poller
        // (legal but easy to do accidentally), the Poller's closure
        // keeps the registry alive, so ICMP dispatch continues to
        // route safely to Stream Handles still registered. Major 2
        // root fix for the "Platform* dangling in Poller ctx" UAF.
        //
        // Overwriting on each create_and_attach is fine: every call
        // passes the same `platform` reference, so the captured
        // shared_ptr points to the same registry; the net effect is
        // a fresh closure with an extra ref-bump on the same
        // underlying control block. Idempotent from Poller's view.
        if (auto reg_sp = platform.icmp_registry_shared_()) {
            poller->set_icmp_callback(
                [reg_sp = std::move(reg_sp)](
                    const ::eph::dpdk::net::ParsedIcmp& parsed) noexcept {
                    // 1. Try local registry first (fast path; matches
                    //    reshape stage 2 behavior byte-for-byte when
                    //    the target stream is owned by this process).
                    if (reg_sp->dispatch_returns_hit(parsed)) return;

                    // 2. Local miss. Consult cross-proc directory if
                    //    mp_topology was set on this Platform; if not,
                    //    drop silently (= old behavior).
                    auto* dir = ::eph::dpdk::detail::g_active_icmp_directory
                                    .load(std::memory_order_acquire);
                    if (dir == nullptr) return;

                    ::eph::dpdk::net::ConnectionTuple t{
                        .src_ip   = parsed.embedded_src_ip,
                        .dst_ip   = parsed.embedded_dst_ip,
                        .src_port = parsed.embedded_src_port,
                        .dst_port = parsed.embedded_dst_port,
                    };
                    auto found = dir->lookup(t, parsed.embedded_proto);
                    if (!found) {
                        if (auto* hdr = dir->header()) {
                            hdr->dropped_no_owner.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                        return;
                    }

                    // Race: directory still says we own but local
                    // registry already moved on (unregister between
                    // dispatch and lookup). Drop — caller would have
                    // observed the removal.
                    const uint8_t self_idx =
                        ::eph::dpdk::detail::g_active_self_proc_index
                            .load(std::memory_order_acquire);
                    if (found->owner_proc == self_idx) return;

                    // 3. Forward via fire-and-forget IPC.
                    auto msg = ::eph::dpdk::detail::make_icmp_dispatch_msg(
                        parsed, found->slot_idx, found->generation);
                    auto r = ::eph::dpdk::detail::mp_ipc_send_oneway(
                        ::eph::dpdk::detail::kIcmpDispatchActionName, msg);
                    if (r) {
                        if (auto* hdr = dir->header()) {
                            hdr->ipc_msgs_sent.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                    }
                });
        }

        // Register the stream as an ICMP Frag Needed target so path-MTU
        // feedback routes to our `sess_.on_icmp_frag_needed`. Platform
        // walks the registry across ALL per-queue pollers, so this works
        // correctly regardless of which RX queue the router's ICMP
        // response happens to land on. The returned RAII handle auto-
        // unregisters on ~DpdkTcpStream.
        auto icmp_reg = platform.register_icmp_target(
            stream->cfg_.dpdk.wire.tuple,
            eph::dpdk::net::kIpProtoTcp,
            stream.get(),
            &DpdkTcpStream::on_icmp_mtu_thunk_);
        if (!icmp_reg) {
            SPDLOG_LOGGER_WARN(log,
                "create_and_attach: register_icmp_target failed: {}",
                icmp_reg.error());
            (void)poller->remove(stream.get());
            // Registry-full is a resource-exhaustion condition, not a
            // misconfiguration — use OutOfMemory to match the rest of
            // the codebase (mbuf alloc failed / Poller full also use it).
            return std::unexpected(core::ErrorInfo{
                core::Error::OutOfMemory,
                "create_and_attach: register_icmp_target failed"});
        }
        stream->icmp_reg_.emplace(std::move(*icmp_reg));

        SPDLOG_LOGGER_INFO(log,
            "create_and_attach: TCP stream attached → port={}, queue={}, mode={}",
            platform.port_id(), target_qid,
            ::eph::net::dpdk::rx_dispatch_mode_name(mode));
        return stream;
    }

    ~DpdkTcpStream() {
        // If still attached to a Poller, remove ourselves first so the
        // Poller's entries_ does not retain a dangling pointer. A non-OK
        // result means the Stream and Poller lifecycles have drifted out
        // of sync (e.g. double-remove or stale attached_to_ after an
        // unexpected path) — log loudly so the bug surfaces instead of
        // being swallowed silently by the dtor.
        if (attached_to_ != nullptr) {
            SPDLOG_LOGGER_DEBUG(detail::tcp_stream_logger(),
                "~DpdkTcpStream: auto-detach");
            auto r = attached_to_->remove(this);
            if (!r) {
                SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                    "~DpdkTcpStream: auto-detach failed: {} — possible "
                    "Poller/Stream lifecycle mismatch",
                    r.error().detail ? r.error().detail : "unknown");
            }
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
        // Fail-fast guard on TLS write-seq desync. Checked BEFORE the
        // attach/established preconditions so the error surface is
        // identical (Disconnected) regardless of whether the desync was
        // observed on a not-yet-attached stream, a live session, or a
        // mid-teardown reconnect retry. The moment this latch trips, the
        // peer's AEAD state is permanently out of sync with ours and no
        // further byte we send can decrypt successfully — caller must
        // reconnect to replace the TLS state entirely. See
        // `kTlsSendDesyncs`.
        if constexpr (EnableTls) {
            if (tls_corrupt_) {
                return std::unexpected(core::ErrorInfo{
                    core::Error::Disconnected,
                    "DpdkTcpStream::send: TLS state desynced "
                    "(partial send advanced write seq past wire) — "
                    "reconnect required"});
            }
        }
        if (attached_to_ == nullptr) {
            return std::unexpected(core::ErrorInfo{
                core::Error::NotAttached,
                "DpdkTcpStream::send called before attach"});
        }
        // T1.1+T1.2: pre-burst daemon-alive check. When the daemon
        // dies while we have bytes queued for send, surface the
        // condition immediately rather than letting rte_eth_tx_burst
        // race against the mempool teardown. `platform_` is non-null
        // only when create_and_attach() was used; bare-create path
        // skips this check (no Platform context).
        if (platform_ != nullptr && !platform_->is_alive()) [[unlikely]] {
            ::eph::net::dpdk::detail::set_daemon_disconnected_detail(
                ::eph::net::dpdk::detail::InFlightStatus::Unsent,
                /*bytes_observed=*/app_payload.size(),
                /*bytes_confirmed=*/0,
                /*phase=*/"DpdkTcpStream::send");
            return std::unexpected(core::ErrorInfo{
                core::Error::DaemonDisconnected,
                "DpdkTcpStream::send: daemon disconnected (Platform::is_alive==false); "
                "bytes are Unsent — see last_daemon_disconnected_detail()"});
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
            // (Desync latch already checked above.)
            tls_send_buf_.clear();
            auto enc = tls_.encrypt_for_send(app_payload.data(),
                                              app_payload.size(),
                                              tls_send_buf_);
            if (!enc) {
                // encrypt_for_send may fail mid-payload after one or more
                // chunks already advanced the AEAD write seq (each successful
                // EVP_AEAD_CTX_seal does seq_++; a later failure leaves the
                // counter ahead of what the wire ever saw — even though we
                // never enter the sess_.send loop, no ciphertext escapes).
                // Once that partial-advance happens, peer's read seq will
                // diverge on the very next record we manage to encrypt and
                // ship, so the latch is the only correct response.
                // Conservative: latch on every encrypt failure (single-chunk
                // failures don't strictly need it, but a false-positive
                // reconnect is benign — the caller is already on the error
                // path).
                tls_corrupt_ = true;
                inc_<::eph::net::StreamMetric::kTlsSendDesyncs>();
                SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                    "DpdkTcpStream::send(TLS): encrypt_for_send failed "
                    "({}) — latching tls_corrupt_; AEAD write seq may have "
                    "partially advanced past wire, reconnect required",
                    enc.error().detail);
                return std::unexpected(enc.error());
            }
            // The encrypted buffer may exceed MSS (TLS record overhead +
            // plaintext), so chunk by MSS before handing to the session.
            //
            // Any failure past this point desyncs the peer: encrypt_for_send
            // already bumped the TLS write seq to cover the full plaintext,
            // so if we bail out without delivering every ciphertext byte,
            // the next record will use a seq the peer does not expect and
            // its AEAD-open will fail permanently. Latch `tls_corrupt_`
            // before returning so the next send() / RX path surface
            // Disconnected and trigger reconnect.
            const std::size_t mss = sess_.mss();
            std::size_t off = 0;
            while (off < tls_send_buf_.size()) {
                const std::size_t chunk =
                    std::min(mss, tls_send_buf_.size() - off);
                auto sr = sess_.send(tls_send_buf_.data() + off, chunk);
                if (!sr) {
                    tls_corrupt_ = true;
                    inc_<::eph::net::StreamMetric::kTlsSendDesyncs>();
                    // Pass through the session's typed error — no re-wrap
                    // needed now that sess_.send returns ErrorInfo.
                    SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                        "DpdkTcpStream::send(TLS): {} "
                        "(off={}/{}, chunk={}, mss={}) — latching "
                        "tls_corrupt_ since TLS write seq was already "
                        "advanced; reconnect required",
                        sr.error().detail, off, tls_send_buf_.size(),
                        chunk, mss);
                    // T1.1+T1.2 post-burst: if daemon died during the
                    // partial send, surface DaemonDisconnected + status
                    // (off > 0 → Uncertain; off == 0 → Unsent). Otherwise
                    // pass through the original session error.
                    if (auto e = check_post_burst_(
                            off > 0
                                ? ::eph::net::dpdk::detail::InFlightStatus::Uncertain
                                : ::eph::net::dpdk::detail::InFlightStatus::Unsent,
                            tls_send_buf_.size(), off,
                            "DpdkTcpStream::send(TLS,post-burst-error)")) {
                        return std::unexpected(*e);
                    }
                    return std::unexpected(sr.error());
                }
                if (*sr == 0) {
                    tls_corrupt_ = true;
                    inc_<::eph::net::StreamMetric::kTlsSendDesyncs>();
                    SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                        "DpdkTcpStream::send(TLS): TcpSession::send returned "
                        "0 bytes (off={}/{}, chunk={}) — latching "
                        "tls_corrupt_",
                        off, tls_send_buf_.size(), chunk);
                    // T1.1+T1.2 post-burst classification — same shape as
                    // the !sr branch above.
                    if (auto e = check_post_burst_(
                            off > 0
                                ? ::eph::net::dpdk::detail::InFlightStatus::Uncertain
                                : ::eph::net::dpdk::detail::InFlightStatus::Unsent,
                            tls_send_buf_.size(), off,
                            "DpdkTcpStream::send(TLS,post-burst-zero)")) {
                        return std::unexpected(*e);
                    }
                    return std::unexpected(core::ErrorInfo{
                        core::Error::BufferFull,
                        "DpdkTcpStream::send: TcpSession::send returned 0"});
                }
                off += *sr;
            }
            inc_<::eph::net::StreamMetric::kBytesSent>(app_payload.size());
            // T1.1+T1.2 post-burst: TLS path sent every byte successfully;
            // if the daemon died between our pre-burst check and now,
            // status is `Sent` (bytes are committed; reconnect path
            // should NOT retransmit lest the peer dedupe).
            if (auto e = check_post_burst_(
                    ::eph::net::dpdk::detail::InFlightStatus::Sent,
                    app_payload.size(), app_payload.size(),
                    "DpdkTcpStream::send(TLS,post-burst)")) {
                return std::unexpected(*e);
            }
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
                    // Account the bytes that DID make it to the wire before
                    // failure, so kBytesSent matches the kernel backend's
                    // semantic — telemetry must not under-report a partial
                    // multi-MSS send. Return value still surfaces the error
                    // (the public Stream contract is all-or-nothing on
                    // success, unchanged).
                    if (off > 0) {
                        inc_<::eph::net::StreamMetric::kBytesSent>(off);
                    }
                    SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                        "DpdkTcpStream::send: {} "
                        "(off={}/{}, chunk={}, mss={})",
                        sr.error().detail, off, app_payload.size(), chunk, mss);
                    // T1.1+T1.2 post-burst classification.
                    if (auto e = check_post_burst_(
                            off > 0
                                ? ::eph::net::dpdk::detail::InFlightStatus::Uncertain
                                : ::eph::net::dpdk::detail::InFlightStatus::Unsent,
                            app_payload.size(), off,
                            "DpdkTcpStream::send(plain,post-burst-error)")) {
                        return std::unexpected(*e);
                    }
                    return std::unexpected(sr.error());
                }
                if (*sr == 0) {
                    if (off > 0) {
                        inc_<::eph::net::StreamMetric::kBytesSent>(off);
                    }
                    SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                        "DpdkTcpStream::send: TcpSession::send returned 0 "
                        "bytes (off={}/{}, chunk={})",
                        off, app_payload.size(), chunk);
                    if (auto e = check_post_burst_(
                            off > 0
                                ? ::eph::net::dpdk::detail::InFlightStatus::Uncertain
                                : ::eph::net::dpdk::detail::InFlightStatus::Unsent,
                            app_payload.size(), off,
                            "DpdkTcpStream::send(plain,post-burst-zero)")) {
                        return std::unexpected(*e);
                    }
                    return std::unexpected(core::ErrorInfo{
                        core::Error::BufferFull,
                        "DpdkTcpStream::send: TcpSession::send returned 0"});
                }
                off += *sr;
            }
            inc_<::eph::net::StreamMetric::kBytesSent>(app_payload.size());
            // T1.1+T1.2 post-burst: full payload through; status `Sent`
            // when daemon died after the burst.
            if (auto e = check_post_burst_(
                    ::eph::net::dpdk::detail::InFlightStatus::Sent,
                    app_payload.size(), app_payload.size(),
                    "DpdkTcpStream::send(plain,post-burst)")) {
                return std::unexpected(*e);
            }
            return app_payload.size();
        }
    }

    /// @brief Initiate a graceful close — sends WS Close frame (when the
    ///        codec is WS-aware) then enqueues TCP FIN.
    ///
    /// When the codec defines `encode_close` (i.e. is `WsCodec`), prepends
    /// an RFC 6455 §7.1.1 Close frame onto the TX path BEFORE the TCP
    /// FIN. The `if constexpr (requires { ... })` branch compiles away
    /// for non-WS codecs (RawStreamCodec, FixCodec, ...). Caller's
    /// poller is responsible for draining the peer's Close-ack on RX;
    /// this method does not wait.
    ///
    /// Threading: caller MUST NOT have a `DpdkPoller` actively driving
    /// this stream from another lcore during the call — the new
    /// `send()` would race with RX-side codec state. Same contract as
    /// `drain()` below.
    [[nodiscard]] std::expected<void, core::ErrorInfo>
    close_gracefully() noexcept {
        // WS-Close emission (RFC 6455 §7.1.1). Compiles out for non-WS
        // codecs. Best-effort — failure here doesn't block TCP FIN.
        if constexpr (requires {
            codec_.encode_close(
                std::declval<uint8_t*>(), std::declval<std::size_t>(),
                uint16_t{}, std::string_view{});
        }) {
            if (sess_.is_established()) {
                uint8_t close_buf[16];  // 14-byte max header + 2-byte status
                auto enc = codec_.encode_close(
                    close_buf, sizeof(close_buf),
                    eph::net::ws::close_code::kNormal);
                if (enc) {
                    auto sr = this->send(
                        std::span<const uint8_t>(close_buf, *enc));
                    if (!sr) {
                        SPDLOG_LOGGER_DEBUG(detail::tcp_stream_logger(),
                            "DpdkTcpStream::close_gracefully: "
                            "WS Close send skipped: {}",
                            sr.error().detail);
                    }
                } else {
                    SPDLOG_LOGGER_DEBUG(detail::tcp_stream_logger(),
                        "DpdkTcpStream::close_gracefully: "
                        "encode_close skipped: {}",
                        enc.error().detail);
                }
            }
        }

        auto r = sess_.close();
        if (!r) {
            SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                "DpdkTcpStream::close_gracefully: {}", r.error().detail);
            return std::unexpected(r.error());
        }
        return {};
    }

    /// @brief Synchronous graceful drain — send our FIN and burst-poll the
    ///        NIC until the peer's FIN-ACK arrives or `timeout` elapses.
    ///
    /// Semantics:
    ///   1. Stream MUST be `state() == Established` on entry; otherwise
    ///      returns `Error::InvalidConfig` without touching the session.
    ///   2. Calls `TcpSession::close()` to send our FIN. State flips to
    ///      `FinWait1`. (close() also picks up CloseWait → LastAck if
    ///      the peer initiated, but the spec restricts drain entry to
    ///      Established so that branch is unreachable from here.)
    ///   3. Drives `TcpSession::poll_rx` in a tight loop, internally
    ///      calling `rte_eth_rx_burst` on every iteration. The TCP state
    ///      machine inside the session handles SYN/FIN/ACK accounting:
    ///      FinWait1 -> FinWait2 (peer ACKs our FIN) -> TimeWait
    ///      (peer's FIN observed). On reaching `TimeWait` we declare
    ///      "peer FIN-ACK observed", force the session to `Closed`
    ///      (the 2*MSL deferral is unnecessary on a single-shot orderly
    ///      shutdown — we will not reuse this 4-tuple), and return Ok.
    ///      We also accept `Closed` as a success terminal in case the
    ///      session reaches it directly via the LastAck path or a peer
    ///      RST race.
    ///   4. Application bytes that arrive during the drain are silently
    ///      discarded — `on_message` is NOT invoked. Drain is a teardown
    ///      path, not a data path.
    ///   5. On timeout: bumps `kRxSessionResets`, calls `sess_.reset()`
    ///      (sends RST so the peer's stack unblocks immediately and the
    ///      4-tuple is freed), and returns `Err(Error::Timeout)`.
    ///
    /// Threading: synchronous and single-threaded. The caller MUST NOT
    /// have a `DpdkPoller` actively poll()-ing this stream from another
    /// lcore during the drain — it would race on the session and steal
    /// RX bursts that drain() needs to advance the close handshake.
    ///
    /// `close_gracefully()` remains the lighter cousin: it sends the FIN
    /// and returns immediately, leaving the wait-for-peer-FIN to the
    /// caller's poller cycle.
    [[nodiscard]] std::expected<void, core::ErrorInfo>
    drain(std::chrono::milliseconds timeout) noexcept {
        auto* log = detail::tcp_stream_logger();
        SPDLOG_LOGGER_INFO(log,
            "DpdkTcpStream::drain entry: state={} timeout_ms={}",
            ::eph::net::tcp_state_name(sess_.state()), timeout.count());

        if (sess_.state() != ::eph::net::TcpState::Established) {
            SPDLOG_LOGGER_WARN(log,
                "DpdkTcpStream::drain: state={} (need Established) — "
                "rejecting", ::eph::net::tcp_state_name(sess_.state()));
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkTcpStream::drain: state != Established"});
        }
        if (timeout <= std::chrono::milliseconds::zero()) {
            SPDLOG_LOGGER_WARN(log,
                "DpdkTcpStream::drain: timeout_ms={} must be > 0",
                timeout.count());
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "DpdkTcpStream::drain: timeout must be > 0"});
        }

        // Step 1: send our FIN.
        auto cr = sess_.close();
        if (!cr) {
            SPDLOG_LOGGER_WARN(log,
                "DpdkTcpStream::drain: close() failed: {}", cr.error().detail);
            return std::unexpected(cr.error());
        }
        // sess_.close() flips state to FinWait1 on success.

        // Step 2: TSC deadline. Cold path — TSC is the codebase
        // convention but we fall back to steady_clock when the TSC
        // wasn't calibrated (uncommon: only in test harnesses that
        // forgot to call TSC::init()).
        const std::uint64_t start_tsc = ::eph::utils::TSC::now();
        auto cycles_opt = ::eph::utils::TSC::to_cycles(timeout);
        std::uint64_t deadline_tsc = 0;
        const bool tsc_available = cycles_opt.has_value();
        if (tsc_available) {
            if (*cycles_opt > UINT64_MAX - start_tsc) {
                deadline_tsc = UINT64_MAX;
            } else {
                deadline_tsc = start_tsc + *cycles_opt;
            }
        }
        const auto deadline_steady =
            std::chrono::steady_clock::now() + timeout;

        // Step 3: drain loop. Burst-poll the NIC; the session's state
        // machine drives FinWait1 -> FinWait2 -> TimeWait on its own
        // as ACK / FIN packets are observed.
        //
        // We drop application bytes the peer may emit before its FIN
        // (it had buffered TX of its own and flushes it before honoring
        // our half-close). The closure below counts them so they appear
        // in trace logs but does not feed them through the codec.
        std::size_t discarded_bytes = 0;
        for (;;) {
            // Run one RX burst through the session. This advances the
            // state machine on every observed segment (ACK of our FIN,
            // peer's FIN, etc.) and emits ACKs as needed.
            auto pr = sess_.poll_rx(
                [&discarded_bytes](const std::uint8_t* /*chunk*/, std::uint16_t len) {
                    discarded_bytes += len;
                });
            if (!pr) {
                // Session-level error — typically Disconnected because
                // the peer RST'd or the session decided to tear down
                // (reorder overflow etc.). Drain semantics: consider
                // this a forced close, not a timeout. State should
                // already be Closed via the session's own bookkeeping.
                SPDLOG_LOGGER_WARN(log,
                    "DpdkTcpStream::drain: poll_rx err={} state={}",
                    pr.error().detail,
                    ::eph::net::tcp_state_name(sess_.state()));
                if (sess_.state() != ::eph::net::TcpState::Closed) {
                    sess_.reset();
                }
                return std::unexpected(pr.error());
            }

            // Terminal-success states: Closed (LastAck/RST-race path)
            // or TimeWait (FIN_WAIT_2 + peer FIN observed). Both mean
            // "peer FIN-ACK confirmed; orderly drain complete".
            const auto st = sess_.state();
            if (st == ::eph::net::TcpState::Closed ||
                st == ::eph::net::TcpState::TimeWait) {
                if (st == ::eph::net::TcpState::TimeWait) {
                    // Force the session out of TimeWait — the 2*MSL
                    // deferral is irrelevant for an orderly drain on
                    // a connection we will not reuse. reset() advances
                    // state to Closed *and* sends an RST; the RST is
                    // unusual but harmless because the peer has already
                    // FIN-closed and any future segment would be a
                    // delayed retransmit.
                    sess_.reset();
                }
                const std::uint64_t end_tsc = ::eph::utils::TSC::now();
                const auto elapsed_ns_opt =
                    ::eph::utils::TSC::delta_ns(start_tsc, end_tsc);
                SPDLOG_LOGGER_INFO(log,
                    "DpdkTcpStream::drain exit: success state={} "
                    "elapsed_ns={} discarded_payload_bytes={}",
                    ::eph::net::tcp_state_name(sess_.state()),
                    elapsed_ns_opt ? *elapsed_ns_opt : 0ull,
                    discarded_bytes);
                return {};
            }

            // Deadline check.
            bool expired = false;
            if (tsc_available) {
                expired = ::eph::utils::TSC::now() >= deadline_tsc;
            } else {
                expired = std::chrono::steady_clock::now() >= deadline_steady;
            }
            if (expired) {
                inc_<::eph::net::StreamMetric::kRxSessionResets>();
                const std::uint64_t end_tsc = ::eph::utils::TSC::now();
                const auto elapsed_ns_opt =
                    ::eph::utils::TSC::delta_ns(start_tsc, end_tsc);
                SPDLOG_LOGGER_WARN(log,
                    "DpdkTcpStream::drain: timeout state={} elapsed_ns={} "
                    "budget_ms={} — forcing reset",
                    ::eph::net::tcp_state_name(sess_.state()),
                    elapsed_ns_opt ? *elapsed_ns_opt : 0ull,
                    timeout.count());
                if (sess_.state() != ::eph::net::TcpState::Closed) {
                    sess_.reset();
                }
                return std::unexpected(core::ErrorInfo{
                    core::Error::Timeout,
                    "DpdkTcpStream::drain: peer FIN-ACK timeout"});
            }

            // Tick the session-level timers (delayed-ACK). Without this,
            // ACKs we owe the peer would only flush at the next outgoing
            // send — but in a drain there is no outgoing send, so the
            // peer never sees our ACK of its FIN and the handshake
            // stalls forever. The function reads TSC internally.
            sess_.flush_pending_ack();

            // Yield very briefly between bursts. We spin the lcore
            // here (kernel-bypass design); a short pause cap avoids
            // pegging the core 100% while waiting on a slow peer.
            // 1us is small compared to typical FIN-ACK RTT (~10us LAN,
            // ~1ms WAN) so we don't materially extend the drain.
            rte_pause();
        }
    }

    [[nodiscard]] bool is_attached() const noexcept {
        return attached_to_ != nullptr;
    }

    [[nodiscard]] ::eph::net::TcpState state() const noexcept {
        // The TCP session reports Established as soon as the 3-way handshake
        // completes — but with TLS/WS configured the connection is not yet
        // usable. Mask those legs to SynSent so callers (and the
        // ReconnectOrchestrator) only see Established once the WHOLE chain is
        // up, matching the kernel backend's coarse `state()` contract. The
        // TcpConnecting leg already reads SynSent from sess_; data-plane test
        // streams (hs_phase_ == Init) pass through unchanged.
        if (hs_phase_ == ::eph::net::HandshakePhase::TlsHandshaking ||
            hs_phase_ == ::eph::net::HandshakePhase::TlsConnected  ||
            hs_phase_ == ::eph::net::HandshakePhase::WsHandshaking) {
            return ::eph::net::TcpState::SynSent;
        }
        return sess_.state();
    }

    /// @brief Fine-grained connect/handshake phase (diagnostic). `state()`
    ///        stays SynSent for the whole pre-Established window.
    [[nodiscard]] ::eph::net::HandshakePhase handshake_phase() const noexcept {
        return hs_phase_;
    }

    /// @brief Block until the connect+handshake chain reaches Established, or
    ///        it fails / `timeout` elapses. Thin convenience over the
    ///        non-blocking core (drives poll_once_, which self-bursts the RX
    ///        queue) for simple / single-stream / test callers. Call it BEFORE
    ///        attaching to a Poller — once Poller-driven, the handshake is
    ///        advanced by process_burst_ instead and calling this would race.
    [[nodiscard]] std::expected<void, core::ErrorInfo>
    connect_blocking(std::chrono::milliseconds timeout) noexcept {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (hs_phase_ != ::eph::net::HandshakePhase::Established) {
            if (hs_phase_ == ::eph::net::HandshakePhase::Failed) {
                return std::unexpected(
                    hs_error_.code != core::Error::Ok
                        ? hs_error_
                        : core::ErrorInfo{core::Error::ConnectFailed,
                              "DpdkTcpStream::connect_blocking: handshake failed"});
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return std::unexpected(core::ErrorInfo{
                    core::Error::Timeout,
                    "DpdkTcpStream::connect_blocking: deadline exceeded"});
            }
            sess_.set_feed_only(false);
            drive_handshake_(nullptr, 0);  // self-burst step
        }
        return {};
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
    // ── Non-blocking connect/handshake state machine ────────────────────
    //
    // create() issues a non-blocking begin_connect() and returns in
    // hs_phase_ == TcpConnecting. poll_once_() (self-burst) or process_burst_()
    // (Poller-routed mbufs) call drive_handshake_() each cycle to walk
    // TCP → TLS → WS → Established without ever blocking the loop. DPDK has no
    // EPOLLOUT interest — every registered pollable is driven every cycle, so
    // (unlike kernel) there is no interest plumbing.

    /// @brief True while an active connect/handshake leg is in flight. Note
    ///        `Init` is NOT handshaking: data-plane test streams
    ///        (make_default_for_test_ + inject_state_for_testing) stay at Init
    ///        and must take the normal RX path, not the handshake driver.
    [[nodiscard]] bool handshaking_() const noexcept {
        switch (hs_phase_) {
            case ::eph::net::HandshakePhase::TcpConnecting:
            case ::eph::net::HandshakePhase::TcpConnected:
            case ::eph::net::HandshakePhase::TlsHandshaking:
            case ::eph::net::HandshakePhase::TlsConnected:
            case ::eph::net::HandshakePhase::WsHandshaking:
                return true;
            default:  // Init / Established / Failed
                return false;
        }
    }

    /// @brief Free a (possibly null) burst of mbufs — used on terminal/ignored
    ///        handshake paths that did not hand the mbufs to the session.
    void free_mbufs_(rte_mbuf** mbufs, uint16_t n) noexcept {
        if (mbufs) for (uint16_t i = 0; i < n; ++i) rte_pktmbuf_free(mbufs[i]);
    }

    /// @brief Extract in-order TCP payload from Poller-routed mbufs and stage
    ///        it in the session's feed buffer so the TLS BIO / WS sink read it
    ///        via poll_rx (rather than self-bursting the Poller-owned queue).
    [[nodiscard]] std::expected<std::uint16_t, core::ErrorInfo>
    feed_payload_(rte_mbuf** mbufs, uint16_t n) noexcept {
        return sess_.process_rx(mbufs, n,
            [this](const uint8_t* p, uint16_t len) { sess_.feed_rx(p, len); });
    }

    /// @brief Advance the connect/handshake machine by one step. `mbufs`==null
    ///        means self-burst (poll_once_/connect_blocking); non-null means
    ///        consume the Poller-routed mbufs (process_burst_).
    void drive_handshake_(rte_mbuf** mbufs, uint16_t n) noexcept {
        if (std::chrono::steady_clock::now() >= connect_deadline_) [[unlikely]] {
            fail_handshake_({core::Error::Timeout,
                "DpdkTcpStream: connect/handshake deadline exceeded"});
            free_mbufs_(mbufs, n);
            return;
        }
        switch (hs_phase_) {
            case ::eph::net::HandshakePhase::TcpConnecting: {
                auto r = mbufs ? sess_.connect_step(mbufs, n)
                               : sess_.connect_step();  // frees mbufs either way
                if (!r) { fail_handshake_(r.error()); return; }
                if (!*r) return;  // still waiting for SYN-ACK
                hs_phase_ = ::eph::net::HandshakePhase::TcpConnected;
                advance_after_tcp_();
                return;
            }
            case ::eph::net::HandshakePhase::TlsHandshaking: {
                if constexpr (EnableTls) {
                    if (mbufs) {
                        sess_.set_feed_only(true);
                        if (auto f = feed_payload_(mbufs, n); !f) {
                            fail_handshake_(f.error());
                            return;
                        }
                    } else {
                        sess_.set_feed_only(false);
                    }
                    auto r = tls_.handshake_step();
                    if (!r) { fail_handshake_(r.error()); return; }
                    if (!*r) return;  // still handshaking
                    if (tls_.was_resumed())
                        inc_<::eph::net::StreamMetric::kTlsResumeCount>();
                    else
                        inc_<::eph::net::StreamMetric::kTlsHandshakeCount>();
                    hs_phase_ = ::eph::net::HandshakePhase::TlsConnected;
                    advance_after_tls_();
                } else {
                    free_mbufs_(mbufs, n);
                }
                return;
            }
            case ::eph::net::HandshakePhase::WsHandshaking: {
                if (mbufs) {
                    sess_.set_feed_only(true);
                    if (auto f = feed_payload_(mbufs, n); !f) {
                        fail_handshake_(f.error());
                        return;
                    }
                } else {
                    sess_.set_feed_only(false);
                }
                auto r = ws_driver_->step(*ws_sink_);
                if (!r) { fail_handshake_(r.error()); return; }
                if (!*r) return;  // pending
                finalize_ws_();
                become_established_();
                return;
            }
            default:
                free_mbufs_(mbufs, n);
                return;
        }
    }

    /// @brief TCP up → begin TLS, else enter WS, else become Established.
    void advance_after_tcp_() noexcept {
        if constexpr (EnableTls) {
            connect_deadline_ = std::chrono::steady_clock::now()
                              + cfg_.tls.handshake_timeout;
            auto b = tls_.begin_handshake(sess_, cfg_.tls);
            if (!b) { fail_handshake_(b.error()); return; }
            hs_phase_ = ::eph::net::HandshakePhase::TlsHandshaking;
        } else {
            advance_after_tls_();
        }
    }

    /// @brief TLS up (or skipped) → enter the WS leg, else become Established.
    void advance_after_tls_() noexcept {
        if (!cfg_.ws.path.empty()) enter_ws_();
        else                       become_established_();
    }

    /// @brief Build the WS Upgrade driver + sink and enter WsHandshaking.
    void enter_ws_() noexcept {
        connect_deadline_ = std::chrono::steady_clock::now() + cfg_.ws.timeout;
        // Host header: explicit ws.host, else TLS SNI, else dst IP:port.
        ws_host_storage_.clear();
        if (!cfg_.ws.host.empty()) {
            ws_host_storage_ = cfg_.ws.host;
        } else if constexpr (EnableTls) {
            if (!cfg_.tls.hostname.empty()) ws_host_storage_ = cfg_.tls.hostname;
        }
        if (ws_host_storage_.empty()) {
            const auto& t = cfg_.dpdk.wire.tuple;
            auto ip_buf = ::eph::dpdk::net::format_ipv4(t.dst_ip);
            ws_host_storage_ = std::string(ip_buf.data()) + ":" +
                               std::to_string(t.dst_port);
        }
        ws_deflate_ = ::eph::net::detail::WsHandshakeDeflate{
            .request                    = cfg_.ws.permessage_deflate,
            .negotiated                 = false,
            .server_no_context_takeover = false,
        };
        auto d = ::eph::net::detail::WsHandshakeDriver::create(
            ws_host_storage_, cfg_.ws.path,
            std::span<const ::eph::net::HttpHeader>(cfg_.ws.extra_headers),
            &ws_deflate_);
        if (!d) { fail_handshake_(d.error()); return; }
        ws_driver_ = std::make_unique<::eph::net::detail::WsHandshakeDriver>(
            std::move(*d));
        if constexpr (EnableTls) ws_sink_.emplace(&sess_, &tls_);
        else                     ws_sink_.emplace(&sess_);
        hs_phase_ = ::eph::net::HandshakePhase::WsHandshaking;
    }

    /// @brief WS handshake complete: apply deflate negotiation + seed over-read.
    void finalize_ws_() noexcept {
        if (ws_deflate_.negotiated) {
            ws_deflate_active_ = true;
            if constexpr (requires(C& c) { c.enable_permessage_deflate(false); }) {
                codec_.enable_permessage_deflate(
                    ws_deflate_.server_no_context_takeover);
            } else {
                SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
                    "DpdkTcpStream: server accepted permessage-deflate but the "
                    "configured codec does not implement enable_permessage_deflate");
            }
        }
        auto lo = ws_driver_->leftover();
        if (!lo.empty()) {
            if (!reasm_.append(lo.data(), lo.size())) {
                fail_handshake_({core::Error::BufferFull,
                    "DpdkTcpStream: ws leftover exceeds reasm capacity"});
                return;
            }
        }
        ws_driver_.reset();
        ws_sink_.reset();
    }

    /// @brief Promote to Established (state() now passes sess_.state() through).
    void become_established_() noexcept {
        hs_phase_ = ::eph::net::HandshakePhase::Established;
        sess_.set_feed_only(false);
        SPDLOG_LOGGER_INFO(detail::tcp_stream_logger(),
            "DpdkTcpStream: connection established "
            "src=0x{:08x}:{} -> dst=0x{:08x}:{}",
            cfg_.dpdk.wire.tuple.src_ip, cfg_.dpdk.wire.tuple.src_port,
            cfg_.dpdk.wire.tuple.dst_ip, cfg_.dpdk.wire.tuple.dst_port);
    }

    /// @brief Terminal handshake failure: stash the specific error, latch
    ///        Failed, and RST the session so state() reports Closed and the
    ///        reconnect loop takes over.
    void fail_handshake_(core::ErrorInfo err) noexcept {
        SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
            "DpdkTcpStream: handshake failed at phase={}: {}",
            ::eph::net::handshake_phase_name(hs_phase_), err.detail);
        hs_error_ = err;
        hs_phase_ = ::eph::net::HandshakePhase::Failed;
        sess_.reset();
        ws_driver_.reset();
        ws_sink_.reset();
    }

    std::size_t poll_once_() noexcept {
        // Connect/handshake phase: drive the non-blocking state machine one
        // self-burst step. No application frames flow until the chain is up.
        if (handshaking_()) [[unlikely]] {
            sess_.set_feed_only(false);
            drive_handshake_(nullptr, 0);
            return 0;
        }
        // Keepalive tick lives on the Poller's per-cycle sweep (see
        // DpdkPoller::poll), driven through on_poll_tick_. Users who
        // drive poll_once_ directly (single-stream, Poller-less)
        // should call `on_poll_tick_(TSC::now())` themselves once per
        // cycle; see `docs/poller-guide.md`.
        if (!sess_.is_established()) return 0;
        if (reasm_overflowed_) return 0;
        // Fail-fast on TLS desync — mirrors process_burst_ above. The
        // reconnect policy replaces this stream; until then nothing
        // flowing through poll_once_ can be trusted.
        if constexpr (EnableTls) {
            if (tls_corrupt_) return 0;
        }
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
            handle_rx_session_error_(
                "DpdkTcpStream::poll_once_", r.error());
            return 0;
        }
        if (reasm_overflowed_) return 0;
        return drain_codec_();
    }

    /// @brief Pollable's is_attached hook — identical to the user-facing
    ///        `is_attached()` above. Two names exist so `DpdkTcpStream`
    ///        simultaneously satisfies `eph::net::Stream` (needs
    ///        `is_attached`) and `eph::net::Pollable` (needs
    ///        `is_attached_`). Forwards to the public one to keep the
    ///        "is this Pollable attached" predicate single-sourced.
    [[nodiscard]] bool is_attached_() const noexcept {
        return is_attached();
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

    // ── TLS session resumption ───────────────────────────────────────────
    //
    // Symmetric with `KernelTcpStream::tls_resumption_ticket()` — see
    // `eph/net/detail/tls_constants.hpp` for the lifecycle. DPDK backend
    // captures tickets identically through the underlying `TlsSession`.
    //
    // Returns empty / false when EnableTls=false.

    /// Move-out the captured server NewSessionTicket bytes. DER-encoded.
    [[nodiscard]] std::vector<uint8_t> tls_resumption_ticket() noexcept {
        if constexpr (EnableTls) {
            return tls_.take_resumption_ticket();
        } else {
            return {};
        }
    }

    /// @brief Post-create stream state snapshot.
    /// @see eph::net::StreamSnapshot for field semantics.
    [[nodiscard]] ::eph::net::StreamSnapshot snapshot() const noexcept {
        ::eph::net::StreamSnapshot s{};
        const auto& t = cfg_.dpdk.wire.tuple;
        s.endpoint.src_ip   = t.src_ip;
        s.endpoint.src_port = t.src_port;
        s.endpoint.dst_ip   = t.dst_ip;
        s.endpoint.dst_port = t.dst_port;
        s.endpoint.src_port_rewritten = src_port_rewritten_;

        s.tcp.enabled             = true;
        s.tcp.recv_window         = static_cast<uint16_t>(
            cfg_.dpdk.wire.recv_window);
        s.tcp.local_mss           = cfg_.dpdk.wire.mss;
        s.tcp.effective_mss       = sess_.effective_mss();
        s.tcp.peer_mss_negotiated = sess_.peer_mss_negotiated();
        // peer_mss is the *raw* SYN-ACK advertisement (per
        // StreamSnapshot's contract). Reporting effective_mss here would
        // lie after an ICMP Frag Needed shrink, where effective < peer's
        // actual advertisement. TcpSession::peer_mss() preserves the raw
        // value through PMTU shrinks; it returns 0 when the peer omitted
        // the option, which is exactly what the snapshot field documents.
        s.tcp.peer_mss            = sess_.peer_mss();
        s.tcp.icmp_pmtu_shrunk    =
            sess_.tcp_stats().icmp_frag_needed_received > 0;

        s.keepalive.active   = !cfg_.keepalive.empty();
        s.keepalive.interval = cfg_.keepalive.interval;
        s.keepalive.probes   = cfg_.keepalive.probes;

        if constexpr (EnableTls) {
            s.tls.enabled       = true;
            s.tls.was_resumed   = tls_.was_resumed();
            s.tls.send_desynced = tls_corrupt_;
            s.tls.sni           = cfg_.tls.hostname;
        }

        s.ws.enabled                   = !cfg_.ws.path.empty();
        s.ws.path                      = cfg_.ws.path;
        s.ws.host                      = cfg_.ws.host;
        s.ws.permessage_deflate_active = ws_deflate_active_;

        s.dpdk.rx_queue                 = cfg_.dpdk.wire.rx_queue_id;
        s.dpdk.tx_queue                 = cfg_.dpdk.wire.tx_queue_id;
        s.dpdk.pool_lcore_hint_resolved = cfg_.dpdk.pool_lcore_hint;
        if (flow_rule_ && flow_rule_->valid()) {
            s.dpdk.flow_rule_handle = flow_rule_->opaque_handle_id();
        }
        return s;
    }

    [[nodiscard]] std::uint64_t metric(::eph::net::StreamMetric m) const noexcept {
        using SM = ::eph::net::StreamMetric;
        // Defensive bounds check: an out-of-range `m` (stale enum from
        // ABI drift, or a reinterpret_cast into the enum) must not
        // index past counters_ — return 0 instead of UB. The sentinel
        // `kCount` itself is also out of range.
        if (static_cast<std::size_t>(m) >=
            static_cast<std::size_t>(SM::kCount)) {
            return 0;
        }
        // kRxBadChecksum is the deprecated-in-place aggregate of the two
        // split counters (TD-1). Compute on-demand so we never maintain
        // a third atomic for the same event. Invariant holds regardless
        // of which backend observes the bad mbuf.
        if (m == SM::kRxBadChecksum) {
            return counters_[static_cast<std::size_t>(SM::kRxIpChecksumBad)]
                       .v.load(std::memory_order_relaxed) +
                   counters_[static_cast<std::size_t>(SM::kRxL4ChecksumBad)]
                       .v.load(std::memory_order_relaxed);
        }
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
            case SM::kPacketsDropped:
                return sess_.tcp_stats().packets_dropped;
            case SM::kFragmentRejected:
                return sess_.tcp_stats().fragment_rejected;
            case SM::kTcpDupSegments:
                return sess_.tcp_stats().dup_segments;
            case SM::kTcpKeepaliveProbesSent:
                return sess_.tcp_stats().keepalive_probes_sent;
            case SM::kTcpKeepaliveSendFailures:
                return sess_.tcp_stats().keepalive_send_failures;
            case SM::kTcpMssNegotiationApplied:
                return sess_.tcp_stats().mss_negotiations_applied;
            case SM::kIcmpFragNeededReceived:
                return sess_.tcp_stats().icmp_frag_needed_received;
            case SM::kNumericalAnomaliesDetected:
                // Lazy read from TcpSession::Stats — incremented at any
                // session-level guard that catches a NaN / overflow /
                // saturating fallback (e.g. uncalibrated TSC keepalive
                // interval). T3.5 from the 2026-05-05 action list.
                return sess_.tcp_stats().numerical_anomalies;
            default:
                break;
        }
        // WS permessage-deflate counters live on the codec instance,
        // not on this stream — pull them lazily so that codecs that
        // don't implement the contract (RawStreamCodec, LengthPrefix,
        // etc.) leave the metric at 0 without any per-byte cost.
        if (m == SM::kWsDeflateBytesIn) {
            if constexpr (requires (const C& c) { c.ws_deflate_bytes_in(); }) {
                return codec_.ws_deflate_bytes_in();
            } else {
                return 0;
            }
        }
        if (m == SM::kWsDeflateBytesOut) {
            if constexpr (requires (const C& c) { c.ws_deflate_bytes_out(); }) {
                return codec_.ws_deflate_bytes_out();
            } else {
                return 0;
            }
        }
        return counters_[static_cast<std::size_t>(m)]
            .v.load(std::memory_order_relaxed);
    }

    // ── Poller-facing friend hooks ───────────────────────────────────────

    /// @brief Invoked by `DpdkPoller::add` after the routing entry is
    ///        built. Establishes back-pointer so destructor can auto-detach.
    void notify_attached_(DpdkPoller<void>* p) noexcept { attached_to_ = p; }

    /// @brief TD-2 injection hook. `create_and_attach` copies the
    /// effective Platform strict flag into the stream so the hot path
    /// can branch on a cheap stack-local `bool`. Not for user code.
    void set_strict_rx_checksum_(bool v) noexcept { strict_rx_cksum_ = v; }

    /// @brief T1.1+T1.2 post-burst classification helper. Called from
    /// `send()` at each return point AFTER the underlying
    /// `sess_.send()` has completed (success, partial, or error).
    /// Returns `nullopt` when the daemon is still alive — caller
    /// returns its normal value. Returns a populated `ErrorInfo`
    /// (and stamps `last_daemon_disconnected_detail()`) when the
    /// daemon has died during this send — caller should propagate
    /// `DaemonDisconnected` upstream.
    ///
    /// `presumed_status` is what the caller infers from the burst
    /// result:
    ///   - All bytes acknowledged sent → `InFlightStatus::Sent`
    ///   - Partial bytes (off > 0 < total) → `InFlightStatus::Uncertain`
    ///   - Zero bytes through → `InFlightStatus::Unsent`
    /// Caller is responsible for picking the right one based on its
    /// branch.
    [[nodiscard]] std::optional<core::ErrorInfo>
    check_post_burst_(::eph::net::dpdk::detail::InFlightStatus presumed_status,
                      std::size_t bytes_observed,
                      std::size_t bytes_confirmed,
                      const char* phase) noexcept {
        if (platform_ == nullptr) [[likely]] return std::nullopt;
        if (platform_->is_alive()) [[likely]] return std::nullopt;
        ::eph::net::dpdk::detail::set_daemon_disconnected_detail(
            presumed_status, bytes_observed, bytes_confirmed, phase);
        return core::ErrorInfo{
            core::Error::DaemonDisconnected,
            "post-burst is_alive() observed daemon disconnected — "
            "see last_daemon_disconnected_detail() for InFlightStatus"};
    }

    /// @brief Invoked by `DpdkPoller::remove` and `~DpdkPoller`.
    void notify_detached_() noexcept { attached_to_ = nullptr; }

    /// @brief Supplies the registered 5-tuple to the Poller at add-time.
    ///        `*proto` is set to `kIpProtoTcp` so the poller routing table
    ///        can coexist with UDP Pollables sharing the same 4-tuple.
    void tuple_for_poller_(uint32_t* src_ip, uint32_t* dst_ip,
                            uint16_t* src_port, uint16_t* dst_port,
                            uint8_t*  proto) noexcept {
        const auto& t = cfg_.dpdk.wire.tuple;
        *src_ip   = t.src_ip;
        *dst_ip   = t.dst_ip;
        *src_port = t.src_port;
        *dst_port = t.dst_port;
        *proto    = eph::dpdk::net::kIpProtoTcp;
    }

    /// @brief Per-poll-cycle tick — drives the TCP session's keepalive
    ///        probe emission. Called by DpdkPoller::poll() once per
    ///        cycle for every registered entry, regardless of whether
    ///        the burst actually delivered mbufs, so idle connections
    ///        still get their probes fired.
    void on_poll_tick_(uint64_t tsc) noexcept {
        sess_.tick_keepalive(tsc);
    }

    /// @brief ICMP Frag Needed callback registered with Platform via
    ///        `register_icmp_target`. Platform has already matched the
    ///        embedded 4-tuple to this stream — we just forward the
    ///        MTU down to the session. noexcept so a mis-dispatch can
    ///        never unwind through the Poller's hot path.
    static void on_icmp_mtu_thunk_(void* user, uint16_t next_hop_mtu) noexcept {
        auto* self = static_cast<DpdkTcpStream*>(user);
        if (self == nullptr) return;
        self->sess_.on_icmp_frag_needed(next_hop_mtu);
    }

    /// @brief Hot-path burst dispatch entry point called by DpdkPoller.
    ///        Feeds the mbuf(s) into the TCP session, then drains the
    ///        reassembly buffer through the codec.
    ///
    /// @note TD-3 (lucky-giggling-kahan review) closed below: the
    /// RX checksum gate at the top of this method mirrors
    /// DpdkUdpSocket::process_burst_'s UDP-side handling. Gated by
    /// PlatformConfig::enable_rx_checksum_offload; when opt-in is off
    /// the NIC never stamps BAD and the gate is a no-op branch.
    void process_burst_(rte_mbuf** mbufs, uint16_t n,
                         uint64_t rx_tsc) noexcept {
        // Hot-path RX checksum drop — symmetric to DpdkUdpSocket. Runs
        // BEFORE every other gate (TLS desync, session state, reasm) so
        // bad-cksum packets never touch TCP session state or AEAD. We
        // compact survivors in place to preserve the mbufs[]→sess_.process_rx
        // burst contract. Two modes controlled by `strict_rx_cksum_`
        // (set from Platform::strict_rx_checksum() at create_and_attach —
        // see TD-2):
        //   false (default, best-effort):
        //     drop iff (olf & IP_CKSUM_BAD) | (olf & L4_CKSUM_BAD) set.
        //   true (strict):
        //     drop iff (olf & *_CKSUM_MASK) != *_CKSUM_GOOD per layer.
        // Drop attribution split per layer (TD-1). Aggregate
        // kRxBadChecksum computed lazily as ip_bad + l4_bad.
        constexpr uint64_t kRxIpCksumBad  =
            static_cast<uint64_t>(RTE_MBUF_F_RX_IP_CKSUM_BAD);
        constexpr uint64_t kRxL4CksumBad  =
            static_cast<uint64_t>(RTE_MBUF_F_RX_L4_CKSUM_BAD);
        constexpr uint64_t kRxIpCksumMask =
            static_cast<uint64_t>(RTE_MBUF_F_RX_IP_CKSUM_MASK);
        constexpr uint64_t kRxL4CksumMask =
            static_cast<uint64_t>(RTE_MBUF_F_RX_L4_CKSUM_MASK);
        constexpr uint64_t kRxIpCksumGood =
            static_cast<uint64_t>(RTE_MBUF_F_RX_IP_CKSUM_GOOD);
        constexpr uint64_t kRxL4CksumGood =
            static_cast<uint64_t>(RTE_MBUF_F_RX_L4_CKSUM_GOOD);
        const bool strict = strict_rx_cksum_;  // stack-local for codegen
        uint16_t write = 0;
        for (uint16_t read = 0; read < n; ++read) {
            const uint64_t olf = mbufs[read]->ol_flags;
            // TD-6 precision: non-strict must test `(olf & MASK) == BAD`,
            // NOT `(olf & BAD_bit) != 0`. DPDK encodes NONE as
            // `BAD_bit | GOOD_bit`, so the naive bit test also matches
            // NONE. Using the mask == BAD equality keeps non-strict
            // drop exactly BAD. Strict uses `!= GOOD` to drop every
            // non-GOOD value (UNKNOWN, BAD, NONE).
            const bool ip_bad = strict
                ? ((olf & kRxIpCksumMask) != kRxIpCksumGood)
                : ((olf & kRxIpCksumMask) == kRxIpCksumBad);
            const bool l4_bad = strict
                ? ((olf & kRxL4CksumMask) != kRxL4CksumGood)
                : ((olf & kRxL4CksumMask) == kRxL4CksumBad);
            if (ip_bad || l4_bad) [[unlikely]] {
                if (ip_bad) inc_<::eph::net::StreamMetric::kRxIpChecksumBad>();
                if (l4_bad) inc_<::eph::net::StreamMetric::kRxL4ChecksumBad>();
                SPDLOG_LOGGER_TRACE(detail::tcp_stream_logger(),
                    "process_burst_: drop bad-checksum mbuf ol_flags={:#018x}"
                    " (strict={})", olf, strict);
                rte_pktmbuf_free(mbufs[read]);
                continue;
            }
            if (write != read) mbufs[write] = mbufs[read];
            ++write;
        }
        n = write;
        if (n == 0) return;

        // Fail-fast on TLS desync (see send() above). We cannot trust the
        // decrypt path once the peer's seq diverged from ours: even inbound
        // records may arrive OK, but any auto-response (WS pong, close-ack)
        // routed through send() would be silently dropped. Drop incoming
        // mbufs until the reconnect loop tears us down.
        if constexpr (EnableTls) {
            if (tls_corrupt_) {
                for (uint16_t i = 0; i < n; ++i) rte_pktmbuf_free(mbufs[i]);
                return;
            }
        }
        // Connect/handshake phase: drive the state machine from the
        // Poller-routed mbufs (the Poller owns the RX queue, so the session
        // cannot self-burst). drive_handshake_ consumes/frees the mbufs.
        if (handshaking_()) [[unlikely]] {
            sess_.set_last_rx_burst_tsc(rx_tsc);
            drive_handshake_(mbufs, n);
            return;
        }
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
            handle_rx_session_error_(
                "DpdkTcpStream::process_burst_", r.error());
            return;
        }
        if (reasm_overflowed_) return;
        sess_.flush_pending_ack();
        (void)drain_codec_();
    }

#ifdef EPH_DPDK_TCP_STREAM_TEST_HOOKS
    /// @brief Test-only: forces the TLS desync latch on so the fail-fast
    ///        path in send() / process_burst_ / poll_once_ can be exercised
    ///        without driving a real AEAD partial-send. Guarded by the
    ///        `EPH_DPDK_TCP_STREAM_TEST_HOOKS` macro so production builds
    ///        do not expose the setter. Paired metric bump mirrors the
    ///        real-path bookkeeping in `send()`.
    void force_tls_desync_for_test_() noexcept {
        if constexpr (EnableTls) {
            if (!tls_corrupt_) {
                tls_corrupt_ = true;
                inc_<::eph::net::StreamMetric::kTlsSendDesyncs>();
            }
        }
    }

    /// @brief Test-only: bypass `create()`'s config validation + live
    ///        connect + TLS handshake. Returns a default-constructed stream
    ///        whose session is NOT established. Only useful for exercising
    ///        input-guard behaviour (desync latch, attach preconditions)
    ///        that short-circuits before touching the session. Guarded by
    ///        `EPH_DPDK_TCP_STREAM_TEST_HOOKS`.
    ///
    /// `[[nodiscard]]` because the returned stream owns the test fixture's
    /// lifetime (poller pin, TLS state); discarding it immediately tears
    /// the session down and the rest of the test silently no-ops on the
    /// destroyed object via dangling reference. Surface the typo at
    /// compile time.
    [[nodiscard]] static std::unique_ptr<DpdkTcpStream>
    make_default_for_test_() {
        StreamConfig cfg{};
        // Minimally fill the legacy TcpConfig so the session constructor
        // doesn't trip internal asserts — validity of the values is
        // irrelevant, we never drive the session to Established.
        cfg.dpdk.wire.tuple.src_ip   = 0x0A000001;
        cfg.dpdk.wire.tuple.dst_ip   = 0x0A000002;
        cfg.dpdk.wire.tuple.src_port = 12345;
        cfg.dpdk.wire.tuple.dst_port = 443;
        cfg.dpdk.wire.mss            = 1460;
        cfg.dpdk.wire.recv_window    = 65535;
        return std::unique_ptr<DpdkTcpStream>(
            new DpdkTcpStream(std::move(cfg)));
    }

    /// @brief Test-only: replay the exact branch `process_burst_` /
    ///        `poll_once_` runs when `TcpSession::process_rx` / `poll_rx`
    ///        returns an error. Returns `true` iff `state()` ends as
    ///        `Closed` after the helper runs.
    bool simulate_rx_session_error_for_test_() noexcept {
        handle_rx_session_error_(
            "DpdkTcpStream::simulate_rx_session_error_for_test_",
            core::ErrorInfo{core::Error::Disconnected,
                            "simulated rx session error"});
        return sess_.state() == ::eph::net::TcpState::Closed;
    }
#endif // EPH_DPDK_TCP_STREAM_TEST_HOOKS

private:
    explicit DpdkTcpStream(StreamConfig cfg)
        : cfg_(std::move(cfg))
        , sess_(cfg_.dpdk.wire, cfg_.dpdk.pool)
        , reasm_(cfg_.reasm_capacity > 0 ? cfg_.reasm_capacity : 256 * 1024) {}

    /// @brief Translate an RX-side session error into the stream-layer
    ///        reaction: log + (if not already Closed) force the session
    ///        into Closed and bump `kRxSessionResets` so the app's
    ///        reconnect policy can observe the death via `state()` and
    ///        rebuild.  Centralizing the two call sites (process_burst_,
    ///        poll_once_) keeps any future policy evolution (e.g. burst
    ///        suppression / rate-limited WARN) in one place.
    ///
    ///        The state guard avoids both an unnecessary outbound RST
    ///        (on the peer-RST path, where `process_rx` already set
    ///        state_ = Closed) and a double-count in the metric; the
    ///        counter's semantics — "stream layer proactively reset
    ///        the session from the RX side" — stay clean.
    void handle_rx_session_error_(std::string_view site,
                                   const core::ErrorInfo& err) noexcept {
        SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
            "{}: {} — forcing reset", site, err.detail);
        if (sess_.state() != ::eph::net::TcpState::Closed) {
            sess_.reset();
            inc_<::eph::net::StreamMetric::kRxSessionResets>();
        }
    }

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
            bool        codec_err_latched = false;
            // Captures the original codec error.detail (const char* into
            // codec-owned static storage — no allocation) so the post-
            // process_records_in_place ERROR log can report *which* codec
            // violation forced the reset, rather than the bare "codec err
            // latched". Errno-enrichment applied to the codec-error path.
            const char* codec_err_detail = nullptr;
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

                    detail::MbufView view(feed_ptr, feed_len);
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
                            codec_err_detail  = dr.error().detail;
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
                        "({}) — forcing session reset",
                        codec_err_detail ? codec_err_detail : "no detail");
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
                                       before);

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
    /// @brief Set once the TLS write path has encrypted a record whose
    ///        ciphertext did not fully reach the TCP TX queue. Once tripped,
    ///        every send() and process_burst_ / poll_once_ short-circuits
    ///        so the reconnect loop can take over. Only meaningful when
    ///        `EnableTls=true`; the false-branch member cost is one byte.
    ///        See the detailed rationale in `send()` and the metric
    ///        `kTlsSendDesyncs` in `eph/net/stream_metrics.hpp`.
    bool                                    tls_corrupt_{false};
    /// @brief Snapshot bookkeeping: true iff RSS+pin reverse-pick mutated
    ///        the caller's pre-chosen src_port. Set by `create_and_attach`
    ///        when the original (non-zero) src_port differs from the
    ///        post-pick value. Surfaced via `snapshot().endpoint.src_port_rewritten`.
    bool                                    src_port_rewritten_{false};
    /// @brief Snapshot bookkeeping: true once the WS handshake confirmed
    ///        permessage-deflate negotiation (regardless of whether the
    ///        codec can actually inflate). Surfaced via
    ///        `snapshot().ws.permessage_deflate_active`.
    bool                                    ws_deflate_active_{false};
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

    // ── Non-blocking connect/handshake state (live only until Established) ──
    /// @brief Which leg of the TCP→TLS→WS connect chain is in flight. Drives
    ///        poll_once_() / process_burst_() dispatch; state() reports the
    ///        coarse TcpState (SynSent until Established).
    ::eph::net::HandshakePhase              hs_phase_{::eph::net::HandshakePhase::Init};
    /// @brief Per-leg wall-clock deadline (TCP=connect_timeout, then reset to
    ///        tls.handshake_timeout / ws.timeout as each leg begins).
    std::chrono::steady_clock::time_point   connect_deadline_{};
    /// @brief Specific error of the failed leg, returned by connect_blocking().
    ::eph::core::ErrorInfo                  hs_error_{::eph::core::Error::Ok, ""};
    /// @brief Resumable WS Upgrade driver — non-null only during WsHandshaking.
    std::unique_ptr<::eph::net::detail::WsHandshakeDriver> ws_driver_{};
    /// @brief Persistent WS byte sink across poll cycles (carries TLS decrypt
    ///        staging). Constructed in enter_ws_() pointing at sess_ / tls_.
    std::conditional_t<EnableTls,
                       std::optional<detail::TlsDpdkWsSink<>>,
                       std::optional<detail::PlainDpdkWsSink<>>> ws_sink_{};
    /// @brief permessage-deflate negotiation state for the WS handshake.
    ::eph::net::detail::WsHandshakeDeflate  ws_deflate_{};
    /// @brief Backing storage for the WS `Host:` header (outlives the driver).
    std::string                             ws_host_storage_{};
    /// @brief TD-2 strict RX checksum mode. Off by default.
    /// `create_and_attach` sets it from `Platform::strict_rx_checksum()`;
    /// plain `create()` leaves it at the safe best-effort default.
    bool                                    strict_rx_cksum_{false};
    DpdkPoller<void>*                       attached_to_{nullptr};
    /// @brief Platform back-pointer for `is_alive()` checks on the
    /// burst path (T1.1+T1.2 wire-up, 2026-05-05). Populated by
    /// `create_and_attach`; remains `nullptr` for streams created via
    /// the bare `create()` path. Lifetime: the Platform must outlive
    /// every Stream attached to it (the same contract that the Poller
    /// already imposes; sharing the back-pointer doesn't extend it).
    /// Naming convention `_` follows the rest of the private members.
    ::eph::dpdk::Platform*                  platform_{nullptr};
    /// @brief RAII handle for the rte_flow rule installed by
    /// `create_and_attach` in FlowDirector mode (engaged only on NICs
    /// where `Platform::dispatch_mode() == FlowDirector`). When the
    /// stream is destroyed the rule is automatically removed from the
    /// NIC via `~FlowRule` → `rte_flow_destroy`. Empty in Software /
    /// RssPartitioned mode.
    std::optional<::eph::net::dpdk::FlowRule> flow_rule_{};

    /// @brief RAII handle for the ICMP Frag Needed registration on
    ///        Platform. Engaged only when `create_and_attach` succeeds.
    ///        Destructor auto-unregisters, so an exception / rollback /
    ///        normal teardown all leave Platform's registry clean.
    std::optional<::eph::dpdk::Platform::IcmpTargetHandle> icmp_reg_{};

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
