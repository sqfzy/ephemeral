/// @file observability_demo.cpp
///
/// Demonstrates the two-layer observability architecture for stream
/// metrics. Spins up a localhost loopback echo server, drives a
/// `KernelTcpStream<RawStreamCodec>` against it for a few seconds,
/// and periodically `publish_metrics()` to a `ConsoleSink` so you
/// can see the counters tick.
///
/// Run:
///   $ xmake run observability_demo
///
/// Expected output (every 250 ms; `publish_metrics` walks the full
/// `StreamMetric` enum so all 26 counters are emitted unconditionally —
/// the kernel TCP stream legitimately bumps only the first three for
/// this workload, and the rest stay at zero):
///   [COUNTER] net.stream.bytes_sent              = N {venue=demo}
///   [COUNTER] net.stream.bytes_recv              = N {venue=demo}
///   [COUNTER] net.stream.frames_decoded          = N {venue=demo}
///   [COUNTER] net.stream.reasm_overflows         = 0 {venue=demo}
///   [COUNTER] net.stream.codec_errors            = 0 {venue=demo}
///   [COUNTER] net.stream.tls.cross_record_frames = 0 {venue=demo}
///   ... 19 more zero-valued counters (TCP-specific, TLS, WS, DPDK) ...
///   [COUNTER] net.stream.tls.handshake_count     = 0 {venue=demo}
/// See docs/observability-guide.md for the full list.

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <span>
#include <system_error>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/core/metrics_concept.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/net/stream_metrics.hpp"
#include "eph/utils/console_sink.hpp"

namespace ek = eph::net::kernel;
namespace en = eph::net;
namespace ec = eph::codec;

using namespace std::chrono_literals;

// ── Loopback echo server (single client) ─────────────────────────────────

namespace {

/// Tiny single-client TCP echo server bound to an OS-allocated loopback
/// port. Public `port` is non-zero only after the ctor's bind+getsockname
/// completed; check `is_valid()` before using `port`.
///
/// Failure model: every syscall return is checked. On any error during
/// setup the server logs the errno via spdlog and leaves `port == 0`;
/// the caller (main) detects this and exits with a clear message rather
/// than letting the demo client `connect()` to port 0 and surface an
/// opaque "Connection refused" / `bad_expected_access`. This used to be
/// the failure mode — every syscall return was discarded so a sandboxed
/// runner that denied SOCK_STREAM (or hit the per-process fd limit at
/// startup) just produced a confusing crash inside `Stream::create(...)`
/// downstream.
struct EchoServer {
    int               listen_fd{-1};
    uint16_t          port{0};
    std::thread       th;
    std::atomic<bool> stop{false};

    EchoServer() noexcept {
        listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) {
            spdlog::error("EchoServer: socket() failed: {} ({})",
                          errno, std::strerror(errno));
            return;
        }
        int yes = 1;
        if (::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                         &yes, sizeof(yes)) != 0) {
            // Non-fatal — bind on an OS-allocated ephemeral port should
            // succeed without REUSEADDR. Just warn so an operator who
            // sees a follow-up bind failure has the breadcrumb.
            spdlog::warn("EchoServer: SO_REUSEADDR failed: {} ({})",
                         errno, std::strerror(errno));
        }
        sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port        = 0;
        if (::bind(listen_fd,
                   reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
            spdlog::error("EchoServer: bind(127.0.0.1:0) failed: {} ({})",
                          errno, std::strerror(errno));
            ::close(listen_fd);
            listen_fd = -1;
            return;
        }
        socklen_t len = sizeof(sa);
        if (::getsockname(listen_fd,
                          reinterpret_cast<sockaddr*>(&sa), &len) != 0) {
            spdlog::error("EchoServer: getsockname failed: {} ({})",
                          errno, std::strerror(errno));
            ::close(listen_fd);
            listen_fd = -1;
            return;
        }
        port = ntohs(sa.sin_port);
        if (::listen(listen_fd, 1) != 0) {
            spdlog::error("EchoServer: listen failed: {} ({})",
                          errno, std::strerror(errno));
            ::close(listen_fd);
            listen_fd = -1;
            port = 0;
            return;
        }
        try {
            th = std::thread([this] { run(); });
        } catch (const std::system_error& e) {
            spdlog::error("EchoServer: spawn accept thread failed: {}",
                          e.what());
            ::close(listen_fd);
            listen_fd = -1;
            port = 0;
        }
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return listen_fd >= 0 && port != 0;
    }

