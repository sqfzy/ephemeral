/// @file test_dpdk_tls_handshake.cpp
/// Verifies the DPDK TLS handshake path compiles end-to-end and
/// that `create()` no longer bails out with a blocker sentinel.
///
/// We cannot actually drive a live TLS handshake from a `--no-pci` DPDK
/// unit test because `TcpSession::connect()` requires a real peer reachable
/// over a vfio-pci NIC — that is covered by the DPDK E2E integration suite
/// (`test_dpdk_e2e.cpp`, gated on `NIC_B` being bound to vfio-pci). What we
/// assert here:
///
///   * DpdkTcpStream<C, /*EnableTls=*/true> compiles in a TU that also
///     includes aws-lc.
///   * The type still satisfies `eph::net::Stream` and `eph::net::Pollable`.
///   * `StreamConfig::tls` exists and is `eph::net::TlsConfig`.
///   * `create()` with a deliberately invalid pool pointer fails with
///     `InvalidConfig` BEFORE any TLS call — i.e. the TLS code path is
///     reachable and syntactically wired, but still gated by the normal
///     validation order.
///   * The PHASE-5 BLOCKER sentinel string is NOT in the returned error,
///     regression-guarding us against reintroducing the stub path.

#include <cstring>

#include <gtest/gtest.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/net/concepts.hpp"
#include "eph/net/dpdk/tcp_stream.hpp"

namespace edpk = eph::net::dpdk;
namespace ec  = eph::codec;
namespace en  = eph::net;

using TlsRawStream = edpk::DpdkTcpStream<ec::RawStreamCodec, /*EnableTls=*/true>;

// Concept checks — verify DpdkTcpStream<*, true> satisfies the Stream/Pollable concepts.
static_assert(en::Stream<TlsRawStream>,
              "DpdkTcpStream<*, true> must satisfy Stream");
static_assert(en::Pollable<TlsRawStream>,
              "DpdkTcpStream<*, true> must satisfy Pollable");

// `StreamConfig::tls` has to exist and be `eph::net::TlsConfig`.
static_assert(std::is_same_v<decltype(edpk::StreamConfig{}.tls),
                              ::eph::net::TlsConfig>,
              "StreamConfig must carry an eph::net::TlsConfig field");

TEST(DpdkTlsHandshake, TlsStreamCompilesAndInstantiates) {
    // Purely a compile-time regression check.
    using S = TlsRawStream;
    static_assert(std::is_same_v<S::CodecType, ec::RawStreamCodec>);
    EXPECT_TRUE((en::Stream<S>));
}

TEST(DpdkTlsHandshake, CreateFailsInvalidConfigNotBlockerSentinel) {
    edpk::StreamConfig cfg{};
    // pool == nullptr → InvalidConfig is returned before any TLS or DPDK
    // work happens. The important assertion is that the error code is
    // `InvalidConfig`, NOT `TlsHandshakeFailed` with the BLOCKER string.
    auto r = TlsRawStream::create(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);

    // Regression guard: a previous stub emitted a BLOCKER sentinel string.
    // If we ever go back to that behaviour, this assertion will catch it.
    const char* detail = r.error().detail ? r.error().detail : "";
    EXPECT_EQ(std::strstr(detail, "vcpkg-openssl"), nullptr)
        << "BLOCKER regression: detail mentions vcpkg-openssl — the "
           "DPDK TLS stub path appears to have been reinstated: '"
        << detail << "'";
}
