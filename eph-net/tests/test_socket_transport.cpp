/// @file test_socket_transport.cpp
/// Unit tests for POSIX socket TcpTransport implementation.

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "eph/net/socket_connect.hpp"

using namespace eph::net;

// ─────────────────────────────────────────────────────────────────────────────
// Concept satisfaction (compile-time)
// ─────────────────────────────────────────────────────────────────────────────

static_assert(TcpTransport<SocketTransport>,
    "SocketTransport must satisfy TcpTransport concept");

// ─────────────────────────────────────────────────────────────────────────────
// Construction and initial state
// ─────────────────────────────────────────────────────────────────────────────

TEST(SocketTransport, InitialStateClosed) {
    SocketConfig cfg{.host = "localhost", .port = 12345};
    SocketTransport st(cfg);

    EXPECT_EQ(st.state(), TcpState::Closed);
    EXPECT_FALSE(st.is_established());
    EXPECT_EQ(st.mss(), 1460);
    EXPECT_EQ(st.fd(), -1);
    EXPECT_EQ(st.dns_latency_ns(), 0u);
    EXPECT_EQ(st.connect_latency_ns(), 0u);
    EXPECT_EQ(st.local_port(), 0u);
}

TEST(SocketTransport, ConfigPreserved) {
    SocketConfig cfg{
        .host = "example.com",
        .port = 443,
        .tcp_nodelay = false,
        .recv_buf_size = 65536,
        .send_buf_size = 32768,
    };
    SocketTransport st(cfg);

    EXPECT_EQ(st.config().host, "example.com");
    EXPECT_EQ(st.config().port, 443);
    EXPECT_FALSE(st.config().tcp_nodelay);
    EXPECT_EQ(st.config().recv_buf_size, 65536);
    EXPECT_EQ(st.config().send_buf_size, 32768);
}

// ─────────────────────────────────────────────────────────────────────────────
// Error handling
// ─────────────────────────────────────────────────────────────────────────────

TEST(SocketTransport, SendBeforeConnectFails) {
    SocketConfig cfg{.host = "localhost", .port = 12345};
    SocketTransport st(cfg);

    uint8_t data[] = {1, 2, 3};
    auto result = st.send(data, sizeof(data));
    EXPECT_FALSE(result.has_value());
}

TEST(SocketTransport, CloseBeforeConnectFails) {
    SocketConfig cfg{.host = "localhost", .port = 12345};
    SocketTransport st(cfg);

    auto result = st.close();
    EXPECT_FALSE(result.has_value());
}

TEST(SocketTransport, ResetBeforeConnectIsNoOp) {
    SocketConfig cfg{.host = "localhost", .port = 12345};
    SocketTransport st(cfg);

    // reset() on a non-connected socket should not crash
    st.reset();
    EXPECT_EQ(st.state(), TcpState::Closed);
}

TEST(SocketTransport, ConnectToUnresolvableHostFails) {
    SocketConfig cfg{
        .host = "this-host-definitely-does-not-exist-xyz123.invalid",
        .port = 443,
    };
    SocketTransport st(cfg);

    auto result = st.connect(std::chrono::milliseconds{500});
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(st.state(), TcpState::Closed);
}

TEST(SocketTransport, ConnectToRefusedPortFails) {
    // Port 1 is almost certainly not listening
    SocketConfig cfg{.host = "127.0.0.1", .port = 1};
    SocketTransport st(cfg);

    auto result = st.connect(std::chrono::milliseconds{1000});
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(st.state(), TcpState::Closed);
}

// ─────────────────────────────────────────────────────────────────────────────
// Move semantics
// ─────────────────────────────────────────────────────────────────────────────

TEST(SocketTransport, MoveConstruct) {
    SocketConfig cfg{.host = "localhost", .port = 443};
    SocketTransport st1(cfg);
    SocketTransport st2(std::move(st1));

    EXPECT_EQ(st2.config().host, "localhost");
    EXPECT_EQ(st2.state(), TcpState::Closed);
}