    void run() noexcept {
        sockaddr_in peer{};
        socklen_t pl = sizeof(peer);
        int cfd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &pl);
        if (cfd < 0) {
            // accept(-1, …) gives EBADF; legitimate accept-after-shutdown
            // gives EINVAL. Either way the demo client side will see
            // ECONNREFUSED downstream — log so the journal carries the
            // root cause.
            spdlog::warn("EchoServer::run: accept failed: {} ({})",
                         errno, std::strerror(errno));
            return;
        }
        while (!stop.load(std::memory_order_relaxed)) {
            char buf[1024];
            ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            (void)::send(cfd, buf, n, MSG_NOSIGNAL);
        }
        ::close(cfd);
    }

    ~EchoServer() {
        stop.store(true, std::memory_order_relaxed);
        if (listen_fd >= 0) ::close(listen_fd);
        if (th.joinable()) th.join();
    }
};

std::atomic<bool> g_running{true};

void on_signal(int) noexcept { g_running.store(false, std::memory_order_relaxed); }

} // namespace

int main() {
    spdlog::set_level(spdlog::level::info);
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    EchoServer srv;
    if (!srv.is_valid()) {
        spdlog::error("observability_demo: echo server bring-up failed "
                      "(see prior errors). Aborting.");
        return 2;
    }
    spdlog::info("observability_demo: echo server listening on 127.0.0.1:{}",
                 srv.port);

    auto poller_r = ek::KernelPoller::create();
    if (!poller_r) {
        spdlog::error("observability_demo: KernelPoller::create failed: {}",
                      poller_r.error().detail);
        return 3;
    }
    auto poller = std::move(*poller_r);

    ek::StreamConfig cfg{};
    cfg.remote = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, srv.port};
    using Stream = ek::KernelTcpStream<ec::RawStreamCodec, /*EnableTls=*/false>;
    auto stream_r = Stream::create(cfg);
    if (!stream_r) {
        spdlog::error("observability_demo: Stream::create failed: {} "
                      "(target=127.0.0.1:{})",
                      stream_r.error().detail, srv.port);
        return 4;
    }
    auto stream = std::move(*stream_r);

    std::size_t echoes = 0;
    stream->on_message = [&](std::span<const uint8_t> app_frame) {
        ++echoes;
        spdlog::debug("rx {} bytes ({} echoes total)",
                      app_frame.size(), echoes);
    };

    if (auto r = poller->add(stream.get()); !r) {
        spdlog::error("poller add failed: {}", r.error().detail);
        return 1;
    }

    // Application-chosen sink. Swap ConsoleSink for any MetricsSink
    // implementation (PrometheusSink, OtelSink, NullSink for prod, etc).
    eph::utils::ConsoleSink sink;
    std::array tags{
        eph::core::MetricTag{"venue",     "demo"},
        eph::core::MetricTag{"transport", "tcp"},
    };

    auto next_publish = std::chrono::steady_clock::now() + 250ms;
    auto next_send    = std::chrono::steady_clock::now() + 50ms;
    const auto deadline = std::chrono::steady_clock::now() + 3s;

    while (g_running.load(std::memory_order_relaxed)
           && std::chrono::steady_clock::now() < deadline) {
        poller->poll(20ms);

        const auto now = std::chrono::steady_clock::now();

        // Workload: send a small payload every 50 ms.
        if (now >= next_send) {
            const uint8_t payload[] = {'p', 'i', 'n', 'g'};
            (void)stream->send(payload);
            next_send = now + 50ms;
        }

        // Observability: every 250 ms publish all stream counters.
        if (now >= next_publish) {
            spdlog::info("─── stream metrics snapshot ───");
            en::publish_metrics(*stream, sink, tags);
            next_publish = now + 250ms;
        }
    }

    spdlog::info("observability_demo: done — final {} echoes", echoes);
    return 0;
}
