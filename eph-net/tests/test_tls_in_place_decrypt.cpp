/// @file test_tls_in_place_decrypt.cpp
/// End-to-end correctness for the in-place AES-GCM AEAD primitive
/// behind the zero-copy TLS path. The header under test lives at
/// `eph/net/detail/tls_inplace.hpp` and is exercised here without any
/// network or DPDK dependency.
///
/// What we verify:
///   1. Construct an `EVP_AEAD_CTX` via TlsInPlaceDecryptor and a
///      matching encryptor via the legacy `eph::net::TlsEncryptor`.
///   2. Encrypt a known plaintext into a TLS 1.3 record buffer.
///   3. Hand the same buffer to `TlsInPlaceDecryptor::open_in_place()`
///      and verify it returns the original plaintext, with `buf+5`
///      now holding the plaintext bytes (true in-place mutation).
///   4. Sequence number bookkeeping: encryptor and decryptor must agree
///      on `seq=0` for the first record.
///   5. Bad-tag rejection: a single-bit ciphertext flip must fail
///      open_in_place().
///
/// This is a structural test of the zero-copy primitive — it does not run
/// a full TLS handshake. The design constraint is that the in-place path
/// produces the same plaintext as the legacy non-in-place path.

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "eph/net/detail/tls_inplace.hpp"
#include "eph/net/detail/tls_constants.hpp"
#include "eph/net/detail/tls_encryptor.hpp"

namespace en  = eph::net;
namespace end_ = eph::net::detail;

namespace {

// Build a TlsHotState with deterministic AES-128-GCM key/IV. Both write
// and read directions use the same key (loopback test).
en::TlsHotState make_test_hot_state() {
    en::TlsHotState st{};
    // Fixed key + IV — TLS 1.3 normally derives these via HKDF, but for
    // a unit test we just need any valid key material.
    for (int i = 0; i < 16; ++i) st.write.ki.key[i] = static_cast<uint8_t>(0xA0 + i);
    for (int i = 0; i < 12; ++i) st.write.ki.iv[i]  = static_cast<uint8_t>(0xB0 + i);
    std::memcpy(st.read.ki.key, st.write.ki.key, 16);
    std::memcpy(st.read.ki.iv,  st.write.ki.iv,  12);
    st.write.seq = 0;
    st.read.seq  = 0;
    return st;
}

} // namespace

// ─── Round trip: encrypt then in-place decrypt ──────────────────────────────

TEST(TlsInPlaceDecrypt, RoundTripWithLegacyEncryptor) {
    auto state = make_test_hot_state();

    // Build the legacy encryptor against the write half.
    auto enc_r = en::TlsEncryptor::create(state, /*key_len=*/16);
    ASSERT_TRUE(enc_r.has_value()) << enc_r.error();

    // Build the in-place decryptor against the read half (same keys).
    auto dec_r = end_::TlsInPlaceDecryptor::create(
        state.read.ki.key, 16,
        state.read.ki.iv,  12,
        state.read.seq);
    ASSERT_TRUE(dec_r.has_value()) << dec_r.error().detail;

    // Encrypt a known plaintext.
    constexpr const char* kPlain = "hello, in-place world!";
    const uint16_t plain_len = static_cast<uint16_t>(std::strlen(kPlain));

    std::vector<uint8_t> record(en::TlsEncryptor::encrypted_size(plain_len));
    const uint16_t written = enc_r->encrypt(
        reinterpret_cast<const uint8_t*>(kPlain), plain_len, record.data());
    ASSERT_GT(written, 0u);
    ASSERT_EQ(written, record.size());

    // Decrypt in place. The header (first 5 bytes) is the AAD and stays
    // unchanged; the ciphertext after it is overwritten with plaintext.
    std::size_t out_len = 0;
    ASSERT_TRUE(dec_r->open_in_place(record.data(), record.size(), &out_len));
    EXPECT_EQ(out_len, plain_len);
    EXPECT_EQ(std::memcmp(record.data() + 5, kPlain, plain_len), 0)
        << "in-place plaintext does not match the original";

    // Sequence numbers should both be 1 now.
    EXPECT_EQ(dec_r->read_seq(), 1u);
}

// ─── Auth tag mismatch must fail ────────────────────────────────────────────

TEST(TlsInPlaceDecrypt, AuthTagMismatchRejected) {
    auto state = make_test_hot_state();

    auto enc_r = en::TlsEncryptor::create(state, 16);
    ASSERT_TRUE(enc_r.has_value());
    auto dec_r = end_::TlsInPlaceDecryptor::create(
        state.read.ki.key, 16,
        state.read.ki.iv,  12,
        state.read.seq);
    ASSERT_TRUE(dec_r.has_value());

    constexpr const char* kPlain = "tamper me";
    const uint16_t plain_len = static_cast<uint16_t>(std::strlen(kPlain));
    std::vector<uint8_t> record(en::TlsEncryptor::encrypted_size(plain_len));
    const uint16_t written = enc_r->encrypt(
        reinterpret_cast<const uint8_t*>(kPlain), plain_len, record.data());
    ASSERT_GT(written, 0u);

    // Flip a bit somewhere in the ciphertext + tag region.
    record[record.size() - 3] ^= 0x01;

    std::size_t out_len = 0;
    EXPECT_FALSE(dec_r->open_in_place(record.data(), record.size(), &out_len))
        << "tampered record was accepted";
    // After failure the seq counter MUST NOT advance.
    EXPECT_EQ(dec_r->read_seq(), 0u);
}

