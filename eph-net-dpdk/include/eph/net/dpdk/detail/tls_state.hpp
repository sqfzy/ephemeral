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
        // Snapshot resumption state before extract_hot_state — the
        // session is dropped at scope exit and the captured ticket /
        // resumed flag must survive into the stream's metric path.
        was_resumed_ = sess_r->was_resumed();
        captured_ticket_ = sess_r->take_resumption_ticket();
        SPDLOG_LOGGER_DEBUG(log,
            "TlsState::handshake: resumed={} captured_ticket={}B",
            was_resumed_, captured_ticket_.size());

        auto state_r = sess_r->extract_hot_state();
        if (!state_r) {
            SPDLOG_LOGGER_ERROR(log,
                "TlsState::handshake: extract_hot_state failed "
                "hostname='{}'", cfg.hostname);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsHandshakeFailed,
                "TlsState::handshake: extract_hot_state failed"});
        }
        const std::size_t key_len = sess_r->cipher_key_len();
        if (key_len != 16 && key_len != 32) {
            SPDLOG_LOGGER_ERROR(log,
                "TlsState::handshake: unsupported AEAD key_len={} "
                "(expected 16 or 32) hostname='{}'",
                key_len, cfg.hostname);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsCipherFailed,
                "TlsState::handshake: unsupported AEAD key length"});
        }

        // Build the in-place decryptor (read direction, hot path).
        auto dec_r = ::eph::net::detail::TlsInPlaceDecryptor::create(
            state_r->read.ki.key, key_len,
            state_r->read.ki.iv,  ::eph::net::tls_const::kTls13NonceLen,
            state_r->read.seq);
        if (!dec_r) {
            SPDLOG_LOGGER_ERROR(log,
                "TlsState::handshake: TlsInPlaceDecryptor::create failed "
                "hostname='{}' key_len={} detail='{}'",
                cfg.hostname, key_len, dec_r.error().detail);
            return std::unexpected(dec_r.error());
        }
        dec_ = std::make_unique<::eph::net::detail::TlsInPlaceDecryptor>(
            std::move(*dec_r));

        // Build the encryptor (write direction). The legacy TlsEncryptor
        // is fine here — TX is not on the hot decrypt path and the
        // existing implementation handles the TLS record framing.
        auto enc_r = ::eph::net::TlsEncryptor::create(*state_r, key_len);
        if (!enc_r) {
            SPDLOG_LOGGER_ERROR(log,
                "TlsState::handshake: TlsEncryptor::create failed "
                "hostname='{}' key_len={}",
                cfg.hostname, key_len);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsCipherFailed,
                "TlsState::handshake: TlsEncryptor::create failed"});
        }
        enc_ = std::make_unique<::eph::net::TlsEncryptor>(std::move(*enc_r));

        established_ = true;
        return {};
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
    /// `buf` (specifically `buf + 5` for that record).
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
            // ★ ZERO-COPY in-place AEAD: input == output == rec+5.
            if (!dec_->open_in_place(rec, total, &plaintext_len, &inner_ct)) {
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::TlsCipherFailed,
                    "TlsState::process_records_in_place: AEAD open failed"});
            }
            // TLS 1.3 inner content type filter (RFC 8446 §5.2):
            //   0x17 (23) = application_data → emit to codec
            //   0x16 (22) = handshake (NewSessionTicket, KeyUpdate) → skip
            //   0x15 (21) = alert → skip (connection will close via TCP)
            // Only application data reaches the codec; post-handshake control
            // messages are silently consumed to keep the sequence counter in
            // sync without corrupting the application byte stream.
            if (inner_ct == 0x17 && plaintext_len > 0) {
                emit(rec + ::eph::net::tls_record::kRecordHeaderLen, plaintext_len);
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
            const uint16_t enc_size = ::eph::net::TlsEncryptor::encrypted_size(chunk);
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
    std::unique_ptr<::eph::net::detail::TlsInPlaceDecryptor> dec_;
    std::unique_ptr<::eph::net::TlsEncryptor>                 enc_;
    bool                                                       established_ = false;
    /// TLS 1.3 abbreviated-handshake flag (set inside `handshake()` from
    /// `SSL_session_reused`). Read-only after handshake.
    bool                                                       was_resumed_ = false;
    /// DER-encoded server NewSessionTicket captured during handshake.
    /// See `take_resumption_ticket()` for move-out semantics.
    std::vector<uint8_t>                                       captured_ticket_;
};

} // namespace eph::net::dpdk::detail
