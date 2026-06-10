#pragma once

/// @file tls_state.hpp
/// Real TLS 1.3 state for `KernelTcpStream<C, true>`.
///
/// This header builds a thin adapter (`ByteSocketTcpAdapter`) that makes
/// `detail::ByteSocket` look like the `eph::net::TcpTransport` concept,
/// then drives the existing `eph::net::TlsSession<>` for the handshake
/// and `eph::net::TlsRecordCrypto` for the data plane. The adapter is
/// local to this file (TU-private).
///
/// Architecture:
///
///     KernelTcpStream<C, true>::create()
///        ├── ByteSocket::connect()                // TCP 3-way handshake
///        ├── ByteSocketTcpAdapter wraps the socket
///        ├── TlsSession<adapter>::create()        // aws-lc
///        ├── session.handshake()                  // blocking, control thread
///        ├── session.extract_hot_state()          // pulls TLS 1.3 keys
///        └── TlsRecordCrypto::create(hot_state)   // hot-path AEAD
///
///     KernelTcpStream<C, true>::poll_once_()
///        ├── ByteSocket::recv() into reasm buffer
///        ├── parse TLS records from reasm buffer
///        ├── decrypt each record (TlsDecryptor) into a plaintext staging
///        │   buffer (kernel side has no mbuf to mutate; the staging
///        │   buffer is owned by TlsState)
///        └── feed plaintext to codec.decode()
///
///     KernelTcpStream<C, true>::send(data)
///        ├── encrypt via TlsEncryptor into a record buffer
///        └── ByteSocket::send(record_buffer)
///
/// Zero-copy story for kernel: there is none. The kernel-side TCP
/// receive buffer is owned by the kernel and is copied into user space
/// by recv(); a TLS plaintext buffer must be distinct from the
/// ciphertext anyway. The zero-copy story is the DPDK path
/// (`DpdkTcpStream::process_burst_`), where the mbuf hands the codec a
/// writable pointer into the same bytes the NIC DMA'd into.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "eph/core/detail/logger.hpp"
#include "eph/core/error.hpp"
#include "eph/net/kernel/detail/byte_socket.hpp"
#include "eph/net/tcp_state.hpp"
#include "eph/net/detail/tls_constants.hpp"
#include "eph/net/detail/tls_record.hpp"
#include "eph/net/detail/tls_session.hpp"

namespace eph::net::kernel::detail {

/// @brief Lazily-initialized logger for the kernel-backend TLS state.
///
/// Added in batch3-round2: the previous implementation had zero logging,
/// making every TLS handshake / record-plane failure a silent "TLS error"
/// with no breadcrumb about WHICH sub-step (session create, handshake,
/// extract_hot_state, key length, AEAD create, decrypt, record-header
/// parse) tripped. See review-audit-net-batch3-round2 HIGH-1.
inline spdlog::logger* tls_state_logger() {
    static auto* l = ::eph::core::detail::make_logger("net.kernel.tls_state");
    return l;
}

// ---------------------------------------------------------------------------
// ByteSocketTcpAdapter — make ByteSocket look like a legacy TcpTransport.
// ---------------------------------------------------------------------------
//
// The `eph::net::TlsSession<TcpImpl>` template requires the wrapped type
// to satisfy the TcpTransport concept (string-typed errors, `mss()`,
// `is_established()`, `last_rx_burst_tsc()`, etc.). `ByteSocket` returns
// `eph::core::ErrorInfo` and has no transport state, so a thin adapter is
// needed for the duration of the handshake.
//
// The adapter holds a non-owning pointer to a `ByteSocket` plus a small
// scratch buffer it uses to satisfy `poll_rx`'s callback contract. It is
// only used during handshake; once `extract_hot_state()` succeeds the
// adapter is destroyed and the bare ByteSocket resumes ownership of the
// fd for the data plane.

class ByteSocketTcpAdapter {
public:
    explicit ByteSocketTcpAdapter(ByteSocket* sock) noexcept : sock_(sock) {}

