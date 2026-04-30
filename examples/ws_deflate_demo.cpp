/// @file ws_deflate_demo.cpp
///
/// RFC 7692 permessage-deflate demo over the kernel WebSocket backend.
///
/// What's demonstrated:
///
///   * `StreamConfig::ws.permessage_deflate = true` (default) — the WS
///     handshake driver injects `Sec-WebSocket-Extensions: permessage-
///     deflate; client_no_context_takeover` into the upgrade request and
///     activates inflate on RSV1-flagged frames if the server agrees.
///     Toggle with `--no-deflate` to opt out (e.g. for venues that
///     mis-implement the extension). (Field path post-T3.19; the flat
///     `ws_permessage_deflate` was folded into the `WsConfig` substruct.)
///   * Reading the `kWsDeflateBytesIn` / `kWsDeflateBytesOut` counters
///     directly via `stream->metric(...)` to compute on-the-fly
///     compression ratio — two atomics, one division.
///   * `eph::net::publish_ws_deflate_ratio(stream, sink, tags)` — the
///     packaged derived gauge that does the same divide and pushes a
///     `net.stream.ws.deflate_ratio` sample into any `MetricsSink`. We
///     publish into `eph::utils::ConsoleSink` once per second so the
///     ratio is visible in the demo's output.
///
/// What's deliberately out of scope:
///
///   * No in-process echo server — RFC 7692 requires a server-side
///     deflate implementation we don't ship under `eph-codec` (we ship
///     inflate only; that's all the venue cases need). The demo only
///     runs against a real venue.
///   * No JSON book/order-book parsing — see `binance_book.cpp` for
///     that surface. The point here is the codec/extension wiring.
///
/// Required environment: an IPv4-literal pointing at a deflate-capable
/// WS server. Binance Spot streaming (`stream.binance.com:9443`,
/// path `/ws/btcusdt@bookTicker`) is the canonical target — every
/// market-data frame above ~50 bytes ends up RSV1=1 with a 4-7x ratio
/// in our experience. Resolve externally:
///
///     dig +short stream.binance.com
///
/// Usage:
///
///   # No --host: print config block + key invariants and exit
///   ./ws_deflate_demo
///
///   # Live against Binance:
///   ./ws_deflate_demo --host 54.249.151.10 --port 9443
///                     --sni stream.binance.com
///                     --path /ws/btcusdt@bookTicker
///                     --duration 8

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

#include "eph/codec/ws_codec.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/net/stream_metrics.hpp"   // publish_ws_deflate_ratio + metric names
#include "eph/utils/console_sink.hpp"

namespace en = eph::net::kernel;
namespace ec = eph::codec;
using namespace std::chrono_literals;

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running.store(false, std::memory_order_release); }

