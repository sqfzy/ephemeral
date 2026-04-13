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
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "eph/core/error.hpp"
#include "eph/net/kernel/detail/byte_socket.hpp"
#include "eph/net/tcp_state.hpp"
#include "eph/net/detail/tls_constants.hpp"
#include "eph/net/detail/tls_record.hpp"
#include "eph/net/detail/tls_session.hpp"

namespace eph::net::kernel::detail {

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

    // Data transfer — forward to the byte socket.
    [[nodiscard]] std::expected<std::size_t, std::string>
    send(const void* data, std::size_t len) noexcept {
        std::span<const uint8_t> view(static_cast<const uint8_t*>(data), len);
        auto r = sock_->send(view);
        if (!r) return std::unexpected(std::string{r.error().detail});
        return *r;
    }

    /// Poll RX once: read up to 16K from the socket and invoke `cb` for the
    /// data we got. The TlsSession's BIO uses this to feed handshake bytes.
    ///
    /// Return type matches the legacy `TcpTransport::poll_rx` contract:
    /// `std::expected<uint16_t, std::string>` where the value is the byte
    /// count delivered to `cb` this call (0 on WouldBlock).
    template <class Cb>
    [[nodiscard]] std::expected<std::uint16_t, std::string>
    poll_rx(Cb&& cb) noexcept {
        uint8_t buf[16 * 1024];
        auto r = sock_->recv(buf, sizeof(buf));
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
            cb(buf, n);
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
        ByteSocketTcpAdapter adapter(&sock);

        auto sess_r = ::eph::net::TlsSession<ByteSocketTcpAdapter>::create(
            adapter, cfg);
        if (!sess_r) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsHandshakeFailed,
                "TlsState::handshake: TlsSession::create failed"});
        }
        auto& session = *sess_r;

        if (auto h = session.handshake(); !h) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsHandshakeFailed,
                "TlsState::handshake: TLS handshake failed"});
        }

        // Pull traffic keys + sequence numbers from the SSL session.
        auto state_r = session.extract_hot_state();
        if (!state_r) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsHandshakeFailed,
                "TlsState::handshake: extract_hot_state failed"});
        }

        const std::size_t key_len = session.cipher_key_len();
        if (key_len != 16 && key_len != 32) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsCipherFailed,
                "TlsState::handshake: unsupported AEAD key length"});
        }
        auto crypto_r = ::eph::net::TlsRecordCrypto::create(*state_r, key_len);
        if (!crypto_r) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsCipherFailed,
                "TlsState::handshake: TlsRecordCrypto::create failed"});
        }
        crypto_      = std::make_unique<::eph::net::TlsRecordCrypto>(std::move(*crypto_r));
        established_ = true;
        return {};
    }

    [[nodiscard]] bool is_established() const noexcept { return established_; }

    /// @brief Decrypt as many complete TLS records as available in
    ///        `[in_ptr, in_ptr + in_len)`, append plaintext to `out`,
    ///        return the number of input bytes consumed.
    ///
    /// Records that are partially-buffered (header parses but payload not
    /// yet fully present) are not consumed — the caller leaves them for
    /// the next call.
    [[nodiscard]] std::expected<std::size_t, ::eph::core::ErrorInfo>
    process_records(const uint8_t* in_ptr, std::size_t in_len,
                     std::vector<uint8_t>& out) noexcept {
        if (!established_ || !crypto_) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsHandshakeFailed,
                "TlsState::process_records: not established"});
        }

        std::size_t consumed = 0;
        while (in_len - consumed >= ::eph::net::tls_record::kRecordHeaderLen) {
            const uint8_t* rec = in_ptr + consumed;
            uint8_t  ct;
            uint16_t payload_len;
            if (!::eph::net::tls_record::parse_record_header(rec, ct, payload_len)) {
                // Bad header — surface as cipher failure.
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::TlsRecordBad,
                    "TlsState::process_records: bad record header"});
            }
            const std::size_t total = ::eph::net::tls_record::kRecordHeaderLen + payload_len;
            if (in_len - consumed < total) break;  // partial record

            // Worst-case plaintext is `payload_len` bytes (no auth tag, no inner CT).
            const std::size_t out_off = out.size();
            out.resize(out_off + payload_len);
            uint16_t plaintext_len = 0;
            if (!crypto_->decrypt(rec, static_cast<uint16_t>(total),
                                   out.data() + out_off, plaintext_len)) {
                out.resize(out_off);  // unwind on failure
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::TlsCipherFailed,
                    "TlsState::process_records: TLS decrypt failed"});
            }
            out.resize(out_off + plaintext_len);
            consumed += total;
        }
        return consumed;
    }

    /// @brief Encrypt `data` into one or more TLS records appended to `out`.
    [[nodiscard]] std::expected<void, ::eph::core::ErrorInfo>
    encrypt_for_send(const uint8_t* data, std::size_t len,
                      std::vector<uint8_t>& out) noexcept {
        if (!established_ || !crypto_) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsHandshakeFailed,
                "TlsState::encrypt_for_send: not established"});
        }
        std::size_t off = 0;
        while (off < len) {
            const uint16_t chunk = static_cast<uint16_t>(std::min<std::size_t>(
                ::eph::net::tls_const::kMaxRecordPayload, len - off));
            const uint16_t enc_size = ::eph::net::TlsRecordCrypto::encrypted_size(chunk);
            const std::size_t out_off = out.size();
            out.resize(out_off + enc_size);
            const uint16_t written = crypto_->encrypt(data + off, chunk,
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
    // Heap-allocated so the move constructor stays trivial — TlsRecordCrypto
    // contains an EVP_AEAD_CTX which is bitwise-copyable for AES-GCM but the
    // unique_ptr indirection makes the surrounding KernelTcpStream layout
    // simpler (no inline 200-byte AEAD struct).
    std::unique_ptr<::eph::net::TlsRecordCrypto> crypto_;
    bool                                          established_ = false;
};

} // namespace eph::net::kernel::detail
