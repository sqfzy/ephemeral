#pragma once

/// Macro signal to `eph/net/dpdk/tcp_stream.hpp` that the real TlsState
/// (with aws-lc dependency) is being injected. Must be defined BEFORE
/// `tcp_stream.hpp` is included by the user's TU.
#define EPH_NET_DPDK_TLS_STATE_HPP_INCLUDED 1

/// @file tls_state.hpp
/// Phase 5: real TLS 1.3 state for `DpdkTcpStream<C, true>` with the
/// **zero-copy in-place AES-GCM decrypt** path enabled.
///
/// Replaces the Phase 4 placeholder TlsState{ handshake_completed } stub
/// with an aws-lc / BoringSSL handshake driven through the legacy
/// `eph::dpdk::TcpSession<>` (which already satisfies the legacy
/// `eph::net::TcpTransport` concept), plus a hot-path AEAD context that
/// can decrypt records **directly into the same buffer the ciphertext
/// occupies**. That is the headline zero-copy primitive behind the v3.3
/// design — see .artifacts/design-eph-v3.3-architecture-20260410.md.
///
/// Architecture:
///
///     DpdkTcpStream<C, true>::create()
///        ├── TcpSession::connect()                // Phase 4 (TCP 3-way over DPDK)
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
///        │                                           the legacy state machine
///        │                                           owns the byte pipe shape)
///        ├── parse + decrypt-in-place each TLS record on the reasm buffer
///        │   ── EVP_AEAD_CTX_open(in==out)        // <-- THE ZERO-COPY POINT
///        └── codec.decode(MbufView{plaintext, len})
///
/// The "zero-copy" applies to the codec→TLS-plaintext leg: the codec
/// reads plaintext directly out of the same memory we just decrypted
/// over (no second buffer, no second memcpy). The legacy TcpSession
/// still copies mbuf payload into the reasm vector once — Phase 6/7
/// migrates the legacy session into the new dpdk module so the path
/// can become a true mbuf-chain in-place pipeline.
///
/// To exercise the AES-GCM in-place primitive without the legacy reasm
/// copy in the test path, see `test_tls_in_place_decrypt` which feeds
/// raw records into `TlsInPlaceDecryptor::open_in_place()` directly.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "eph/core/error.hpp"
#include "eph/dpdk/tcp.hpp"
#include "eph/net/detail/tls_inplace.hpp"
#include "eph/net/detail/tls_constants.hpp"
#include "eph/net/detail/tls_record.hpp"
#include "eph/net/detail/tls_session.hpp"

namespace eph::net::dpdk::detail {

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
        auto sess_r =
            ::eph::net::TlsSession<::eph::dpdk::TcpSession<>>::create(sess, cfg);
        if (!sess_r) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsHandshakeFailed,
                "TlsState::handshake: TlsSession::create failed"});
        }
        if (auto h = sess_r->handshake(); !h) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsHandshakeFailed,
                "TlsState::handshake: handshake() failed"});
        }
        auto state_r = sess_r->extract_hot_state();
        if (!state_r) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsHandshakeFailed,
                "TlsState::handshake: extract_hot_state failed"});
        }
        const std::size_t key_len = sess_r->cipher_key_len();
        if (key_len != 16 && key_len != 32) {
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
            return std::unexpected(dec_r.error());
        }
        dec_ = std::make_unique<::eph::net::detail::TlsInPlaceDecryptor>(
            std::move(*dec_r));

        // Build the encryptor (write direction). The legacy TlsEncryptor
        // is fine here — TX is not on the hot decrypt path and the
        // existing implementation handles the TLS record framing.
        auto enc_r = ::eph::net::TlsEncryptor::create(*state_r, key_len);
        if (!enc_r) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsCipherFailed,
                "TlsState::handshake: TlsEncryptor::create failed"});
        }
        enc_ = std::make_unique<::eph::net::TlsEncryptor>(std::move(*enc_r));

        established_ = true;
        return {};
    }

    [[nodiscard]] bool is_established() const noexcept { return established_; }

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
            // ★ ZERO-COPY in-place AEAD: input == output == rec+5.
            if (!dec_->open_in_place(rec, total, &plaintext_len)) {
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::TlsCipherFailed,
                    "TlsState::process_records_in_place: AEAD open failed"});
            }
            // Hand the plaintext window to the codec via the emit callback.
            // The pointer aliases into `buf` so the caller MUST consume it
            // before the next call to process_records_in_place.
            emit(rec + ::eph::net::tls_record::kRecordHeaderLen, plaintext_len);

            consumed += total;
        }
        return consumed;
    }

    /// @brief Encrypt `data` into TLS records appended to `out`.
    [[nodiscard]] std::expected<void, ::eph::core::ErrorInfo>
    encrypt_for_send(const uint8_t* data, std::size_t len,
                      std::vector<uint8_t>& out) noexcept {
        if (!established_ || !enc_) {
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
};

} // namespace eph::net::dpdk::detail
