/// @file production_client_v3.cpp
/// v3.3 rewrite of production_client.cpp.
///
/// Shows the production-quality knobs a real HFT client wires onto a
/// KernelTcpStream: TLS on, reconnect policy, TCP_NODELAY, bounded reasm
/// buffer, signal-driven shutdown. No exchange-specific logic — the file
/// is deliberately a template that a strategy can drop into.
///
/// Part of Phase 6 of the v3.3 architecture refactor
/// (.artifacts/design-eph-v3.3-architecture-20260410.md).

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

#include "eph/codec/ws_codec.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/net/reconnect_policy.hpp"
#include "eph/net/socket_addr.hpp"

namespace en = eph::net::kernel;
namespace ec = eph::codec;
using namespace std::chrono_literals;

// ── Signal-driven shutdown ─────────────────────────────────────────────────

static std::atomic<bool> g_running{true};
static void on_signal(int) {
    g_running.store(false, std::memory_order_release);
}

// ── Run one session to completion (exits on disconnect or signal) ──────────

static int run_session(const std::string& host, uint16_t port, bool use_tls) {
    auto ip = eph::net::Ipv4Addr::parse(host);
    if (!ip) {
        spdlog::error("production_client_v3: --host must be an IPv4 literal, "
                      "got '{}'", host);
        return 1;
    }

    auto poller = en::KernelPoller::create({}).value();

    en::StreamConfig cfg{};
    cfg.remote          = eph::net::SocketAddr{*ip, port};
    cfg.reasm_capacity  = 256 * 1024;   // room for burst of snapshot frames
    cfg.connect_timeout = 3s;
    cfg.tcp_nodelay     = true;

    // Reconnect policy lives on the stream config — outer recovery loop
    // can consult `cfg.reconnect` to decide whether to re-create the
    // stream after a drop. ReconnectPolicyConfig defaults are already
    // production-sane (exponential back-off, bounded attempts).
    cfg.reconnect = eph::net::ReconnectPolicyConfig{};

    auto make_stream = [&]()
        -> std::expected<
             std::unique_ptr<en::KernelTcpStream<ec::WsCodec, /*Tls=*/true>>,
             eph::core::ErrorInfo> {
        return en::KernelTcpStream<ec::WsCodec, true>::create(cfg);
    };
    auto make_stream_plain = [&]()
        -> std::expected<
             std::unique_ptr<en::KernelTcpStream<ec::WsCodec, /*Tls=*/false>>,
             eph::core::ErrorInfo> {
        return en::KernelTcpStream<ec::WsCodec, false>::create(cfg);
    };

    if (use_tls) {
        auto sr = make_stream();
        if (!sr) {
            spdlog::error("create (tls) failed: {}", sr.error().detail);
            return 2;
        }
        auto stream = std::move(*sr);
        stream->on_message = [](const uint8_t*, uint16_t len) {
            SPDLOG_DEBUG("prod: rx {} bytes", len);
            (void)len;
        };
        if (auto r = poller->add(stream.get()); !r) {
            spdlog::error("add failed: {}", r.error().detail);
            return 3;
        }
        while (g_running.load(std::memory_order_acquire)
               && stream->state() == eph::net::TcpState::Established) {
            (void)poller->poll(100ms);
        }
    } else {
        auto sr = make_stream_plain();
        if (!sr) {
            spdlog::error("create (plain) failed: {}", sr.error().detail);
            return 2;
        }
        auto stream = std::move(*sr);
        stream->on_message = [](const uint8_t*, uint16_t len) {
            SPDLOG_DEBUG("prod: rx {} bytes", len);
            (void)len;
        };
        if (auto r = poller->add(stream.get()); !r) {
            spdlog::error("add failed: {}", r.error().detail);
            return 3;
        }
        while (g_running.load(std::memory_order_acquire)
               && stream->state() == eph::net::TcpState::Established) {
            (void)poller->poll(100ms);
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    spdlog::set_level(spdlog::level::info);

    std::string host    = "127.0.0.1";
    uint16_t    port    = 9443;
    bool        use_tls = false;  // flip to true when pointing at a real peer

    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if      (a == "--host" && i + 1 < argc) host = argv[++i];
        else if (a == "--port" && i + 1 < argc) port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--tls")                  use_tls = true;
        else if (a == "--no-tls")               use_tls = false;
    }

    spdlog::info("production_client_v3: host={}:{} tls={}", host, port, use_tls);
    return run_session(host, port, use_tls);
}
