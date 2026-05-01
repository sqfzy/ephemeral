/// @file ws_via_proxy.cpp
///
/// HTTP CONNECT proxy demo (kernel backend only).
///
/// When `StreamConfig::proxy` (an `eph::net::ProxyConfig`) is set,
/// `KernelTcpStream::create` performs the full CONNECT handshake
/// internally: it TCP-connects to `proxy.host:proxy.port`, sends a
/// `CONNECT remote.host:remote.port HTTP/1.1` request, validates the
/// `200 Connection Established` response, then (if enabled) runs TLS
/// and/or the WS Upgrade inside the tunnel. On the DPDK backend the
/// `proxy` field has been removed from the StreamConfig entirely
/// (post-T3.19) — passing a proxy is a **compile error**, not a
/// runtime `InvalidConfig`. SOCKS5 is intentionally not supported
/// (see `.artifacts/phase-9-scope-decision.md`).
///
/// This example exposes the CLI surface (`--proxy-host`, `--proxy-port`)
/// and demonstrates how to populate `cfg.proxy` when both flags are
/// given. With no `--proxy-host` it falls through to a direct connect
/// so the binary is useful as a plaintext-WS smoke-test too.
///
/// Usage:
///   ws_via_proxy [--target-ip <ip>] [--target-port <port>]
///                [--proxy-host <host>] [--proxy-port <port>]
///                [--http-connect]
///
///   --target-ip      Origin server IPv4 (default: 127.0.0.1)
///   --target-port    Origin server port  (default: 9443)
///   --proxy-host     HTTP CONNECT proxy host (empty = direct connect)
///   --proxy-port     HTTP CONNECT proxy port (required with --proxy-host)
///   --http-connect   Forward-compat marker (HTTP CONNECT is the only
///                    supported scheme today; no-op).

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
#include "eph/net/proxy.hpp"
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

    // `--http-connect` is accepted for forward compatibility; today the
    // only supported proxy scheme IS HTTP CONNECT, so the flag is a no-op
    // marker. A future SOCKS5 path (see .artifacts/phase-9-scope-decision.md
    // for why it's deliberately out of scope) would key off a new flag.
    (void)http_connect;

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

    // When a proxy is given, wire `cfg.proxy` — `KernelTcpStream::create`
    // then TCP-connects to the proxy, runs the HTTP CONNECT handshake,
    // and (since `EnableTls=false` + empty `cfg.ws.path` here) returns the
    // post-CONNECT plaintext stream. Flip `EnableTls=true` or set
    // `cfg.ws.path` to layer TLS / WS inside the tunnel.
    if (!proxy_host.empty()) {
        eph::net::ProxyConfig pcfg{};
        pcfg.host = proxy_host;
        pcfg.port = proxy_port;
        cfg.proxy = std::move(pcfg);
        spdlog::info("ws_via_proxy: routing via HTTP CONNECT proxy {}:{}",
                     proxy_host, proxy_port);
    }

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