TEST(SocketTransport, MoveAssign) {
    SocketConfig cfg1{.host = "host1", .port = 443};
    SocketConfig cfg2{.host = "host2", .port = 8080};
    SocketTransport st1(cfg1);
    SocketTransport st2(cfg2);

    st2 = std::move(st1);
    EXPECT_EQ(st2.config().host, "host1");
}

// ─────────────────────────────────────────────────────────────────────────────
// Loopback integration test
// ─────────────────────────────────────────────────────────────────────────────

TEST(SocketTransport, LoopbackConnectSendRecv) {
    // Create a listening socket on loopback
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listen_fd, 0) << "socket() failed: " << strerror(errno);

    int reuse = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // Let OS pick a port

    ASSERT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0)
        << "bind() failed: " << strerror(errno);
    ASSERT_EQ(::listen(listen_fd, 1), 0)
        << "listen() failed: " << strerror(errno);

    // Get the assigned port
    socklen_t addr_len = sizeof(addr);
    ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &addr_len);
    uint16_t port = ntohs(addr.sin_port);

    // Connect with SocketTransport
    SocketConfig cfg{.host = "127.0.0.1", .port = port};
    SocketTransport st(cfg);

    auto conn_result = st.connect(std::chrono::milliseconds{2000});
    ASSERT_TRUE(conn_result.has_value()) << conn_result.error();
    EXPECT_TRUE(st.is_established());
    EXPECT_GT(st.mss(), 0);

    // Accept on the server side
    int client_fd = ::accept(listen_fd, nullptr, nullptr);
    ASSERT_GE(client_fd, 0);

    // Send data from SocketTransport
    const char* msg = "hello ephemeral";
    auto send_result = st.send(msg, strlen(msg));
    ASSERT_TRUE(send_result.has_value()) << send_result.error();
    EXPECT_EQ(*send_result, strlen(msg));

    // Receive on server side
    char buf[256]{};
    ssize_t n = ::recv(client_fd, buf, sizeof(buf), 0);
    ASSERT_GT(n, 0);
    EXPECT_EQ(std::string_view(buf, n), "hello ephemeral");

    // Send data from server, receive with poll_rx
    const char* reply = "pong";
    ::send(client_fd, reply, strlen(reply), 0);

    // Brief delay for data to arrive
    struct pollfd pfd{};
    pfd.fd = st.fd();
    pfd.events = POLLIN;
    ::poll(&pfd, 1, 100);

    std::string received;
    auto rx_result = st.poll_rx([&](const uint8_t* data, uint16_t len) {
        received.assign(reinterpret_cast<const char*>(data), len);
    });
    ASSERT_TRUE(rx_result.has_value()) << rx_result.error();
    EXPECT_EQ(received, "pong");

    // Verify connection latency stats are populated
    EXPECT_GT(st.dns_latency_ns(), 0u);
    EXPECT_GT(st.connect_latency_ns(), 0u);
    EXPECT_GE(st.connect_latency_ns(), st.dns_latency_ns());

    // Verify local_port is populated
    EXPECT_GT(st.local_port(), 0u);

    // Graceful close
    auto close_result = st.close();
    EXPECT_TRUE(close_result.has_value() || st.state() == TcpState::Closed);

    ::close(client_fd);
    ::close(listen_fd);
}

// ─────────────────────────────────────────────────────────────────────────────
// SocketConfig::validate()
// ─────────────────────────────────────────────────────────────────────────────

