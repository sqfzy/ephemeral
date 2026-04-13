/// @file test_stream_config_validation.cpp
/// Field-level validation tests for `eph::net::kernel::StreamConfig`.
///
/// The v3.3 `StreamConfig` is a plain data struct (no member `validate()`);
/// validation happens inside `KernelTcpStream::create()` and — for embedded
/// `ProxyConfig` / `TlsConfig` — via their own `validate()` methods. These
/// tests exercise both paths:
///
///   1. Default-construction sanity (every field has a safe default).
///   2. Struct-level field assignment round-trips.
///   3. Drive-through-create validation for failure modes that the factory
///      catches (reasm_capacity==0, bad proxy, bad TLS, etc.).
///   4. `ProxyConfig::validate()` direct calls for each rule.
///   5. `TlsConfig::validate()` direct calls for each rule.
///   6. `ReconnectPolicy` clamping behavior (invalid cfg is auto-fixed).
///
/// Baseline parentage: `test_transport_config.cpp` (83 cases) — validation
/// intent ported, `TransportConfig` → `StreamConfig`, new 9.5/9.6 fields
/// (`ws_path`, `ws_host`, `ws_timeout`, `ws_extra_headers`, `proxy`) added.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/net/http.hpp"
#include "eph/net/kernel/config.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/net/proxy.hpp"
#include "eph/net/reconnect_policy.hpp"
#include "eph/net/socket_addr.hpp"

namespace ek = eph::net::kernel;
namespace en = eph::net;

using PlainStream = ek::KernelTcpStream<eph::codec::RawStreamCodec, false>;

// ---------------------------------------------------------------------------
// Lightweight loopback echo server — reused across many create() tests so
// valid-config paths can assert success without skipping the connect step.
// ---------------------------------------------------------------------------
namespace {

struct OneShotEcho {
    int         fd{-1};
    uint16_t    port{0};
    std::thread thread;

    OneShotEcho() {
        fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return;
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        sa.sin_port        = 0;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0
            || ::listen(fd, 1) != 0) {
            ::close(fd);
            fd = -1;
            return;
        }
        socklen_t len = sizeof(sa);
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &len);
        port = ::ntohs(sa.sin_port);
        thread = std::thread([this] {
            sockaddr_in cli{};
            socklen_t   clen = sizeof(cli);
            int c = ::accept(fd, reinterpret_cast<sockaddr*>(&cli), &clen);
            if (c < 0) return;
            // Immediately close — the tests only care that TCP handshake
            // succeeds or the create() path picks up a config error earlier.
            ::close(c);
        });
    }
    ~OneShotEcho() {
        // Close the listen fd first so any `accept(2)` still blocked in the
        // worker thread returns with EBADF/EINVAL; otherwise tests that
        // never successfully connect (e.g. validation-failure paths) would
        // deadlock in thread.join().
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
            fd = -1;
        }
        if (thread.joinable()) thread.join();
    }
    [[nodiscard]] en::SocketAddr addr() const noexcept {
        return en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
    }
};

