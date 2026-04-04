/// @file test_tls_config.cpp
/// Tests for TlsConfig validation/warnings/serialization, tls_keygen key
/// derivation, and TlsHotState structural invariants.

#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include <openssl/mem.h>

#include "eph/transport/detail/tls_constants.hpp"

using namespace eph::net;

// =======================================================================
// TlsConfig::validate
// =======================================================================

TEST(TlsConfigValidate, DefaultConfigIsValid) {
    TlsConfig cfg;
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(TlsConfigValidate, ZeroHandshakeTimeoutRejected) {
    TlsConfig cfg;
    cfg.handshake_timeout = std::chrono::milliseconds(0);
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("handshake_timeout"), std::string_view::npos);
}

TEST(TlsConfigValidate, NegativeHandshakeTimeoutRejected) {
    TlsConfig cfg;
    cfg.handshake_timeout = std::chrono::milliseconds(-1);
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("handshake_timeout"), std::string_view::npos);
}

TEST(TlsConfigValidate, ClientCertWithoutKeyRejected) {
    TlsConfig cfg;
    cfg.client_cert_path = "/some/cert.pem";
    // client_key_path empty
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("client_cert_path"), std::string_view::npos);
}

TEST(TlsConfigValidate, ClientKeyWithoutCertRejected) {
    TlsConfig cfg;
    cfg.client_key_path = "/some/key.pem";
    // client_cert_path empty
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
}

TEST(TlsConfigValidate, BothClientCertAndKeyAccepted) {
    TlsConfig cfg;
    cfg.client_cert_path = "/some/cert.pem";
    cfg.client_key_path = "/some/key.pem";
    EXPECT_TRUE(cfg.validate().empty());
}

// =======================================================================
// TlsConfig::warnings
// =======================================================================

TEST(TlsConfigWarnings, NoWarningsForReasonableConfig) {
    TlsConfig cfg;
    cfg.hostname = "example.com";
    cfg.verify_peer = true;
    cfg.handshake_timeout = std::chrono::milliseconds(5000);
    auto w = cfg.warnings();
    EXPECT_TRUE(w.empty());
}

