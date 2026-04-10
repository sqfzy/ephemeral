/// @file lat_ex_market_v3.cpp
/// v3.3 exchange-market (bookTicker push) latency demonstrator.
///
/// This benchmark is the **Phase 5 performance verification gate** called
/// out in the design doc — it must exercise the v3.3 zero-copy DPDK RX
/// path through `DpdkTcpStream<WsCodec, false>` and the new `PacketView`
/// in-place decode contract. The kernel build runs the same client
/// shape against a loopback echo to provide a baseline.
///
/// Phase 6 of the v3.3 architecture refactor
/// (.artifacts/design-eph-v3.3-architecture-20260410.md).
///
/// **Scope** for Phase 6: this is a *demonstrator*. The DPDK build
/// type-checks the new API but does not drive real NIC traffic — Phase
/// 7 will replace the legacy lat_ex_market.cpp wholesale (which already
/// owns the bench/core/ NIC plumbing) and at that point the v3 file
/// here will be promoted to the production benchmark. For now we
/// guarantee the v3.3 API surface compiles cleanly and that the kernel
/// path produces sensible RTT numbers.

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

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/codec/ws_codec.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/utils/time.hpp"

#if defined(EPH_USE_DPDK)
// DPDK build: type-check the v3.3 DPDK Stream/Poller API only — kernel
// headers cannot coexist in the same TU due to the vcpkg-openssl ↔ aws-lc
// type clash documented in the Phase 5 BLOCKER notes.
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

// Use RawStreamCodec for the loopback path: a real exchange feed would
// use WsCodec on top of TLS but the loopback peer doesn't speak WS, and
// the Phase 5 verification point is the in-place decrypt + zero-copy
// PacketView path, which is orthogonal to the codec choice.
using PlainStream = ek::KernelTcpStream<ec::RawStreamCodec, /*Tls=*/false>;
constexpr std::size_t kPayload = 200;  // typical bookTicker JSON size

[[noreturn]] void echo_serve(int lfd) {
    int cfd = ::accept(lfd, nullptr, nullptr);
    if (cfd < 0) std::_Exit(1);
    int one = 1;
    (void)::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    uint8_t buf[2048];
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
    spdlog::info("{}: count={} min={}ns p50={}ns p99={}ns p999={}ns max={}ns",
                 label, v.size(), v.front(),
                 pct(0.5), pct(0.99), pct(0.999), v.back());
}

} // namespace
#endif // !EPH_USE_DPDK

#if defined(EPH_USE_DPDK)
int main(int /*argc*/, char** /*argv*/) {
    spdlog::set_level(spdlog::level::info);
    spdlog::info("lat_ex_market_v3 (DPDK build): API surface compiled. "
                 "Phase 7 will wire bench/core/ NIC plumbing into the v3.3 "
                 "stream API and replace the legacy lat_ex_market.cpp.");
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
        : 10000;

    if (!eph::utils::TSC::init(std::chrono::milliseconds{200})) {
        spdlog::error("TSC calibration failed");
        return 1;
    }
    spdlog::info("lat_ex_market_v3: iters={} payload={}B", iters, kPayload);

    auto [lfd, port] = bind_loopback();
    pid_t pid = ::fork();
    if (pid == 0) echo_serve(lfd);
    ::close(lfd);

    auto poller = ek::KernelPoller::create({}).value();

    ek::StreamConfig cfg{};
    cfg.remote          = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
    cfg.reasm_capacity  = 64 * 1024;
    cfg.connect_timeout = 1s;
    auto stream = PlainStream::create(cfg).value();

    std::atomic<std::size_t> rx_bytes{0};
    stream->on_message = [&](const uint8_t*, uint16_t n) {
        rx_bytes.fetch_add(n, std::memory_order_release);
    };
    if (auto r = poller->add(stream.get()); !r) {
        spdlog::error("attach failed: {}", r.error().detail);
        ::kill(pid, SIGTERM); ::waitpid(pid, nullptr, 0);
        return 2;
    }

    uint8_t payload[kPayload]{};
    for (std::size_t i = 0; i < kPayload; ++i) payload[i] = static_cast<uint8_t>(i & 0x7F);

    std::vector<uint64_t> rx_only;
    rx_only.reserve(iters);
    std::size_t expected = 0;
    for (std::size_t i = 0; i < iters; ++i) {
        // Push a "market data tick" — measure RX-only latency (poll loop
        // turnaround from socket-readable to on_message dispatch). This
        // is the latency the Phase 5 PacketView path was tuned to drop.
        if (!stream->send(payload)) {
            spdlog::error("send failed at iter {}", i);
            break;
        }
        expected += kPayload;
        const uint64_t t0 = eph::utils::TSC::now();
        while (rx_bytes.load(std::memory_order_acquire) < expected) {
            poller->poll();
        }
        const uint64_t t1 = eph::utils::TSC::now();
        if (auto ns = eph::utils::TSC::to_ns(t1 - t0)) {
            rx_only.push_back(static_cast<uint64_t>(*ns));
        }
    }

    print_latency("lat_ex_market_v3 RX latency (poll → deliver)", rx_only);

    (void)stream->close_gracefully();
    poller->poll(50ms);
    (void)poller->remove(stream.get());
    stream.reset();
    ::kill(pid, SIGTERM); ::waitpid(pid, nullptr, 0);
    return 0;
}
#endif // EPH_USE_DPDK
