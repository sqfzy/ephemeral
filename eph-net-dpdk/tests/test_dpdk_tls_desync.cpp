/// @file test_dpdk_tls_desync.cpp
/// Unit tests for `DpdkTcpStream<C, EnableTls=true>`'s TLS write-sequence
/// desync fail-fast latch (`tls_corrupt_`).
///
/// Background: `send()` calls `TlsState::encrypt_for_send(entire_payload)`
/// which advances the AEAD write sequence counter by the number of records
/// the payload produces. It then chunks the ciphertext by MSS and hands
/// each chunk to `TcpSession::send`. If a chunk fails (BufferFull / 0 /
/// error) partway, the write seq is ALREADY advanced but the peer will
/// never see the complete record series — its decrypt-side seq/nonce is
/// now permanently out of sync with ours. Every subsequent record will
/// fail AEAD-open on the peer.
///
/// The fix: latch `tls_corrupt_` on any partial-send failure, then refuse
/// further send() / process_burst_ / poll_once_ so the reconnect loop
/// replaces the TLS state entirely. This test uses the
/// `EPH_DPDK_TCP_STREAM_TEST_HOOKS`-guarded accessor
/// (`force_tls_desync_for_test_`) to drive the latch directly — exercising
/// real partial-send semantics would require a full TLS handshake against
/// a live peer (E2E scope). The decrypt-side "nonce can't be reused"
/// property is covered by the AEAD primitive tests elsewhere; here we
/// verify only the stream-level fail-fast wiring.
///
/// Coverage:
///   1. Fresh stream reports `is_tls_send_desynced() == false`.
///   2. After the latch trips, `send()` returns `Disconnected` (and NOT
///      `NotAttached`) — the desync guard runs before any precondition
///      check, so the error surface is consistent regardless of whether
///      the stream is attached or the session Established.
///   3. The latch increments `kTlsSendDesyncs` exactly once; a second
///      `force_tls_desync_for_test_` call is idempotent.
///   4. Non-TLS streams never report desynced and have no observable
///      write-seq state to corrupt.

// Enable the test-only factory + force-desync hook in `tcp_stream.hpp`.
// MUST be defined before any eph header include so the `#ifdef` sees it.
#define EPH_DPDK_TCP_STREAM_TEST_HOOKS 1

#include <cstdint>
#include <span>

#include <gtest/gtest.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/core/error.hpp"
#include "eph/net/dpdk/tcp_stream.hpp"
#include "eph/net/stream_metrics.hpp"

namespace edpk = eph::net::dpdk;
namespace ec   = eph::codec;
namespace en   = eph::net;

using TlsRawStream   = edpk::DpdkTcpStream<ec::RawStreamCodec, /*EnableTls=*/true>;
using PlainRawStream = edpk::DpdkTcpStream<ec::RawStreamCodec, /*EnableTls=*/false>;

TEST(DpdkTlsDesync, FreshStreamIsNotDesynced) {
    auto stream = TlsRawStream::make_default_for_test_();
    ASSERT_NE(stream, nullptr);
    EXPECT_FALSE(stream->is_tls_send_desynced());
    EXPECT_EQ(stream->metric(en::StreamMetric::kTlsSendDesyncs), 0u);
}

TEST(DpdkTlsDesync, LatchFlipsIsTlsSendDesynced) {
    auto stream = TlsRawStream::make_default_for_test_();
    ASSERT_NE(stream, nullptr);

    EXPECT_FALSE(stream->is_tls_send_desynced());
    stream->force_tls_desync_for_test_();
    EXPECT_TRUE(stream->is_tls_send_desynced());
    EXPECT_EQ(stream->metric(en::StreamMetric::kTlsSendDesyncs), 1u);
}

TEST(DpdkTlsDesync, SendRejectedWithDisconnectedAfterLatch) {
    auto stream = TlsRawStream::make_default_for_test_();
    ASSERT_NE(stream, nullptr);
    stream->force_tls_desync_for_test_();

    const uint8_t payload[4] = {0x61, 0x62, 0x63, 0x64};
    auto r = stream->send(std::span<const uint8_t>(payload, sizeof(payload)));
    ASSERT_FALSE(r.has_value());
    // Critical: desynced must surface as Disconnected, NOT NotAttached,
    // so the app-level reconnect policy treats it as a dead connection
    // rather than "configure me more and retry".
    EXPECT_EQ(r.error().code, eph::core::Error::Disconnected);
    if (const char* detail = r.error().detail) {
        // Message should mention desync so operators see it in logs.
        EXPECT_NE(std::string_view(detail).find("desynced"),
                  std::string_view::npos);
    }
}

TEST(DpdkTlsDesync, LatchIsIdempotent) {
    // Second force call must not double-bump the metric — the real send()
    // path returns immediately on entry if the latch is already set.
    auto stream = TlsRawStream::make_default_for_test_();
    ASSERT_NE(stream, nullptr);

    stream->force_tls_desync_for_test_();
    stream->force_tls_desync_for_test_();
    stream->force_tls_desync_for_test_();
    EXPECT_EQ(stream->metric(en::StreamMetric::kTlsSendDesyncs), 1u);
    EXPECT_TRUE(stream->is_tls_send_desynced());
}

TEST(DpdkTlsDesync, PlainStreamNeverReportsDesynced) {
    // EnableTls=false has no write-seq counter to corrupt, so the
    // accessor must always return false regardless of bookkeeping.
    auto stream = PlainRawStream::make_default_for_test_();
    ASSERT_NE(stream, nullptr);
    EXPECT_FALSE(stream->is_tls_send_desynced());
    // force_tls_desync_for_test_ is a no-op on plain streams — it lives
    // under the same macro guard but the `if constexpr (EnableTls)`
    // inside flips nothing.
    stream->force_tls_desync_for_test_();
    EXPECT_FALSE(stream->is_tls_send_desynced());
    EXPECT_EQ(stream->metric(en::StreamMetric::kTlsSendDesyncs), 0u);
}

TEST(DpdkTlsDesync, DesyncGuardRunsBeforeAttachCheck) {
    // A stream that was never attached should still surface the desync
    // error (not `NotAttached`) once latched — proves the guard ordering
    // is correct for the mid-reconnect race where send() may fire after
    // `notify_detached_()` but before the reconnect loop rebuilds state.
    auto stream = TlsRawStream::make_default_for_test_();
    ASSERT_NE(stream, nullptr);

    // Baseline: unattached + clean → NotAttached, not Disconnected.
    const uint8_t payload[1] = {0xAA};
    auto clean = stream->send(std::span<const uint8_t>(payload, 1));
    ASSERT_FALSE(clean.has_value());
    EXPECT_EQ(clean.error().code, eph::core::Error::NotAttached);

    // Latch → even unattached, desync error takes precedence.
    stream->force_tls_desync_for_test_();
    auto after = stream->send(std::span<const uint8_t>(payload, 1));
    ASSERT_FALSE(after.has_value());
    EXPECT_EQ(after.error().code, eph::core::Error::Disconnected);
}
