#pragma once

/// Macro signal to `eph/net/dpdk/tcp_stream.hpp` that the real TlsState
/// (with aws-lc dependency) is being injected. Must be defined BEFORE
/// `tcp_stream.hpp` is included by the user's TU.
#define EPH_NET_DPDK_TLS_STATE_HPP_INCLUDED 1

/// @file tls_state.hpp
/// Real TLS 1.3 state for `DpdkTcpStream<C, true>` with the **zero-copy
/// in-place AES-GCM decrypt** path enabled.
///
/// Uses an aws-lc / BoringSSL handshake driven through
/// `eph::dpdk::TcpSession<>` (which satisfies the `eph::net::TcpTransport`
/// concept), plus a hot-path AEAD context that can decrypt records
/// **directly into the same buffer the ciphertext occupies**.
///
/// Architecture:
///
///     DpdkTcpStream<C, true>::create()
///        ├── TcpSession::connect()                // TCP 3-way over DPDK
///        ├── TlsSession<TcpSession>::create()     // legacy aws-lc
///        ├── session.handshake()                  // blocking, control thread,
///        │                                          drives the NIC RX queue
///        │                                          via TcpSession::poll_rx
///        ├── session.extract_hot_state()          // pulls TLS 1.3 keys
///        └── TlsInPlaceDecryptor::create()        // hot-path in-place AEAD
///
///     DpdkTcpStream<C, true>::process_burst_(mbufs, n, rx_tsc)
///        ├── TcpSession::process_rx (legacy)      // mbufs → reasm_ via callback
///        │                                          (this still copies once
///        │                                           into the reasm vector;
///        │                                           the state machine owns
///        │                                           the byte pipe shape)
///        ├── parse + decrypt-in-place each TLS record on the reasm buffer
///        │   ── EVP_AEAD_CTX_open(in==out)        // <-- THE ZERO-COPY POINT
///        └── codec.decode(MbufView{plaintext, len})
///
/// The "zero-copy" applies to the codec->TLS-plaintext leg: the codec
/// reads plaintext directly out of the same memory we just decrypted
/// over (no second buffer, no second memcpy). The TcpSession still
/// copies mbuf payload into the reasm vector once; a future migration
/// of the session into the dpdk module would make the path a true
/// mbuf-chain in-place pipeline.
///
/// To exercise the AES-GCM in-place primitive without the legacy reasm
/// copy in the test path, see `test_tls_in_place_decrypt` which feeds
/// raw records into `TlsInPlaceDecryptor::open_in_place()` directly.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>
#include "eph/core/detail/logger.hpp"

#include "eph/core/error.hpp"
#include "eph/dpdk/tcp.hpp"
#include "eph/net/detail/tls_inplace.hpp"
#include "eph/net/detail/tls_constants.hpp"
#include "eph/net/detail/tls_record.hpp"
#include "eph/net/detail/tls_session.hpp"

namespace eph::net::dpdk::detail {

/// @brief Lazily-initialized logger for the DPDK TLS state subsystem.
inline spdlog::logger* tls_state_logger() {
    static auto* l = ::eph::core::detail::make_logger("net.dpdk.tls_state");
    return l;
}

// ---------------------------------------------------------------------------
// TlsState — owns the post-handshake AEAD context for DpdkTcpStream.
// ---------------------------------------------------------------------------

class TlsState {
public:
    /// @brief Tag used by `tcp_stream.hpp`'s static_assert to confirm
    ///        that the real (aws-lc-backed) TlsState was included rather
    ///        than the opt-in stub.
    static constexpr bool kIsRealTlsState = true;

    TlsState() = default;
    TlsState(const TlsState&)            = delete;
    TlsState& operator=(const TlsState&) = delete;
    TlsState(TlsState&&) noexcept        = default;
    TlsState& operator=(TlsState&&) noexcept = default;