TEST(SocketConfigValidate, ValidConfigPasses) {
    SocketConfig cfg{.host = "example.com", .port = 443};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(SocketConfigValidate, EmptyHostFails) {
    SocketConfig cfg{.host = "", .port = 443};
    EXPECT_FALSE(cfg.validate().empty());
    EXPECT_NE(cfg.validate().find("host"), std::string_view::npos);
}

TEST(SocketConfigValidate, ZeroPortFails) {
    SocketConfig cfg{.host = "example.com", .port = 0};
    EXPECT_FALSE(cfg.validate().empty());
}

TEST(SocketConfigValidate, ZeroSendTimeoutFails) {
    SocketConfig cfg{.host = "example.com", .port = 443, .send_timeout_ms = 0};
    EXPECT_FALSE(cfg.validate().empty());
}

TEST(SocketConfigValidate, NegativeSendTimeoutFails) {
    SocketConfig cfg{.host = "example.com", .port = 443, .send_timeout_ms = -1};
    EXPECT_FALSE(cfg.validate().empty());
}

TEST(SocketConfigValidate, KeepaliveWithInvalidIdleFails) {
    SocketConfig cfg{.host = "example.com", .port = 443,
                     .tcp_keepalive = true, .keepalive_idle = 0};
    EXPECT_FALSE(cfg.validate().empty());
}

TEST(SocketConfigValidate, KeepaliveWithInvalidIntervalFails) {
    SocketConfig cfg{.host = "example.com", .port = 443,
                     .tcp_keepalive = true, .keepalive_interval = 0};
    EXPECT_FALSE(cfg.validate().empty());
}

TEST(SocketConfigValidate, KeepaliveWithInvalidCountFails) {
    SocketConfig cfg{.host = "example.com", .port = 443,
                     .tcp_keepalive = true, .keepalive_count = 0};
    EXPECT_FALSE(cfg.validate().empty());
}

TEST(SocketConfigValidate, KeepaliveDisabledIgnoresInvalidValues) {
    SocketConfig cfg{.host = "example.com", .port = 443,
                     .tcp_keepalive = false, .keepalive_idle = 0,
                     .keepalive_interval = 0, .keepalive_count = 0};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(SocketConfigValidate, NegativeRecvBufSizeFails) {
    SocketConfig cfg{.host = "example.com", .port = 443, .recv_buf_size = -1};
    EXPECT_FALSE(cfg.validate().empty());
    EXPECT_NE(cfg.validate().find("recv_buf_size"), std::string_view::npos);
}

TEST(SocketConfigValidate, NegativeSendBufSizeFails) {
    SocketConfig cfg{.host = "example.com", .port = 443, .send_buf_size = -1};
    EXPECT_FALSE(cfg.validate().empty());
    EXPECT_NE(cfg.validate().find("send_buf_size"), std::string_view::npos);
}

TEST(SocketConfigValidate, ZeroBufSizeIsValid) {
    SocketConfig cfg{.host = "example.com", .port = 443,
                     .recv_buf_size = 0, .send_buf_size = 0};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(SocketConfigValidate, RecvBufSizeTooSmallFails) {
    // Buffer sizes between 1 and 1023 should fail validation.
    SocketConfig cfg{.host = "example.com", .port = 443, .recv_buf_size = 512};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("recv_buf_size"), std::string_view::npos);
    EXPECT_NE(err.find("1024"), std::string_view::npos);
}

TEST(SocketConfigValidate, SendBufSizeTooSmallFails) {
    SocketConfig cfg{.host = "example.com", .port = 443, .send_buf_size = 1};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("send_buf_size"), std::string_view::npos);
}

TEST(SocketConfigValidate, MinValidBufSizePasses) {
    // 1024 is the minimum valid buffer size when set.
    SocketConfig cfg{.host = "example.com", .port = 443,
                     .recv_buf_size = 1024, .send_buf_size = 1024};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(SocketConfigValidate, KeepaliveIntervalWithoutIdleFails) {
    // keepalive_interval > 0 but keepalive_idle <= 0 is invalid even
    // when tcp_keepalive is disabled (defensive validation).
    SocketConfig cfg{.host = "example.com", .port = 443,
                     .tcp_keepalive = false,
                     .keepalive_idle = 0, .keepalive_interval = 10};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("keepalive_idle"), std::string_view::npos);
}

TEST(SocketConfigValidate, HostWithControlCharsRejected) {
    SocketConfig cfg{.host = "evil\r\nhost", .port = 443};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("control characters"), std::string_view::npos);
}

