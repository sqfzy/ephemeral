/// @file test_kernel_tls_state.cpp
/// Phase 5: structural tests for `KernelTcpStream<C, true>`'s TLS path
/// without dragging in a live TLS server.
///
/// What this test verifies:
///   1. The `EnableTls=true` template instantiation compiles for the
///      common codec types — i.e. the new `detail::TlsState` interacts
///      cleanly with `[[no_unique_address]]` and the surrounding member
///      layout.
///   2. The `eph::net::Stream` and `eph::net::Pollable` concepts are
///      satisfied for `KernelTcpStream<C, true>` (Phase 3 only checked
///      `EnableTls=false`).
///   3. `create()` against a non-existent peer with `EnableTls=true`
///      fails with a TLS error code (the network connect itself will
///      time out — we just verify the API surface returns the expected
///      typed error rather than crashing).
///
/// A full handshake test would need a localhost mock TLS server (much
/// like `eph-transport/tests/test_tls_session.cpp`). That coverage is
/// orthogonal to the v3.3 refactor and is left to the legacy test for
/// the time being.

#include <chrono>

#include <gtest/gtest.h>

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/codec/ws_codec.hpp"
#include "eph/net/concepts.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/net/socket_addr.hpp"

namespace enk = eph::net::kernel;
namespace ec  = eph::codec;
namespace en  = eph::net;

// ─── Concept conformance for the TLS-enabled instantiation ─────────────────

using TlsRawStream = enk::KernelTcpStream<ec::RawStreamCodec, /*EnableTls=*/true>;
using TlsWsStream  = enk::KernelTcpStream<ec::WsCodec,        /*EnableTls=*/true>;

static_assert(en::Pollable<TlsRawStream>,
              "KernelTcpStream<RawStreamCodec, true> must satisfy Pollable");
static_assert(en::Stream<TlsRawStream>,
              "KernelTcpStream<RawStreamCodec, true> must satisfy Stream");
static_assert(en::Pollable<TlsWsStream>,
              "KernelTcpStream<WsCodec, true> must satisfy Pollable");
static_assert(en::Stream<TlsWsStream>,
              "KernelTcpStream<WsCodec, true> must satisfy Stream");

TEST(KernelTlsStream, AssociatedTypesPresent) {
    using S = TlsWsStream;
    static_assert(std::is_same_v<S::CodecType, ec::WsCodec>);
    static_assert(std::is_same_v<S::PacketView, enk::detail::SpanView>);
    EXPECT_TRUE((en::Stream<S>));
}

TEST(KernelTlsStream, ConnectToBlackholeFailsTypedError) {
    // Reserved TEST-NET-1 (192.0.2.0/24, RFC 5737) — guaranteed unreachable.
    enk::StreamConfig cfg{};
    cfg.remote          = en::SocketAddr{en::Ipv4Addr{192, 0, 2, 1}, 443};
    cfg.connect_timeout = std::chrono::milliseconds{100};
    cfg.tls.hostname    = "example.invalid";
    cfg.tls.verify_peer = false;

    auto r = TlsWsStream::create(cfg);
    ASSERT_FALSE(r.has_value())
        << "create() against TEST-NET-1 must not succeed";
    // We may fail in either ConnectFailed (TCP) or Timeout (poll deadline).
    // The point of the test is that the typed error path works without
    // a crash and that the TLS code at least compiles correctly when
    // EnableTls=true.
    const auto code = r.error().code;
    EXPECT_TRUE(code == eph::core::Error::ConnectFailed ||
                code == eph::core::Error::Timeout ||
                code == eph::core::Error::TlsHandshakeFailed)
        << "unexpected error code: " << static_cast<int>(code)
        << " detail=" << r.error().detail;
}