// Factory for a minimally-valid StreamConfig pointed at `echo`.
ek::StreamConfig make_valid_config(const OneShotEcho& echo) {
    ek::StreamConfig cfg{};
    cfg.remote          = echo.addr();
    cfg.connect_timeout = std::chrono::milliseconds{500};
    cfg.reasm_capacity  = 16 * 1024;
    return cfg;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// 1. Default construction sanity (≥10 cases)
// ═══════════════════════════════════════════════════════════════════════

TEST(StreamConfigValidation, DefaultConstructible) {
    ek::StreamConfig cfg{};
    EXPECT_EQ(cfg.remote.port, 0);
}

TEST(StreamConfigValidation, DefaultConnectTimeoutPositive) {
    ek::StreamConfig cfg{};
    EXPECT_GT(cfg.connect_timeout.count(), 0);
}

TEST(StreamConfigValidation, DefaultReasmCapacityNonzero) {
    ek::StreamConfig cfg{};
    EXPECT_GT(cfg.reasm_capacity, 0u);
}

TEST(StreamConfigValidation, DefaultTcpNodelayTrue) {
    ek::StreamConfig cfg{};
    EXPECT_TRUE(cfg.tcp_nodelay);
}

TEST(StreamConfigValidation, DefaultReconnectConfigWellFormed) {
    ek::StreamConfig cfg{};
    EXPECT_GT(cfg.reconnect.initial_backoff.count(), 0);
    EXPECT_GE(cfg.reconnect.max_backoff, cfg.reconnect.initial_backoff);
}

TEST(StreamConfigValidation, DefaultWsPathEmpty) {
    ek::StreamConfig cfg{};
    EXPECT_TRUE(cfg.ws_path.empty());
}

TEST(StreamConfigValidation, DefaultWsHostEmpty) {
    ek::StreamConfig cfg{};
    EXPECT_TRUE(cfg.ws_host.empty());
}

TEST(StreamConfigValidation, DefaultWsExtraHeadersEmpty) {
    ek::StreamConfig cfg{};
    EXPECT_TRUE(cfg.ws_extra_headers.empty());
}

TEST(StreamConfigValidation, DefaultWsTimeoutPositive) {
    ek::StreamConfig cfg{};
    EXPECT_GT(cfg.ws_timeout.count(), 0);
}

TEST(StreamConfigValidation, DefaultProxyNotSet) {
    ek::StreamConfig cfg{};
    EXPECT_FALSE(cfg.proxy.has_value());
}

TEST(StreamConfigValidation, DefaultTlsHostnameEmpty) {
    ek::StreamConfig cfg{};
    EXPECT_TRUE(cfg.tls.hostname.empty());
}

// ═══════════════════════════════════════════════════════════════════════
// 2. Field round-trip assignment (≥10 cases)
// ═══════════════════════════════════════════════════════════════════════

TEST(StreamConfigValidation, AssignRemoteField) {
    ek::StreamConfig cfg{};
    cfg.remote = en::SocketAddr{en::Ipv4Addr{10, 0, 0, 1}, 8080};
    EXPECT_EQ(cfg.remote.port, 8080);
    EXPECT_EQ(cfg.remote.ip.octets[0], 10);
}

TEST(StreamConfigValidation, AssignConnectTimeout) {
    ek::StreamConfig cfg{};
    cfg.connect_timeout = std::chrono::milliseconds{2500};
    EXPECT_EQ(cfg.connect_timeout, std::chrono::milliseconds{2500});
}

TEST(StreamConfigValidation, AssignReasmCapacity) {
    ek::StreamConfig cfg{};
    cfg.reasm_capacity = 128 * 1024;
    EXPECT_EQ(cfg.reasm_capacity, 128u * 1024u);
}

TEST(StreamConfigValidation, AssignTcpNodelayFalse) {
    ek::StreamConfig cfg{};
    cfg.tcp_nodelay = false;
    EXPECT_FALSE(cfg.tcp_nodelay);
}

TEST(StreamConfigValidation, AssignWsPath) {
    ek::StreamConfig cfg{};
    cfg.ws_path = "/ws/btcusdt@bookTicker";
    EXPECT_EQ(cfg.ws_path, "/ws/btcusdt@bookTicker");
}

TEST(StreamConfigValidation, AssignWsHost) {
    ek::StreamConfig cfg{};
    cfg.ws_host = "stream.binance.com";
    EXPECT_EQ(cfg.ws_host, "stream.binance.com");
}

TEST(StreamConfigValidation, AssignWsExtraHeaders) {
    ek::StreamConfig cfg{};
    cfg.ws_extra_headers.push_back(en::HttpHeader{"X-API-Key", "abcd"});
    ASSERT_EQ(cfg.ws_extra_headers.size(), 1u);
    EXPECT_EQ(cfg.ws_extra_headers[0].name, "X-API-Key");
}

TEST(StreamConfigValidation, AssignWsTimeout) {
    ek::StreamConfig cfg{};
    cfg.ws_timeout = std::chrono::seconds{5};
    EXPECT_EQ(cfg.ws_timeout, std::chrono::milliseconds{5000});
}

TEST(StreamConfigValidation, AssignProxyConfig) {
    ek::StreamConfig cfg{};
    en::ProxyConfig p;
    p.host = "10.1.2.3";
    p.port = 3128;
    cfg.proxy = p;
    ASSERT_TRUE(cfg.proxy.has_value());
    EXPECT_EQ(cfg.proxy->host, "10.1.2.3");
    EXPECT_EQ(cfg.proxy->port, 3128);
}

TEST(StreamConfigValidation, AssignReconnectConfig) {
    ek::StreamConfig cfg{};
    cfg.reconnect.max_attempts = 5;
    cfg.reconnect.initial_backoff = std::chrono::milliseconds{200};
    EXPECT_EQ(cfg.reconnect.max_attempts, 5u);
    EXPECT_EQ(cfg.reconnect.initial_backoff, std::chrono::milliseconds{200});
}

TEST(StreamConfigValidation, AssignTlsHostname) {
    ek::StreamConfig cfg{};
    cfg.tls.hostname = "api.example.com";
    EXPECT_EQ(cfg.tls.hostname, "api.example.com");
}

// ═══════════════════════════════════════════════════════════════════════
// 3. Validation-through-create: reasm_capacity / remote (≥8 cases)
// ═══════════════════════════════════════════════════════════════════════

TEST(StreamConfigValidation, Create_ZeroReasmCapacity_InvalidConfig) {
    ek::StreamConfig cfg{};
    cfg.remote          = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, 1};
    cfg.reasm_capacity  = 0;
    cfg.connect_timeout = std::chrono::milliseconds{100};
    auto r = PlainStream::create(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
}

TEST(StreamConfigValidation, Create_ZeroReasm_ErrorDetailMentionsField) {
    ek::StreamConfig cfg{};
    cfg.remote          = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, 1};
    cfg.reasm_capacity  = 0;
    cfg.connect_timeout = std::chrono::milliseconds{100};
    auto r = PlainStream::create(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(std::string_view{r.error().detail}.find("reasm_capacity"),
              std::string_view::npos);
}

TEST(StreamConfigValidation, Create_ValidMinimumConfigSucceeds) {
    OneShotEcho echo;
    ASSERT_GE(echo.fd, 0);
    auto cfg = make_valid_config(echo);
    auto r = PlainStream::create(cfg);
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().detail);
}