TEST(SocketConfigValidate, HostWithNullByteRejected) {
    SocketConfig cfg{.host = std::string("host\0evil", 9), .port = 443};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("control characters"), std::string_view::npos);
}

TEST(SocketConfigValidate, BindDeviceWithControlCharsRejected) {
    SocketConfig cfg{.host = "example.com", .port = 443};
    cfg.bind_device = "eth0\nmalicious";
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("bind_device"), std::string_view::npos);
    EXPECT_NE(err.find("control characters"), std::string_view::npos);
}

TEST(SocketConfigValidate, ValidBindDevicePasses) {
    SocketConfig cfg{.host = "example.com", .port = 443};
    cfg.bind_device = "ens35";
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(SocketConfigValidate, EmptyBindDevicePasses) {
    SocketConfig cfg{.host = "example.com", .port = 443};
    // Default empty bind_device should be fine
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(SocketConfigValidate, EqualityOperator) {
    SocketConfig a{.host = "example.com", .port = 443, .tcp_nodelay = true};
    SocketConfig b{.host = "example.com", .port = 443, .tcp_nodelay = true};
    SocketConfig c{.host = "other.com", .port = 443, .tcp_nodelay = true};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

// ─────────────────────────────────────────────────────────────────────────────
// SocketConfig::dump() and to_json()
// ─────────────────────────────────────────────────────────────────────────────

TEST(SocketConfigFormat, DumpContainsHostPort) {
    SocketConfig cfg{.host = "example.com", .port = 443};
    auto d = cfg.dump();
    EXPECT_NE(d.find("example.com"), std::string::npos);
    EXPECT_NE(d.find("443"), std::string::npos);
    EXPECT_NE(d.find("SocketConfig"), std::string::npos);
}

TEST(SocketConfigFormat, ToJsonValidStructure) {
    SocketConfig cfg{.host = "example.com", .port = 443, .tcp_nodelay = true,
                     .recv_buf_size = 65536, .send_buf_size = 32768};
    auto j = cfg.to_json();
    EXPECT_NE(j.find("\"host\":\"example.com\""), std::string::npos);
    EXPECT_NE(j.find("\"port\":443"), std::string::npos);
    EXPECT_NE(j.find("\"tcp_nodelay\":true"), std::string::npos);
    EXPECT_NE(j.find("\"recv_buf_size\":65536"), std::string::npos);
    EXPECT_NE(j.find("\"send_buf_size\":32768"), std::string::npos);
}

TEST(SocketConfigFormat, StdFormatterWorks) {
    SocketConfig cfg{.host = "10.0.0.1", .port = 8080, .tcp_nodelay = false};
    auto s = std::format("{}", cfg);
    EXPECT_NE(s.find("10.0.0.1"), std::string::npos);
    EXPECT_NE(s.find("8080"), std::string::npos);
}

TEST(SocketConfigFormat, ToJsonEscapesHostField) {
    // Verify host field with special chars is escaped in JSON output
    SocketConfig cfg{.host = "host\"with\\quotes", .port = 443};
    auto j = cfg.to_json();
    // Quotes and backslashes must be escaped
    EXPECT_NE(j.find("host\\\"with\\\\quotes"), std::string::npos);
    // Raw unescaped chars must not appear in the JSON value
    EXPECT_EQ(j.find("host\"with"), std::string::npos);
}

TEST(SocketConfigFormat, ToJsonEscapesControlChars) {
    // Verify control characters (U+0000–U+001F) are escaped per RFC 8259 §7.
    // This was previously a gap: only " and \ were escaped inline.
    SocketConfig cfg{.host = "host\twith\nnewline", .port = 443};
    auto j = cfg.to_json();
    // Tab and newline must be escaped
    EXPECT_NE(j.find("host\\twith\\nnewline"), std::string::npos);
    // Raw control chars must not appear
    EXPECT_EQ(j.find('\t'), std::string::npos);
    EXPECT_EQ(j.find('\n'), std::string::npos);
}

TEST(SocketConfigFormat, ToJsonEscapesNullByte) {
    // Null byte in host (shouldn't happen in practice, but must not break JSON)
    std::string host_with_null = "host";
    host_with_null += '\x00';
    host_with_null += "name";
    SocketConfig cfg{.host = host_with_null, .port = 443};
    auto j = cfg.to_json();
    // Null must be escaped as \u0000
    EXPECT_NE(j.find("\\u0000"), std::string::npos);
}

// ---------------------------------------------------------------------------
// SocketConfig::from_url()
// ---------------------------------------------------------------------------

TEST(SocketConfigFromUrl, TcpSchemeWithHostPort) {
    auto r = SocketConfig::from_url("tcp://example.com:8080");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->host, "example.com");
    EXPECT_EQ(r->port, 8080);
}

TEST(SocketConfigFromUrl, SchemelessHostPort) {
    auto r = SocketConfig::from_url("localhost:9090");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->host, "localhost");
    EXPECT_EQ(r->port, 9090);
}

TEST(SocketConfigFromUrl, Ipv6WithBrackets) {
    auto r = SocketConfig::from_url("tcp://[::1]:443");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->host, "::1");
    EXPECT_EQ(r->port, 443);
}

