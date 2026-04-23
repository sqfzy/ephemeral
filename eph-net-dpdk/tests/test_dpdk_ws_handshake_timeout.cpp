/// @file test_dpdk_ws_handshake_timeout.cpp
/// Integration between `eph::net::detail::perform_ws_handshake` and the
/// DPDK-side ByteSink adapters. Verifies that when a session never
/// delivers bytes, the sink's WouldBlock semantics correctly cause the
/// outer deadline logic to trip at `ws_timeout`, returning `Error::Timeout`
/// rather than hanging.
///
/// Complements `eph-net/tests/test_ws_handshake.cpp::WsHandshake.
/// ReadDeadlineTriggersTimeout`, which covers the generic FakeByteSink
/// path. That one proves `perform_ws_handshake`'s deadline math is sound;
/// this one proves the DPDK sink's WouldBlock translation drives that
/// math correctly.
///
/// The measured wall-clock upper bound is 200 ms — generous enough to
/// absorb CI scheduling jitter without admitting a silently-hung sink.

#include <array>
#include <chrono>
#include <cstdint>

#include <gtest/gtest.h>

#include "eph/core/error.hpp"
#include "eph/net/detail/ws_handshake.hpp"
#include "eph/net/dpdk/tcp_stream.hpp"

#include "fake_ws_session.hpp"
#include "fake_ws_tls_state.hpp"

namespace edd   = ::eph::net::dpdk::detail;
namespace etest = ::eph::net::dpdk::testing;

using ::eph::core::Error;

using PlainSink = edd::PlainDpdkWsSink<etest::FakeDpdkSessionForWs>;
using TlsSink   = edd::TlsDpdkWsSink<etest::FakeDpdkSessionForWs,
                                     etest::FakeTlsStateForWs>;

namespace {

constexpr auto kHandshakeDeadline = std::chrono::milliseconds(50);

// 200 ms ceiling: perform_ws_handshake's deadline math is steady_clock
// based, sink recv does a bounded retry then returns WouldBlock, so the
// wall-clock time should sit near the 50 ms deadline plus a few retries'
// worth of overhead. 200 ms leaves 3x margin for CI jitter. Anything
// above this budget means the sink is hanging rather than cooperating
// with the external deadline — exactly the regression this test catches.
constexpr auto kWallClockCeiling = std::chrono::milliseconds(200);

} // namespace

TEST(DpdkWsHandshakeTimeout, PlainSinkBlockingSessionTripsDeadline) {
    etest::FakeDpdkSessionForWs sess;
    sess.block_forever = true;
    PlainSink sink(&sess);

    const auto t0 = std::chrono::steady_clock::now();
    auto r = ::eph::net::detail::perform_ws_handshake(
        sink, "example.com", "/ws/feed",
        std::span<const ::eph::net::HttpHeader>{}, kHandshakeDeadline);
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::Timeout)
        << "expected Timeout; got detail=" << r.error().detail;
    EXPECT_LE(elapsed, kWallClockCeiling)
        << "handshake hung instead of tripping deadline — elapsed="
        << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
        << "ms, ceiling=" << kWallClockCeiling.count() << "ms";
    // The sink DID forward the request bytes before the deadline caught
    // the read phase — prove we actually entered the recv loop.
    EXPECT_FALSE(sess.tx_captured.empty());
}

TEST(DpdkWsHandshakeTimeout, TlsSinkBlockingSessionTripsDeadline) {
    etest::FakeDpdkSessionForWs sess;
    sess.block_forever = true;
    etest::FakeTlsStateForWs tls;
    TlsSink sink(&sess, &tls);

    const auto t0 = std::chrono::steady_clock::now();
    auto r = ::eph::net::detail::perform_ws_handshake(
        sink, "example.com", "/ws/feed",
        std::span<const ::eph::net::HttpHeader>{}, kHandshakeDeadline);
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::Timeout)
        << "expected Timeout; got detail=" << r.error().detail;
    EXPECT_LE(elapsed, kWallClockCeiling)
        << "handshake hung instead of tripping deadline — elapsed="
        << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
        << "ms, ceiling=" << kWallClockCeiling.count() << "ms";
    // TLS sink wraps plaintext in a fake-encrypted record before hand-
    // off; prove bytes went out and encryption advanced through the
    // TcpSession stand-in.
    EXPECT_FALSE(sess.tx_captured.empty());
}
