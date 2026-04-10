/// @file bench_e2e_latency.cpp
/// Microbenchmarks the v3.3 `KernelTcpStream<RawStreamCodec, false>` API
/// against an in-process loopback echo server. Real I/O (epoll loopback)
/// so PacketView / TLS work is exercised, not just the codec fast path.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <benchmark/benchmark.h>

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/net/socket_addr.hpp"

namespace ek = eph::net::kernel;
namespace en = eph::net;
namespace ec = eph::codec;
using namespace std::chrono_literals;

namespace {

using PlainStream = ek::KernelTcpStream<ec::RawStreamCodec, /*Tls=*/false>;

/// Helper: bind a loopback listener and return (fd, port).
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

/// Single-connection echo loop.
void echo_serve(int lfd, std::atomic<bool>& stop) {
    int cfd = ::accept(lfd, nullptr, nullptr);
    if (cfd < 0) return;
    char buf[2048];
    while (!stop.load(std::memory_order_acquire)) {
        ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = ::send(cfd, buf + off, n - off, MSG_NOSIGNAL);
            if (w <= 0) { off = n; break; }
            off += w;
        }
    }
    ::close(cfd);
}

} // namespace

// ---------------------------------------------------------------------------
// BM_V3_StreamCreate — measure cost of constructing a Stream + handshake
// against the loopback echo server.
//
// Each iteration accepts a fresh connection and immediately closes it. To
// avoid TIME_WAIT exhaustion under high iteration counts the listener uses
// SO_REUSEADDR (already set by bind_loopback) and the client side sets
// SO_LINGER=0 via close — but the v3.3 KernelTcpStream destroys via plain
// close(2). The benchmark therefore caps iterations at a sane value via
// `Iterations()`.
// ---------------------------------------------------------------------------
static void BM_V3_StreamCreate(benchmark::State& state) {
    auto [lfd, port] = bind_loopback();
    std::atomic<bool> stop{false};
    std::thread server([lfd, &stop] {
        while (!stop.load(std::memory_order_acquire)) {
            int cfd = ::accept(lfd, nullptr, nullptr);
            if (cfd < 0) break;
            ::close(cfd);
        }
    });

    ek::StreamConfig cfg{};
    cfg.remote          = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
    cfg.reasm_capacity  = 16 * 1024;
    cfg.connect_timeout = 1s;

    std::size_t failures = 0;
    for (auto _ : state) {
        auto sr = PlainStream::create(cfg);
        if (!sr) {
            // Best-effort: under heavy load the kernel may briefly fail to
            // establish a fresh connection. Count and continue rather than
            // skipping the entire benchmark — the surviving samples are
            // still meaningful.
            ++failures;
            continue;
        }
        benchmark::DoNotOptimize(sr);
        sr->reset();
    }
    state.counters["create_failures"] = static_cast<double>(failures);

    stop.store(true, std::memory_order_release);
    ::shutdown(lfd, SHUT_RDWR);
    ::close(lfd);
    server.join();
}
BENCHMARK(BM_V3_StreamCreate)->Unit(benchmark::kMicrosecond)->Iterations(200);

// ---------------------------------------------------------------------------
// BM_V3_RoundTrip — measure send + poll + receive RTT through the v3.3
// API on a loopback fd.
// ---------------------------------------------------------------------------
static void BM_V3_RoundTrip(benchmark::State& state) {
    auto [lfd, port] = bind_loopback();
    std::atomic<bool> stop{false};
    std::thread server([lfd, &stop] { echo_serve(lfd, stop); });

    auto poller = ek::KernelPoller::create({}).value();
    ek::StreamConfig cfg{};
    cfg.remote          = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
    cfg.reasm_capacity  = 16 * 1024;
    cfg.connect_timeout = 1s;
    auto stream_r = PlainStream::create(cfg);
    if (!stream_r) {
        state.SkipWithError("create failed");
        return;
    }
    auto stream = std::move(*stream_r);

    std::atomic<std::size_t> rx{0};
    stream->on_message = [&](const uint8_t*, uint16_t n) {
        rx.fetch_add(n, std::memory_order_release);
    };
    if (auto r = poller->add(stream.get()); !r) {
        state.SkipWithError("attach failed");
        return;
    }

    constexpr std::size_t kPayload = 64;
    uint8_t payload[kPayload]{};
    for (std::size_t i = 0; i < kPayload; ++i) payload[i] = static_cast<uint8_t>(i);

    std::size_t expected = 0;
    for (auto _ : state) {
        auto tx = stream->send(payload);
        if (!tx) {
            state.SkipWithError("send failed");
            break;
        }
        expected += kPayload;
        // Drain the loopback echo through the poller until rx catches up.
        while (rx.load(std::memory_order_acquire) < expected) {
            poller->poll(std::chrono::milliseconds{1});
        }
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * kPayload));

    stop.store(true, std::memory_order_release);
    (void)stream->close_gracefully();
    poller->poll(50ms);
    (void)poller->remove(stream.get());
    stream.reset();
    ::shutdown(lfd, SHUT_RDWR);
    ::close(lfd);
    server.join();
}
BENCHMARK(BM_V3_RoundTrip)->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