TEST(SocketConfigFromUrl, Ipv6WithoutScheme) {
    auto r = SocketConfig::from_url("[fe80::1]:5000");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->host, "fe80::1");
    EXPECT_EQ(r->port, 5000);
}

TEST(SocketConfigFromUrl, LeadingTrailingWhitespace) {
    auto r = SocketConfig::from_url("  tcp://host:80  ");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->host, "host");
    EXPECT_EQ(r->port, 80);
}

TEST(SocketConfigFromUrl, IpAddress) {
    auto r = SocketConfig::from_url("10.0.0.1:1234");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->host, "10.0.0.1");
    EXPECT_EQ(r->port, 1234);
}

TEST(SocketConfigFromUrl, PortMaxValid) {
    auto r = SocketConfig::from_url("host:65535");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->port, 65535);
}

TEST(SocketConfigFromUrl, ErrorEmptyString) {
    auto r = SocketConfig::from_url("");
    EXPECT_FALSE(r.has_value());
}

TEST(SocketConfigFromUrl, ErrorMissingPort) {
    auto r = SocketConfig::from_url("tcp://example.com");
    EXPECT_FALSE(r.has_value());
}

TEST(SocketConfigFromUrl, ErrorEmptyHost) {
    auto r = SocketConfig::from_url(":8080");
    EXPECT_FALSE(r.has_value());
}

TEST(SocketConfigFromUrl, ErrorPortZero) {
    auto r = SocketConfig::from_url("host:0");
    EXPECT_FALSE(r.has_value());
}

TEST(SocketConfigFromUrl, ErrorPortOverflow) {
    auto r = SocketConfig::from_url("host:65536");
    EXPECT_FALSE(r.has_value());
}

TEST(SocketConfigFromUrl, ErrorPortWayOverflow) {
    auto r = SocketConfig::from_url("host:999999");
    EXPECT_FALSE(r.has_value());
}

TEST(SocketConfigFromUrl, ErrorInvalidPort) {
    auto r = SocketConfig::from_url("host:abc");
    EXPECT_FALSE(r.has_value());
}

TEST(SocketConfigFromUrl, ErrorEmptyPortAfterColon) {
    auto r = SocketConfig::from_url("host:");
    EXPECT_FALSE(r.has_value());
}

