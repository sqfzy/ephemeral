/// @file tests/test_mockex_udp_echo.cpp
/// UDP echo scenario integration test. Covers both `udp` and
/// `ex_md_udp` — their protocol is identical, so one test suite
/// parameterised on scenario name / section name covers both.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <poll.h>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string write_tmp_conf(uint16_t port, const char* section) {
    const std::string path = "/tmp/mockex_test_" +
                             std::to_string(::getpid()) + "_" +
                             section + ".conf";
    std::ofstream f(path);
    f << "mock_ip = 127.0.0.1\n"
      << "client_ip = 127.0.0.1\n"
      << "cpu_mock = -1\n"
      << "warmup_samples = 1\n"
      << "[" << section << "]\n"
      << "port = " << port << "\n";
    return path;
}

uint16_t pick_port() {
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
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

/// Run the shared echo protocol check for either UDP scenario.
void run_echo_check(const char* scenario, const char* section) {
    const uint16_t port = pick_port();
    const auto conf = write_tmp_conf(port, section);
    pid_t pid = spawn_mockex(scenario, conf);
    ASSERT_GT(pid, 0);

    // Small sleep so the mock's bind() lands. UDP has no accept() we
    // can retry against from the client side without wasting a real
    // datagram, so a 100ms sleep is the cleanest approach for a test.
    std::this_thread::sleep_for(std::chrono::milliseconds{200});

    int cfd = ::socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(cfd, 0);

    sockaddr_in srv{}; srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    srv.sin_port = htons(port);

    constexpr size_t kPayload = 256;
    std::vector<uint8_t> tx(kPayload, 0);
    const uint64_t t_client = 0x0a0b0c0d0e0f1011ull;
    for (int i = 0; i < 8; ++i) tx[static_cast<size_t>(i)] =
        static_cast<uint8_t>((t_client >> (i * 8)) & 0xFFu);
    for (size_t i = 24; i < kPayload; ++i) tx[i] =
        static_cast<uint8_t>((i * 13u + 2u) & 0xFFu);

    ASSERT_EQ(::sendto(cfd, tx.data(), tx.size(), 0,
                       reinterpret_cast<sockaddr*>(&srv), sizeof(srv)),
              static_cast<ssize_t>(tx.size()));

    // Bounded poll for the reply so a mock hang doesn't deadlock CI.
    pollfd pfd{ .fd = cfd, .events = POLLIN, .revents = 0 };
    ASSERT_EQ(::poll(&pfd, 1, 3000), 1) << "UDP echo reply timeout";

    std::vector<uint8_t> rx(kPayload, 0xAB);
    ssize_t n = ::recv(cfd, rx.data(), rx.size(), 0);
    ASSERT_EQ(n, static_cast<ssize_t>(kPayload));

    EXPECT_EQ(get_u64_le(rx.data(), 0), t_client);
    EXPECT_GT(get_u64_le(rx.data(), 8),  0u);
    EXPECT_GT(get_u64_le(rx.data(), 16), 0u);
    EXPECT_GE(get_u64_le(rx.data(), 16),  get_u64_le(rx.data(), 8));
    for (size_t i = 24; i < kPayload; ++i) {
        ASSERT_EQ(rx[i], tx[i]) << "filler mismatch at " << i;
    }

    ::close(cfd);
    kill_and_reap(pid);
    std::remove(conf.c_str());
}

} // namespace

TEST(MockexUdpEcho, EchoesDatagramAndStampsTimestamps) {
    run_echo_check("udp", "lat_udp");
}

TEST(MockexExMdUdpEcho, SameProtocolAsUdpEcho) {
    run_echo_check("ex_md_udp", "lat_ex_md_udp");
}
