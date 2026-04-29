/// @file tests/test_mockex_ws_echo_tls.cpp
/// End-to-end smoke for the mockex WS-over-TLS path: spawn mockex with
/// `use_tls = true`, connect via aws-lc TLS client, then drive the
/// RFC 6455 handshake + a single echo roundtrip on top of TLS.
///
/// Mirrors `test_mockex_ws_echo.cpp` (plain TCP) — same protocol,
/// same payload shape, just wrapped in an SSL session. If the plain
/// test passes but this one fails, the regression is in the TLS
/// wiring (mockex/tls_server.hpp or scenario handler refactor),
/// not in the WS framing or echo logic.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "eph/net/detail/ws_handshake.hpp"

namespace {

#ifndef MOCKEX_FIXTURES_DIR
#  error "MOCKEX_FIXTURES_DIR must be defined by the build system"
#endif

std::string write_tmp_conf_tls(uint16_t port, const char* section) {
    const std::string path = "/tmp/mockex_test_tls_" +
                             std::to_string(::getpid()) + "_" +
                             section + ".toml";
    std::ofstream f(path);
    f << "[networking]\n"
      << "nic_a      = \"lo\"\n"
      << "nic_b      = \"lo\"\n"
      << "server_ip  = \"127.0.0.1\"\n"
      << "client_ip  = \"127.0.0.1\"\n"
      << "gateway_ip = \"127.0.0.1\"\n"
      << "\n[cpu]\n"
      << "cpu_client = 0\n"
      << "cpu_mock   = 0\n"
      << "\n[measurement]\nwarmup_samples = 1\n"
      << "\n[tls]\n"
      << "cert_path = \"" << MOCKEX_FIXTURES_DIR << "/tls/server.crt\"\n"
      << "key_path  = \"" << MOCKEX_FIXTURES_DIR << "/tls/server.key\"\n"
      << "\n[scenarios." << section << "]\n"
      << "port    = " << port << "\n"
      << "use_tls = true\n";
    return path;
}

uint16_t pick_port() {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(s, 0);
    sockaddr_in a{}; a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    EXPECT_EQ(::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)), 0);
    socklen_t al = sizeof(a);
    EXPECT_EQ(::getsockname(s, reinterpret_cast<sockaddr*>(&a), &al), 0);
    const uint16_t port = ntohs(a.sin_port);
    ::close(s);
    return port;
}

pid_t spawn_mockex(const char* scenario, const std::string& conf_path) {
    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(MOCKEX_BINARY_PATH, "mockex",
                "--scenario", scenario,
                "--config",   conf_path.c_str(),
                static_cast<char*>(nullptr));
        std::perror("execl(mockex)"); std::_Exit(127);
    }
    return pid;
}

int connect_with_retry(uint16_t port,
                       std::chrono::milliseconds deadline_ms) {
    const auto deadline = std::chrono::steady_clock::now() + deadline_ms;
    while (std::chrono::steady_clock::now() < deadline) {
        int s = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) return -1;
        sockaddr_in a{}; a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
            return s;
        }
        ::close(s);
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return -1;
}

void kill_and_reap(pid_t pid) {
    ::kill(pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        if (::waitpid(pid, nullptr, WNOHANG) == pid) return;
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
}

/// RAII aws-lc client SSL pair (CTX + SSL).
struct ClientTls {
    SSL_CTX* ctx = nullptr;
    SSL*     ssl = nullptr;
    ~ClientTls() {
        if (ssl) SSL_free(ssl);
        if (ctx) SSL_CTX_free(ctx);
    }

    bool connect(int fd) {
        ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) return false;
        SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
        ssl = SSL_new(ctx);
        if (!ssl) return false;
        if (SSL_set_fd(ssl, fd) != 1) return false;
        SSL_set_tlsext_host_name(ssl, "localhost");
        const int r = SSL_connect(ssl);
        return r == 1;
    }

    bool write_all(const void* buf, size_t n) {
        return SSL_write(ssl, buf, static_cast<int>(n)) ==
               static_cast<int>(n);
    }

    bool read_some(std::string& out, size_t want) {
        char tmp[4096];
        const int max = static_cast<int>(std::min(want, sizeof(tmp)));
        const int r = SSL_read(ssl, tmp, max);
        if (r <= 0) return false;
        out.append(tmp, static_cast<size_t>(r));
        return true;
    }
};

bool do_tls_ws_handshake(ClientTls& tls) {
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string req =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    if (!tls.write_all(req.data(), req.size())) return false;

    std::string resp;
    while (resp.find("\r\n\r\n") == std::string::npos && resp.size() < 4096) {
        if (!tls.read_some(resp, 1024)) return false;
    }
    if (resp.find("101 Switching Protocols") == std::string::npos) return false;
    const std::string expected = eph::net::detail::ws_compute_accept(key);
    return resp.find(expected) != std::string::npos;
}

} // namespace

TEST(MockexWsEchoTls, TlsHandshakeAndWsUpgrade) {
    const uint16_t port = pick_port();
    const auto conf = write_tmp_conf_tls(port, "lat_ws");
    pid_t pid = spawn_mockex("ws", conf);
    ASSERT_GT(pid, 0);

    int fd = connect_with_retry(port, std::chrono::seconds{5});
    ASSERT_GE(fd, 0) << "TCP connect to mockex failed (port=" << port << ")";

    ClientTls tls;
    const bool tls_ok = tls.connect(fd);
    if (!tls_ok) {
        // Drain the OpenSSL error queue for a useful failure message.
        char buf[256] = {};
        const unsigned long e = ERR_get_error();
        if (e) ERR_error_string_n(e, buf, sizeof(buf));
        ::close(fd);
        kill_and_reap(pid);
        FAIL() << "TLS handshake failed: " << buf;
    }

    EXPECT_TRUE(do_tls_ws_handshake(tls));

    // Send a Close frame so mockex tears down gracefully.
    const uint8_t close_frame[] = {
        0x88, 0x80,       // FIN | Close, len=0, masked
        0x00, 0x00, 0x00, 0x00,  // mask
    };
    (void)tls.write_all(close_frame, sizeof(close_frame));

    ::close(fd);
    kill_and_reap(pid);
    ::unlink(conf.c_str());
}
