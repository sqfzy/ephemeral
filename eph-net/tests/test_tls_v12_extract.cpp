/// @file test_tls_v12_extract.cpp
/// Stage 2 smoke for TLS 1.2 handshake + `TlsSession::extract_hot_state()`
/// dual-version key derivation.
///
/// What this verifies:
///   - `TlsConfig::min_version = Tls12` enables 1.2 negotiation in
///     `TlsSession::create` (SSL_CTX cipher list + min_proto).
///   - `extract_hot_state()` correctly identifies the negotiated version
///     and AEAD construction, populating `state.version` /
///     `state.record_format`.
///   - All three in-scope TLS 1.2 AEAD ciphers (AES-128-GCM,
///     AES-256-GCM, CHACHA20-POLY1305) end up with the right
///     `record_format` and a non-zero key.
///
/// Stage 3 will add the per-record encrypt/decrypt branches that
/// actually consume `record_format`. Stage 4 wires in mockex for full
/// end-to-end coverage.
///
/// Architecture mirrors `test_tls_no_close_notify_after_extract.cpp`:
/// in-memory MockTcp + an in-process aws-lc server using memory BIOs.
/// No sockets, no files.

#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "eph/net/detail/tls_session.hpp"
#include "eph/net/detail/tls_record.hpp"  // TlsEncryptor / TlsDecryptor / TlsRecordCrypto

namespace {

// ─── MockTcp (TcpImpl concept) ─────────────────────────────────────────
struct MockTcp {
    std::vector<uint8_t> sent_data;
    std::vector<uint8_t> rx_buffer;
    size_t rx_pos = 0;

    void feed_rx(const uint8_t* data, size_t len) {
        rx_buffer.insert(rx_buffer.end(), data, data + len);
    }

    [[nodiscard]] size_t mss() const noexcept { return 1460; }
    [[nodiscard]] bool is_established() const noexcept { return true; }

    template <typename F>
    [[nodiscard]] std::expected<uint16_t, std::string>
    poll_rx(F&& callback) {
        if (rx_pos >= rx_buffer.size()) return uint16_t{0};
        size_t available = rx_buffer.size() - rx_pos;
        uint16_t len = static_cast<uint16_t>(
            std::min(available, size_t{65535}));
        callback(rx_buffer.data() + rx_pos, len);
        rx_pos += len;
        return len;
    }

    [[nodiscard]] std::expected<size_t, std::string>
    send(const void* data, size_t len) {
        auto* p = static_cast<const uint8_t*>(data);
        sent_data.insert(sent_data.end(), p, p + len);
        return len;
    }
};

// ─── Self-signed P-256 ECDSA cert (in memory) ──────────────────────────
struct SelfSignedCert {
    EVP_PKEY* key  = nullptr;
    X509*     cert = nullptr;

    ~SelfSignedCert() {
        if (cert) X509_free(cert);
        if (key)  EVP_PKEY_free(key);
    }

    static SelfSignedCert generate() {
        SelfSignedCert sc;
        sc.key = EVP_PKEY_new();
        auto* ec_key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
        EC_KEY_generate_key(ec_key);
        EVP_PKEY_assign_EC_KEY(sc.key, ec_key);

        sc.cert = X509_new();
        X509_set_version(sc.cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(sc.cert), 1);
        X509_gmtime_adj(X509_get_notBefore(sc.cert), 0);
        X509_gmtime_adj(X509_get_notAfter(sc.cert), 365 * 24 * 3600);
        X509_set_pubkey(sc.cert, sc.key);

        auto* name = X509_get_subject_name(sc.cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
        X509_set_issuer_name(sc.cert, name);
        X509_sign(sc.cert, sc.key, EVP_sha256());
        return sc;
    }
};

// ─── In-process server that pins a specific (version, cipher) combo ────
struct PinnedServer {
    SSL_CTX* ctx = nullptr;
    SSL*     ssl = nullptr;
    BIO*     rbio = nullptr;
    BIO*     wbio = nullptr;

    ~PinnedServer() {
        if (ssl) SSL_free(ssl);
        if (ctx) SSL_CTX_free(ctx);
    }