TEST(StreamConfigValidation, Create_VerySmallTimeoutIsAccepted) {
    OneShotEcho echo;
    auto cfg = make_valid_config(echo);
    cfg.connect_timeout = std::chrono::milliseconds{50};
    auto r = PlainStream::create(cfg);
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().detail);
}

TEST(StreamConfigValidation, Create_LargeReasmCapacityAccepted) {
    OneShotEcho echo;
    auto cfg = make_valid_config(echo);
    cfg.reasm_capacity = 1 * 1024 * 1024;  // 1 MiB
    auto r = PlainStream::create(cfg);
    EXPECT_TRUE(r.has_value());
}

TEST(StreamConfigValidation, Create_TcpNodelayFalseAccepted) {
    OneShotEcho echo;
    auto cfg = make_valid_config(echo);
    cfg.tcp_nodelay = false;
    auto r = PlainStream::create(cfg);
    EXPECT_TRUE(r.has_value());
}

TEST(StreamConfigValidation, Create_PortZeroRejectedAsConnectFailure) {
    ek::StreamConfig cfg{};
    cfg.remote          = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, 0};
    cfg.reasm_capacity  = 4096;
    cfg.connect_timeout = std::chrono::milliseconds{200};
    auto r = PlainStream::create(cfg);
    EXPECT_FALSE(r.has_value());
}

TEST(StreamConfigValidation, Create_AllZeroConfigFails) {
    ek::StreamConfig cfg{};
    cfg.connect_timeout = std::chrono::milliseconds{100};
    auto r = PlainStream::create(cfg);
    EXPECT_FALSE(r.has_value());
}

// ═══════════════════════════════════════════════════════════════════════
// 4. ProxyConfig::validate() direct calls (≥10 cases)
// ═══════════════════════════════════════════════════════════════════════

TEST(StreamConfigValidation, Proxy_EmptyHostRejected) {
    en::ProxyConfig p;
    p.host = "";
    p.port = 3128;
    auto r = p.validate();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
    EXPECT_NE(std::string_view{r.error().detail}.find("host"),
              std::string_view::npos);
}

