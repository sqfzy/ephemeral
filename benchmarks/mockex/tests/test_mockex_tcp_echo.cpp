/// @file tests/test_mockex_tcp_echo.cpp
/// Integration test for the `tcp` scenario.
///
/// Forks a mockex child with `--scenario tcp --config <tmp>`, waits
/// for the listener to come up, connects as a plain TCP client, sends
/// a 256-byte payload with the bench Phase-11.1 timestamp block, and
/// asserts that the echoed bytes carry the mock's t_recv / t_send
/// stamps at [8:16] / [16:24] — both non-zero, t_send ≥ t_recv, and
/// the filler block at [24:] is preserved verbatim.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

/// Write a minimal TOML config.toml to a scratch path and return the path.
std::string write_tmp_conf(uint16_t port, const char* section,
                           const char* extra_scenario_keys = "") {
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
      << "port = " << port << "\n"
      << extra_scenario_keys;
    return path;
}

/// Pick an ephemeral port by binding to 0 and reading back sockname,
/// then closing. Small race window but acceptable for tests.
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

/// Fork mockex as a child process. Returns the PID on success; the
/// caller is responsible for kill + waitpid.
pid_t spawn_mockex(const char* scenario, const std::string& conf_path) {
    pid_t pid = ::fork();
    if (pid == 0) {
        // Child. exec the binary so we inherit whatever signal handlers
        // it installs rather than fighting with gtest's.
        ::execl(MOCKEX_BINARY_PATH, "mockex",
                "--scenario", scenario,
                "--config",   conf_path.c_str(),
                static_cast<char*>(nullptr));
        // If execl returns, something broke. Exit hard — the parent
        // detects via waitpid.
        std::perror("execl(mockex)");
        std::_Exit(127);
    }
    return pid;
}

/// Open a TCP client to 127.0.0.1:port. Retries connect() until the
/// mock's listener is ready (handshake race) or the deadline fires.
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
            int one = 1;
            ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            return s;
        }
        ::close(s);
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return -1;
}

/// Read a little-endian uint64 starting at byte `off` in `buf`.
uint64_t get_u64_le(const uint8_t* buf, size_t off) noexcept {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(buf[off + static_cast<size_t>(i)])
             << (i * 8);
    }
    return v;
}

/// Signal the child, waitpid with a bounded timeout, and if it's still
/// alive hit it with SIGKILL. Zombies from hung tests are toxic to CI.
void kill_and_reap(pid_t pid) {
    ::kill(pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        int status = 0;
        pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) return;
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
}

} // namespace

TEST(MockexTcpEcho, EchoesPayloadAndStampsMockTimestamps) {
    const uint16_t port = pick_port();
    const auto conf = write_tmp_conf(port, "lat_tcp");
    pid_t pid = spawn_mockex("tcp", conf);
    ASSERT_GT(pid, 0);

    int cfd = connect_with_retry(port, std::chrono::seconds{3});
    ASSERT_GE(cfd, 0) << "failed to connect within deadline";

    // Build a 256-byte payload: [0:8]=t_client, [8:16]=0 placeholder,
    // [16:24]=0 placeholder, [24:] = pseudo-random filler with a
    // recognizable pattern so we can assert it round-trips intact.
    constexpr size_t kPayload = 256;
    std::vector<uint8_t> tx(kPayload, 0);
    const uint64_t t_client = 0x0102030405060708ull;  // arbitrary marker
    for (int i = 0; i < 8; ++i) tx[static_cast<size_t>(i)] =
        static_cast<uint8_t>((t_client >> (i * 8)) & 0xFFu);
    for (size_t i = 24; i < kPayload; ++i) tx[i] =
        static_cast<uint8_t>((i * 31u + 7u) & 0xFFu);

    ASSERT_EQ(::send(cfd, tx.data(), tx.size(), 0),
              static_cast<ssize_t>(tx.size()));

    std::vector<uint8_t> rx(kPayload, 0xAB);
    size_t got = 0;
    while (got < kPayload) {
        ssize_t n = ::recv(cfd, rx.data() + got, kPayload - got, 0);
        ASSERT_GT(n, 0) << "short read: got=" << got;
        got += static_cast<size_t>(n);
    }

    // Client timestamp must round-trip unchanged.
    EXPECT_EQ(get_u64_le(rx.data(), 0), t_client);
    // Mock must have stamped [8:16] and [16:24] with non-zero values.
    const uint64_t t_mock_recv = get_u64_le(rx.data(), 8);
    const uint64_t t_mock_send = get_u64_le(rx.data(), 16);
    EXPECT_GT(t_mock_recv, 0u);
    EXPECT_GT(t_mock_send, 0u);
    EXPECT_GE(t_mock_send, t_mock_recv)
        << "t_mock_send=" << t_mock_send
        << " must be >= t_mock_recv=" << t_mock_recv;

    // Filler block must be byte-identical.
    for (size_t i = 24; i < kPayload; ++i) {
        ASSERT_EQ(rx[i], tx[i]) << "filler mismatch at byte " << i;
    }

    ::close(cfd);
    kill_and_reap(pid);
    std::remove(conf.c_str());
}