    /// `proto_version` = TLS1_2_VERSION or TLS1_3_VERSION (pinned exactly via
    /// min == max). `cipher_list` is the OpenSSL cipher list string for
    /// TLS 1.2 (ignored for TLS 1.3 — that uses ciphersuites separately).
    bool init(const SelfSignedCert& sc, int proto_version,
              const char* cipher_list) {
        ctx = SSL_CTX_new(TLS_server_method());
        if (!ctx) return false;
        SSL_CTX_use_certificate(ctx, sc.cert);
        SSL_CTX_use_PrivateKey(ctx, sc.key);
        if (!SSL_CTX_set_min_proto_version(ctx, proto_version)) return false;
        if (!SSL_CTX_set_max_proto_version(ctx, proto_version)) return false;
        if (proto_version == TLS1_2_VERSION && cipher_list) {
            if (!SSL_CTX_set_cipher_list(ctx, cipher_list)) return false;
        }

        ssl = SSL_new(ctx);
        if (!ssl) return false;

        rbio = BIO_new(BIO_s_mem());
        wbio = BIO_new(BIO_s_mem());
        SSL_set_bio(ssl, rbio, wbio);
        SSL_set_accept_state(ssl);
        return true;
    }

    /// Feed client→server bytes, drive handshake, return server→client bytes.
    std::vector<uint8_t> step(const std::vector<uint8_t>& c2s) {
        if (!c2s.empty()) {
            BIO_write(rbio, c2s.data(), static_cast<int>(c2s.size()));
        }
        ERR_clear_error();
        SSL_do_handshake(ssl);
        std::vector<uint8_t> resp;
        char buf[4096];
        int n;
        while ((n = BIO_read(wbio, buf, sizeof(buf))) > 0) {
            resp.insert(resp.end(), buf, buf + n);
        }
        return resp;
    }
};

/// Run a complete handshake between an eph-net `TlsSession<MockTcp>`
/// client and `PinnedServer`. Returns true if the handshake converged.
bool drive_handshake(eph::net::TlsSession<MockTcp>& sess,
                     MockTcp& tcp, PinnedServer& srv) {
    for (int i = 0; i < 32; ++i) {
        tcp.sent_data.clear();
        auto h = sess.handshake();
        if (h.has_value()) return true;
        auto resp = srv.step(tcp.sent_data);
        if (!resp.empty()) tcp.feed_rx(resp.data(), resp.size());
    }
    return false;
}

eph::net::TlsConfig make_client_cfg(eph::net::TlsVersion min_v) {
    eph::net::TlsConfig cfg;
    cfg.hostname = "localhost";
    cfg.verify_peer = false;  // self-signed
    cfg.min_version = min_v;
    // Short timeout: the in-process server returns synchronously after
    // each `step()` call, so handshake convergence is bounded by the
    // BIO shuttle loop (drive_handshake), not real network latency.
    // Keeping this small avoids a multi-second wait when an iteration
    // doesn't immediately succeed.
    cfg.handshake_timeout = std::chrono::milliseconds{200};
    return cfg;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// TLS 1.2 — three AEAD ciphers
// ═══════════════════════════════════════════════════════════════════════

TEST(TlsV12Extract, Aes128Gcm) {
    auto sc = SelfSignedCert::generate();
    PinnedServer srv;
    ASSERT_TRUE(srv.init(sc, TLS1_2_VERSION,
        "ECDHE-ECDSA-AES128-GCM-SHA256"));

    MockTcp tcp;
    auto cfg = make_client_cfg(eph::net::TlsVersion::Tls12);
    auto sess_r = eph::net::TlsSession<MockTcp>::create(tcp, cfg);
    ASSERT_TRUE(sess_r.has_value()) << sess_r.error();

    ASSERT_TRUE(drive_handshake(*sess_r, tcp, srv))
        << "TLS 1.2 AES128-GCM handshake did not converge";

    auto state_r = sess_r->extract_hot_state();
    ASSERT_TRUE(state_r.has_value()) << state_r.error();

    EXPECT_EQ(state_r->version, eph::net::TlsVersion::Tls12);
    EXPECT_EQ(state_r->record_format, eph::net::TlsRecordFormat::Tls12AesGcm);
    // Key present (non-zero)
    bool has_key = false;
    for (auto b : state_r->write.ki.key) if (b != 0) { has_key = true; break; }
    EXPECT_TRUE(has_key) << "write key is all-zero — extraction probably failed";
    // 4-byte implicit IV: iv[0..3] populated, iv[4..11] zero
    bool implicit_iv_set = false;
    for (size_t i = 0; i < 4; ++i)
        if (state_r->write.ki.iv[i] != 0) { implicit_iv_set = true; break; }
    EXPECT_TRUE(implicit_iv_set);
    for (size_t i = 4; i < eph::net::tls_const::kTls13NonceLen; ++i) {
        EXPECT_EQ(state_r->write.ki.iv[i], 0)
            << "AES-GCM 1.2 must leave iv[4..11] zero (i=" << i << ")";
    }
}

TEST(TlsV12Extract, Aes256Gcm) {
    auto sc = SelfSignedCert::generate();
    PinnedServer srv;
    ASSERT_TRUE(srv.init(sc, TLS1_2_VERSION,
        "ECDHE-ECDSA-AES256-GCM-SHA384"));

    MockTcp tcp;
    auto cfg = make_client_cfg(eph::net::TlsVersion::Tls12);
    auto sess_r = eph::net::TlsSession<MockTcp>::create(tcp, cfg);
    ASSERT_TRUE(sess_r.has_value());

    ASSERT_TRUE(drive_handshake(*sess_r, tcp, srv));
    auto state_r = sess_r->extract_hot_state();
    ASSERT_TRUE(state_r.has_value()) << state_r.error();

    EXPECT_EQ(state_r->version, eph::net::TlsVersion::Tls12);
    EXPECT_EQ(state_r->record_format, eph::net::TlsRecordFormat::Tls12AesGcm);
    // 32-byte AES-256 key — last bytes of 32-byte buffer should be populated
    bool last_half_set = false;
    for (size_t i = 16; i < 32; ++i)
        if (state_r->write.ki.key[i] != 0) { last_half_set = true; break; }
    EXPECT_TRUE(last_half_set) << "AES-256 key did not populate bytes 16..31";
}

TEST(TlsV12Extract, Chacha20Poly1305) {
    auto sc = SelfSignedCert::generate();
    PinnedServer srv;
    ASSERT_TRUE(srv.init(sc, TLS1_2_VERSION,
        "ECDHE-ECDSA-CHACHA20-POLY1305"));

    MockTcp tcp;
    auto cfg = make_client_cfg(eph::net::TlsVersion::Tls12);
    auto sess_r = eph::net::TlsSession<MockTcp>::create(tcp, cfg);
    ASSERT_TRUE(sess_r.has_value());

    ASSERT_TRUE(drive_handshake(*sess_r, tcp, srv));
    auto state_r = sess_r->extract_hot_state();
    ASSERT_TRUE(state_r.has_value()) << state_r.error();

    EXPECT_EQ(state_r->version, eph::net::TlsVersion::Tls12);
    EXPECT_EQ(state_r->record_format, eph::net::TlsRecordFormat::Tls12Chacha20);
    // CHACHA20-POLY1305 in TLS 1.2 (RFC 7905) populates the full 12B IV
    // (no implicit/explicit split). All 12 bytes should be the key-block IV.
    int nonzero_iv_bytes = 0;
    for (size_t i = 0; i < eph::net::tls_const::kTls13NonceLen; ++i)
        if (state_r->write.ki.iv[i] != 0) ++nonzero_iv_bytes;
    EXPECT_GE(nonzero_iv_bytes, 4)
        << "CHACHA20 1.2 should populate the full 12B IV; "
        << "found only " << nonzero_iv_bytes << " non-zero bytes";
}

// ═══════════════════════════════════════════════════════════════════════
// TLS 1.2 record encrypt → decrypt self-roundtrip
//
// Each case constructs an encryptor and a decryptor from the SAME
// TlsHotState (so they share key+IV+seq), exercises a roundtrip on one
// or more plaintexts, and verifies plaintext equality. Because both
// paths go through our own implementation, this proves consistency
// (right AAD/nonce/wire layout between our encrypt and decrypt) — full
// spec-compliance is verified by Stage 4 e2e against mockex.
// ═══════════════════════════════════════════════════════════════════════

namespace {

struct V12RoundtripCase {
    eph::net::TlsRecordFormat format;
    size_t                    key_len;
    const char*               label;
};

void run_v12_roundtrip(const V12RoundtripCase& c, const std::string& plaintext) {
    eph::net::TlsHotState state{};
    state.version       = eph::net::TlsVersion::Tls12;
    state.record_format = c.format;

    // Deterministic key/IV bytes — content doesn't matter for roundtrip,
    // only that encryptor and decryptor see the same material.
    for (size_t i = 0; i < c.key_len; ++i) {
        state.write.ki.key[i] = static_cast<uint8_t>(0xA0 + i);
        state.read.ki.key[i]  = static_cast<uint8_t>(0xA0 + i);
    }
    // For AES-GCM 1.2 we need a 4B implicit IV in iv[0..3]; for CHACHA20
    // we need 12B. Populate the full 12B regardless — the encryptor /
    // decryptor only consume the bytes meaningful for their format.
    for (size_t i = 0; i < eph::net::tls_const::kTls13NonceLen; ++i) {
        state.write.ki.iv[i] = static_cast<uint8_t>(0x10 + i);
        state.read.ki.iv[i]  = static_cast<uint8_t>(0x10 + i);
    }
    state.write.seq = 0;
    state.read.seq  = 0;

    auto enc_r = eph::net::TlsEncryptor::create(state, c.key_len);
    ASSERT_TRUE(enc_r.has_value()) << c.label << ": " << enc_r.error();
    auto dec_r = eph::net::TlsDecryptor::create(state, c.key_len);
    ASSERT_TRUE(dec_r.has_value()) << c.label << ": " << dec_r.error();

    const uint16_t pt_len = static_cast<uint16_t>(plaintext.size());
    const uint16_t expected_size = enc_r->encrypted_size(pt_len);

    std::vector<uint8_t> record(expected_size);
    const uint16_t written = enc_r->encrypt(
        reinterpret_cast<const uint8_t*>(plaintext.data()), pt_len,
        record.data());
    ASSERT_EQ(written, expected_size)
        << c.label << ": encrypted_size mismatch (expected " << expected_size
        << ", actual written " << written << ")";

    std::vector<uint8_t> recovered(pt_len);
    uint16_t recovered_len = 0;
    uint8_t  inner_ct      = 0;
    const bool ok = dec_r->decrypt(record.data(), written,
                                    recovered.data(), recovered_len,
                                    &inner_ct);
    ASSERT_TRUE(ok) << c.label << ": decrypt failed";
    EXPECT_EQ(recovered_len, pt_len) << c.label;
    EXPECT_EQ(inner_ct, eph::net::tls_record::kContentTypeAppData) << c.label;
    EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(recovered.data()),
                                recovered_len),
              plaintext)
        << c.label << ": plaintext mismatch";
}

} // namespace

TEST(TlsV12Roundtrip, AesGcm128) {
    run_v12_roundtrip({eph::net::TlsRecordFormat::Tls12AesGcm, 16,
                       "AES-128 GCM"}, "hello, TLS 1.2 AES-128-GCM!");
}

TEST(TlsV12Roundtrip, AesGcm256) {
    run_v12_roundtrip({eph::net::TlsRecordFormat::Tls12AesGcm, 32,
                       "AES-256 GCM"}, "hello, TLS 1.2 AES-256-GCM!");
}

TEST(TlsV12Roundtrip, Chacha20Poly1305) {
    run_v12_roundtrip({eph::net::TlsRecordFormat::Tls12Chacha20, 32,
                       "CHACHA20-POLY1305"},
                      "hello, TLS 1.2 ChaCha20-Poly1305!");
}

// Multi-record sequence — verifies seq increments across consecutive records.
TEST(TlsV12Roundtrip, AesGcm128_MultiRecord) {
    eph::net::TlsHotState state{};
    state.version       = eph::net::TlsVersion::Tls12;
    state.record_format = eph::net::TlsRecordFormat::Tls12AesGcm;
    for (size_t i = 0; i < 16; ++i) {
        state.write.ki.key[i] = static_cast<uint8_t>(0xA0 + i);
        state.read.ki.key[i]  = static_cast<uint8_t>(0xA0 + i);
    }
    for (size_t i = 0; i < eph::net::tls_const::kTls13NonceLen; ++i) {
        state.write.ki.iv[i] = static_cast<uint8_t>(0x10 + i);
        state.read.ki.iv[i]  = static_cast<uint8_t>(0x10 + i);
    }

    auto enc = eph::net::TlsEncryptor::create(state, 16);
    auto dec = eph::net::TlsDecryptor::create(state, 16);
    ASSERT_TRUE(enc.has_value());
    ASSERT_TRUE(dec.has_value());

    for (int i = 0; i < 5; ++i) {
        std::string pt = "record #" + std::to_string(i);
        std::vector<uint8_t> rec(enc->encrypted_size(static_cast<uint16_t>(pt.size())));
        uint16_t w = enc->encrypt(reinterpret_cast<const uint8_t*>(pt.data()),
                                   static_cast<uint16_t>(pt.size()),
                                   rec.data());
        ASSERT_GT(w, 0u);
        std::vector<uint8_t> out(pt.size());
        uint16_t out_len = 0;
        ASSERT_TRUE(dec->decrypt(rec.data(), w, out.data(), out_len));
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(out.data()), out_len),
                  pt) << "record #" << i;
    }
    EXPECT_EQ(enc->write_seq(), 5u);
    EXPECT_EQ(dec->read_seq(),  5u);
}

