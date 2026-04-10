/// @file lat_ws_v3.cpp
/// v3.3 WebSocket latency demonstrator using
/// `KernelTcpStream<WsCodec, false>`.
///
/// Phase 6 of the v3.3 architecture refactor
/// (.artifacts/design-eph-v3.3-architecture-20260410.md).
///
/// **Scope**: this benchmark exercises the v3.3 codec contract for WS by
/// driving WS-binary frames through a self-contained loopback echo loop.
/// We do NOT perform a real HTTP/1.1 Upgrade handshake — the codec under
/// test is the *frame* layer, not the upgrade dance. The peer is a tiny
/// custom TCP echoer that just bounces every byte; combined with WsCodec
/// on the client side this measures the codec's per-frame parsing cost
/// inside the v3.3 RX pipeline.

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
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "eph/codec/ws_codec.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/utils/time.hpp"

#if defined(EPH_USE_DPDK)
// DPDK build: type-check only — vcpkg-openssl ↔ aws-lc TU clash blocks
// pulling the kernel headers in the same TU as the DPDK headers.
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

using WsStream = ek::KernelTcpStream<ec::WsCodec, /*Tls=*/false>;

[[noreturn]] void echo_serve(int lfd) {
    int cfd = ::accept(lfd, nullptr, nullptr);
    if (cfd < 0) std::_Exit(1);
    int one = 1;
    (void)::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    uint8_t buf[4096];
    for (;;) {
        ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
        if (n <= 0) std::_Exit(0);
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = ::send(cfd, buf + off, n - off, MSG_NOSIGNAL);
            if (w <= 0) std::_Exit(0);
            off += w;
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
    ::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::listen(lfd, 1);
    socklen_t alen = sizeof(addr);
    ::getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &alen);
    return {lfd, ::ntohs(addr.sin_port)};
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
    spdlog::info("lat_ws_v3 (DPDK build): API surface compiled.");
    using DpdkStream = eph::net::dpdk::DpdkTcpStream<ec::WsCodec, /*Tls=*/false>;
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
        : 5000;
    constexpr std::size_t kPayload = 64;

    if (!eph::utils::TSC::init(std::chrono::milliseconds{200})) {
        spdlog::error("TSC calibration failed");
        return 1;
    }
    spdlog::info("lat_ws_v3: iters={} payload={}B (WS-binary frame)",
                 iters, kPayload);

    auto [lfd, port] = bind_loopback();
    pid_t pid = ::fork();
    if (pid == 0) echo_serve(lfd);
    ::close(lfd);

    auto poller = ek::KernelPoller::create({}).value();

    ek::StreamConfig cfg{};
    cfg.remote          = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
    cfg.reasm_capacity  = 64 * 1024;
    cfg.connect_timeout = 1s;
    auto sr = WsStream::create(cfg);
    if (!sr) {
        spdlog::error("create failed: {}", sr.error().detail);
        ::kill(pid, SIGTERM); ::waitpid(pid, nullptr, 0);
        return 2;
    }
    auto stream = std::move(*sr);

    std::atomic<std::size_t> rx_frames{0};
    stream->on_message = [&](const uint8_t*, uint16_t) {
        rx_frames.fetch_add(1, std::memory_order_release);
    };
    if (auto r = poller->add(stream.get()); !r) {
        spdlog::error("attach failed: {}", r.error().detail);
        ::kill(pid, SIGTERM); ::waitpid(pid, nullptr, 0);
        return 3;
    }

    // Pre-encode the WS-binary frame once. WsCodec::encode is the unmasked
    // server-side variant; for a *server* echo (which is how the loopback
    // peer behaves) the unmasked client frame is what the codec parses on
    // RX. To keep the demonstrator self-contained we send our own
    // pre-encoded unmasked frame and let the echoer bounce it back.
    uint8_t payload[kPayload]{};
    for (std::size_t i = 0; i < kPayload; ++i) payload[i] = static_cast<uint8_t>(i);

    uint8_t frame[kPayload + 16];
    ec::WsCodec encoder{};
    auto enc = encoder.encode(frame, sizeof(frame),
                              std::span<const uint8_t>(payload, kPayload));
    if (!enc) {
        spdlog::error("WsCodec::encode failed: {}", enc.error().detail);
        ::kill(pid, SIGTERM); ::waitpid(pid, nullptr, 0);
        return 4;
    }
    std::span<const uint8_t> wire(frame, *enc);

    std::vector<uint64_t> rtt;
    rtt.reserve(iters);
    std::size_t expected = 0;
    for (std::size_t i = 0; i < iters; ++i) {
        const uint64_t t0 = eph::utils::TSC::now();
        if (!stream->send(wire)) {
            spdlog::error("send failed at iter {}", i);
            break;
        }
        ++expected;
        while (rx_frames.load(std::memory_order_acquire) < expected) {
            poller->poll();
        }
        const uint64_t t1 = eph::utils::TSC::now();
        if (auto ns = eph::utils::TSC::to_ns(t1 - t0)) {
            rtt.push_back(static_cast<uint64_t>(*ns));
        }
    }

    print_latency("lat_ws_v3 RTT (kernel client + WsCodec)", rtt);

    (void)stream->close_gracefully();
    poller->poll(50ms);
    (void)poller->remove(stream.get());
    stream.reset();

    ::kill(pid, SIGTERM); ::waitpid(pid, nullptr, 0);
    return 0;
}
#endif // EPH_USE_DPDK