    /// @brief Run TLS 1.3 handshake against the given TcpSession and
    ///        snapshot read/write keys into hot-path AEAD contexts.
    [[nodiscard]] std::expected<void, ::eph::core::ErrorInfo>
    handshake(::eph::dpdk::TcpSession<>& sess,
               const ::eph::net::TlsConfig& cfg) noexcept {
        // Bring DPDK-side TLS handshake diagnostics up to parity with the
        // kernel sibling (eph-net-kernel/.../detail/tls_state.hpp), which
        // logs an ERROR with hostname/verify_peer context on every error
        // branch. Without these the operator sees only the typed
        // `TlsHandshakeFailed` ErrorInfo and has to retry under a debugger
        // to find out *which* sub-step failed.
        auto* log = tls_state_logger();
        SPDLOG_LOGGER_DEBUG(log,
            "TlsState::handshake: entry hostname='{}' verify_peer={}",
            cfg.hostname, cfg.verify_peer);

        auto sess_r =
            ::eph::net::TlsSession<::eph::dpdk::TcpSession<>>::create(sess, cfg);
        if (!sess_r) {
            SPDLOG_LOGGER_ERROR(log,
                "TlsState::handshake: TlsSession::create failed "
                "hostname='{}'", cfg.hostname);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsHandshakeFailed,
                "TlsState::handshake: TlsSession::create failed"});
        }
        if (auto h = sess_r->handshake(); !h) {
            SPDLOG_LOGGER_ERROR(log,
                "TlsState::handshake: handshake() failed hostname='{}'",
                cfg.hostname);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsHandshakeFailed,
                "TlsState::handshake: handshake() failed"});
        }
        return finalize_from_session_(*sess_r, cfg);
    }

    /// @brief Begin a NON-blocking TLS handshake.
    ///
    /// The poll-loop counterpart of `handshake()`. Keeps the aws-lc session
    /// alive as a member (`hs_session_`) across poll cycles — its BIO holds a
    /// pointer to `sess`, which the owning `DpdkTcpStream` keeps stable. Drive
    /// it with repeated `handshake_step()` until it returns `true`.
    [[nodiscard]] std::expected<void, ::eph::core::ErrorInfo>
    begin_handshake(::eph::dpdk::TcpSession<>& sess,
                    const ::eph::net::TlsConfig& cfg) noexcept {
        auto* log = tls_state_logger();
        SPDLOG_LOGGER_DEBUG(log,
            "TlsState::begin_handshake: hostname='{}'", cfg.hostname);
        auto sess_r =
            ::eph::net::TlsSession<::eph::dpdk::TcpSession<>>::create(sess, cfg);
        if (!sess_r) {
            SPDLOG_LOGGER_ERROR(log,
                "TlsState::begin_handshake: TlsSession::create failed "
                "hostname='{}'", cfg.hostname);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsHandshakeFailed,
                "TlsState::begin_handshake: TlsSession::create failed"});
        }
        hs_session_ = std::make_unique<
            ::eph::net::TlsSession<::eph::dpdk::TcpSession<>>>(std::move(*sess_r));
        hs_cfg_ = cfg;
        return {};
    }

    /// @brief Advance the non-blocking TLS handshake by one step.
    /// @return `true`  — complete (keys extracted, `is_established()` true);
    ///         `false` — pending (call again next poll cycle);
    ///         `unexpected` — fatal handshake error.
    [[nodiscard]] std::expected<bool, ::eph::core::ErrorInfo>
    handshake_step() noexcept {
        auto* log = tls_state_logger();
        if (!hs_session_) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsHandshakeFailed,
                "TlsState::handshake_step: begin_handshake() not called"});
        }
        auto step = hs_session_->handshake_step();
        if (!step) {
            SPDLOG_LOGGER_ERROR(log,
                "TlsState::handshake_step: handshake failed: {}", step.error());
            hs_session_.reset();
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsHandshakeFailed,
                "TlsState::handshake_step: TLS handshake failed"});
        }
        if (!*step) return false;  // still handshaking
        auto fin = finalize_from_session_(*hs_session_, hs_cfg_);
        hs_session_.reset();
        if (!fin) return std::unexpected(fin.error());
        return true;
    }

    [[nodiscard]] bool is_established() const noexcept { return established_; }

    /// True if the handshake was a TLS 1.3 abbreviated / PSK resumption.
    [[nodiscard]] bool was_resumed() const noexcept { return was_resumed_; }

    /// Move-out captured server NewSessionTicket (DER-encoded
    /// `i2d_SSL_SESSION` bytes). Empty if no ticket arrived. See
    /// `eph::net::TlsConfig::tls_resumption_ticket` for the consumer side.
    [[nodiscard]] std::vector<uint8_t> take_resumption_ticket() noexcept {
        return std::move(captured_ticket_);
    }

    /// Read-only view (does not consume). Used by tests.
    [[nodiscard]] std::span<const uint8_t> peek_resumption_ticket() const noexcept {
        return std::span<const uint8_t>(captured_ticket_.data(),
                                         captured_ticket_.size());
    }

    /// @brief Decrypt complete TLS records starting at `buf` (mutable!),
    ///        in place. Returns the number of input bytes consumed and
    ///        appends pointers to plaintext slices via `emit`.
    ///
    /// `emit` is called with `(uint8_t* plaintext, size_t len)` for each
    /// decrypted record's plaintext window — the pointer aliases into
    /// `buf`. The exact offset depends on the negotiated record format:
    ///   - TLS 1.3 / 1.2 CHACHA20: plaintext starts at `rec + 5` (header only)
    ///   - TLS 1.2 AES-GCM:        plaintext starts at `rec + 13`
    ///                              (header + 8B explicit nonce)
    /// `eph::net::plaintext_offset_for(format)` centralizes this knowledge.
    template <class Emit>
    [[nodiscard]] std::expected<std::size_t, ::eph::core::ErrorInfo>
    process_records_in_place(uint8_t* buf, std::size_t len, Emit&& emit) noexcept {
        if (!established_ || !dec_) {
            SPDLOG_LOGGER_WARN(tls_state_logger(),
                "TlsState::process_records_in_place: not established "
                "(established_={} dec_={} len={})",
                established_, static_cast<bool>(dec_), len);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsHandshakeFailed,
                "TlsState::process_records_in_place: not established"});
        }
        std::size_t consumed = 0;
        while (len - consumed >= ::eph::net::tls_record::kRecordHeaderLen) {
            uint8_t* rec = buf + consumed;
            uint8_t  ct;
            uint16_t payload_len;
            if (!::eph::net::tls_record::parse_record_header(rec, ct, payload_len)) {
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::TlsRecordBad,
                    "TlsState::process_records_in_place: bad header"});
            }
            const std::size_t total = ::eph::net::tls_record::kRecordHeaderLen + payload_len;
            if (len - consumed < total) break;  // partial record

            std::size_t plaintext_len = 0;
            uint8_t inner_ct = 0;
            // ★ ZERO-COPY in-place AEAD. Output offset within `rec` is
            //   format-dependent (5B header for 1.3 / CHACHA20-1.2,
            //   5B header + 8B explicit nonce for AES-GCM-1.2).
            if (!dec_->open_in_place(rec, total, &plaintext_len, &inner_ct)) {
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::TlsCipherFailed,
                    "TlsState::process_records_in_place: AEAD open failed"});
            }
            // Inner content-type filter:
            //   TLS 1.3 (RFC 8446 §5.2):
            //     0x17 = application_data → emit
            //     0x16 = handshake (NewSessionTicket, KeyUpdate) → skip
            //     0x15 = alert → skip
            //   TLS 1.2: the record header carries the real type, which the
            //   in-place decryptor surfaces through `inner_ct` for symmetry.
            //   Same filter applies.
            if (inner_ct == 0x17 && plaintext_len > 0) {
                const uint16_t pt_off =
                    ::eph::net::plaintext_offset_for(dec_->record_format());
                emit(rec + pt_off, plaintext_len);
            } else {
                SPDLOG_LOGGER_DEBUG(tls_state_logger(),
                    "TlsState: skipping non-appdata record inner_ct={:#x} len={}",
                    inner_ct, plaintext_len);
            }

            consumed += total;
        }
        return consumed;
    }

    /// @brief Encrypt `data` into TLS records appended to `out`.
    [[nodiscard]] std::expected<void, ::eph::core::ErrorInfo>
    encrypt_for_send(const uint8_t* data, std::size_t len,
                      std::vector<uint8_t>& out) noexcept {
        if (!established_ || !enc_) {
            SPDLOG_LOGGER_WARN(tls_state_logger(),
                "TlsState::encrypt_for_send: not established "
                "(established_={} enc_={} len={})",
                established_, static_cast<bool>(enc_), len);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsHandshakeFailed,
                "TlsState::encrypt_for_send: not established"});
        }
        std::size_t off = 0;
        while (off < len) {
            const uint16_t chunk = static_cast<uint16_t>(std::min<std::size_t>(
                ::eph::net::tls_const::kMaxRecordPayload, len - off));
            // Instance method — format-aware (TLS 1.3 / 1.2 GCM / 1.2
            // CHACHA20). Branch is fixed for the session; the static
            // 1.3-only helper this replaced was correct only by accident
            // when the stack supported just one format.
            const uint16_t enc_size = enc_->encrypted_size(chunk);
            const std::size_t out_off = out.size();
            out.resize(out_off + enc_size);
            const uint16_t written = enc_->encrypt(data + off, chunk,
                                                    out.data() + out_off);
            if (written == 0) {
                out.resize(out_off);
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::TlsCipherFailed,
                    "TlsState::encrypt_for_send: TLS encrypt failed"});
            }
            out.resize(out_off + written);
            off += chunk;
        }
        return {};
    }