TEST(StreamConfigValidation, Proxy_ZeroPortRejected) {
    en::ProxyConfig p;
    p.host = "10.0.0.1";
    p.port = 0;
    auto r = p.validate();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
    EXPECT_NE(std::string_view{r.error().detail}.find("port"),
              std::string_view::npos);
}

TEST(StreamConfigValidation, Proxy_ValidMinimalAccepted) {
    en::ProxyConfig p;
    p.host = "10.0.0.1";
    p.port = 3128;
    EXPECT_TRUE(p.validate().has_value());
}

TEST(StreamConfigValidation, Proxy_UserWithoutPassRejected) {
    en::ProxyConfig p;
    p.host            = "10.0.0.1";
    p.port            = 3128;
    p.basic_auth_user = "alice";  // pass missing
    auto r = p.validate();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
}

TEST(StreamConfigValidation, Proxy_PassWithoutUserRejected) {
    en::ProxyConfig p;
    p.host            = "10.0.0.1";
    p.port            = 3128;
    p.basic_auth_pass = "secret";  // user missing
    auto r = p.validate();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
}

TEST(StreamConfigValidation, Proxy_BothAuthFieldsSetAccepted) {
    en::ProxyConfig p;
    p.host            = "10.0.0.1";
    p.port            = 3128;
    p.basic_auth_user = "alice";
    p.basic_auth_pass = "secret";
    EXPECT_TRUE(p.validate().has_value());
}

TEST(StreamConfigValidation, Proxy_ZeroTimeoutRejected) {
    en::ProxyConfig p;
    p.host    = "10.0.0.1";
    p.port    = 3128;
    p.timeout = std::chrono::milliseconds{0};
    auto r = p.validate();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
}

TEST(StreamConfigValidation, Proxy_NegativeTimeoutRejected) {
    en::ProxyConfig p;
    p.host    = "10.0.0.1";
    p.port    = 3128;
    p.timeout = std::chrono::milliseconds{-5};
    auto r = p.validate();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
}

TEST(StreamConfigValidation, Proxy_LargeTimeoutAccepted) {
    en::ProxyConfig p;
    p.host    = "10.0.0.1";
    p.port    = 3128;
    p.timeout = std::chrono::hours{1};
    EXPECT_TRUE(p.validate().has_value());
}