    // Connection lifecycle — the TCP handshake already happened above us.
    // TlsSession only calls `is_established()` and never `connect()`/`close()`.
    [[nodiscard]] std::expected<void, std::string>
    connect(std::chrono::milliseconds /*timeout*/) noexcept {
        return std::unexpected(std::string{"adapter: connect() not supported"});
    }
    [[nodiscard]] std::expected<void, std::string>
    close() noexcept { return {}; }
    void reset() noexcept {}

    // Data transfer — forward to the byte socket. `handshake_bytes` holds
    // TLS protocol bytes (ClientHello / Finished / …) that aws-lc's BIO is
    // pushing to the wire; from this adapter's perspective they're just
    // bytes to ship.
    [[nodiscard]] std::expected<std::size_t, std::string>
    send(const void* handshake_bytes, std::size_t len) noexcept {
        std::span<const uint8_t> view(
            static_cast<const uint8_t*>(handshake_bytes), len);
        auto r = sock_->send(view);
        if (!r) return std::unexpected(std::string{r.error().detail});
        return *r;
    }

    /// Poll RX once: read up to 16K from the socket and invoke `cb` for the
    /// handshake bytes we got. The TlsSession's BIO uses this callback to
    /// feed ClientHello/ServerHello/... into the handshake state machine.
    /// At this adapter layer the bytes are raw TLS handshake bytes (not
    /// yet encrypted app data); aws-lc's BIO pipeline peels them.
    ///
    /// Return type matches the legacy `TcpTransport::poll_rx` contract:
    /// `std::expected<uint16_t, std::string>` where the value is the byte
    /// count delivered to `cb` this call (0 on WouldBlock).
    template <class Cb>
    [[nodiscard]] std::expected<std::uint16_t, std::string>
    poll_rx(Cb&& cb) noexcept {
        uint8_t handshake_scratch[16 * 1024];
        auto r = sock_->recv(handshake_scratch, sizeof(handshake_scratch));
        if (!r) {
            const auto& err = r.error();
            // WouldBlock is not an error from poll_rx's perspective — return 0.
            if (err.code == ::eph::core::Error::WouldBlock) {
                return std::uint16_t{0};
            }
            return std::unexpected(std::string{err.detail});
        }
        if (*r > 0) {
            const std::uint16_t n = static_cast<std::uint16_t>(
                *r > 0xFFFFu ? 0xFFFFu : *r);
            cb(handshake_scratch, n);
            last_rx_tsc_ = 0;  // not tracked at this layer
            return n;
        }
        return std::uint16_t{0};
    }

    [[nodiscard]] std::uint64_t last_rx_burst_tsc() const noexcept { return last_rx_tsc_; }

    [[nodiscard]] std::uint16_t mss() const noexcept { return 1460; }
    [[nodiscard]] ::eph::net::TcpState state() const noexcept {
        return is_established() ? ::eph::net::TcpState::Established
                                 : ::eph::net::TcpState::Closed;
    }
    [[nodiscard]] bool is_established() const noexcept {
        return sock_ != nullptr && sock_->is_open();
    }

private:
    ByteSocket* sock_ = nullptr;
    uint64_t    last_rx_tsc_ = 0;
};

// The `eph::net::TcpTransport` concept is no longer a requirement on
// `TlsSession` (the concept header remains in eph-core for backward
// compatibility, but `TlsSession` is now an unconstrained template).
// Method-set compliance is enforced by ordinary template instantiation
// inside `TlsSession<ByteSocketTcpAdapter>`.

// ---------------------------------------------------------------------------
// TlsState — owns the post-handshake AEAD context + record parsing buffer.
// ---------------------------------------------------------------------------
//
// One instance lives inside each `KernelTcpStream<C, true>` (folded into
// the `[[no_unique_address]]` slot). After construction it is empty;
// `handshake()` brings it online; from there `process_records()` decrypts
// freshly received bytes and `encrypt_for_send()` produces TLS records
// for the TX path.

class TlsState {
public:
    TlsState() = default;
    TlsState(const TlsState&)            = delete;
    TlsState& operator=(const TlsState&) = delete;
    TlsState(TlsState&&) noexcept        = default;
    TlsState& operator=(TlsState&&) noexcept = default;

