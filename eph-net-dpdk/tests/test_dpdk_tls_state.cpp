/// @file test_dpdk_tls_state.cpp
/// Phase 7: surface tests for `DpdkTcpStream<C, true>` now that the
/// vcpkg-openssl ↔ aws-lc TU conflict has been resolved (by deleting the
/// legacy eph-transport / eph-dpdk modules and switching `RAND_bytes` call
/// sites to `getrandom(2)`).
///
/// What we verify:
///   1. Concept conformance for `DpdkTcpStream<C, true>` — the type
///      compiles with the real aws-lc-backed TlsState, satisfies `Stream`
///      and `Pollable`, and exposes the expected associated types.
///   2. `create()` with invalid config still fails cleanly with
///      `InvalidConfig` BEFORE any TLS work happens. Real live handshakes
///      against a vfio-pci NIC + CA are covered by the DPDK E2E integration
///      suite (`test_dpdk_e2e.cpp`); a `--no-pci` unit test cannot drive a
///      real TLS handshake because TcpSession::connect needs a real peer.
///   3. The `MbufView` PacketView satisfies the formal `eph::core::PacketView`
///      concept (the static_assert lives in `mbuf_view.hpp` itself; this
///      test just confirms the assertion is reachable from the test TU).
///
/// The zero-copy in-place AEAD primitive is exhaustively tested in
/// `eph-net/tests/test_tls_in_place_decrypt.cpp`, which lives in a non-DPDK
/// TU and exercises the AES-GCM path on raw records.

#include <gtest/gtest.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/core/packet_view.hpp"
#include "eph/net/concepts.hpp"
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/dpdk/tcp_stream.hpp"
#include "eph/net/dpdk/detail/mbuf_view.hpp"

namespace edpk = eph::net::dpdk;
namespace ec  = eph::codec;
namespace en  = eph::net;

using TlsRawStream = edpk::DpdkTcpStream<ec::RawStreamCodec, /*EnableTls=*/true>;

// Phase 5: confirm the formal PacketView concept is satisfied at the
// test TU level (the static_assert in mbuf_view.hpp covers the header).
static_assert(eph::core::PacketView<edpk::detail::MbufView>,
              "Phase 5: MbufView must satisfy eph::core::PacketView");

static_assert(en::Pollable<TlsRawStream>,
              "DpdkTcpStream<RawStreamCodec, true> must satisfy Pollable");
static_assert(en::Stream<TlsRawStream>,
              "DpdkTcpStream<RawStreamCodec, true> must satisfy Stream");

TEST(DpdkTlsStream, AssociatedTypesPresent) {
    using S = TlsRawStream;
    static_assert(std::is_same_v<S::CodecType, ec::RawStreamCodec>);
    static_assert(std::is_same_v<S::PacketView, edpk::detail::MbufView>);
    EXPECT_TRUE((en::Stream<S>));
}

TEST(DpdkTlsStream, EmptyConfigFailsInvalidConfig) {
    edpk::StreamConfig cfg{};
    // pool == nullptr → InvalidConfig before TLS is even attempted.
    auto r = TlsRawStream::create(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
}