TEST(StreamConfigValidation, Proxy_CreateWithBadProxyFails) {
    OneShotEcho echo;
    auto cfg = make_valid_config(echo);
    en::ProxyConfig p;
    p.host = "";  // deliberately invalid
    p.port = 3128;
    cfg.proxy = p;
    auto r = PlainStream::create(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
}

// ═══════════════════════════════════════════════════════════════════════
// 5. TlsConfig::validate() direct calls (≥8 cases)
// ═══════════════════════════════════════════════════════════════════════

TEST(StreamConfigValidation, Tls_DefaultConfigValidates) {
    en::TlsConfig t;
    EXPECT_TRUE(t.validate().empty());
}

TEST(StreamConfigValidation, Tls_ZeroHandshakeTimeoutRejected) {
    en::TlsConfig t;
    t.handshake_timeout = std::chrono::milliseconds{0};
    EXPECT_FALSE(t.validate().empty());
}

TEST(StreamConfigValidation, Tls_NegativeHandshakeTimeoutRejected) {
    en::TlsConfig t;
    t.handshake_timeout = std::chrono::milliseconds{-1};
    EXPECT_FALSE(t.validate().empty());
}

TEST(StreamConfigValidation, Tls_ClientCertWithoutKeyRejected) {
    en::TlsConfig t;
    t.client_cert_path = "/tmp/cert.pem";
    // key_path deliberately unset
    EXPECT_FALSE(t.validate().empty());
}

TEST(StreamConfigValidation, Tls_ClientKeyWithoutCertRejected) {
    en::TlsConfig t;
    t.client_key_path = "/tmp/key.pem";
    EXPECT_FALSE(t.validate().empty());
}

TEST(StreamConfigValidation, Tls_BothMtlsPathsSetAccepted) {
    en::TlsConfig t;
    t.client_cert_path = "/tmp/cert.pem";
    t.client_key_path  = "/tmp/key.pem";
    EXPECT_TRUE(t.validate().empty());
}

TEST(StreamConfigValidation, Tls_EmptyHostnameAcceptedByValidate) {
    // Empty SNI is a warning, not a hard error (see tls warnings()).
    en::TlsConfig t;
    t.hostname = "";
    EXPECT_TRUE(t.validate().empty());
}

TEST(StreamConfigValidation, Tls_VerifyPeerFalseAccepted) {
    en::TlsConfig t;
    t.verify_peer = false;  // dangerous but not a validation error
    EXPECT_TRUE(t.validate().empty());
}

// ═══════════════════════════════════════════════════════════════════════
// 6. Reconnect policy clamping (≥5 cases)
// ═══════════════════════════════════════════════════════════════════════
//
// Unlike the baseline (which returned errors), v3.3 ReconnectPolicy clamps
// invalid values at construction so a default config is always safe.

TEST(StreamConfigValidation, Reconnect_MultiplierBelowOneClamped) {
    en::ReconnectPolicyConfig cfg{};
    cfg.multiplier = 0.5;  // invalid
    en::ReconnectPolicy p{cfg};
    EXPECT_GT(p.config().multiplier, 1.0);
}

TEST(StreamConfigValidation, Reconnect_NegativeJitterClampedToZero) {
    en::ReconnectPolicyConfig cfg{};
    cfg.jitter_factor = -0.5;
    en::ReconnectPolicy p{cfg};
    EXPECT_GE(p.config().jitter_factor, 0.0);
}

TEST(StreamConfigValidation, Reconnect_JitterAtOneClampedBelow) {
    en::ReconnectPolicyConfig cfg{};
    cfg.jitter_factor = 1.0;
    en::ReconnectPolicy p{cfg};
    EXPECT_LT(p.config().jitter_factor, 1.0);
}

TEST(StreamConfigValidation, Reconnect_MaxBelowInitialBackoffClamped) {
    en::ReconnectPolicyConfig cfg{};
    cfg.initial_backoff = std::chrono::milliseconds{500};
    cfg.max_backoff     = std::chrono::milliseconds{100};  // < initial
    en::ReconnectPolicy p{cfg};
    EXPECT_GE(p.config().max_backoff, p.config().initial_backoff);
}

TEST(StreamConfigValidation, Reconnect_MaxAttemptsZeroMeansUnlimited) {
    en::ReconnectPolicyConfig cfg{};
    cfg.max_attempts = 0;
    en::ReconnectPolicy p{cfg};
    // With max_attempts == 0, should_reconnect must remain true indefinitely.
    EXPECT_TRUE(p.should_reconnect());
}

TEST(StreamConfigValidation, Reconnect_MaxAttemptsFiniteStopsAfterN) {
    en::ReconnectPolicyConfig cfg{};
    cfg.max_attempts  = 2;
    cfg.jitter_factor = 0.0;
    en::ReconnectPolicy p{cfg};
    (void)p.next_backoff();
    (void)p.next_backoff();
    EXPECT_FALSE(p.should_reconnect());
}

// ═══════════════════════════════════════════════════════════════════════
// 7. WS / Proxy field consistency with create() (≥7 cases)
// ═══════════════════════════════════════════════════════════════════════
//
// These catch that the 9.5 ws_* and 9.6 proxy fields are actually plumbed
// into `KernelTcpStream::create()` — not just stored in the struct.

TEST(StreamConfigValidation, Ws_EmptyPathSkipsUpgradeStillSucceeds) {
    OneShotEcho echo;
    auto cfg = make_valid_config(echo);
    cfg.ws_path = "";
    auto r = PlainStream::create(cfg);
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().detail);
}

TEST(StreamConfigValidation, Ws_NonEmptyPathCausesCreateToDriveHandshake) {
    // The one-shot echo closes immediately after accept, so any request the
    // WS handshake sends will result in a handshake failure — that's the
    // signal the field is plumbed through.
    OneShotEcho echo;
    auto cfg = make_valid_config(echo);
    cfg.ws_path = "/ws/feed";
    cfg.ws_timeout = std::chrono::milliseconds{500};
    auto r = PlainStream::create(cfg);
    EXPECT_FALSE(r.has_value());
}

