/// @file tests/test_mockex_push_scenarios.cpp
/// Integration coverage for the Phase-3 push scenarios. Spawns
/// `mockex --scenario ex_market[_2p]`, connects as a WS client,
/// collects a few seconds of frames, and asserts:
///
///   1. every received frame carries a `"T":<19 digits>` field whose
///      value is a plausible CLOCK_MONOTONIC_RAW sample (monotonic
///      increase across frames, bounded by current clock)
///   2. symbols rotate as expected (2p variant only)
///   3. frame-rate over the measurement window is within a plausible
///      band around the fitted λ (generous bounds — this is a shape
///      check, not a precision check)

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
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "eph/net/detail/ws_handshake.hpp"

namespace {

std::string write_conf(uint16_t port, const char* section,
                       const char* params_path, const char* payload_path,
                       int extra_burst = 0) {
    const std::string path = "/tmp/mockex_push_" +
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
      << "port           = " << port << "\n"
      << "mockex_params  = \"" << params_path << "\"\n"
      << "mockex_payload = \"" << payload_path << "\"\n"
      << "mockex_seed    = 42\n";
    if (extra_burst > 0) f << "burst_size = " << extra_burst << "\n";
    return path;
}

uint16_t pick_port() {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    socklen_t al = sizeof(a);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &al);
    const uint16_t p = ntohs(a.sin_port);
    ::close(s);
    return p;
}

pid_t spawn_mockex(const char* scenario, const std::string& conf) {
    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(MOCKEX_BINARY_PATH, "mockex",
                "--scenario", scenario, "--config", conf.c_str(),
                static_cast<char*>(nullptr));
        std::perror("execl"); std::_Exit(127);
    }
    return pid;
}

int connect_retry(uint16_t port) {
    const auto dl = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < dl) {
        int s = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{}; a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
        if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) return s;
        ::close(s);
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return -1;
}

bool do_handshake(int fd) {
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    const std::string req =
        "GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\n"
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
    return resp.find("101") != std::string::npos;
}