// AES-GCM 1.2 wire layout sanity check: header + explicit_nonce(8) + ciphertext + tag(16).
TEST(TlsV12Roundtrip, AesGcm_WireLayout) {
    eph::net::TlsHotState state{};
    state.version       = eph::net::TlsVersion::Tls12;
    state.record_format = eph::net::TlsRecordFormat::Tls12AesGcm;
    for (size_t i = 0; i < 16; ++i) {
        state.write.ki.key[i] = 0x42;
    }
    state.write.ki.iv[0] = 0xAA;
    state.write.ki.iv[1] = 0xBB;
    state.write.ki.iv[2] = 0xCC;
    state.write.ki.iv[3] = 0xDD;

    auto enc = eph::net::TlsEncryptor::create(state, 16);
    ASSERT_TRUE(enc.has_value());

    const std::string pt = "hi";
    std::vector<uint8_t> rec(enc->encrypted_size(static_cast<uint16_t>(pt.size())));
    const uint16_t w = enc->encrypt(reinterpret_cast<const uint8_t*>(pt.data()),
                                     static_cast<uint16_t>(pt.size()),
                                     rec.data());
    ASSERT_EQ(w, 5 + 8 + 2 + 16);  // header + explicit_nonce + plaintext + tag
    // Header: 0x17, 0x03, 0x03, len_hi, len_lo
    EXPECT_EQ(rec[0], 0x17);  // application_data
    EXPECT_EQ(rec[1], 0x03);
    EXPECT_EQ(rec[2], 0x03);
    const uint16_t wire_len = (uint16_t(rec[3]) << 8) | rec[4];
    EXPECT_EQ(wire_len, 8 + 2 + 16);  // length-on-wire = explicit_nonce + pt + tag
    // First record's explicit nonce (= seq=0 big-endian) is all zeros.
    for (size_t i = 5; i < 13; ++i) EXPECT_EQ(rec[i], 0) << "explicit_nonce[" << i-5 << "]";
}

