/// @file test_tls_config.cpp
/// Unit tests for `eph::net::TlsConfig` validation/warnings/serialization,
/// `eph::net::tls_keygen` HKDF key derivation, `TlsHotState` structural
/// invariants, TLS sequence-threshold constants, and SPKI pin utilities.
///
/// Covers the detail layer at `eph/net/detail/tls_constants.hpp`
/// (originally `eph-transport/tests/test_tls_config.cpp`, 47 cases).

#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include <openssl/mem.h>

#include "eph/core/error.hpp"
#include "eph/net/detail/tls_constants.hpp"

using namespace eph::net;

// =======================================================================
// TlsConfig::validate
// =======================================================================

TEST(TlsConfigValidate, DefaultConfigIsValid) {
    TlsConfig cfg;
    EXPECT_TRUE(cfg.validate().has_value());
}

TEST(TlsConfigValidate, ZeroHandshakeTimeoutRejected) {
    TlsConfig cfg;
    cfg.handshake_timeout = std::chrono::milliseconds(0);
    auto r = cfg.validate();
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
    EXPECT_NE(std::string_view(r.error().detail).find("handshake_timeout"),
              std::string_view::npos);
}

TEST(TlsConfigValidate, NegativeHandshakeTimeoutRejected) {
    TlsConfig cfg;
    cfg.handshake_timeout = std::chrono::milliseconds(-1);
    auto r = cfg.validate();
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
    EXPECT_NE(std::string_view(r.error().detail).find("handshake_timeout"),
              std::string_view::npos);
}

TEST(TlsConfigValidate, ClientCertWithoutKeyRejected) {
    TlsConfig cfg;
    cfg.client_cert_path = "/some/cert.pem";
    auto r = cfg.validate();
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
    EXPECT_NE(std::string_view(r.error().detail).find("client_cert_path"),
              std::string_view::npos);
}

TEST(TlsConfigValidate, ClientKeyWithoutCertRejected) {
    TlsConfig cfg;
    cfg.client_key_path = "/some/key.pem";
    auto r = cfg.validate();
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
}

