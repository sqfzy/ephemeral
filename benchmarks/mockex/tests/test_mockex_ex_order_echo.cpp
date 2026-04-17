/// @file tests/test_mockex_ex_order_echo.cpp
/// Order-path WS echo: client sends a JSON order, mock injects
/// `t_mock_recv` / `t_mock_send`, client parses them by field name.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "eph/net/detail/ws_handshake.hpp"

namespace {

std::string write_tmp_conf(uint16_t port) {
    const std::string path = "/tmp/mockex_test_order_" +
                             std::to_string(::getpid()) + ".conf";
    std::ofstream f(path);
    f << "mock_ip = 127.0.0.1\n"
      << "cpu_mock = -1\n"
      << "warmup_samples = 1\n"
      << "[lat_ex_order]\n"
      << "port = " << port << "\n";
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

pid_t spawn_mockex(const std::string& conf_path) {
    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(MOCKEX_BINARY_PATH, "mockex",
                "--scenario", "ex_order",
                "--config",   conf_path.c_str(),
                static_cast<char*>(nullptr));
        std::perror("execl"); std::_Exit(127);
    }
    return pid;
}

int connect_with_retry(uint16_t port) {
    const auto dl = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < dl) {
        int s = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) return -1;
        sockaddr_in a{}; a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) return s;
        ::close(s);
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return -1;
}

bool do_handshake(int fd) {
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string req =
        "GET /order HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    if (::send(fd, req.data(), req.size(), 0) !=
        static_cast<ssize_t>(req.size())) return false;
    std::string resp; char buf[1024];
    while (resp.find("\r\n\r\n") == std::string::npos) {
        pollfd p{ .fd = fd, .events = POLLIN, .revents = 0 };
        if (::poll(&p, 1, 2000) <= 0) return false;
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        resp.append(buf, static_cast<size_t>(n));
    }
    return resp.find("101") != std::string::npos &&
           resp.find(eph::net::detail::ws_compute_accept(key)) != std::string::npos;
}

std::vector<uint8_t> build_client_binary(std::string_view body) {
    std::vector<uint8_t> out;
    out.reserve(body.size() + 14);
    out.push_back(0x82);  // FIN + binary
    const size_t n = body.size();
    if (n < 126) out.push_back(static_cast<uint8_t>(0x80 | n));
    else if (n < (1u << 16)) {
        out.push_back(0x80 | 126);
        out.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(n & 0xFF));
    } else { return {}; }  // test uses small JSON payload; 64K overflow not expected
    const uint8_t mask[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    out.insert(out.end(), mask, mask + 4);
    for (size_t i = 0; i < n; ++i)
        out.push_back(static_cast<uint8_t>(body[i]) ^ mask[i & 3]);
    return out;
}

bool recv_exact_timed(int fd, void* buf, size_t n) {
    const auto dl = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    size_t got = 0;
    while (got < n) {
        auto now = std::chrono::steady_clock::now();
        if (now >= dl) return false;
        const int rem = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                dl - now).count());
        pollfd p{ .fd = fd, .events = POLLIN, .revents = 0 };
        if (::poll(&p, 1, rem) <= 0) return false;
        ssize_t r = ::recv(fd, static_cast<uint8_t*>(buf) + got, n - got, 0);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

/// Read one server→client frame. Returns payload or empty on error.
std::vector<uint8_t> recv_server_frame(int fd) {
    uint8_t hdr[2];
    if (!recv_exact_timed(fd, hdr, 2)) return {};
    const uint8_t raw_len = hdr[1] & 0x7F;
    uint64_t length = raw_len;
    if (raw_len == 126) {
        uint8_t ext[2];
        if (!recv_exact_timed(fd, ext, 2)) return {};
        length = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (raw_len == 127) {
        uint8_t ext[8];
        if (!recv_exact_timed(fd, ext, 8)) return {};
        length = 0;
        for (int i = 0; i < 8; ++i) length = (length << 8) | ext[i];
    }
    std::vector<uint8_t> out(length, 0);
    if (length > 0 && !recv_exact_timed(fd, out.data(), length)) return {};
    return out;
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

} // namespace

TEST(MockexExOrderEcho, InjectsMockTimestampsIntoJson) {
    const uint16_t port = pick_port();
    const auto conf = write_tmp_conf(port);
    pid_t pid = spawn_mockex(conf);
    ASSERT_GT(pid, 0);

    int fd = connect_with_retry(port);
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(do_handshake(fd));

    const std::string order =
        R"({"e":"NewOrder","id":42,"t_client":1234567890})";
    auto frame = build_client_binary(order);
    ASSERT_EQ(::send(fd, frame.data(), frame.size(), 0),
              static_cast<ssize_t>(frame.size()));

    auto reply = recv_server_frame(fd);
    ASSERT_FALSE(reply.empty());
    const std::string reply_s(reinterpret_cast<const char*>(reply.data()),
                              reply.size());

    // Client-side fields must survive (the client reads `id`,
    // `t_client` by name via scan_json_uint_field).
    EXPECT_NE(reply_s.find("\"id\":42"), std::string::npos) << reply_s;
    EXPECT_NE(reply_s.find("\"t_client\":1234567890"),
              std::string::npos) << reply_s;
    // Mock must have injected its two timestamps.
    EXPECT_NE(reply_s.find("\"t_mock_recv\":"),
              std::string::npos) << reply_s;
    EXPECT_NE(reply_s.find("\"t_mock_send\":"),
              std::string::npos) << reply_s;
    // Closing brace must still terminate — simple structural sanity.
    EXPECT_EQ(reply_s.back(), '}');

    ::close(fd);
    kill_and_reap(pid);
    std::remove(conf.c_str());
}