// CHACHA20 1.2 wire layout: no explicit nonce — header + ciphertext + tag.
TEST(TlsV12Roundtrip, Chacha20_WireLayout) {
    eph::net::TlsHotState state{};
    state.version       = eph::net::TlsVersion::Tls12;
    state.record_format = eph::net::TlsRecordFormat::Tls12Chacha20;
    for (size_t i = 0; i < 32; ++i) state.write.ki.key[i] = 0x11;
    for (size_t i = 0; i < 12; ++i) state.write.ki.iv[i]  = 0x22;

    auto enc = eph::net::TlsEncryptor::create(state, 32);
    ASSERT_TRUE(enc.has_value());

    const std::string pt = "hi";
    std::vector<uint8_t> rec(enc->encrypted_size(static_cast<uint16_t>(pt.size())));
    const uint16_t w = enc->encrypt(reinterpret_cast<const uint8_t*>(pt.data()),
                                     static_cast<uint16_t>(pt.size()),
                                     rec.data());
    ASSERT_EQ(w, 5 + 2 + 16);  // header + plaintext + tag (NO explicit nonce)
    EXPECT_EQ(rec[0], 0x17);
    const uint16_t wire_len = (uint16_t(rec[3]) << 8) | rec[4];
    EXPECT_EQ(wire_len, 2 + 16);
}

// ═══════════════════════════════════════════════════════════════════════
// TLS 1.3 control — verify the existing path still labels correctly
// ═══════════════════════════════════════════════════════════════════════

TEST(TlsV12Extract, Tls13ControlPathLabelsAsTls13) {
    auto sc = SelfSignedCert::generate();
    PinnedServer srv;
    // For TLS 1.3 the cipher list arg is unused; pass nullptr.
    ASSERT_TRUE(srv.init(sc, TLS1_3_VERSION, nullptr));

    MockTcp tcp;
    auto cfg = make_client_cfg(eph::net::TlsVersion::Tls13);
    auto sess_r = eph::net::TlsSession<MockTcp>::create(tcp, cfg);
    ASSERT_TRUE(sess_r.has_value());

    ASSERT_TRUE(drive_handshake(*sess_r, tcp, srv));
    auto state_r = sess_r->extract_hot_state();
    ASSERT_TRUE(state_r.has_value()) << state_r.error();

    EXPECT_EQ(state_r->version, eph::net::TlsVersion::Tls13);
    EXPECT_EQ(state_r->record_format, eph::net::TlsRecordFormat::Tls13);
}
