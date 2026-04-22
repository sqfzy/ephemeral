/// @file tests/test_mockex_ws_echo.cpp
/// WS echo scenario integration test. Drives the handshake manually
/// (raw bytes) so we don't pull WsCodec into the test — the whole
/// point is to verify mockex's server-side framing, which is the
/// inverse of WsCodec's client-side framing.

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
#include <string>
#include <thread>
#include <vector>

#include "eph/net/detail/ws_handshake.hpp"   // ws_compute_accept

namespace {

std::string write_tmp_conf(uint16_t port, const char* section) {
    const std::string path = "/tmp/mockex_test_" +
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
      << "\n[scenarios." << section << "]\n"
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

uint64_t get_u64_le(const uint8_t* buf, size_t off) noexcept {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(buf[off + static_cast<size_t>(i)])
             << (i * 8);
    }
    return v;
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

/// Read exactly n bytes from fd with a bounded deadline. Gtest-friendly
/// wrapper — the underlying primitive (`eph::net::posix::recv_exact`) is
/// blocking; here we add a poll-based timeout so a broken mock doesn't
/// wedge the test process.
bool recv_exact_timed(int fd, void* buf, size_t n,
                      std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    size_t got = 0;
    while (got < n) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;
        const int remain_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now).count());
        pollfd p{ .fd = fd, .events = POLLIN, .revents = 0 };
        int rv = ::poll(&p, 1, remain_ms);
        if (rv <= 0) return false;
        ssize_t r = ::recv(fd, static_cast<uint8_t*>(buf) + got,
                           n - got, 0);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

/// Perform an RFC 6455 client handshake with a fixed key so the
/// expected Sec-WebSocket-Accept is deterministic.
bool do_client_handshake(int fd) {
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";  // canonical RFC example
    const std::string req =
        "GET /ws HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    if (::send(fd, req.data(), req.size(), 0) !=
        static_cast<ssize_t>(req.size())) return false;

    // Read response until \r\n\r\n.
    std::string resp;
    char buf[1024];
    while (resp.find("\r\n\r\n") == std::string::npos && resp.size() < 4096) {
        pollfd p{ .fd = fd, .events = POLLIN, .revents = 0 };
        if (::poll(&p, 1, 2000) <= 0) return false;
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        resp.append(buf, static_cast<size_t>(n));
    }
    if (resp.find("101 Switching Protocols") == std::string::npos) return false;
    const std::string expected_accept = eph::net::detail::ws_compute_accept(key);
    if (resp.find(expected_accept) == std::string::npos) return false;
    return true;
}

/// Build a masked client frame with given opcode and payload.
std::vector<uint8_t>
build_client_frame(uint8_t opcode, std::span<const uint8_t> payload) {
    std::vector<uint8_t> out;
    out.reserve(payload.size() + 14);
    out.push_back(static_cast<uint8_t>(0x80 | (opcode & 0x0F)));  // FIN
    const size_t n = payload.size();
    if (n < 126) {
        out.push_back(static_cast<uint8_t>(0x80 | n));  // masked
    } else if (n < (1u << 16)) {
        out.push_back(0x80 | 126);
        out.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(n & 0xFF));
    } else {
        out.push_back(0x80 | 127);
        for (int i = 7; i >= 0; --i)
            out.push_back(static_cast<uint8_t>((n >> (i * 8)) & 0xFF));
    }
    const uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};
    out.insert(out.end(), mask, mask + 4);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(payload[i] ^ mask[i & 3]);
    }
    return out;
}

} // namespace

TEST(MockexWsEcho, HandshakeAndStampedEcho) {
    const uint16_t port = pick_port();
    const auto conf = write_tmp_conf(port, "lat_ws");
    pid_t pid = spawn_mockex("ws", conf);
    ASSERT_GT(pid, 0);

    int fd = connect_with_retry(port, std::chrono::seconds{3});
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(do_client_handshake(fd));

    // Build a 64-byte payload with the Phase-11.1 timestamp block.
    constexpr size_t kPayload = 64;
    std::vector<uint8_t> body(kPayload, 0);
    const uint64_t t_client = 0x1234567890abcdefull;
    for (int i = 0; i < 8; ++i) body[static_cast<size_t>(i)] =
        static_cast<uint8_t>((t_client >> (i * 8)) & 0xFFu);
    for (size_t i = 24; i < kPayload; ++i) body[i] =
        static_cast<uint8_t>((i + 0x5Au) & 0xFFu);

    auto tx = build_client_frame(/*opcode=binary*/ 0x2, body);
    ASSERT_EQ(::send(fd, tx.data(), tx.size(), 0),
              static_cast<ssize_t>(tx.size()));

    // Read the server frame: FIN+binary, unmasked, same payload size.
    uint8_t hdr[2];
    ASSERT_TRUE(recv_exact_timed(fd, hdr, 2,
                                 std::chrono::seconds{3}));
    EXPECT_EQ(hdr[0], 0x82);          // FIN + binary
    ASSERT_EQ(hdr[1] & 0x80, 0);      // server frames are never masked
    ASSERT_EQ(hdr[1] & 0x7F, kPayload);  // short-length path

    std::vector<uint8_t> rx(kPayload, 0);
    ASSERT_TRUE(recv_exact_timed(fd, rx.data(), kPayload,
                                 std::chrono::seconds{3}));

    EXPECT_EQ(get_u64_le(rx.data(), 0), t_client);
    EXPECT_GT(get_u64_le(rx.data(), 8),  0u);
    EXPECT_GT(get_u64_le(rx.data(), 16), 0u);
    EXPECT_GE(get_u64_le(rx.data(), 16), get_u64_le(rx.data(), 8));
    for (size_t i = 24; i < kPayload; ++i) {
        ASSERT_EQ(rx[i], body[i]) << "filler mismatch at " << i;
    }

    ::close(fd);
    kill_and_reap(pid);
    std::remove(conf.c_str());
}
