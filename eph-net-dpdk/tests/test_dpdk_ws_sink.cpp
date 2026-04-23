/// @file test_dpdk_ws_sink.cpp
/// Unit tests for `PlainDpdkWsSink<Session>` and `TlsDpdkWsSink<Session, Tls>`
/// (both in `eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp`).
///
/// Verifies the sink's role as a ByteSink adapter: correct translation of
/// the underlying TcpSession / TlsState errors into the `core::ErrorInfo`
/// shape that `perform_ws_handshake` expects, including:
///   - empty-burst loop → `Error::WouldBlock` (drives external deadline)
///   - session failure → `Error::Disconnected`
///   - send returning 0 → `Error::BufferFull`
///   - decrypt failure (TLS) → error propagated verbatim
///
/// Runs pure in-memory with no threads, no real sockets, no DPDK EAL.

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "eph/core/error.hpp"
#include "eph/net/dpdk/tcp_stream.hpp"

#include "fake_ws_session.hpp"
#include "fake_ws_tls_state.hpp"

namespace edd   = ::eph::net::dpdk::detail;
namespace etest = ::eph::net::dpdk::testing;

using ::eph::core::Error;
using ::eph::core::ErrorInfo;

using PlainSink = edd::PlainDpdkWsSink<etest::FakeDpdkSessionForWs>;
using TlsSink   = edd::TlsDpdkWsSink<etest::FakeDpdkSessionForWs,
                                     etest::FakeTlsStateForWs>;

// ═══════════════════════════════════════════════════════════════════════
// Plain sink — recv / send boundary cases
// ═══════════════════════════════════════════════════════════════════════

TEST(PlainDpdkWsSink, RecvEmptyBurstReturnsWouldBlock) {
    etest::FakeDpdkSessionForWs sess;
    sess.block_forever = true;
    PlainSink sink(&sess);

    std::array<uint8_t, 128> buf{};
    auto r = sink.recv(buf.data(), buf.size());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WouldBlock);
}

TEST(PlainDpdkWsSink, RecvSessionFailureReturnsDisconnected) {
    etest::FakeDpdkSessionForWs sess;
    sess.fail_poll_rx = true;
    PlainSink sink(&sess);

    std::array<uint8_t, 128> buf{};
    auto r = sink.recv(buf.data(), buf.size());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::Disconnected);
}

TEST(PlainDpdkWsSink, SendSessionFailureReturnsDisconnected) {
    etest::FakeDpdkSessionForWs sess;
    sess.fail_send = true;
    PlainSink sink(&sess);

    const uint8_t payload[] = {'G','E','T',' ','/'};
    auto r = sink.send(std::span<const uint8_t>(payload, sizeof(payload)));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::Disconnected);
}

TEST(PlainDpdkWsSink, SendSessionReturnsZeroMapsToBufferFull) {
    etest::FakeDpdkSessionForWs sess;
    // Small MSS forces a single iteration; zero return on the first
    // call triggers the BufferFull path.
    sess.mss_val = 8;
    sess.send_returns_zero_at = 0;
    PlainSink sink(&sess);

    const uint8_t payload[] = "hello";
    auto r = sink.send(std::span<const uint8_t>(payload, sizeof(payload) - 1));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::BufferFull);
}

TEST(PlainDpdkWsSink, RecvDrainsStagedBeforePollRx) {
    etest::FakeDpdkSessionForWs sess;
    // One burst of 6 bytes will stage into the sink; caller asks for 3
    // at a time so the sink must return the last 3 from its staged
    // buffer on the second call without re-entering poll_rx.
    const uint8_t script[] = {'A','B','C','D','E','F'};
    sess.rx_script.assign(script, script + sizeof(script));
    PlainSink sink(&sess);

    std::array<uint8_t, 3> buf{};
    auto r1 = sink.recv(buf.data(), buf.size());
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(*r1, 3u);
    EXPECT_EQ(std::memcmp(buf.data(), "ABC", 3), 0);

    // Exhaust the session BEFORE the second recv so we prove the second
    // recv is served from the staged buffer, not from another poll_rx
    // burst. (FakeDpdkSessionForWs delivers in 64-B batches, so the 6-B
    // script fit in the first burst — rx_off is already at end.)
    auto r2 = sink.recv(buf.data(), buf.size());
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(*r2, 3u);
    EXPECT_EQ(std::memcmp(buf.data(), "DEF", 3), 0);
}