    /// @brief Run the full TLS 1.3 handshake against `sock` and snapshot
    ///        the traffic keys into our hot-path AEAD context.
    ///
    /// Synchronous / blocking: must be called from the control thread,
    /// before the stream is added to a Poller. Once we return success the
    /// `sock` is back to its bare ByteSocket state and is owned by the
    /// caller — no live SSL pointers remain on the data plane.
    [[nodiscard]] std::expected<void, ::eph::core::ErrorInfo>
    handshake(ByteSocket& sock, const ::eph::net::TlsConfig& cfg) noexcept {
        [[maybe_unused]] auto* log = tls_state_logger();
        SPDLOG_LOGGER_DEBUG(log,
            "TlsState::handshake: entry fd={} hostname='{}' verify_peer={}",
            sock.fd(), cfg.hostname, cfg.verify_peer);

        ByteSocketTcpAdapter adapter(&sock);

        auto sess_r = ::eph::net::TlsSession<ByteSocketTcpAdapter>::create(
            adapter, cfg);
        if (!sess_r) {
            SPDLOG_LOGGER_ERROR(log,
                "TlsState::handshake: TlsSession::create failed fd={} "
                "hostname='{}'", sock.fd(), cfg.hostname);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsHandshakeFailed,
                "TlsState::handshake: TlsSession::create failed"});
        }
        auto& session = *sess_r;