private:
    /// @brief Shared completion: pull resumption state + traffic keys out of a
    ///        finished `TlsSession` and stand up the hot-path AEAD contexts.
    ///        Used by both the blocking `handshake()` and the non-blocking
    ///        `handshake_step()`. On success `is_established()` becomes true.
    [[nodiscard]] std::expected<void, ::eph::core::ErrorInfo>
    finalize_from_session_(
        ::eph::net::TlsSession<::eph::dpdk::TcpSession<>>& session,
        const ::eph::net::TlsConfig& cfg) noexcept {
        auto* log = tls_state_logger();
        // Snapshot resumption state before extract_hot_state — the session is
        // dropped after this and the captured ticket / resumed flag must
        // survive into the stream's metric path.
        was_resumed_     = session.was_resumed();
        captured_ticket_ = session.take_resumption_ticket();
        SPDLOG_LOGGER_DEBUG(log,
            "TlsState::finalize: resumed={} captured_ticket={}B hostname='{}'",
            was_resumed_, captured_ticket_.size(), cfg.hostname);

        auto state_r = session.extract_hot_state();
        if (!state_r) {
            SPDLOG_LOGGER_ERROR(log,
                "TlsState::finalize: extract_hot_state failed hostname='{}'",
                cfg.hostname);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsHandshakeFailed,
                "TlsState::finalize: extract_hot_state failed"});
        }
        const std::size_t key_len = session.cipher_key_len();
        if (key_len != 16 && key_len != 32) {
            SPDLOG_LOGGER_ERROR(log,
                "TlsState::finalize: unsupported AEAD key_len={} hostname='{}'",
                key_len, cfg.hostname);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsCipherFailed,
                "TlsState::finalize: unsupported AEAD key length"});
        }

        // In-place decryptor (read direction, hot path) — format-aware.
        auto dec_r = ::eph::net::detail::TlsInPlaceDecryptor::create(
            state_r->read.ki.key, key_len,
            state_r->read.ki.iv,  ::eph::net::tls_const::kTls13NonceLen,
            state_r->read.seq,
            state_r->record_format);
        if (!dec_r) {
            SPDLOG_LOGGER_ERROR(log,
                "TlsState::finalize: TlsInPlaceDecryptor::create failed "
                "hostname='{}' key_len={} detail='{}'",
                cfg.hostname, key_len, dec_r.error().detail);
            return std::unexpected(dec_r.error());
        }
        dec_ = std::make_unique<::eph::net::detail::TlsInPlaceDecryptor>(
            std::move(*dec_r));

        // Encryptor (write direction) — TX is not on the hot decrypt path.
        auto enc_r = ::eph::net::TlsEncryptor::create(*state_r, key_len);
        if (!enc_r) {
            SPDLOG_LOGGER_ERROR(log,
                "TlsState::finalize: TlsEncryptor::create failed "
                "hostname='{}' key_len={}", cfg.hostname, key_len);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsCipherFailed,
                "TlsState::finalize: TlsEncryptor::create failed"});
        }
        enc_ = std::make_unique<::eph::net::TlsEncryptor>(std::move(*enc_r));

        established_ = true;
        return {};
    }

    std::unique_ptr<::eph::net::detail::TlsInPlaceDecryptor> dec_;
    std::unique_ptr<::eph::net::TlsEncryptor>                 enc_;
    bool                                                       established_ = false;

    // ── Non-blocking handshake state (live only between begin_handshake and
    //    the handshake_step() that completes it) ──
    std::unique_ptr<::eph::net::TlsSession<::eph::dpdk::TcpSession<>>> hs_session_;
    ::eph::net::TlsConfig                                     hs_cfg_{};
    /// TLS 1.3 abbreviated-handshake flag (set inside `handshake()` from
    /// `SSL_session_reused`). Read-only after handshake.
    bool                                                       was_resumed_ = false;
    /// DER-encoded server NewSessionTicket captured during handshake.
    /// See `take_resumption_ticket()` for move-out semantics.
    std::vector<uint8_t>                                       captured_ticket_;
};

} // namespace eph::net::dpdk::detail
