/// @file lat_udp_v3.cpp
/// v3.3 UDP latency demonstrator using `KernelUdpSocket<RawDatagramCodec>`.
///
/// Phase 6 of the v3.3 architecture refactor
/// (.artifacts/design-eph-v3.3-architecture-20260410.md).
///
/// Same shape as lat_tcp_v3.cpp: forks a kernel UDP echo peer, sends a
/// fixed-size datagram in a tight loop through the v3.3 client API, and
/// prints the resulting RTT distribution. Single-file demonstrator.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "eph/codec/raw_datagram_codec.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/utils/time.hpp"

#if defined(EPH_USE_DPDK)
// DPDK build: type-check the DPDK API only. The kernel client below does
// not coexist with the DPDK headers in the same TU due to the
// vcpkg-openssl ↔ aws-lc TU clash documented in the Phase 5 BLOCKER notes.
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/dpdk/udp_socket.hpp"
#else
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/udp_socket.hpp"
#endif

namespace en = eph::net;
namespace ec = eph::codec;
using namespace std::chrono_literals;

#if !defined(EPH_USE_DPDK)
namespace ek = eph::net::kernel;
namespace {

constexpr std::size_t kPayload = 64;
using PlainUdp = ek::KernelUdpSocket<ec::RawDatagramCodec>;

[[noreturn]] void echo_serve(int fd) {
    uint8_t buf[2048];
    sockaddr_in src{};
    socklen_t   slen = sizeof(src);
    for (;;) {
        ssize_t n = ::recvfrom(fd, buf, sizeof(buf), 0,
                                reinterpret_cast<sockaddr*>(&src), &slen);
        if (n <= 0) std::_Exit(0);
        ::sendto(fd, buf, static_cast<size_t>(n), 0,
                 reinterpret_cast<sockaddr*>(&src), slen);
    }
}

std::pair<int, uint16_t> bind_loopback_udp() {
    int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    return {fd, ::ntohs(addr.sin_port)};
}

void print_latency(const char* label, std::vector<uint64_t>& v) {
    if (v.empty()) { spdlog::info("{}: no samples", label); return; }
    std::sort(v.begin(), v.end());
    auto pct = [&](double p) {
        return v[std::min(v.size() - 1, static_cast<std::size_t>(p * (v.size() - 1)))];
    };
    spdlog::info("{}: count={} min={}ns p50={}ns p99={}ns max={}ns",
                 label, v.size(), v.front(), pct(0.5), pct(0.99), v.back());
}

} // namespace
#endif // !EPH_USE_DPDK

#if defined(EPH_USE_DPDK)
int main(int /*argc*/, char** /*argv*/) {
    spdlog::set_level(spdlog::level::info);
    spdlog::info("lat_udp_v3 (DPDK build): API surface compiled.");
    using DpdkUdp    = eph::net::dpdk::DpdkUdpSocket<ec::RawDatagramCodec>;
    using DpdkPoller = eph::net::dpdk::DpdkPoller<>;
    static_assert(sizeof(DpdkUdp*) > 0);
    static_assert(sizeof(DpdkPoller*) > 0);
    return 0;
}
#else
int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::info);

    const std::size_t iters = (argc > 1)
        ? static_cast<std::size_t>(std::atoll(argv[1]))
        : 10000;

    if (!eph::utils::TSC::init(std::chrono::milliseconds{200})) {
        spdlog::error("TSC calibration failed");
        return 1;
    }
    spdlog::info("lat_udp_v3: iters={} payload={}B", iters, kPayload);

    auto [peer_fd, peer_port] = bind_loopback_udp();
    pid_t pid = ::fork();
    if (pid == 0) echo_serve(peer_fd);
    ::close(peer_fd);

    auto poller = ek::KernelPoller::create({}).value();

    ek::UdpConfig cfg{};
    cfg.bind = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, 0};
    auto sock = PlainUdp::create(cfg).value();

    std::atomic<std::size_t> rx_count{0};
    sock->on_datagram = [&](const uint8_t*, uint16_t, const en::SocketAddr&) {
        rx_count.fetch_add(1, std::memory_order_release);
    };
    if (auto r = poller->add(sock.get()); !r) {
        spdlog::error("attach failed: {}", r.error().detail);
        ::kill(pid, SIGTERM); ::waitpid(pid, nullptr, 0);
        return 2;
    }

    en::SocketAddr peer{en::Ipv4Addr{127, 0, 0, 1}, peer_port};
    uint8_t payload[kPayload]{};

    std::vector<uint64_t> rtt;
    rtt.reserve(iters);
    std::size_t expected = 0;
    for (std::size_t i = 0; i < iters; ++i) {
        const uint64_t t0 = eph::utils::TSC::now();
        if (!sock->send_to(payload, peer)) {
            spdlog::error("send_to failed at iter {}", i);
            break;
        }
        ++expected;
        while (rx_count.load(std::memory_order_acquire) < expected) {
            poller->poll();
        }
        const uint64_t t1 = eph::utils::TSC::now();
        if (auto ns = eph::utils::TSC::to_ns(t1 - t0)) {
            rtt.push_back(static_cast<uint64_t>(*ns));
        }
    }

    print_latency("lat_udp_v3 RTT (kernel client)", rtt);

    ::kill(pid, SIGTERM); ::waitpid(pid, nullptr, 0);
    return 0;
}
#endif // EPH_USE_DPDK