// ─── Two records back-to-back: seq numbers tick ────────────────────────────

TEST(TlsInPlaceDecrypt, SequenceNumberAdvancesAcrossRecords) {
    auto state = make_test_hot_state();

    auto enc_r = en::TlsEncryptor::create(state, 16);
    ASSERT_TRUE(enc_r.has_value());
    auto dec_r = end_::TlsInPlaceDecryptor::create(
        state.read.ki.key, 16,
        state.read.ki.iv,  12,
        state.read.seq);
    ASSERT_TRUE(dec_r.has_value());

    auto roundtrip = [&](const char* plaintext) {
        const uint16_t pl = static_cast<uint16_t>(std::strlen(plaintext));
        std::vector<uint8_t> rec(en::TlsEncryptor::encrypted_size(pl));
        const uint16_t w = enc_r->encrypt(
            reinterpret_cast<const uint8_t*>(plaintext), pl, rec.data());
        ASSERT_GT(w, 0u);
        std::size_t out_len = 0;
        ASSERT_TRUE(dec_r->open_in_place(rec.data(), rec.size(), &out_len));
        EXPECT_EQ(out_len, pl);
        EXPECT_EQ(std::memcmp(rec.data() + 5, plaintext, pl), 0);
    };

    roundtrip("first");
    EXPECT_EQ(dec_r->read_seq(), 1u);
    roundtrip("second");
    EXPECT_EQ(dec_r->read_seq(), 2u);
    roundtrip("third");
    EXPECT_EQ(dec_r->read_seq(), 3u);
}

// ─── Truncated record rejected ──────────────────────────────────────────────

TEST(TlsInPlaceDecrypt, TooShortRecordRejected) {
    auto state = make_test_hot_state();
    auto dec_r = end_::TlsInPlaceDecryptor::create(
        state.read.ki.key, 16,
        state.read.ki.iv,  12,
        state.read.seq);
    ASSERT_TRUE(dec_r.has_value());

    // Header(5) + auth tag(16) is the bare minimum; anything shorter must fail.
    std::array<uint8_t, 20> tiny{};
    std::size_t out_len = 0;
    EXPECT_FALSE(dec_r->open_in_place(tiny.data(), tiny.size(), &out_len));
}

// ─── Inner content type is correctly reported ──────────────────────────────

TEST(TlsInPlaceDecrypt, InnerContentTypeReportedForAppData) {
    auto state = make_test_hot_state();
    auto enc_r = en::TlsEncryptor::create(state, 16);
    ASSERT_TRUE(enc_r.has_value());
    auto dec_r = end_::TlsInPlaceDecryptor::create(
        state.read.ki.key, 16,
        state.read.ki.iv,  12,
        state.read.seq);
    ASSERT_TRUE(dec_r.has_value());

    // TlsEncryptor hardcodes inner_ct = 0x17 (application_data).
    constexpr const char* kPlain = "app data";
    const uint16_t pl = static_cast<uint16_t>(std::strlen(kPlain));
    std::vector<uint8_t> rec(en::TlsEncryptor::encrypted_size(pl));
    ASSERT_GT(enc_r->encrypt(reinterpret_cast<const uint8_t*>(kPlain), pl,
                              rec.data()), 0u);

    std::size_t out_len = 0;
    uint8_t inner_ct = 0;
    ASSERT_TRUE(dec_r->open_in_place(rec.data(), rec.size(), &out_len, &inner_ct));
    EXPECT_EQ(inner_ct, 0x17) << "application_data records must have inner_ct=0x17";
    EXPECT_EQ(out_len, pl);
}

// ─── Inner content type: null pointer safety ──────────────────────────────
//
// Note on "poison pill" testing: TlsEncryptor hardcodes inner_ct=0x17 and
// AEAD prevents post-encryption tampering, so we can't forge a non-0x17
// record at this layer. The inner_ct *filtering* (skip NewSessionTicket etc.)
// is tested at the TlsState layer in tls_state.hpp, not here. This test
// verifies that the inner_ct out-parameter works and that nullptr is safe.

TEST(TlsInPlaceDecrypt, InnerCtNullptrDoesNotCrash) {
    // Verify that passing inner_ct=nullptr (the default) still works.
    auto state = make_test_hot_state();
    auto enc_r = en::TlsEncryptor::create(state, 16);
    ASSERT_TRUE(enc_r.has_value());
    auto dec_r = end_::TlsInPlaceDecryptor::create(
        state.read.ki.key, 16, state.read.ki.iv, 12, state.read.seq);
    ASSERT_TRUE(dec_r.has_value());

    constexpr const char* kPlain = "null ct test";
    const uint16_t pl = static_cast<uint16_t>(std::strlen(kPlain));
    std::vector<uint8_t> rec(en::TlsEncryptor::encrypted_size(pl));
    ASSERT_GT(enc_r->encrypt(reinterpret_cast<const uint8_t*>(kPlain), pl,
                              rec.data()), 0u);

    std::size_t out_len = 0;
    // inner_ct = nullptr (default) — must not crash.
    ASSERT_TRUE(dec_r->open_in_place(rec.data(), rec.size(), &out_len));
    EXPECT_EQ(out_len, pl);
}