        if (auto h = session.handshake(); !h) {
            SPDLOG_LOGGER_ERROR(log,
                "TlsState::handshake: session.handshake() failed fd={} "
                "hostname='{}'", sock.fd(), cfg.hostname);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsHandshakeFailed,
                "TlsState::handshake: TLS handshake failed"});
        }

        return finalize_from_session_(session, cfg, sock.fd());
    }

    /// @brief Begin a NON-blocking TLS handshake.
    ///
    /// The poll-loop counterpart of `handshake()`. Unlike the blocking path
    /// (which keeps the SSL session in a local and runs it to completion), this
    /// stashes the adapter + SSL session as members so they survive across
    /// `poll_once_()` cycles. Drive it with repeated `handshake_step()` until
    /// it returns `true`. Precondition: `sock` is TCP-connected.
    [[nodiscard]] std::expected<void, ::eph::core::ErrorInfo>
    begin_handshake(ByteSocket& sock, const ::eph::net::TlsConfig& cfg) noexcept {
        [[maybe_unused]] auto* log = tls_state_logger();
        SPDLOG_LOGGER_DEBUG(log,
            "TlsState::begin_handshake: entry fd={} hostname='{}'",
            sock.fd(), cfg.hostname);
        // The adapter must outlive the session (the SSL BIO holds a pointer
        // to it), and both must be stable across poll cycles — hence heap.
        hs_adapter_ = std::make_unique<ByteSocketTcpAdapter>(&sock);
        auto sess_r = ::eph::net::TlsSession<ByteSocketTcpAdapter>::create(
            *hs_adapter_, cfg);
        if (!sess_r) {
            hs_adapter_.reset();
            SPDLOG_LOGGER_ERROR(log,
                "TlsState::begin_handshake: TlsSession::create failed fd={} "
                "hostname='{}'", sock.fd(), cfg.hostname);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsHandshakeFailed,
                "TlsState::begin_handshake: TlsSession::create failed"});
        }
        hs_session_ = std::make_unique<
            ::eph::net::TlsSession<ByteSocketTcpAdapter>>(std::move(*sess_r));
        hs_cfg_ = cfg;
        hs_fd_  = sock.fd();
        return {};
    }

    /// @brief Advance the non-blocking TLS handshake by one step.
    /// @return `true`  — complete (keys extracted, `is_established()` now true);
    ///         `false` — pending (call again on the next readable/writable poll);
    ///         `unexpected` — fatal handshake error.
    [[nodiscard]] std::expected<bool, ::eph::core::ErrorInfo>
    handshake_step() noexcept {
        [[maybe_unused]] auto* log = tls_state_logger();
        if (!hs_session_) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsHandshakeFailed,
                "TlsState::handshake_step: begin_handshake() not called"});
        }
        auto step = hs_session_->handshake_step();
        if (!step) {
            SPDLOG_LOGGER_ERROR(log,
                "TlsState::handshake_step: handshake failed fd={}: {}",
                hs_fd_, step.error());
            hs_session_.reset();
            hs_adapter_.reset();
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsHandshakeFailed,
                "TlsState::handshake_step: TLS handshake failed"});
        }
        if (!*step) return false;  // still handshaking

        // Done: extract keys, then drop the live SSL session — the data plane
        // runs on crypto_ from here, exactly like the blocking path.
        auto fin = finalize_from_session_(*hs_session_, hs_cfg_, hs_fd_);
        hs_session_.reset();
        hs_adapter_.reset();
        if (!fin) return std::unexpected(fin.error());
        return true;
    }

    [[nodiscard]] bool is_established() const noexcept { return established_; }

    /// True if the just-completed handshake was a TLS 1.3 PSK / ticket
    /// resumption (1-RTT abbreviated). Snapshot taken inside `handshake()`
    /// before the underlying TlsSession is dropped.
    [[nodiscard]] bool was_resumed() const noexcept { return was_resumed_; }

    /// Move-out the captured server-issued NewSessionTicket bytes, if any.
    /// Returned bytes are DER-encoded (`i2d_SSL_SESSION`) and ready to
    /// feed into a future `TlsConfig::tls_resumption_ticket` for
    /// abbreviated reconnect. Move-out semantics: a subsequent call
    /// returns empty until a new ticket is captured.
    [[nodiscard]] std::vector<uint8_t> take_resumption_ticket() noexcept {
        return std::move(captured_ticket_);
    }

    /// Read-only view of the captured ticket without consuming it. Used
    /// primarily by tests that need to assert capture without disturbing
    /// the user-facing move-out semantics.
    [[nodiscard]] std::span<const uint8_t> peek_resumption_ticket() const noexcept {
        return std::span<const uint8_t>(captured_ticket_.data(),
                                         captured_ticket_.size());
    }

    /// @brief Decrypt as many complete TLS records as available in
    ///        `[ciphertext_in, ciphertext_in + in_len)`, append plaintext
    ///        to `plaintext_out`, return the number of ciphertext bytes
    ///        consumed.
    ///
    /// Records that are partially-buffered (header parses but payload not
    /// yet fully present) are not consumed — the caller leaves them for
    /// the next call.
    [[nodiscard]] std::expected<std::size_t, ::eph::core::ErrorInfo>
    process_records(const uint8_t* ciphertext_in, std::size_t in_len,
                     std::vector<uint8_t>& plaintext_out) noexcept {
        if (!established_ || !crypto_) {
            SPDLOG_LOGGER_ERROR(tls_state_logger(),
                "TlsState::process_records: called before established "
                "(in_len={})", in_len);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsHandshakeFailed,
                "TlsState::process_records: not established"});
        }

        std::size_t consumed = 0;
        while (in_len - consumed >= ::eph::net::tls_record::kRecordHeaderLen) {
            const uint8_t* rec = ciphertext_in + consumed;
            uint8_t  ct;
            uint16_t payload_len;
            if (!::eph::net::tls_record::parse_record_header(rec, ct, payload_len)) {
                // Bad header — surface as cipher failure. Log with context
                // (offset, first few header bytes) so operators can triage
                // between "proxy injected garbage" and "decrypt key desync".
                SPDLOG_LOGGER_ERROR(tls_state_logger(),
                    "TlsState::process_records: bad record header "
                    "offset={} total_buffered={} ct_byte=0x{:02x} "
                    "len_bytes=0x{:02x}{:02x}",
                    consumed, in_len, rec[0], rec[3], rec[4]);
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::TlsRecordBad,
                    "TlsState::process_records: bad record header"});
            }
            const std::size_t total = ::eph::net::tls_record::kRecordHeaderLen + payload_len;
            if (in_len - consumed < total) break;  // partial record

            // Worst-case plaintext is `payload_len` bytes (no auth tag, no inner CT).
            const std::size_t out_off = plaintext_out.size();
            plaintext_out.resize(out_off + payload_len);
            uint16_t plaintext_len = 0;
            uint8_t  inner_ct = 0;
            if (!crypto_->decrypt(rec, static_cast<uint16_t>(total),
                                   plaintext_out.data() + out_off, plaintext_len,
                                   &inner_ct)) {
                plaintext_out.resize(out_off);  // unwind on failure
                SPDLOG_LOGGER_ERROR(tls_state_logger(),
                    "TlsState::process_records: TLS decrypt failed "
                    "offset={} record_total={} payload_len={} ct=0x{:02x}",
                    consumed, total, payload_len, ct);
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::TlsCipherFailed,
                    "TlsState::process_records: TLS decrypt failed"});
            }
            // TLS 1.3 inner content type filter (RFC 8446 §5.2):
            //   0x17 (23) = application_data → forward to codec
            //   0x16 (22) = handshake        (NewSessionTicket, KeyUpdate) → skip
            //   0x15 (21) = alert            → skip (TCP close will follow on
            //                                  fatal alert; close_notify is
            //                                  observable via the TCP FIN
            //                                  read on the next poll cycle)
            // Non-appdata records are consumed (sequence counter advances)
            // but their plaintext is discarded so the codec only sees the
            // application byte stream. DEBUG-log them so operators can
            // distinguish "peer sent KeyUpdate" from "peer sent alert"
            // when triaging a connection that closes shortly after — the
            // DPDK side logs the same way (symmetry).
            if (inner_ct == 0x17) {
                plaintext_out.resize(out_off + plaintext_len);
            } else {
                plaintext_out.resize(out_off);  // discard non-appdata plaintext
                SPDLOG_LOGGER_DEBUG(tls_state_logger(),
                    "TlsState::process_records: skipping non-appdata record "
                    "inner_ct=0x{:02x} plaintext_len={} record_total={} "
                    "offset={}",
                    inner_ct, plaintext_len, total, consumed);
            }
            consumed += total;
        }
        return consumed;
    }

    /// @brief Encrypt `plaintext` into one or more TLS records appended to
    ///        `ciphertext_out`. `plaintext` is application-layer bytes
    ///        (post-codec); `ciphertext_out` receives the AEAD-sealed
    ///        TLS records ready for the socket.
    [[nodiscard]] std::expected<void, ::eph::core::ErrorInfo>
    encrypt_for_send(const uint8_t* plaintext, std::size_t len,
                      std::vector<uint8_t>& ciphertext_out) noexcept {
        if (!established_ || !crypto_) {
            SPDLOG_LOGGER_ERROR(tls_state_logger(),
                "TlsState::encrypt_for_send: called before established "
                "(len={})", len);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsHandshakeFailed,
                "TlsState::encrypt_for_send: not established"});
        }
        std::size_t off = 0;
        while (off < len) {
            const uint16_t chunk = static_cast<uint16_t>(std::min<std::size_t>(
                ::eph::net::tls_const::kMaxRecordPayload, len - off));
            // Instance method — format-aware. The negotiated format is
            // fixed for the session, so this is a single load + branch
            // (predictable). Was a static call before TLS 1.2 support
            // when only the 1.3 layout existed.
            const uint16_t enc_size = crypto_->encrypted_size(chunk);
            const std::size_t out_off = ciphertext_out.size();
            ciphertext_out.resize(out_off + enc_size);
            const uint16_t written = crypto_->encrypt(plaintext + off, chunk,
                                                       ciphertext_out.data() + out_off);
            if (written == 0) {
                ciphertext_out.resize(out_off);
                SPDLOG_LOGGER_ERROR(tls_state_logger(),
                    "TlsState::encrypt_for_send: TLS encrypt failed "
                    "off={} chunk={} total_len={}",
                    off, chunk, len);
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::TlsCipherFailed,
                    "TlsState::encrypt_for_send: TLS encrypt failed"});
            }
            ciphertext_out.resize(out_off + written);
            off += chunk;
        }
        return {};
    }

