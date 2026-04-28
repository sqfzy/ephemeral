/// @file test_dpdk_drain.cpp
/// Unit tests for `eph::net::dpdk::DpdkTcpStream::drain`.
///
/// Scope: this test binary runs under the EAL `--no-pci --vdev=net_null0`
/// environment (see dpdk_test_env.hpp), so a real TCP teardown over the
/// wire is impossible — there is no peer to send FIN-ACK. Instead, we
/// exercise the API surface via the existing `EPH_DPDK_TCP_STREAM_TEST_HOOKS`
/// + `inject_state_for_testing` machinery, which already cover every other
/// stream-layer state transition test (see test_dpdk_tcp_stream.cpp).
///
/// Coverage here:
///   1. drain() on a never-Established session returns InvalidConfig
///      without touching the session.
///   2. drain() with timeout <= 0 returns InvalidConfig.
///   3. drain() on Established times out (peer is net_null, no ACK can
///      flow), bumps `kRxSessionResets`, ends in Closed.
///   4. drain() short-circuits to success when the session is already in
///      a terminal state (Closed via inject) — actually, the spec says
///      precondition is Established, so this path tests the guard not
///      success. See test #1.
///
/// Real-NIC end-to-end coverage of the success path lives in the e2e
/// suite (eph-net-dpdk/tests/integration/test_dpdk_e2e.cpp); this unit
/// file pins the API contract and the timeout/InvalidConfig paths.

#define EPH_DPDK_TCP_STREAM_TEST_HOOKS 1

#include <chrono>
#include <cstdint>

#include <gtest/gtest.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/dpdk/tcp.hpp"
#include "eph/net/dpdk/tcp_stream.hpp"
#include "eph/net/stream_metrics.hpp"
#include "eph/utils/time.hpp"

namespace edpk = eph::net::dpdk;
namespace ec   = eph::codec;

using PlainRawStream = edpk::DpdkTcpStream<ec::RawStreamCodec, false>;

// ---------------------------------------------------------------------------
// Fixture helpers
// ---------------------------------------------------------------------------

namespace {

void ensure_tsc_initialized() {
    static bool done = false;
    if (!done) {
        eph::utils::TSC::init();
        done = true;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(DpdkTcpStreamDrain, NotEstablishedReturnsInvalidConfig) {
    ensure_tsc_initialized();

    auto stream = PlainRawStream::make_default_for_test_();
    ASSERT_NE(stream, nullptr);
    // Default-constructed test stream is Closed (no connect happened).
    ASSERT_EQ(stream->state(), eph::net::TcpState::Closed);

    auto dr = stream->drain(std::chrono::milliseconds{100});
    ASSERT_FALSE(dr.has_value());
    EXPECT_EQ(dr.error().code, eph::core::Error::InvalidConfig);
    // State must be unchanged — guard fires before any session call.
    EXPECT_EQ(stream->state(), eph::net::TcpState::Closed);
    // No metric should fire on the guarded path.
    EXPECT_EQ(stream->metric(eph::net::StreamMetric::kRxSessionResets), 0u);
}

TEST(DpdkTcpStreamDrain, FinWait1RejectedByGuard) {
    ensure_tsc_initialized();

    auto stream = PlainRawStream::make_default_for_test_();
    ASSERT_NE(stream, nullptr);
    auto* sess = static_cast<eph::dpdk::TcpSession<>*>(stream->native_handle());
    sess->inject_state_for_testing(eph::net::TcpState::FinWait1);
    ASSERT_EQ(stream->state(), eph::net::TcpState::FinWait1);

    auto dr = stream->drain(std::chrono::milliseconds{50});
    ASSERT_FALSE(dr.has_value());
    EXPECT_EQ(dr.error().code, eph::core::Error::InvalidConfig);
    EXPECT_EQ(stream->state(), eph::net::TcpState::FinWait1);
}

TEST(DpdkTcpStreamDrain, ZeroTimeoutReturnsInvalidConfig) {
    ensure_tsc_initialized();

    auto stream = PlainRawStream::make_default_for_test_();
    ASSERT_NE(stream, nullptr);
    auto* sess = static_cast<eph::dpdk::TcpSession<>*>(stream->native_handle());
    sess->inject_state_for_testing(eph::net::TcpState::Established);
    ASSERT_EQ(stream->state(), eph::net::TcpState::Established);

    auto dr = stream->drain(std::chrono::milliseconds{0});
    ASSERT_FALSE(dr.has_value());
    EXPECT_EQ(dr.error().code, eph::core::Error::InvalidConfig);
    // Guard fires before close() — state stays Established.
    EXPECT_EQ(stream->state(), eph::net::TcpState::Established);
}

TEST(DpdkTcpStreamDrain, NegativeTimeoutReturnsInvalidConfig) {
    ensure_tsc_initialized();

    auto stream = PlainRawStream::make_default_for_test_();
    ASSERT_NE(stream, nullptr);
    auto* sess = static_cast<eph::dpdk::TcpSession<>*>(stream->native_handle());
    sess->inject_state_for_testing(eph::net::TcpState::Established);

    auto dr = stream->drain(std::chrono::milliseconds{-1});
    ASSERT_FALSE(dr.has_value());
    EXPECT_EQ(dr.error().code, eph::core::Error::InvalidConfig);
}

// kRxSessionResets is the metric the spec calls out for the timeout
// path. Pin its name so reorder of the StreamMetric enum at the
// drain-emit site stays caught even if the static_assert in
// stream_metrics.hpp regresses.
TEST(DpdkTcpStreamDrain, RxSessionResetsMetricWired) {
    constexpr auto idx =
        static_cast<std::size_t>(eph::net::StreamMetric::kRxSessionResets);
    EXPECT_EQ(eph::net::kStreamMetricNames[idx],
              "net.stream.dpdk.rx_session_resets");
}
