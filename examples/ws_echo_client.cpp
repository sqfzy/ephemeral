/// @file ws_echo_client.cpp
///
/// Demonstrates the simplest possible TX+RX loop against a local echo
/// server using the eph::net::kernel API. Pair with a simple netcat-style
/// echoer:
///
///   $ nc -lk 9001
///
/// and run:
///
///   $ ./ws_echo_client 127.0.0.1 9001

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>

#include <spdlog/spdlog.h>

#include "eph/codec/raw_stream_codec.hpp"
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

    const char* ip_str = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t    port   = (argc > 2) ? static_cast<uint16_t>(std::atoi(argv[2])) : 9001;

    auto ip = eph::net::Ipv4Addr::parse(ip_str);
    if (!ip) {
        spdlog::error("bad IP literal '{}'", ip_str);
        return 1;
    }

    auto poller = en::KernelPoller::create({}).value();

    // Plain TCP echo — RawStreamCodec passes bytes through unchanged.
    using Stream = en::KernelTcpStream<ec::RawStreamCodec, /*EnableTls=*/false>;
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

    std::size_t pong_count = 0;
    stream->on_message = [&](std::span<const uint8_t> app_frame) {
        ++pong_count;
        spdlog::info("<< echo #{}: {} bytes", pong_count, app_frame.size());
    };

    if (auto r = poller->add(stream.get()); !r) {
        spdlog::error("poller->add failed: {}", r.error().detail);
        return 3;
    }

    // Fire one ping so the echo loop can start.
    const char* payload = "hello eph\n";
    auto tx = stream->send(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(payload), std::strlen(payload)));
    if (!tx) {
        spdlog::warn("send failed: {}", tx.error().detail);
    } else {
        spdlog::info(">> {} bytes", *tx);
    }

    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (g_running.load(std::memory_order_acquire)
           && pong_count == 0
           && std::chrono::steady_clock::now() < deadline) {
        (void)poller->poll(100ms);
    }
    spdlog::info("ws_echo_client: exiting (pongs={})", pong_count);
    return 0;
}