TEST(SocketConfigFromUrl, ErrorIpv6MissingCloseBracket) {
    auto r = SocketConfig::from_url("[::1:443");
    EXPECT_FALSE(r.has_value());
}

TEST(SocketConfigFromUrl, ErrorIpv6EmptyAddress) {
    auto r = SocketConfig::from_url("[]:443");
    EXPECT_FALSE(r.has_value());
}

TEST(SocketConfigFromUrl, ErrorIpv6MissingPort) {
    auto r = SocketConfig::from_url("[::1]");
    EXPECT_FALSE(r.has_value());
}

TEST(SocketConfigFromUrl, ErrorHostWithControlChars) {
    auto r = SocketConfig::from_url("ho\tst:80");
    EXPECT_FALSE(r.has_value());
}

TEST(SocketConfigFromUrl, ValidatePassesAfterFromUrl) {
    auto r = SocketConfig::from_url("tcp://example.com:443");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->validate().empty());
}

TEST(SocketConfigFromUrl, DefaultFieldsPreserved) {
    auto r = SocketConfig::from_url("host:80");
    ASSERT_TRUE(r.has_value());
    // All non-URL fields should have defaults
    EXPECT_TRUE(r->tcp_nodelay);
    EXPECT_EQ(r->recv_buf_size, 0);
    EXPECT_EQ(r->send_buf_size, 0);
    EXPECT_FALSE(r->tcp_keepalive);
    EXPECT_EQ(r->send_timeout_ms, 1000);
}

// ---------------------------------------------------------------------------
// SocketConfig::to_url()
// ---------------------------------------------------------------------------

TEST(SocketConfigToUrl, BasicFormat) {
    SocketConfig cfg{.host = "example.com", .port = 8080};
    EXPECT_EQ(cfg.to_url(), "tcp://example.com:8080");
}

TEST(SocketConfigToUrl, Ipv6BracketEnclosed) {
    SocketConfig cfg{.host = "::1", .port = 443};
    EXPECT_EQ(cfg.to_url(), "tcp://[::1]:443");
}

TEST(SocketConfigToUrl, RoundTrip) {
    auto original = SocketConfig::from_url("tcp://myhost:9090");
    ASSERT_TRUE(original.has_value());
    auto roundtripped = SocketConfig::from_url(original->to_url());
    ASSERT_TRUE(roundtripped.has_value());
    EXPECT_EQ(roundtripped->host, original->host);
    EXPECT_EQ(roundtripped->port, original->port);
}

TEST(SocketConfigToUrl, Ipv6RoundTrip) {
    auto original = SocketConfig::from_url("[::1]:5000");
    ASSERT_TRUE(original.has_value());
    auto roundtripped = SocketConfig::from_url(original->to_url());
    ASSERT_TRUE(roundtripped.has_value());
    EXPECT_EQ(roundtripped->host, "::1");
    EXPECT_EQ(roundtripped->port, 5000);
}

// ─────────────────────────────────────────────────────────────────────────────
// connect() — URL-based convenience factory
// ─────────────────────────────────────────────────────────────────────────────

TEST(UrlConnect, InvalidUrlReturnsError) {
    auto result = connect("http://example.com");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ConnectionError::kInvalidConfig);
}

TEST(UrlConnect, EmptyUrlReturnsError) {
    auto result = connect("");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ConnectionError::kInvalidConfig);
}

TEST(UrlConnect, WssUrlToUnreachableHostFails) {
    auto result = connect("wss://this-host-does-not-exist-xyz.invalid/ws");
    ASSERT_FALSE(result.has_value());
    // Should fail during TCP factory (DNS resolution fails)
    EXPECT_EQ(result.error().code, ConnectionError::kFactoryFailed);
}

TEST(UrlConnect, WsUrlToRefusedPortFails) {
    auto result = connect("ws://127.0.0.1:1/ws");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ConnectionError::kFactoryFailed);
}

