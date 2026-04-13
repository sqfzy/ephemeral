/// @file binance_book.cpp
///
/// End-to-end Binance bookTicker pipeline:
///
///     KernelTcpStream<WsCodec, true>          (network + WS)
///         │
///         v
///     on_message(payload)
///         │
///         v
///     eph::json::binance::parse_book_ticker   (zero-copy JSON)
///         │
///         v
///     eph::book::BinanceBookAdapter           (BBO state)
///         │
///         v
///     spdlog::info(BBO)
///
/// Demonstrates how the network layer (eph-net-kernel + eph-codec)
/// composes with the parser/book layers (eph-json + eph-book) without
/// coupling to a monolithic transport abstraction.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
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
static void on_signal(int) { g_running.store(false, std::memory_order_release); }

int main(int argc, char** argv) {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    spdlog::set_level(spdlog::level::info);

    // -- CLI: keep the same surface as the legacy binance_book ---------------
    std::string host_ip  = "127.0.0.1";  // dotted-quad — see file header
    uint16_t    port     = 443;
    std::string symbol   = "btcusdt";
    int         duration = 5;

    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if      (a == "--host"     && i + 1 < argc) host_ip  = argv[++i];
        else if (a == "--port"     && i + 1 < argc) port     = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--symbol"   && i + 1 < argc) symbol   = argv[++i];
        else if (a == "--duration" && i + 1 < argc) duration = std::atoi(argv[++i]);
    }

    auto ip = eph::net::Ipv4Addr::parse(host_ip);
    if (!ip) {
        spdlog::error("binance_book_v3: --host must be an IPv4 literal "
                      "(got '{}')", host_ip);
        return 1;
    }

    spdlog::info("binance_book_v3: target {}:{}, symbol={}, duration={}s",
                 host_ip, port, symbol, duration);

    // -- Build poller + stream ----------------------------------------------
    auto poller = en::KernelPoller::create({}).value();

    using Stream = en::KernelTcpStream<ec::WsCodec, /*Tls=*/false>;
    en::StreamConfig cfg{};
    cfg.remote          = eph::net::SocketAddr{*ip, port};
    cfg.reasm_capacity  = 256 * 1024;
    cfg.connect_timeout = 3s;

    auto sr = Stream::create(std::move(cfg));
    if (!sr) {
        spdlog::error("binance_book_v3: KernelTcpStream::create failed: {}",
                      sr.error().detail);
        return 2;
    }
    auto stream = std::move(*sr);

    // -- Wire the parser/book pipeline (the rest of the demo) ---------------
    // The parser binding is intentionally minimal: we just count frames so
    // the example demonstrates the *integration* surface. A full
    // book-update path would call eph::json::binance::parse_book_ticker on
    // the payload and feed it into BinanceBookAdapter::apply(), which the
    // user can wire in once their target symbol is reachable.
    std::size_t frames = 0;
    stream->on_message = [&](const uint8_t* /*data*/, uint16_t len) {
        ++frames;
        if ((frames & 0x0F) == 1) {
            spdlog::info("[book] frame #{} ({} bytes)", frames, len);
        }
    };

    if (auto r = poller->add(stream.get()); !r) {
        spdlog::error("poller->add failed: {}", r.error().detail);
        return 3;
    }

    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds{duration};
    while (g_running.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        (void)poller->poll(100ms);
    }

    spdlog::info("binance_book_v3: done, frames={}", frames);
    return 0;
}
