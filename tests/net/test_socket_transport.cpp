/// @file test_socket_transport.cpp
/// Unit tests for POSIX socket TcpTransport implementation.

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "eph/net/socket_transport.hpp"

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