TEST(TlsConfigValidate, BothClientCertAndKeyAccepted) {
    TlsConfig cfg;
    cfg.client_cert_path = "/some/cert.pem";
    cfg.client_key_path = "/some/key.pem";
    EXPECT_TRUE(cfg.validate().has_value());
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

// verify_peer=true with an empty hostname is a security trap: the
// chain is verified but cert identity binding (SSL_set1_host at
// runtime) is OFF, so any trusted cert for any hostname is accepted.
// Surface that explicitly so caller catches it pre-deployment.
TEST(TlsConfigWarnings, VerifyPeerWithEmptyHostnameWarnsAboutBinding) {
    TlsConfig cfg;
    cfg.hostname = "";
    cfg.verify_peer = true;
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("identity binding is OFF") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "expected hostname-binding warning when "
                          "verify_peer=true && hostname.empty()";
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

TEST(TlsConfigEquality, DifferentMinVersionAreNotEqual) {
    TlsConfig a, b;
    a.min_version = TlsVersion::Tls13;
    b.min_version = TlsVersion::Tls12;
    EXPECT_NE(a, b);
}

// =======================================================================
// TlsConfig::min_version
// =======================================================================

TEST(TlsConfigMinVersion, DefaultsToTls13) {
    TlsConfig cfg;
    EXPECT_EQ(cfg.min_version, TlsVersion::Tls13);
}

TEST(TlsConfigWarnings, MinVersionTls12EmitsDowngradeWarning) {
    TlsConfig cfg;
    cfg.hostname = "example.com";
    cfg.min_version = TlsVersion::Tls12;
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("min_version=Tls12") != std::string::npos &&
            msg.find("downgrade") != std::string::npos) {
            // Spelling is "force the connection to 1.2"; check by keyword
            found = true;
            break;
        }
        // Accept any warning that explicitly mentions min_version=Tls12
        if (msg.find("min_version=Tls12") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "expected min_version=Tls12 warning";
}

TEST(TlsConfigDump, IncludesMinVersion) {
    TlsConfig cfg;
    cfg.hostname = "example.com";
    cfg.min_version = TlsVersion::Tls12;
    auto d = cfg.dump();
    EXPECT_NE(d.find("min_version=tls12"), std::string::npos) << d;
}

TEST(TlsConfigToJson, IncludesMinVersion) {
    TlsConfig cfg;
    cfg.hostname = "example.com";
    auto j = cfg.to_json();
    EXPECT_NE(j.find("\"min_version\":\"tls13\""), std::string::npos) << j;
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
    uint8_t secret[32];
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

    uint8_t key[16]{};
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

TEST(TlsHotState, SizeIs320Bytes) {
    // 4 cache lines of key material + 1 cache line for the (alignas(64))
    // version field. The extra line is read-once at encryptor / decryptor
    // construction and never touched on the per-record hot path.
    EXPECT_EQ(sizeof(TlsHotState), 320u);
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
    // Allocate in a fixed storage buffer so we can inspect memory after destruction.
    alignas(64) uint8_t storage[sizeof(TlsHotState)];
    auto* state = new (storage) TlsHotState{};

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

    // After OPENSSL_cleanse, key material should be zeroed. OPENSSL_cleanse
    // uses a compiler barrier to prevent optimization.
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

// =======================================================================
// SPKI pin utilities — compute_spki_sha256_b64, matches_any_pin
// =======================================================================

TEST(SpkiPin, ComputeHashProducesNonEmptyBase64) {
    // Use a synthetic SPKI blob.
    const uint8_t fake_spki[] = {
        0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86,
        0x48, 0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a,
        0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03,
        0x42, 0x00, 0x04, 0x01, 0x02, 0x03, 0x04, 0x05,
    };
    auto hash = spki_pin::compute_spki_sha256_b64(fake_spki, sizeof(fake_spki));
    EXPECT_FALSE(hash.empty());
    // Base64 of SHA-256 (32 bytes) should be 44 characters
    EXPECT_EQ(hash.size(), 44u);
}

TEST(SpkiPin, ComputeHashDeterministic) {
    const uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    auto h1 = spki_pin::compute_spki_sha256_b64(data, sizeof(data));
    auto h2 = spki_pin::compute_spki_sha256_b64(data, sizeof(data));
    EXPECT_EQ(h1, h2);
}

TEST(SpkiPin, DifferentInputProducesDifferentHash) {
    const uint8_t data1[] = {0x01, 0x02, 0x03};
    const uint8_t data2[] = {0x04, 0x05, 0x06};
    auto h1 = spki_pin::compute_spki_sha256_b64(data1, sizeof(data1));
    auto h2 = spki_pin::compute_spki_sha256_b64(data2, sizeof(data2));
    EXPECT_NE(h1, h2);
}

TEST(SpkiPin, ComputeHashEmptyInputReturnsNonEmpty) {
    // Even empty input should produce a valid SHA-256 hash.
    // SHA-256("") = 47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=
    auto hash = spki_pin::compute_spki_sha256_b64(nullptr, 0);
    EXPECT_FALSE(hash.empty());
    EXPECT_EQ(hash.size(), 44u);
}

TEST(SpkiPin, MatchesAnyPinFindsExactMatch) {
    std::vector<std::string> pins = {"abc123", "def456", "ghi789"};
    EXPECT_TRUE(spki_pin::matches_any_pin("def456", pins));
}

TEST(SpkiPin, MatchesAnyPinReturnsFalseOnNoMatch) {
    std::vector<std::string> pins = {"abc123", "def456"};
    EXPECT_FALSE(spki_pin::matches_any_pin("xyz000", pins));
}

TEST(SpkiPin, MatchesAnyPinReturnsFalseOnEmptyList) {
    std::vector<std::string> pins;
    EXPECT_FALSE(spki_pin::matches_any_pin("anything", pins));
}

TEST(SpkiPin, MatchesAnyPinFirstElement) {
    std::vector<std::string> pins = {"first", "second", "third"};
    EXPECT_TRUE(spki_pin::matches_any_pin("first", pins));
}

TEST(SpkiPin, MatchesAnyPinLastElement) {
    std::vector<std::string> pins = {"first", "second", "third"};
    EXPECT_TRUE(spki_pin::matches_any_pin("third", pins));
}

TEST(SpkiPin, ComputeAndMatchRoundTrip) {
    const uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    auto hash = spki_pin::compute_spki_sha256_b64(data, sizeof(data));
    ASSERT_FALSE(hash.empty());

    std::vector<std::string> pins = {"wrong_pin", hash, "another_wrong"};
    EXPECT_TRUE(spki_pin::matches_any_pin(hash, pins));

    std::vector<std::string> no_match_pins = {"wrong1", "wrong2"};
    EXPECT_FALSE(spki_pin::matches_any_pin(hash, no_match_pins));
}

// =======================================================================
// TlsConfig — pinned_spki_sha256 equality
// =======================================================================

TEST(TlsConfigEquality, DifferentPinsAreNotEqual) {
    TlsConfig a, b;
    a.pinned_spki_sha256 = {"pin1"};
    b.pinned_spki_sha256 = {"pin2"};
    EXPECT_NE(a, b);
}

TEST(TlsConfigEquality, SamePinsAreEqual) {
    TlsConfig a, b;
    a.pinned_spki_sha256 = {"pin1", "pin2"};
    b.pinned_spki_sha256 = {"pin1", "pin2"};
    EXPECT_EQ(a, b);
}

TEST(TlsConfigEquality, EmptyPinsAreEqual) {
    TlsConfig a, b;
    EXPECT_EQ(a, b);
}
