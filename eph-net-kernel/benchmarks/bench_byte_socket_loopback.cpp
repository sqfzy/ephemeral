/// @file bench_byte_socket_loopback.cpp
/// Microbenchmarks for `eph::net::kernel::detail::ByteSocket` send/recv
/// over a loopback echo server.
///
/// Coverage gap (loop-cleanup R167): `eph-net-kernel` had **zero**
/// benchmarks before this file. ByteSocket is the lowest-level user-space
/// shim above `::send` / `::recv` / `::poll`, and every kernel-backend
/// stream (`KernelTcpStream<C>`, with or without TLS) routes its bytes
/// through it. A regression in the EAGAIN backpressure poll, the
/// MSG_NOSIGNAL flag, the partial-send loop, or the EINTR retry would
/// silently widen tail latency without surfacing in any current bench.
///
/// We bench three shapes:
///   - send_then_recv_64B:  small payload round-trip — pins the fixed
///                          per-syscall overhead, the dominant cost for
///                          order entry and book updates.
///   - send_then_recv_4KB:  4 KiB round-trip — catches regressions in the
///                          partial-send loop above MSS where multiple
///                          ::send() calls may be needed.
///   - send_only_64B:       one-way push without the recv leg — measures
///                          the send hot path in isolation.
///
/// The bench uses a single in-process TCP echo thread on a loopback port,
/// matching the pattern used by `tests/test_byte_socket.cpp`. Loopback
/// keeps the bench reproducible (no external NIC, no DPDK env). Numbers
/// are not directly comparable to a physical-NIC RTT — the goal is to
/// pin the **user-space** cost of the ByteSocket shim relative to the
/// raw syscalls, not to measure hardware latency.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <benchmark/benchmark.h>
#include <spdlog/spdlog.h>

#include "eph/net/kernel/detail/byte_socket.hpp"
#include "eph/net/socket_addr.hpp"

namespace ek = eph::net::kernel::detail;
namespace en = eph::net;

namespace {

/// Silence WARN/INFO logs so any reject-branch noise (e.g. EPIPE on shutdown)
/// does not poison the bench timing.
struct LoggerSilencer {
    LoggerSilencer() { spdlog::set_level(spdlog::level::off); }
};
const LoggerSilencer g_silencer{};

/// In-process TCP echo helper. Accepts one client, loops recv/send until
/// the client closes. Mirrors `tests/test_byte_socket.cpp::echo_once` but
/// runs persistently for the whole bench lifetime.
struct LoopbackEcho {
    int      listen_fd = -1;
    uint16_t port      = 0;
    std::thread thr;
    std::atomic<bool> stop{false};

    LoopbackEcho() {
        listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listen_fd < 0) return;
        int one = 1;
        ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        struct sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        sa.sin_port        = 0;
        if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0
            || ::listen(listen_fd, 4) != 0) {
            ::close(listen_fd);
            listen_fd = -1;
            return;
        }
        socklen_t len = sizeof(sa);
        ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&sa), &len);
        port = ::ntohs(sa.sin_port);

        thr = std::thread([this] {
            // Single-client serializer: the bench drives one connection at
            // a time; serialise here to keep the helper trivial.
            while (!stop.load(std::memory_order_relaxed)) {
                struct sockaddr_in cli{};
                socklen_t clen = sizeof(cli);
                const int c = ::accept(listen_fd,
                                       reinterpret_cast<sockaddr*>(&cli),
                                       &clen);
                if (c < 0) {
                    if (stop.load(std::memory_order_relaxed)) break;
                    continue;
                }
                uint8_t buf[8192];
                for (;;) {
                    ssize_t n = ::recv(c, buf, sizeof(buf), 0);
                    if (n <= 0) break;
                    ssize_t off = 0;
                    while (off < n) {
                        ssize_t w = ::send(c, buf + off, n - off, MSG_NOSIGNAL);
                        if (w <= 0) { off = n; break; }
                        off += w;
                    }
                }
                ::close(c);
            }
        });
    }

    ~LoopbackEcho() {
        stop.store(true, std::memory_order_relaxed);
        if (listen_fd >= 0) {
            // Shutdown to unblock accept().
            ::shutdown(listen_fd, SHUT_RDWR);
            ::close(listen_fd);
        }
        if (thr.joinable()) thr.join();
    }
};

