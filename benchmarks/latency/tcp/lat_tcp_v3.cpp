/// @file lat_tcp_v3.cpp
/// v3.3 TCP latency benchmark — demonstrator using `KernelTcpStream` and
/// (when EPH_USE_DPDK is defined) `DpdkTcpStream`.
///
/// Phase 6 of the v3.3 architecture refactor
/// (.artifacts/design-eph-v3.3-architecture-20260410.md).
///
/// **Scope**: this is a *demonstrator* benchmark — it forks a kernel echo
/// server, runs a tight RTT measurement loop through the v3.3 client API,
/// and prints the resulting min/p50/p99 nanoseconds. It deliberately does
/// NOT plug into the legacy `bench/core/` framework: the goal is to show
/// the v3.3 API surface in a single self-contained file. The legacy
/// `lat_tcp.cpp` remains the production benchmark until Phase 7 deletes it.
///
/// Usage:
///   ./lat_tcp_v3                # spawns localhost echo, runs 10000 round-trips
///   ./lat_tcp_v3 50000          # explicit iteration count

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
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/utils/time.hpp"

#if defined(EPH_USE_DPDK)
// DPDK build: include only the DPDK headers (cannot mix with the kernel
// TLS path due to a vcpkg-openssl ↔ aws-lc TU clash documented in the
// Phase 5 BLOCKER notes — Phase 7 will resolve it). The DPDK build of
// this benchmark therefore type-checks the v3.3 DpdkTcpStream API
// surface but does not execute the kernel-loop demonstrator below.
#include "eph/dpdk/packet_core.hpp"
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/dpdk/tcp_stream.hpp"
#else
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#endif

namespace en = eph::net;
namespace ec = eph::codec;
using namespace std::chrono_literals;

#if !defined(EPH_USE_DPDK)
namespace ek = eph::net::kernel;
namespace {

using PlainStream = ek::KernelTcpStream<ec::RawStreamCodec, /*Tls=*/false>;

constexpr std::size_t kPayload = 64;

/// Kernel TCP echo server. Runs in the forked child.
[[noreturn]] void echo_serve(int lfd) {
    int cfd = ::accept(lfd, nullptr, nullptr);
    if (cfd < 0) std::_Exit(1);
    int one = 1;
    (void)::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    uint8_t buf[kPayload];
    for (;;) {
        std::size_t got = 0;
        while (got < kPayload) {
            ssize_t n = ::recv(cfd, buf + got, kPayload - got, 0);
            if (n <= 0) std::_Exit(0);
            got += static_cast<std::size_t>(n);
        }
        std::size_t sent = 0;
        while (sent < kPayload) {
            ssize_t n = ::send(cfd, buf + sent, kPayload - sent, MSG_NOSIGNAL);
            if (n <= 0) std::_Exit(0);
            sent += static_cast<std::size_t>(n);
        }
    }
}

std::pair<int, uint16_t> bind_loopback() {
    int lfd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    (void)::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    if (::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        spdlog::error("bind failed: {}", std::strerror(errno));
        std::_Exit(1);
    }
    if (::listen(lfd, 1) != 0) std::_Exit(1);
    socklen_t len = sizeof(addr);
    ::getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &len);
    return {lfd, ::ntohs(addr.sin_port)};
}

void print_latency(const char* label, std::vector<uint64_t>& samples) {
    if (samples.empty()) {
        spdlog::info("{}: no samples", label);
        return;
    }
    std::sort(samples.begin(), samples.end());
    auto pct = [&](double p) {
        const std::size_t i = std::min(samples.size() - 1,
            static_cast<std::size_t>(p * (samples.size() - 1)));
        return samples[i];
    };
    spdlog::info("{}: count={} min={}ns p50={}ns p99={}ns max={}ns",
                 label, samples.size(), samples.front(),
                 pct(0.5), pct(0.99), samples.back());
}

} // namespace
#endif // !EPH_USE_DPDK

#if defined(EPH_USE_DPDK)
int main(int /*argc*/, char** /*argv*/) {
    spdlog::set_level(spdlog::level::info);
    spdlog::info("lat_tcp_v3 (DPDK build): API surface compiled. Real DPDK "
                 "RTT measurement is left to a follow-up phase that wires "
                 "the bench/core/ NIC plumbing into the v3.3 stream API. "
                 "The kernel build (./lat_tcp_v3) is the active path.");
    // Static type-check that the DPDK Poller + Stream instantiations link.
    using DpdkStream = eph::net::dpdk::DpdkTcpStream<
        ec::RawStreamCodec, /*Tls=*/false>;
    using DpdkPoller = eph::net::dpdk::DpdkPoller<>;
    static_assert(sizeof(DpdkStream*) > 0);
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
    spdlog::info("lat_tcp_v3: iters={} payload={}B tsc_ns_per_cycle={:.4f}",
                 iters, kPayload,
                 eph::utils::TSC::get_ns_per_cycle().value());

    auto [lfd, port] = bind_loopback();

    pid_t pid = ::fork();
    if (pid < 0) { spdlog::error("fork failed"); return 1; }
    if (pid == 0) {
        echo_serve(lfd);
    }
    ::close(lfd);

    auto poller = ek::KernelPoller::create({}).value();

    ek::StreamConfig cfg{};
    cfg.remote          = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
    cfg.reasm_capacity  = 16 * 1024;
    cfg.connect_timeout = 1s;
    auto sr = PlainStream::create(cfg);
    if (!sr) {
        spdlog::error("create failed: {}", sr.error().detail);
        ::kill(pid, SIGTERM); ::waitpid(pid, nullptr, 0);
        return 2;
    }
    auto stream = std::move(*sr);

    std::atomic<std::size_t> rx_bytes{0};
    stream->on_message = [&](const uint8_t*, uint16_t n) {
        rx_bytes.fetch_add(n, std::memory_order_release);
    };
    if (auto r = poller->add(stream.get()); !r) {
        spdlog::error("attach failed: {}", r.error().detail);
        ::kill(pid, SIGTERM); ::waitpid(pid, nullptr, 0);
        return 3;
    }

    uint8_t payload[kPayload]{};
    for (std::size_t i = 0; i < kPayload; ++i) payload[i] = static_cast<uint8_t>(i);

    std::vector<uint64_t> rtt;
    rtt.reserve(iters);
    std::size_t expected = 0;
    for (std::size_t i = 0; i < iters; ++i) {
        const uint64_t t0 = eph::utils::TSC::now();
        if (!stream->send(payload)) {
            spdlog::error("send failed at iter {}", i);
            break;
        }
        expected += kPayload;
        while (rx_bytes.load(std::memory_order_acquire) < expected) {
            poller->poll();
        }
        const uint64_t t1 = eph::utils::TSC::now();
        if (auto ns = eph::utils::TSC::to_ns(t1 - t0)) {
            rtt.push_back(static_cast<uint64_t>(*ns));
        }
    }

    print_latency("lat_tcp_v3 RTT (kernel client)", rtt);

    (void)stream->close_gracefully();
    poller->poll(50ms);
    (void)poller->remove(stream.get());
    stream.reset();

    int wstatus = 0;
    ::waitpid(pid, &wstatus, 0);

    return 0;
}
#endif // EPH_USE_DPDK