TEST(StreamConfigValidation, Ws_PathAndHostTogether) {
    OneShotEcho echo;
    auto cfg = make_valid_config(echo);
    cfg.ws_path = "/ws/feed";
    cfg.ws_host = "example.com";
    cfg.ws_timeout = std::chrono::milliseconds{500};
    auto r = PlainStream::create(cfg);
    // Server closes immediately → failure, but field is consumed.
    EXPECT_FALSE(r.has_value());
}

TEST(StreamConfigValidation, Ws_ExtraHeadersStored) {
    ek::StreamConfig cfg{};
    cfg.ws_extra_headers.push_back(en::HttpHeader{"Authorization", "Bearer x"});
    cfg.ws_extra_headers.push_back(en::HttpHeader{"X-Trace-Id", "abc"});
    EXPECT_EQ(cfg.ws_extra_headers.size(), 2u);
    EXPECT_EQ(cfg.ws_extra_headers[1].name, "X-Trace-Id");
}

TEST(StreamConfigValidation, Proxy_CreateWithoutProxyFieldDirectConnect) {
    OneShotEcho echo;
    auto cfg = make_valid_config(echo);
    EXPECT_FALSE(cfg.proxy.has_value());
    auto r = PlainStream::create(cfg);
    EXPECT_TRUE(r.has_value());
}

TEST(StreamConfigValidation, Proxy_NonIPv4LiteralHostIsRejectedByCreate) {
    OneShotEcho echo;
    auto cfg = make_valid_config(echo);
    en::ProxyConfig p;
    p.host = "proxy.example.com";  // not a dotted-quad
    p.port = 3128;
    cfg.proxy = p;
    auto r = PlainStream::create(cfg);
    ASSERT_FALSE(r.has_value());
    // KernelTcpStream::create refuses non-literal proxy.host.
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
}

TEST(StreamConfigValidation, Proxy_Ipv4LiteralHostReachesTcpConnect) {
    OneShotEcho echo;
    auto cfg = make_valid_config(echo);
    en::ProxyConfig p;
    p.host = "127.0.0.1";
    p.port = 1;                // reserved → TCP connect to proxy fails
    p.timeout = std::chrono::milliseconds{200};
    cfg.proxy = p;
    cfg.connect_timeout = std::chrono::milliseconds{200};
    auto r = PlainStream::create(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::ProxyConnectFailed);
}

// ═══════════════════════════════════════════════════════════════════════
// 8. PollerConfig / UdpConfig sanity (sibling configs) (≥5 cases)
// ═══════════════════════════════════════════════════════════════════════

TEST(StreamConfigValidation, PollerCfg_DefaultInitialCapacityPositive) {
    ek::PollerConfig cfg{};
    EXPECT_GT(cfg.initial_capacity, 0u);
}

TEST(StreamConfigValidation, PollerCfg_DefaultMaxEventsPositive) {
    ek::PollerConfig cfg{};
    EXPECT_GT(cfg.max_events_per_wait, 0);
}

TEST(StreamConfigValidation, PollerCfg_Assigned) {
    ek::PollerConfig cfg{};
    cfg.initial_capacity    = 128;
    cfg.max_events_per_wait = 256;
    EXPECT_EQ(cfg.initial_capacity, 128u);
    EXPECT_EQ(cfg.max_events_per_wait, 256);
}

TEST(StreamConfigValidation, UdpCfg_DefaultBindZero) {
    ek::UdpConfig cfg{};
    EXPECT_EQ(cfg.bind.port, 0);
}

TEST(StreamConfigValidation, UdpCfg_AssignBindAndBuffers) {
    ek::UdpConfig cfg{};
    cfg.bind    = en::SocketAddr{en::Ipv4Addr{0, 0, 0, 0}, 5555};
    cfg.rcv_buf = 1024 * 1024;
    cfg.snd_buf = 1024 * 1024;
    EXPECT_EQ(cfg.bind.port, 5555);
    EXPECT_EQ(cfg.rcv_buf, 1024u * 1024u);
    EXPECT_EQ(cfg.snd_buf, 1024u * 1024u);
}

TEST(StreamConfigValidation, UdpCfg_ReuseAddrAssign) {
    ek::UdpConfig cfg{};
    cfg.reuse_addr = true;
    EXPECT_TRUE(cfg.reuse_addr);
}
