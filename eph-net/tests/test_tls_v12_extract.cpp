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