private:
    /// @brief Shared completion: pull resumption state + traffic keys out of a
    ///        just-finished `TlsSession` and stand up the hot-path AEAD context.
    ///        Used by both the blocking `handshake()` and the non-blocking
    ///        `handshake_step()`. On success `is_established()` becomes true.
    [[nodiscard]] std::expected<void, ::eph::core::ErrorInfo>
    finalize_from_session_(
        ::eph::net::TlsSession<ByteSocketTcpAdapter>& session,
        [[maybe_unused]] const ::eph::net::TlsConfig& cfg, int fd) noexcept {
        [[maybe_unused]] auto* log = tls_state_logger();
        // Snapshot resumption state BEFORE extract_hot_state (which sets the
        // session's suppress_close_notify_) so was_resumed_ reflects the
        // just-completed full/abbreviated classification.
        was_resumed_     = session.was_resumed();
        captured_ticket_ = session.take_resumption_ticket();
        SPDLOG_LOGGER_DEBUG(log,
            "TlsState::finalize: resumed={} captured_ticket={}B fd={} host='{}'",
            was_resumed_, captured_ticket_.size(), fd, cfg.hostname);

        auto state_r = session.extract_hot_state();
        if (!state_r) {
            SPDLOG_LOGGER_ERROR(log,
                "TlsState::finalize: extract_hot_state failed fd={}", fd);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsHandshakeFailed,
                "TlsState::finalize: extract_hot_state failed"});
        }
        const std::size_t key_len = session.cipher_key_len();
        if (key_len != 16 && key_len != 32) {
            SPDLOG_LOGGER_ERROR(log,
                "TlsState::finalize: unsupported AEAD key length {} fd={}",
                key_len, fd);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsCipherFailed,
                "TlsState::finalize: unsupported AEAD key length"});
        }
        auto crypto_r = ::eph::net::TlsRecordCrypto::create(*state_r, key_len);
        if (!crypto_r) {
            SPDLOG_LOGGER_ERROR(log,
                "TlsState::finalize: TlsRecordCrypto::create failed fd={} "
                "key_len={}", fd, key_len);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsCipherFailed,
                "TlsState::finalize: TlsRecordCrypto::create failed"});
        }
        crypto_      = std::make_unique<::eph::net::TlsRecordCrypto>(
            std::move(*crypto_r));
        established_ = true;
        SPDLOG_LOGGER_DEBUG(log,
            "TlsState::finalize: success fd={} key_len={}", fd, key_len);
        return {};
    }

    // Heap-allocated so the move constructor stays trivial — TlsRecordCrypto
    // contains an EVP_AEAD_CTX which is bitwise-copyable for AES-GCM but the
    // unique_ptr indirection makes the surrounding KernelTcpStream layout
    // simpler (no inline 200-byte AEAD struct).
    std::unique_ptr<::eph::net::TlsRecordCrypto> crypto_;
    bool                                          established_ = false;

    // ── Non-blocking handshake state (only live between begin_handshake and
    //    the handshake_step() that completes it; reset to empty otherwise) ──
    // Declared so hs_session_ destructs BEFORE hs_adapter_ (reverse member
    // order): the session's SSL/BIO must release its pointer into the adapter
    // before the adapter itself is freed.
    std::unique_ptr<ByteSocketTcpAdapter>                          hs_adapter_;
    std::unique_ptr<::eph::net::TlsSession<ByteSocketTcpAdapter>>  hs_session_;
    ::eph::net::TlsConfig                                          hs_cfg_{};
    int                                                            hs_fd_ = -1;
    /// Set to true by `handshake()` when `SSL_session_reused` reports
    /// the TLS 1.3 abbreviated handshake completed instead of a full
    /// cert exchange. Read-only after handshake.
    bool                                          was_resumed_ = false;
    /// Server-issued NewSessionTicket bytes (DER `i2d_SSL_SESSION`)
    /// captured during the handshake's record-processing window. May
    /// be empty if the server hasn't issued a ticket yet — typical
    /// TLS 1.3 servers send one immediately after Finished but some
    /// defer to post-handshake messages.
    std::vector<uint8_t>                          captured_ticket_;
};

} // namespace eph::net::kernel::detail