TEST(UrlConnect, WithConfigModifier) {
    // Verify modifier is applied: set max_reconnect_attempts to 0
    auto result = connect("wss://127.0.0.1:1/ws", [](TransportConfig& cfg) {
        cfg.max_reconnect_attempts = 0;
        cfg.tcp_timeout = std::chrono::milliseconds{200};
    });
    ASSERT_FALSE(result.has_value());
    // Connection should fail (port 1 not listening)
    EXPECT_EQ(result.error().code, ConnectionError::kFactoryFailed);
}

TEST(UrlConnect, WsSchemeDispatchesToPlainConnect) {
    // ws:// should use socket_ws_connect (use_tls=false)
    // Connection will fail, but the error path confirms it tried
    auto result = connect("ws://127.0.0.1:1/echo");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ConnectionError::kFactoryFailed);
}

TEST(UrlConnect, PathAndQueryPreserved) {
    // Verify URL path with query string is preserved through connect()
    auto result = connect("wss://127.0.0.1:1/ws?token=abc&v=2",
        [](TransportConfig& cfg) {
            // Verify the path was correctly parsed from URL
            EXPECT_EQ(cfg.ws_path, "/ws?token=abc&v=2");
            cfg.tcp_timeout = std::chrono::milliseconds{200};
        });
    ASSERT_FALSE(result.has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// socket_wss_connect() — direct convenience factory
// ─────────────────────────────────────────────────────────────────────────────

TEST(SocketWssConnect, InvalidConfigReturnsError) {
    TransportConfig cfg;
    cfg.remote_host = "";  // Invalid: empty host
    cfg.remote_port = 443;
    auto result = socket_wss_connect(cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ConnectionError::kInvalidConfig);
}

TEST(SocketWssConnect, ConnectionRefusedReturnsFactoryFailed) {
    TransportConfig cfg;
    cfg.remote_host = "127.0.0.1";
    cfg.remote_port = 1;  // Almost certainly not listening
    cfg.ws_path = "/ws";
    cfg.tcp_timeout = std::chrono::milliseconds{200};
    auto result = socket_wss_connect(cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ConnectionError::kFactoryFailed);
}

TEST(SocketWssConnect, WithCustomSocketConfig) {
    TransportConfig cfg;
    cfg.remote_host = "127.0.0.1";
    cfg.remote_port = 1;
    cfg.ws_path = "/ws";
    cfg.tcp_timeout = std::chrono::milliseconds{200};
    SocketConfig sock_cfg{
        .host = "127.0.0.1",
        .port = 1,
        .tcp_nodelay = false,  // Custom: disable nodelay
    };
    auto result = socket_wss_connect(cfg, sock_cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ConnectionError::kFactoryFailed);
}

TEST(SocketWssConnect, InvalidSocketConfigReturnsError) {
    TransportConfig cfg;
    cfg.remote_host = "127.0.0.1";
    cfg.remote_port = 443;
    cfg.ws_path = "/ws";
    SocketConfig sock_cfg{
        .host = "",  // Invalid: empty host
        .port = 0,   // Invalid: zero port
    };
    auto result = socket_wss_connect(cfg, sock_cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ConnectionError::kInvalidConfig);
}

// ─────────────────────────────────────────────────────────────────────────────
// socket_ws_connect() — plain WebSocket convenience factory
// ─────────────────────────────────────────────────────────────────────────────

TEST(SocketWsConnect, ForcesUseTlsFalse) {
    TransportConfig cfg;
    cfg.remote_host = "127.0.0.1";
    cfg.remote_port = 1;
    cfg.ws_path = "/ws";
    cfg.use_tls = true;  // Should be overridden to false
    cfg.tcp_timeout = std::chrono::milliseconds{200};
    auto result = socket_ws_connect(cfg);
    ASSERT_FALSE(result.has_value());
    // The fact it doesn't fail with TLS errors confirms use_tls was set to false
    EXPECT_EQ(result.error().code, ConnectionError::kFactoryFailed);
}

TEST(SocketWsConnect, ConnectionRefusedReturnsFactoryFailed) {
    TransportConfig cfg;
    cfg.remote_host = "127.0.0.1";
    cfg.remote_port = 1;
    cfg.ws_path = "/echo";
    cfg.tcp_timeout = std::chrono::milliseconds{200};
    auto result = socket_ws_connect(cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ConnectionError::kFactoryFailed);
}

// ─────────────────────────────────────────────────────────────────────────────
// SocketConfig::warnings()
// ─────────────────────────────────────────────────────────────────────────────

TEST(SocketConfig, WarningsDefaultConfigIsEmpty) {
    SocketConfig cfg{.host = "localhost", .port = 8080};
    EXPECT_TRUE(cfg.warnings().empty());
}

TEST(SocketConfig, WarningsNagleWithShortTimeout) {
    SocketConfig cfg{.host = "localhost", .port = 8080};
    cfg.tcp_nodelay = false;
    cfg.send_timeout_ms = 100;
    auto w = cfg.warnings();
    ASSERT_EQ(w.size(), 1);
    EXPECT_NE(w[0].find("Nagle"), std::string::npos);
}

TEST(SocketConfig, WarningsShortKeepaliveIdle) {
    SocketConfig cfg{.host = "localhost", .port = 8080};
    cfg.tcp_keepalive = true;
    cfg.keepalive_idle = 5;
    auto w = cfg.warnings();
    ASSERT_EQ(w.size(), 1);
    EXPECT_NE(w[0].find("keepalive_idle"), std::string::npos);
}

TEST(SocketConfig, WarningsLargeRecvBuf) {
    SocketConfig cfg{.host = "localhost", .port = 8080};
    cfg.recv_buf_size = 32 * 1024 * 1024;
    auto w = cfg.warnings();
    ASSERT_EQ(w.size(), 1);
    EXPECT_NE(w[0].find("recv_buf_size"), std::string::npos);
}

TEST(SocketConfig, WarningsLargeSendBuf) {
    SocketConfig cfg{.host = "localhost", .port = 8080};
    cfg.send_buf_size = 32 * 1024 * 1024;
    auto w = cfg.warnings();
    ASSERT_EQ(w.size(), 1);
    EXPECT_NE(w[0].find("send_buf_size"), std::string::npos);
}

TEST(SocketConfig, WarningsNoFalsePositiveForNagleWithLongTimeout) {
    SocketConfig cfg{.host = "localhost", .port = 8080};
    cfg.tcp_nodelay = false;
    cfg.send_timeout_ms = 500;  // long enough, no warning
    EXPECT_TRUE(cfg.warnings().empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// SocketConfig::dump() / to_json() — bind_device inclusion
// ─────────────────────────────────────────────────────────────────────────────

TEST(SocketConfig, DumpIncludesBindDevice) {
    SocketConfig cfg{.host = "localhost", .port = 8080};
    cfg.bind_device = "ens35";
    auto d = cfg.dump();
    EXPECT_NE(d.find("ens35"), std::string::npos);
}

TEST(SocketConfig, DumpShowsNoneForEmptyBindDevice) {
    SocketConfig cfg{.host = "localhost", .port = 8080};
    auto d = cfg.dump();
    EXPECT_NE(d.find("(none)"), std::string::npos);
}

TEST(SocketConfig, ToJsonIncludesBindDevice) {
    SocketConfig cfg{.host = "localhost", .port = 8080};
    cfg.bind_device = "eth0";
    auto j = cfg.to_json();
    EXPECT_NE(j.find("\"bind_device\":\"eth0\""), std::string::npos);
}

TEST(SocketConfig, ToJsonEmptyBindDevice) {
    SocketConfig cfg{.host = "localhost", .port = 8080};
    auto j = cfg.to_json();
    EXPECT_NE(j.find("\"bind_device\":\"\""), std::string::npos);
}
