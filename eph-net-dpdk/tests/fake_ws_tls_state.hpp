/// @file fake_ws_tls_state.hpp
/// Test-only fake substituting the real `::eph::net::dpdk::detail::TlsState`
/// in the templated `TlsDpdkWsSink<Session, Tls>`. Implements an **identity
/// encoding** instead of AES-GCM — cipher bytes are `plaintext` wrapped in a
/// 2-byte length prefix, and `process_records_in_place` reverses the wrap.
///
/// This is deliberately NOT a TLS implementation. The sink's contract with
/// `TlsState` is:
///   - `encrypt_for_send(plain, len, out_vec)` appends some bytes to out_vec
///   - `process_records_in_place(buf, len, emit)` consumes some prefix of
///     buf and invokes `emit(plaintext_ptr, plaintext_len)` zero or more
///     times; returns consumed-byte count
///
/// That contract is what the sink depends on — the cipher content is opaque
/// to the sink. Identity encoding lets tests reason about exact byte counts
/// without dragging aws-lc into the test binary.
///
/// Scripted failure injection via `fail_encrypt` / `fail_decrypt` exercises
/// the sink's error-propagation path.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include "eph/core/error.hpp"

namespace eph::net::dpdk::testing {

/// 2-byte big-endian length prefix marking one fake "record".
inline constexpr std::size_t kFakeRecordHeaderLen = 2;

struct FakeTlsStateForWs {
    /// Must evaluate to `false` if any compile-time check downstream asks
    /// for the real TlsState. The real `TlsState` defines
    /// `kIsRealTlsState = true`; we advertise the opposite so any
    /// static_assert guarding aws-lc wiring would fire if this fake ever
    /// leaked into production paths.
    static constexpr bool kIsRealTlsState = false;

    bool fail_encrypt{false};
    bool fail_decrypt{false};

    std::expected<void, ::eph::core::ErrorInfo>
    encrypt_for_send(const uint8_t* data, std::size_t len,
                     std::vector<uint8_t>& out) noexcept {
        if (fail_encrypt) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsCipherFailed,
                "FakeTlsStateForWs::encrypt_for_send: scripted failure"});
        }
        // Frame: [len_hi][len_lo][plaintext...].
        out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(len & 0xFF));
        out.insert(out.end(), data, data + len);
        return {};
    }

    template <class Emit>
    std::expected<std::size_t, ::eph::core::ErrorInfo>
    process_records_in_place(uint8_t* buf, std::size_t len, Emit&& emit) noexcept {
        if (fail_decrypt) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsCipherFailed,
                "FakeTlsStateForWs::process_records_in_place: scripted failure"});
        }
        std::size_t consumed = 0;
        while (len - consumed >= kFakeRecordHeaderLen) {
            const std::size_t header_off = consumed;
            const std::size_t pt_len =
                (static_cast<std::size_t>(buf[header_off]) << 8) |
                 static_cast<std::size_t>(buf[header_off + 1]);
            const std::size_t total = kFakeRecordHeaderLen + pt_len;
            if (len - consumed < total) break;  // partial record
            if (pt_len > 0) {
                emit(buf + header_off + kFakeRecordHeaderLen, pt_len);
            }
            consumed += total;
        }
        return consumed;
    }
};

/// Encode one identity record from a string_view. Useful for tests that
/// want to script the sink's RX side with a known cipher stream.
[[nodiscard]] inline std::vector<uint8_t>
fake_tls_encode_record(std::string_view plaintext) {
    std::vector<uint8_t> out;
    out.reserve(kFakeRecordHeaderLen + plaintext.size());
    out.push_back(static_cast<uint8_t>((plaintext.size() >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(plaintext.size() & 0xFF));
    out.insert(out.end(), plaintext.begin(), plaintext.end());
    return out;
}

} // namespace eph::net::dpdk::testing