namespace {

struct AppArgs {
    std::string host;        // empty = smoke-boot (no network)
    uint16_t    port     = 443;
    std::string sni      = "stream.binance.com";
    std::string path     = "/ws/btcusdt@bookTicker";
    int         duration = 5;
    bool        no_deflate = false;
};

AppArgs parse_args(int argc, char** argv) {
    AppArgs out;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if      (a == "--host"        && i + 1 < argc) out.host     = argv[++i];
        else if (a == "--port"        && i + 1 < argc) out.port     = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--sni"         && i + 1 < argc) out.sni      = argv[++i];
        else if (a == "--path"        && i + 1 < argc) out.path     = argv[++i];
        else if (a == "--duration"    && i + 1 < argc) out.duration = std::atoi(argv[++i]);
        else if (a == "--no-deflate")                  out.no_deflate = true;
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    spdlog::set_level(spdlog::level::info);

    const AppArgs args = parse_args(argc, argv);

    // ── Smoke-boot: no --host means just describe the wiring and exit ────
    if (args.host.empty()) {
        spdlog::info("ws_deflate_demo: smoke-boot (no --host given)");
        spdlog::info("  StreamConfig::ws.permessage_deflate default = true");
        spdlog::info("    handshake injects 'Sec-WebSocket-Extensions: "
                     "permessage-deflate; client_no_context_takeover'");
        spdlog::info("    set --no-deflate to opt out (some venues mis-implement)");
        spdlog::info("  Counters readable via stream->metric():");
        spdlog::info("    StreamMetric::kWsDeflateBytesIn   (compressed RX bytes)");
        spdlog::info("    StreamMetric::kWsDeflateBytesOut  (inflated payload bytes)");
        spdlog::info("  Derived gauge: eph::net::publish_ws_deflate_ratio(...)");
        spdlog::info("    pushes 'net.stream.ws.deflate_ratio' into any MetricsSink");
        spdlog::info("ws_deflate_demo: pass --host <ipv4> to live-test against a "
                     "real WS server (e.g. resolve stream.binance.com)");
        return 0;
    }

    auto ip = eph::net::Ipv4Addr::parse(args.host);
    if (!ip) {
        spdlog::error("ws_deflate_demo: --host must be an IPv4 literal "
                      "(got '{}'); resolve {} externally and paste the literal",
                      args.host, args.sni);
        return 1;
    }

    auto poller = en::KernelPoller::create({}).value();

    // TLS 1.3 + WS upgrade in one create() call — the three transparent
    // handshakes are folded by StreamConfig (TLS via the EnableTls template
    // arg, WS via non-empty cfg.ws.path, deflate via cfg.ws.permessage_deflate).
    using Stream = en::KernelTcpStream<ec::WsCodec, /*Tls=*/true>;
    en::StreamConfig cfg{};
    cfg.remote                  = eph::net::SocketAddr{*ip, args.port};
    cfg.connect_timeout         = 5s;
    cfg.reasm_capacity          = 256 * 1024;
    cfg.tls.hostname            = args.sni;
    cfg.tls.handshake_timeout   = 5s;
    cfg.ws.path                 = args.path;
    cfg.ws.host                 = args.sni;
    cfg.ws.timeout              = 5s;
    cfg.ws.permessage_deflate   = !args.no_deflate;

    spdlog::info("ws_deflate_demo: connecting wss://{}{} (deflate={}, duration={}s)",
                 args.sni, args.path,
                 cfg.ws.permessage_deflate ? "offered" : "off",
                 args.duration);

    auto sr = Stream::create(std::move(cfg));
    if (!sr) {
        spdlog::error("ws_deflate_demo: TLS/WS handshake failed: {}",
                      sr.error().detail);
        return 2;
    }
    auto stream = std::move(*sr);

    std::size_t frames = 0;
    std::size_t app_bytes = 0;
    stream->on_message = [&](std::span<const uint8_t> app_frame) {
        ++frames;
        app_bytes += app_frame.size();
    };

    if (auto r = poller->add(stream.get()); !r) {
        spdlog::error("ws_deflate_demo: poller->add failed: {}", r.error().detail);
        return 3;
    }

    eph::utils::ConsoleSink sink;
    std::array tags{
        eph::core::MetricTag{"venue",     "ws-deflate-demo"},
        eph::core::MetricTag{"transport", "tls+ws"},
    };

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds{args.duration};
    auto next_publish = std::chrono::steady_clock::now() + 1s;
    while (g_running.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        (void)poller->poll(100ms);
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_publish) {
            const auto bytes_in =
                stream->metric(eph::net::StreamMetric::kWsDeflateBytesIn);
            const auto bytes_out =
                stream->metric(eph::net::StreamMetric::kWsDeflateBytesOut);
            const double ratio = (bytes_in == 0)
                ? 0.0
                : static_cast<double>(bytes_out) /
                  static_cast<double>(bytes_in);
            spdlog::info("[deflate] frames={} app_bytes={} compressed_in={} "
                         "inflated_out={} ratio={:.3f} (out/in; >1 means "
                         "expansion, <1 means savings)",
                         frames, app_bytes, bytes_in, bytes_out, ratio);
            // Same numbers via the packaged gauge — pushed into the sink.
            eph::net::publish_ws_deflate_ratio(*stream, sink, tags);
            next_publish = now + 1s;
        }
    }

    spdlog::info("ws_deflate_demo: done, frames={}, app_bytes={}",
                 frames, app_bytes);
    return 0;
}
