/// @file test_dpdk_tls_state.cpp
/// Phase 5: structural surface tests for `DpdkTcpStream<C, true>` after
/// the Phase 5 BLOCKER (vcpkg-openssl ↔ aws-lc TU conflict — see the
/// header note at the top of `eph/net/dpdk/tcp_stream.hpp`).
///
/// What we verify:
///   1. Concept conformance for `DpdkTcpStream<C, true>` still holds —
///      the type compiles, satisfies `Stream` and `Pollable`, and
///      exposes the expected associated types.
///   2. `create()` with `EnableTls=true` returns `TlsHandshakeFailed`
///      with the BLOCKER detail string. Phase 5.5 will turn this into
///      a real handshake.
///   3. The `MbufView` PacketView still satisfies the formal
///      `eph::core::PacketView` concept formalized by Phase 5
///      (the static_assert lives in `mbuf_view.hpp` itself; this test
///      just confirms the assertion is reachable from the test TU).
///
/// The actual zero-copy in-place AEAD primitive (the headline of Phase 5)
/// is exhaustively tested in `eph-net/tests/test_tls_in_place_decrypt.cpp`,
/// which lives in a non-DPDK TU and therefore avoids the openssl conflict.

#include <gtest/gtest.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/core/packet_view.hpp"
#include "eph/net/concepts.hpp"
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/dpdk/tcp_stream.hpp"
#include "eph/net/dpdk/detail/mbuf_view.hpp"

namespace end = eph::net::dpdk;
namespace ec  = eph::codec;
namespace en  = eph::net;

using TlsRawStream = end::DpdkTcpStream<ec::RawStreamCodec, /*EnableTls=*/true>;

// Phase 5: confirm the formal PacketView concept is satisfied at the
// test TU level (the static_assert in mbuf_view.hpp covers the header).
static_assert(eph::core::PacketView<end::detail::MbufView>,
              "Phase 5: MbufView must satisfy eph::core::PacketView");

static_assert(en::Pollable<TlsRawStream>,
              "DpdkTcpStream<RawStreamCodec, true> must satisfy Pollable");
static_assert(en::Stream<TlsRawStream>,
              "DpdkTcpStream<RawStreamCodec, true> must satisfy Stream");

TEST(DpdkTlsStream, AssociatedTypesPresent) {
    using S = TlsRawStream;
    static_assert(std::is_same_v<S::CodecType, ec::RawStreamCodec>);
    static_assert(std::is_same_v<S::PacketView, end::detail::MbufView>);
    EXPECT_TRUE((en::Stream<S>));
}

TEST(DpdkTlsStream, EmptyConfigFailsInvalidConfig) {
    end::StreamConfig cfg{};
    // pool == nullptr → InvalidConfig before TLS is even attempted.
    auto r = TlsRawStream::create(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
}