/// Single shared echo server for the bench process. Built lazily on first
/// use. Static so each binary invocation reuses one server across all
/// benchmarks (Google Benchmark defaults to one process per invocation).
LoopbackEcho& shared_echo() {
    static LoopbackEcho echo;
    return echo;
}

/// Connect a fresh ByteSocket to the shared echo server.
[[nodiscard]] ek::ByteSocket connect_to_echo() {
    auto& echo = shared_echo();
    ek::ByteSocket bs;
    en::SocketAddr target{en::Ipv4Addr{127, 0, 0, 1}, echo.port};
    auto cr = bs.connect(target, std::chrono::milliseconds{1000});
    if (!cr) {
        // Bench harness — fail loudly via std::abort so any setup error is
        // not silently absorbed into a 0-iteration result.
        std::fprintf(stderr,
            "connect_to_echo: %s\n", cr.error().detail);
        std::abort();
    }
    return bs;
}

}  // namespace

// ---------------------------------------------------------------------------
// Round-trip bench: send `payload` then recv `payload.size()` bytes.
// Measures the full user-space loop cost; loopback dominates over the wire
// portion so the delta vs raw `::send/::recv` is the ByteSocket shim.
// ---------------------------------------------------------------------------

template <std::size_t N>
static void RunRoundTrip(benchmark::State& state) {
    auto bs = connect_to_echo();
    std::array<uint8_t, N> tx{};
    for (std::size_t i = 0; i < N; ++i) tx[i] = static_cast<uint8_t>(i);
    std::array<uint8_t, N> rx{};

    for (auto _ : state) {
        auto sr = bs.send(tx);
        benchmark::DoNotOptimize(sr);
        // Loopback echoes back exactly what we sent — drain N bytes.
        // ByteSocket::recv on a non-blocking fd returns WouldBlock when
        // the kernel hasn't delivered yet; busy-spin on that specific
        // error so we measure the round-trip + recv hot loop instead of
        // bailing out the moment the echo thread isn't done.
        std::size_t got = 0;
        while (got < N) {
            auto rr = bs.recv(rx.data() + got, N - got);
            if (rr) {
                got += *rr;
                continue;
            }
            if (rr.error().code == eph::core::Error::WouldBlock) {
                continue;  // spin until echo returns
            }
            state.SkipWithError(rr.error().detail);
            break;
        }
        benchmark::DoNotOptimize(rx);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(2 * N));  // tx + rx
}

static void BM_ByteSocketRoundTrip64B(benchmark::State& state)   { RunRoundTrip<64>(state); }
static void BM_ByteSocketRoundTrip4KB(benchmark::State& state)   { RunRoundTrip<4096>(state); }

BENCHMARK(BM_ByteSocketRoundTrip64B);
BENCHMARK(BM_ByteSocketRoundTrip4KB);

// ---------------------------------------------------------------------------
// Send-only: a fresh connection per invocation, send 64B, drop the recv.
// Pure send hot path. Connection setup is O(1) per benchmark instance, so
// the per-iteration cost is the send loop alone.
// ---------------------------------------------------------------------------

static void BM_ByteSocketSendOnly64B(benchmark::State& state) {
    auto bs = connect_to_echo();
    std::array<uint8_t, 64> tx{};
    for (auto _ : state) {
        auto sr = bs.send(tx);
        benchmark::DoNotOptimize(sr);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * 64);
}
BENCHMARK(BM_ByteSocketSendOnly64B);

BENCHMARK_MAIN();
