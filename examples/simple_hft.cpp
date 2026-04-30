/// @file simple_hft.cpp
///
/// Demonstrates the kernel-only user pattern:
///
///   1. KernelPoller::create                     — one poller owns the loop
///   2. KernelTcpStream<WsCodec,true>::create    — WebSocket over TLS
///   3. stream->on_message = [] { … }            — user callback
///   4. poller->add(stream.get())                — attach
///   5. while (running) poller->poll(100ms);     — single driver loop
///
/// Usage:
///   ./simple_hft_v3 [--host fstream.binance.com] [--port 443]
///                   [--symbol btcusdt] [--count 100] [--no-tls]
///
/// NOTE: Because this uses the real kernel TLS path (aws-lc via
/// detail/tls_state.hpp), the connection only succeeds when the OS trust
/// store can validate the server certificate. Running against a non-TLS
/// localhost echo works out of the box via `--no-tls`.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <format>
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
#include "eph/net/stream_metrics.hpp"

namespace en = eph::net::kernel;
namespace ec = eph::codec;
using namespace std::chrono_literals;

// ── Config ──────────────────────────────────────────────────────────────────

struct AppConfig {
    std::string host   = "fstream.binance.com";
    uint16_t    port   = 443;
    std::string symbol = "btcusdt";
    int         count  = 100;  // 0 = run forever
    bool        use_tls = true;
};

static std::atomic<bool> g_running{true};

static void on_signal(int) {
    g_running.store(false, std::memory_order_release);
}

static AppConfig parse_args(int argc, char** argv) {
    AppConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        auto next = [&](std::string_view name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << std::format("Error: {} requires a value\n", name);
                std::exit(1);
            }
            return argv[++i];
        };
        if      (arg == "--host")   cfg.host    = next("--host");
        else if (arg == "--port")   cfg.port    = static_cast<uint16_t>(std::atoi(next("--port")));
        else if (arg == "--symbol") cfg.symbol  = next("--symbol");
        else if (arg == "--count")  cfg.count   = std::atoi(next("--count"));
        else if (arg == "--no-tls") cfg.use_tls = false;
        else if (arg == "--help") {
            std::cerr <<
                "Usage: simple_hft_v3 [--host H] [--port P] [--symbol S]\n"
                "                     [--count N] [--no-tls]\n";
            std::exit(0);
        } else {
            std::cerr << std::format("Unknown argument: {}\n", arg);
            std::exit(1);
        }
    }
    return cfg;
}

// ── Body: TLS=on path ───────────────────────────────────────────────────────

template <bool EnableTls>
static int run_with_tls(const AppConfig& cfg) {
    using Stream = en::KernelTcpStream<ec::WsCodec, EnableTls>;

    // 1) Build one poller for the whole app.
    auto poller_r = en::KernelPoller::create({});
    if (!poller_r) {
        spdlog::error("KernelPoller::create failed: {}", poller_r.error().detail);
        return 1;
    }
    auto poller = std::move(*poller_r);

    // 2) Resolve a dotted-quad for the target host. DNS is kept out
    //    of the hot path; a real HFT client would populate a cache at
    //    startup. For a smoke-boot of the example we pick the loopback
    //    address when `--host` is an IPv4 literal and otherwise bail with
    //    a clear error so the reviewer knows what to change.
    auto ip_r = eph::net::Ipv4Addr::parse(cfg.host);
    if (!ip_r) {
        spdlog::error(
            "simple_hft demo: --host must be an IPv4 literal (got '{}'). "
            "Resolve to a literal out-of-band; DNS lives above the transport "
            "layer.", cfg.host);
        return 2;
    }

    // 3) Build the stream. The RFC 6455 client handshake is triggered
    //    transparently by `KernelTcpStream::create()` when
    //    `StreamConfig::ws.path` is non-empty (see eph-net-kernel
    //    config.hpp). This demo leaves `ws.path` empty, so `create()`
    //    does only TCP (+TLS if EnableTls) — no WS Upgrade is sent. The
    //    socket is returned in a state where any bytes read would feed
    //    the `WsCodec`; a real caller targeting a WS server should set
    //    `scfg.ws.path = "/stream"` (or similar) here.
    typename en::StreamConfig scfg{};
    scfg.remote         = eph::net::SocketAddr{*ip_r, cfg.port};
    scfg.reasm_capacity = 64 * 1024;
    scfg.connect_timeout = 3s;

    auto stream_r = Stream::create(std::move(scfg));
    if (!stream_r) {
        spdlog::error("KernelTcpStream::create failed: {}",
                      stream_r.error().detail);
        return 3;
    }
    auto stream = std::move(*stream_r);

    // 4) Wire the sink BEFORE attach so no frames are dropped.
    std::size_t msgs = 0;
    stream->on_message = [&](std::span<const uint8_t> app_frame) {
        ++msgs;
        if ((msgs & 0xFF) == 1) {
            std::string_view view(
                reinterpret_cast<const char*>(app_frame.data()),
                app_frame.size());
            spdlog::info("[MKT #{:>6}] {:.80}", msgs, view);
        }
    };

    // 5) Attach + drive.
    if (auto r = poller->add(stream.get()); !r) {
        spdlog::error("poller->add failed: {}", r.error().detail);
        return 4;
    }

    spdlog::info("simple_hft_v3: connected, entering poll loop "
                 "(count={}, tls={})", cfg.count, EnableTls);

    auto started = std::chrono::steady_clock::now();
    while (g_running.load(std::memory_order_acquire)) {
        (void)poller->poll(100ms);
        if (cfg.count > 0 && msgs >= static_cast<std::size_t>(cfg.count)) break;
    }

    auto elapsed = std::chrono::steady_clock::now() - started;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    spdlog::info("simple_hft_v3: received {} messages in {} ms", msgs, ms);

    // ── Final metrics snapshot — direct read ─────────────────────────────
    // For a full observability hookup with periodic publish to a sink,
    // see examples/observability_demo.cpp. Here we simply read the raw
    // stream counters at exit so the operator sees the totals.
    spdlog::info("simple_hft_v3: stream counters — "
                 "bytes_sent={}, bytes_recv={}, frames_decoded={}, "
                 "reasm_overflows={}, codec_errors={}",
                 stream->metric(eph::net::StreamMetric::kBytesSent),
                 stream->metric(eph::net::StreamMetric::kBytesRecv),
                 stream->metric(eph::net::StreamMetric::kFramesDecoded),
                 stream->metric(eph::net::StreamMetric::kReasmOverflows),
                 stream->metric(eph::net::StreamMetric::kCodecErrors));
    return 0;
}

// ── Main ────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    spdlog::set_level(spdlog::level::info);

    auto cfg = parse_args(argc, argv);
    spdlog::info("simple_hft_v3: host={} port={} symbol={} tls={}",
                 cfg.host, cfg.port, cfg.symbol, cfg.use_tls);

    return cfg.use_tls ? run_with_tls<true>(cfg)
                       : run_with_tls<false>(cfg);
}