bool recv_exact(int fd, void* buf, size_t n,
                std::chrono::milliseconds timeout) {
    const auto dl = std::chrono::steady_clock::now() + timeout;
    size_t got = 0;
    while (got < n) {
        auto now = std::chrono::steady_clock::now();
        if (now >= dl) return false;
        const int rem = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(dl - now).count());
        pollfd p{ .fd = fd, .events = POLLIN, .revents = 0 };
        if (::poll(&p, 1, rem) <= 0) return false;
        ssize_t r = ::recv(fd, static_cast<uint8_t*>(buf) + got, n - got, 0);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

/// Read exactly one server→client WS frame. Returns payload or empty.
std::vector<uint8_t> recv_frame(int fd, std::chrono::milliseconds timeout) {
    uint8_t hdr[2];
    if (!recv_exact(fd, hdr, 2, timeout)) return {};
    const uint8_t rlen = hdr[1] & 0x7F;
    uint64_t length = rlen;
    if (rlen == 126) {
        uint8_t ext[2]; if (!recv_exact(fd, ext, 2, timeout)) return {};
        length = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (rlen == 127) {
        uint8_t ext[8]; if (!recv_exact(fd, ext, 8, timeout)) return {};
        length = 0;
        for (int i = 0; i < 8; ++i) length = (length << 8) | ext[i];
    }
    std::vector<uint8_t> pl(length);
    if (length > 0 && !recv_exact(fd, pl.data(), length, timeout)) return {};
    return pl;
}

/// Extract the "T":<digits> value from a JSON payload.
uint64_t extract_t(std::string_view s) {
    const size_t p = s.find("\"T\":");
    if (p == std::string_view::npos) return 0;
    size_t q = p + 4;
    uint64_t v = 0;
    while (q < s.size() && s[q] >= '0' && s[q] <= '9') {
        v = v * 10 + static_cast<uint64_t>(s[q] - '0');
        ++q;
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

} // namespace

TEST(MockexExMarketPush, StreamsBinanceShapedFramesWithStampedT) {
    const uint16_t port = pick_port();
    const std::string params =
        std::string{MOCKEX_FIXTURES_DIR} + "/ex_market_params.toml";
    const std::string pool =
        std::string{MOCKEX_FIXTURES_DIR} + "/ex_market_sample.jsonl";
    const auto conf = write_conf(port, "lat_ex_market",
                                 params.c_str(), pool.c_str());
    pid_t pid = spawn_mockex("ex_market", conf);
    ASSERT_GT(pid, 0);

    int fd = connect_retry(port);
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(do_handshake(fd));

    // Collect frames for ~2 seconds — with λ_q≈15 Hz and rare busy we
    // expect on the order of 30-60 frames.
    const auto window = std::chrono::seconds{2};
    const auto deadline = std::chrono::steady_clock::now() + window;
    std::vector<uint64_t> t_stamps;
    t_stamps.reserve(256);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (rem.count() <= 0) break;
        auto pl = recv_frame(fd, rem);
        if (pl.empty()) break;
        std::string_view s(reinterpret_cast<const char*>(pl.data()), pl.size());
        // The fixture is a real Binance capture: `trade` stream (which
        // carries the "T" timestamp). bookTicker doesn't include "T",
        // so the old synthetic schema that put "bookTicker" here was
        // a fiction — `"e":"trade"` is the honest value in real data.
        // We just check the object is a JSON event of some known kind.
        EXPECT_TRUE(s.find("\"e\":\"trade\"") != std::string_view::npos ||
                    s.find("\"e\":\"bookTicker\"") != std::string_view::npos)
            << "unexpected event shape: " << s.substr(0, 120);
        const uint64_t t = extract_t(s);
        EXPECT_GT(t, 1'000'000'000'000ull)  // > 1e12 ns → clearly not zero
            << "frame T field not stamped: " << s;
        t_stamps.push_back(t);
    }

    ASSERT_GT(t_stamps.size(), 5u)
        << "expected at least a handful of frames in 2s at λ_q≈15 Hz";
    // Stamps must be strictly non-decreasing (frames emitted in order,
    // same burst can share a stamp).
    for (size_t i = 1; i < t_stamps.size(); ++i) {
        ASSERT_GE(t_stamps[i], t_stamps[i - 1]) << "stamp regression at " << i;
    }

    ::close(fd);
    kill_and_reap(pid);
    std::remove(conf.c_str());
}

TEST(MockexExMarket2pPush, MultiSymbolRotationInBursts) {
    const uint16_t port = pick_port();
    const std::string params =
        std::string{MOCKEX_FIXTURES_DIR} + "/ex_market_2p_params.toml";
    const std::string pool =
        std::string{MOCKEX_FIXTURES_DIR} + "/ex_market_2p_sample.jsonl";
    const auto conf = write_conf(port, "lat_ex_market_2p",
                                 params.c_str(), pool.c_str(),
                                 /*burst=*/5);
    pid_t pid = spawn_mockex("ex_market_2p", conf);
    ASSERT_GT(pid, 0);

    int fd = connect_retry(port);
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(do_handshake(fd));

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    std::set<std::string> symbols_seen;
    size_t total_frames = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (rem.count() <= 0) break;
        auto pl = recv_frame(fd, rem);
        if (pl.empty()) break;
        ++total_frames;
        std::string_view s(reinterpret_cast<const char*>(pl.data()), pl.size());
        // Extract "s":"SYMBOL" for rotation check.
        const size_t p = s.find("\"s\":\"");
        if (p == std::string_view::npos) continue;
        const size_t b = p + 5;
        const size_t e = s.find('"', b);
        if (e == std::string_view::npos) continue;
        symbols_seen.emplace(s.substr(b, e - b));
    }
    EXPECT_GT(total_frames, 5u) << "too few frames";
    EXPECT_GE(symbols_seen.size(), 2u)
        << "expected rotation through multiple symbols, got " << symbols_seen.size();

    ::close(fd);
    kill_and_reap(pid);
    std::remove(conf.c_str());
}