// ═══════════════════════════════════════════════════════════════════════
// TLS sink — recv / send boundary + error paths
// ═══════════════════════════════════════════════════════════════════════

TEST(TlsDpdkWsSink, RecvEmptyBurstReturnsWouldBlock) {
    etest::FakeDpdkSessionForWs sess;
    sess.block_forever = true;
    etest::FakeTlsStateForWs tls;
    TlsSink sink(&sess, &tls);

    std::array<uint8_t, 128> buf{};
    auto r = sink.recv(buf.data(), buf.size());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WouldBlock);
}

TEST(TlsDpdkWsSink, RecvSessionFailureReturnsDisconnected) {
    etest::FakeDpdkSessionForWs sess;
    sess.fail_poll_rx = true;
    etest::FakeTlsStateForWs tls;
    TlsSink sink(&sess, &tls);

    std::array<uint8_t, 128> buf{};
    auto r = sink.recv(buf.data(), buf.size());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::Disconnected);
}

TEST(TlsDpdkWsSink, RecvDecryptFailurePropagatesError) {
    etest::FakeDpdkSessionForWs sess;
    // Preload one record's worth of ciphertext so decrypt actually runs.
    auto record = etest::fake_tls_encode_record("hello");
    sess.rx_script.assign(record.begin(), record.end());
    etest::FakeTlsStateForWs tls;
    tls.fail_decrypt = true;   // scripted AEAD failure
    TlsSink sink(&sess, &tls);

    std::array<uint8_t, 128> buf{};
    auto r = sink.recv(buf.data(), buf.size());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::TlsCipherFailed);
}

TEST(TlsDpdkWsSink, SendEncryptFailurePropagatesError) {
    etest::FakeDpdkSessionForWs sess;
    etest::FakeTlsStateForWs tls;
    tls.fail_encrypt = true;
    TlsSink sink(&sess, &tls);

    const uint8_t payload[] = {'x','y','z'};
    auto r = sink.send(std::span<const uint8_t>(payload, sizeof(payload)));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::TlsCipherFailed);

    // Encryption failed before any bytes could reach the session.
    EXPECT_TRUE(sess.tx_captured.empty());
}

// ═══════════════════════════════════════════════════════════════════════
// FakeTlsStateForWs self-check — identity (encrypt → decrypt == original)
//
// Prevents silent fake bugs from masking real sink regressions. If this
// test ever fails, the fake's encoding is broken and the other TLS-sink
// cases can no longer be trusted.
// ═══════════════════════════════════════════════════════════════════════

TEST(FakeTlsStateForWs, IdentityEncryptDecryptRoundTrip) {
    etest::FakeTlsStateForWs tls;
    std::vector<uint8_t> cipher;
    const uint8_t plaintext[] = {'r','o','u','n','d','-','t','r','i','p'};
    auto enc = tls.encrypt_for_send(plaintext, sizeof(plaintext), cipher);
    ASSERT_TRUE(enc.has_value());

    std::vector<uint8_t> emitted;
    auto dec = tls.process_records_in_place(
        cipher.data(), cipher.size(),
        [&](uint8_t* chunk, std::size_t n) {
            emitted.insert(emitted.end(), chunk, chunk + n);
        });
    ASSERT_TRUE(dec.has_value());
    EXPECT_EQ(*dec, cipher.size());
    ASSERT_EQ(emitted.size(), sizeof(plaintext));
    EXPECT_EQ(std::memcmp(emitted.data(), plaintext, sizeof(plaintext)), 0);
}