TEST(TlsConfigWarnings, VerifyPeerFalseWarns) {
    TlsConfig cfg;
    cfg.hostname = "example.com";
    cfg.verify_peer = false;
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("verify_peer") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(TlsConfigWarnings, EmptyHostnameWarns) {
    TlsConfig cfg;
    cfg.hostname = "";
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("hostname") != std::string::npos &&
            msg.find("empty") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(TlsConfigWarnings, VeryShortTimeoutWarns) {
    TlsConfig cfg;
    cfg.hostname = "example.com";
    cfg.handshake_timeout = std::chrono::milliseconds(100);
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("very short") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(TlsConfigWarnings, VeryLongTimeoutWarns) {
    TlsConfig cfg;
    cfg.hostname = "example.com";
    cfg.handshake_timeout = std::chrono::milliseconds(60000);
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("very long") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(TlsConfigWarnings, CaCertWithoutVerifyPeerWarns) {
    TlsConfig cfg;
    cfg.hostname = "example.com";
    cfg.verify_peer = false;
    cfg.ca_cert_path = "/etc/ssl/certs/ca.pem";
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("ca_cert_path") != std::string::npos &&
            msg.find("verify_peer=false") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

// =======================================================================
// TlsConfig serialization
// =======================================================================

TEST(TlsConfigDump, ContainsAllFields) {
    TlsConfig cfg;
    cfg.hostname = "ws.example.com";
    cfg.verify_peer = true;
    cfg.handshake_timeout = std::chrono::milliseconds(3000);
    auto d = cfg.dump();
    EXPECT_NE(d.find("ws.example.com"), std::string::npos);
    EXPECT_NE(d.find("verify_peer=true"), std::string::npos) << d;
    EXPECT_NE(d.find("3000"), std::string::npos);
}

TEST(TlsConfigToJson, ValidJsonFormat) {
    TlsConfig cfg;
    cfg.hostname = "test.host";
    cfg.ca_cert_path = "/ca.pem";
    cfg.verify_peer = false;
    cfg.handshake_timeout = std::chrono::milliseconds(2000);
    auto j = cfg.to_json();
    EXPECT_NE(j.find("\"hostname\":\"test.host\""), std::string::npos);
    EXPECT_NE(j.find("\"verify_peer\":false"), std::string::npos);
    EXPECT_NE(j.find("\"handshake_timeout_ms\":2000"), std::string::npos);
    EXPECT_NE(j.find("\"ca_cert_path\":\"/ca.pem\""), std::string::npos);
}

TEST(TlsConfigToJson, EscapesSpecialCharactersInHostname) {
    TlsConfig cfg;
    cfg.hostname = "host\"with\\quotes";
    auto j = cfg.to_json();
    // Should not contain unescaped quotes
    EXPECT_EQ(j.find("host\"with"), std::string::npos);
}

// =======================================================================
// TlsConfig equality
// =======================================================================

TEST(TlsConfigEquality, IdenticalConfigsAreEqual) {
    TlsConfig a, b;
    a.hostname = b.hostname = "test";
    a.verify_peer = b.verify_peer = true;
    EXPECT_EQ(a, b);
}

TEST(TlsConfigEquality, DifferentHostnamesAreNotEqual) {
    TlsConfig a, b;
    a.hostname = "foo";
    b.hostname = "bar";
    EXPECT_NE(a, b);
}

TEST(TlsConfigEquality, DifferentVerifyPeerAreNotEqual) {
    TlsConfig a, b;
    a.verify_peer = true;
    b.verify_peer = false;
    EXPECT_NE(a, b);
}

// =======================================================================
// TlsConfig formatter
// =======================================================================

TEST(TlsConfigFormatter, FormatsViaDump) {
    TlsConfig cfg;
    cfg.hostname = "fmt.test";
    auto result = std::format("{}", cfg);
    EXPECT_NE(result.find("fmt.test"), std::string::npos);
}

// =======================================================================
// tls_keygen — HKDF key derivation
// =======================================================================

TEST(TlsKeygen, DeriveKeyIvProducesNonZeroOutput) {
    // Use a known secret (32 bytes for SHA-256)
    uint8_t secret[32];
    for (size_t i = 0; i < sizeof(secret); ++i)
        secret[i] = static_cast<uint8_t>(i + 1);

    uint8_t key[tls_const::kAes256KeyLen]{};
    uint8_t iv[tls_const::kTls13NonceLen]{};

    bool ok = tls_keygen::derive_key_iv(secret, sizeof(secret),
                                        key, sizeof(key), iv, sizeof(iv));
    EXPECT_TRUE(ok);

    // Derived key should not be all zeros
    bool key_all_zero = true;
    for (auto b : key) { if (b != 0) { key_all_zero = false; break; } }
    EXPECT_FALSE(key_all_zero);

    // Derived IV should not be all zeros
    bool iv_all_zero = true;
    for (auto b : iv) { if (b != 0) { iv_all_zero = false; break; } }
    EXPECT_FALSE(iv_all_zero);
}

TEST(TlsKeygen, DeriveKeyIvDeterministic) {
    uint8_t secret[32];
    for (size_t i = 0; i < sizeof(secret); ++i)
        secret[i] = static_cast<uint8_t>(i + 1);

    uint8_t key1[tls_const::kAes256KeyLen]{}, iv1[tls_const::kTls13NonceLen]{};
    uint8_t key2[tls_const::kAes256KeyLen]{}, iv2[tls_const::kTls13NonceLen]{};

    ASSERT_TRUE(tls_keygen::derive_key_iv(secret, sizeof(secret),
                                          key1, sizeof(key1), iv1, sizeof(iv1)));
    ASSERT_TRUE(tls_keygen::derive_key_iv(secret, sizeof(secret),
                                          key2, sizeof(key2), iv2, sizeof(iv2)));

    EXPECT_EQ(std::memcmp(key1, key2, sizeof(key1)), 0);
    EXPECT_EQ(std::memcmp(iv1, iv2, sizeof(iv1)), 0);
}

TEST(TlsKeygen, DifferentSecretsProduceDifferentKeys) {
    uint8_t secret1[32], secret2[32];
    for (size_t i = 0; i < 32; ++i) {
        secret1[i] = static_cast<uint8_t>(i + 1);
        secret2[i] = static_cast<uint8_t>(i + 100);
    }

    uint8_t key1[tls_const::kAes256KeyLen]{}, iv1[tls_const::kTls13NonceLen]{};
    uint8_t key2[tls_const::kAes256KeyLen]{}, iv2[tls_const::kTls13NonceLen]{};

    ASSERT_TRUE(tls_keygen::derive_key_iv(secret1, sizeof(secret1),
                                          key1, sizeof(key1), iv1, sizeof(iv1)));
    ASSERT_TRUE(tls_keygen::derive_key_iv(secret2, sizeof(secret2),
                                          key2, sizeof(key2), iv2, sizeof(iv2)));

    EXPECT_NE(std::memcmp(key1, key2, sizeof(key1)), 0);
}

TEST(TlsKeygen, DeriveKeyIvWith48ByteSecretUsesSha384) {
    // 48-byte secret triggers SHA-384 code path
    uint8_t secret[48];
    for (size_t i = 0; i < sizeof(secret); ++i)
        secret[i] = static_cast<uint8_t>(i + 1);

    uint8_t key[tls_const::kAes256KeyLen]{};
    uint8_t iv[tls_const::kTls13NonceLen]{};

    bool ok = tls_keygen::derive_key_iv(secret, sizeof(secret),
                                        key, sizeof(key), iv, sizeof(iv));
    EXPECT_TRUE(ok);

    bool key_all_zero = true;
    for (auto b : key) { if (b != 0) { key_all_zero = false; break; } }
    EXPECT_FALSE(key_all_zero);
}

TEST(TlsKeygen, DeriveAes128Key) {
    uint8_t secret[32];
    for (size_t i = 0; i < sizeof(secret); ++i)
        secret[i] = static_cast<uint8_t>(i + 1);

    uint8_t key[16]{};  // AES-128
    uint8_t iv[tls_const::kTls13NonceLen]{};

    bool ok = tls_keygen::derive_key_iv(secret, sizeof(secret),
                                        key, sizeof(key), iv, sizeof(iv));
    EXPECT_TRUE(ok);

    bool key_all_zero = true;
    for (auto b : key) { if (b != 0) { key_all_zero = false; break; } }
    EXPECT_FALSE(key_all_zero);
}

// =======================================================================
// TlsHotState — structural invariants
// =======================================================================

TEST(TlsHotState, SizeIs256Bytes) {
    EXPECT_EQ(sizeof(TlsHotState), 256u);
}

TEST(TlsKeyIv, SizeIs64Bytes) {
    EXPECT_EQ(sizeof(TlsKeyIv), 64u);
}

TEST(TlsKeyMaterial, SizeIs128Bytes) {
    EXPECT_EQ(sizeof(TlsKeyMaterial), 128u);
}

TEST(TlsHotState, AccessorsReturnCorrectPointers) {
    TlsHotState state{};
    state.write.ki.key[0] = 0xAA;
    state.read.ki.key[0] = 0xBB;
    state.write.ki.iv[0] = 0xCC;
    state.read.ki.iv[0] = 0xDD;

    EXPECT_EQ(state.write_key()[0], 0xAA);
    EXPECT_EQ(state.read_key()[0], 0xBB);
    EXPECT_EQ(state.write_iv()[0], 0xCC);
    EXPECT_EQ(state.read_iv()[0], 0xDD);
}

TEST(TlsHotState, ConstAccessorsWork) {
    TlsHotState state{};
    state.write.ki.key[0] = 0x42;
    const auto& cstate = state;
    EXPECT_EQ(cstate.write_key()[0], 0x42);
}

TEST(TlsHotState, DestructorScrubsKeyMaterial) {
    // Allocate on heap so we can inspect memory after destruction
    alignas(64) uint8_t storage[sizeof(TlsHotState)];
    auto* state = new (storage) TlsHotState{};

    // Fill with known pattern
    for (size_t i = 0; i < tls_const::kAes256KeyLen; ++i) {
        state->write.ki.key[i] = static_cast<uint8_t>(0xAA);
        state->read.ki.key[i] = static_cast<uint8_t>(0xBB);
    }
    for (size_t i = 0; i < tls_const::kTls13NonceLen; ++i) {
        state->write.ki.iv[i] = static_cast<uint8_t>(0xCC);
        state->read.ki.iv[i] = static_cast<uint8_t>(0xDD);
    }

    // Destroy (should cleanse)
    state->~TlsHotState();

    // After OPENSSL_cleanse, key material should be zeroed
    // (OPENSSL_cleanse uses a compiler barrier to prevent optimization)
    bool write_key_zeroed = true;
    bool read_key_zeroed = true;
    for (size_t i = 0; i < tls_const::kAes256KeyLen; ++i) {
        if (storage[offsetof(TlsHotState, write) + offsetof(TlsKeyMaterial, ki) +
                    offsetof(TlsKeyIv, key) + i] != 0)
            write_key_zeroed = false;
        if (storage[offsetof(TlsHotState, read) + offsetof(TlsKeyMaterial, ki) +
                    offsetof(TlsKeyIv, key) + i] != 0)
            read_key_zeroed = false;
    }
    EXPECT_TRUE(write_key_zeroed) << "Write key was not scrubbed by destructor";
    EXPECT_TRUE(read_key_zeroed) << "Read key was not scrubbed by destructor";
}

TEST(TlsHotState, InitialSequenceNumbersAreZero) {
    TlsHotState state{};
    EXPECT_EQ(state.write.seq, 0u);
    EXPECT_EQ(state.read.seq, 0u);
}

// =======================================================================
// tls_record constants — sanity checks
// =======================================================================

TEST(TlsRecordConstants, SequenceThresholdsAreConsistent) {
    EXPECT_LT(tls_record::kSequenceWarnThreshold,
              tls_record::kSequenceReconnectThreshold);
    EXPECT_LT(tls_record::kSequenceReconnectThreshold,
              tls_record::kMaxSequenceNumber);
}

TEST(TlsRecordConstants, WarnThresholdIs90Percent) {
    EXPECT_EQ(tls_record::kSequenceWarnThreshold,
              tls_record::kMaxSequenceNumber * 9 / 10);
}

TEST(TlsRecordConstants, ReconnectThresholdIs95Percent) {
    EXPECT_EQ(tls_record::kSequenceReconnectThreshold,
              tls_record::kMaxSequenceNumber * 95 / 100);
}
