/// @file ws_via_proxy.cpp
///
/// The proxy/tunnel handshake is done OUTSIDE the stream factory: user
/// code (or a small helper above the StreamConfig) negotiates the
/// SOCKS5 / HTTP CONNECT exchange against a connected kernel fd, then
/// hands the now-tunnelled fd to the stream. This example constructs
/// the stream against a *direct* address but exposes the
/// `--proxy-host` / `--proxy-port` CLI surface for use with the proxy
/// handshake helpers.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>

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

    std::string target_ip   = "127.0.0.1";
    uint16_t    target_port = 9443;
    std::string proxy_host  = "";        // empty = no proxy
    uint16_t    proxy_port  = 0;
    bool        http_connect = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if      (a == "--target-ip"   && i + 1 < argc) target_ip   = argv[++i];
        else if (a == "--target-port" && i + 1 < argc) target_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--proxy-host"  && i + 1 < argc) proxy_host  = argv[++i];
        else if (a == "--proxy-port"  && i + 1 < argc) proxy_port  = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--http-connect")               http_connect = true;
    }

    if (!proxy_host.empty()) {
        spdlog::warn(
            "ws_via_proxy: proxy CLI parsed (host={}:{}, http_connect={}), "
            "but the proxy handshake helper is not yet available. "
            "Falling through to a direct connection — see the file header.",
            proxy_host, proxy_port, http_connect);
    }

    auto ip = eph::net::Ipv4Addr::parse(target_ip);
    if (!ip) {
        spdlog::error("bad --target-ip: '{}'", target_ip);
        return 1;
    }

    auto poller = en::KernelPoller::create({}).value();

    using Stream = en::KernelTcpStream<ec::WsCodec, /*Tls=*/false>;
    en::StreamConfig cfg{};
    cfg.remote          = eph::net::SocketAddr{*ip, target_port};
    cfg.reasm_capacity  = 64 * 1024;
    cfg.connect_timeout = 3s;

    auto sr = Stream::create(std::move(cfg));
    if (!sr) {
        spdlog::error("create failed: {}", sr.error().detail);
        return 2;
    }
    auto stream = std::move(*sr);
    stream->on_message = [](std::span<const uint8_t> app_frame) {
        spdlog::info("[via-proxy] rx {} bytes", app_frame.size());
    };
    if (auto r = poller->add(stream.get()); !r) {
        spdlog::error("add failed: {}", r.error().detail);
        return 3;
    }

    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (g_running.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        (void)poller->poll(100ms);
    }
    return 0;
}
