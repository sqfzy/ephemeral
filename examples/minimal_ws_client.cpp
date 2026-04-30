/// @file minimal_ws_client.cpp
///
/// The shortest possible kernel-backed WebSocket client using the
/// eph::net::kernel + eph::codec API.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>

#include <spdlog/spdlog.h>

#include "eph/codec/ws_codec.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/net/socket_addr.hpp"

namespace en = eph::net::kernel;
namespace ec = eph::codec;
using namespace std::chrono_literals;

static std::atomic<bool> g_running{true};
static void on_sigint(int) { g_running.store(false, std::memory_order_release); }

int main(int argc, char** argv) {
    std::signal(SIGINT, on_sigint);
    spdlog::set_level(spdlog::level::info);

    // Minimum configurable surface: target IPv4 literal + port.
    const char* ip_str = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t    port   = (argc > 2) ? static_cast<uint16_t>(std::atoi(argv[2])) : 8080;

    auto ip = eph::net::Ipv4Addr::parse(ip_str);
    if (!ip) {
        spdlog::error("minimal_ws_client: bad IP literal '{}'", ip_str);
        return 1;
    }

    // One poller owns the event loop.
    auto poller = en::KernelPoller::create({}).value();

    // Plaintext WS client template. Setting `cfg.ws.path = "/stream"`
    // (or whatever path the server expects) turns `Stream::create` into
    // a full RFC 6455 client handshake: KernelTcpStream drives the HTTP
    // Upgrade, validates the Sec-WebSocket-Accept, and only returns the
    // stream once the handshake completes — see
    // eph-net-kernel/.../tcp_stream.hpp and `StreamConfig::ws.path`.
    // This minimal skeleton leaves `cfg.ws.path` empty so the demo opens
    // only a raw TCP socket; `WsCodec` is still the frame parser and
    // will decode WS frames once they arrive.
    using Stream = en::KernelTcpStream<ec::WsCodec, /*EnableTls=*/false>;
    en::StreamConfig cfg{};
    cfg.remote          = eph::net::SocketAddr{*ip, port};
    cfg.reasm_capacity  = 16 * 1024;
    cfg.connect_timeout = 2s;

    auto sr = Stream::create(std::move(cfg));
    if (!sr) {
        spdlog::error("create failed: {}", sr.error().detail);
        return 2;
    }
    auto stream = std::move(*sr);

    stream->on_message = [](std::span<const uint8_t> app_frame) {
        spdlog::info("<< {} bytes", app_frame.size());
    };

    if (auto r = poller->add(stream.get()); !r) {
        spdlog::error("poller->add failed: {}", r.error().detail);
        return 3;
    }

    // Drive the loop until Ctrl+C.
    while (g_running.load(std::memory_order_acquire)) {
        (void)poller->poll(100ms);
    }

    spdlog::info("minimal_ws_client: exiting");
    return 0;
}
