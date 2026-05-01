/// @file test_posix_listener.cpp
/// Unit tests for posix_listener.hpp — TCP/UDP bind helpers + accept loop.

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <thread>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "eph/net/posix_io.hpp"
#include "eph/net/posix_listener.hpp"

using namespace eph::net::posix;

// ---------------------------------------------------------------------------
// RAII fd wrapper for test cleanup
// ---------------------------------------------------------------------------

struct ScopedFd {
    int fd = -1;
    explicit ScopedFd(int f = -1) : fd(f) {}
    ~ScopedFd() { if (fd >= 0) ::close(fd); }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ScopedFd(ScopedFd&& o) noexcept : fd(o.fd) { o.fd = -1; }
    int release() { int f = fd; fd = -1; return f; }
    explicit operator bool() const { return fd >= 0; }
};

// ---------------------------------------------------------------------------
// tcp_bind_listen — success
// ---------------------------------------------------------------------------

TEST(PosixListener, tcp_bind_listen_on_ephemeral_port) {
    auto result = tcp_bind_listen("127.0.0.1", 0);
    ASSERT_TRUE(result.has_value()) << result.error();
    ScopedFd fd(*result);
    EXPECT_GE(fd.fd, 0);
}

TEST(PosixListener, tcp_bind_listen_returns_listening_socket) {
    auto result = tcp_bind_listen("127.0.0.1", 0);
    ASSERT_TRUE(result.has_value());
    ScopedFd fd(*result);

    // Get the port assigned
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    ASSERT_EQ(::getsockname(fd.fd, reinterpret_cast<sockaddr*>(&addr), &len), 0);
    EXPECT_GT(ntohs(addr.sin_port), 0);
}

// ---------------------------------------------------------------------------
// tcp_bind_listen — error cases
// ---------------------------------------------------------------------------

TEST(PosixListener, tcp_bind_listen_invalid_ip_returns_error) {
    auto result = tcp_bind_listen("not-an-ip", 12345);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("invalid bind IP"), std::string::npos);
}

TEST(PosixListener, tcp_bind_listen_empty_ip_returns_error) {
    auto result = tcp_bind_listen("", 12345);
    ASSERT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// udp_bind — success
// ---------------------------------------------------------------------------

TEST(PosixListener, udp_bind_on_ephemeral_port) {
    auto result = udp_bind("127.0.0.1", 0);
    ASSERT_TRUE(result.has_value()) << result.error();
    ScopedFd fd(*result);
    EXPECT_GE(fd.fd, 0);
}

TEST(PosixListener, udp_bind_returns_bound_socket) {
    auto result = udp_bind("127.0.0.1", 0);
    ASSERT_TRUE(result.has_value());
    ScopedFd fd(*result);

    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    ASSERT_EQ(::getsockname(fd.fd, reinterpret_cast<sockaddr*>(&addr), &len), 0);
    EXPECT_GT(ntohs(addr.sin_port), 0);
}

// ---------------------------------------------------------------------------
// udp_bind — error cases
// ---------------------------------------------------------------------------

TEST(PosixListener, udp_bind_invalid_ip_returns_error) {
    auto result = udp_bind("bad-ip", 0);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("invalid bind IP"), std::string::npos);
}

TEST(PosixListener, udp_bind_empty_ip_returns_error) {
    // Symmetry with `tcp_bind_listen_empty_ip_returns_error`: empty IP
    // string must surface "invalid bind IP" from inet_pton's rejection
    // rather than silently using INADDR_ANY (which the field
    // sin_addr.s_addr would otherwise zero-init to). The fd must be
    // closed before the error is returned — otherwise repeated calls
    // here would leak fds. We can't directly observe the close, but
    // pinning the rejection ensures the path is exercised.
    auto result = udp_bind("", 0);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("invalid bind IP"), std::string::npos)
        << "error: " << result.error();
}

// ---------------------------------------------------------------------------
// accept_one — success path with a connecting client
// ---------------------------------------------------------------------------

TEST(PosixListener, accept_one_returns_client_fd) {
    auto listen_result = tcp_bind_listen("127.0.0.1", 0);
    ASSERT_TRUE(listen_result.has_value()) << listen_result.error();
    ScopedFd listen_fd(*listen_result);

    // Get the port
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    ASSERT_EQ(::getsockname(listen_fd.fd,
        reinterpret_cast<sockaddr*>(&addr), &len), 0);
    uint16_t port = ntohs(addr.sin_port);

    std::atomic<bool> running{true};

    // Accept in background thread
    std::thread acceptor([&] {
        auto client = accept_one(listen_fd.fd, running);
        ASSERT_TRUE(client.has_value()) << client.error();
        ScopedFd client_fd(*client);
        EXPECT_GE(client_fd.fd, 0);

        // Echo back one byte to prove connection works
        uint8_t buf;
        EXPECT_TRUE(recv_exact(client_fd.fd, &buf, 1));
        EXPECT_TRUE(send_all(client_fd.fd, &buf, 1));
    });

    // Connect as client
    int cfd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(cfd, 0);
    ScopedFd client_fd(cfd);

    sockaddr_in caddr{};
    caddr.sin_family = AF_INET;
    caddr.sin_port = htons(port);
    caddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::connect(cfd, reinterpret_cast<sockaddr*>(&caddr), sizeof(caddr)), 0);

    // Round-trip one byte
    uint8_t tx = 0x42;
    ASSERT_TRUE(send_all(cfd, &tx, 1));
    uint8_t rx = 0;
    ASSERT_TRUE(recv_exact(cfd, &rx, 1));
    EXPECT_EQ(rx, 0x42);

    running.store(false, std::memory_order_release);
    acceptor.join();
}

// ---------------------------------------------------------------------------
// accept_one — shutdown signal before client arrives
// ---------------------------------------------------------------------------

TEST(PosixListener, accept_one_returns_minus_one_on_shutdown) {
    auto listen_result = tcp_bind_listen("127.0.0.1", 0);
    ASSERT_TRUE(listen_result.has_value()) << listen_result.error();
    ScopedFd listen_fd(*listen_result);

    std::atomic<bool> running{true};

    // Start accept, then signal shutdown after a short delay
    std::thread killer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        running.store(false, std::memory_order_release);
    });

    auto result = accept_one(listen_fd.fd, running);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, -1); // shutdown path returns -1

    killer.join();
}

// ---------------------------------------------------------------------------
// tcp_bind_listen — SO_REUSEADDR allows rebind
// ---------------------------------------------------------------------------

TEST(PosixListener, tcp_rebind_same_port_with_reuseaddr) {
    // Bind, get port, close, rebind same port
    auto r1 = tcp_bind_listen("127.0.0.1", 0);
    ASSERT_TRUE(r1.has_value());

    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    ASSERT_EQ(::getsockname(*r1, reinterpret_cast<sockaddr*>(&addr), &len), 0);
    uint16_t port = ntohs(addr.sin_port);
    ::close(*r1);

    // Should succeed thanks to SO_REUSEADDR
    auto r2 = tcp_bind_listen("127.0.0.1", port);
    ASSERT_TRUE(r2.has_value()) << r2.error();
    ScopedFd fd2(*r2);
}
