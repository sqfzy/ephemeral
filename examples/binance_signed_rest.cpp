/// @file binance_signed_rest.cpp
///
/// Binance Spot/Futures REST example: HMAC-SHA256-sign a SIGNED-endpoint
/// query via `eph::net::SignedRequest<BinanceSignTraits>`, splice the
/// returned `X-MBX-SIGNATURE` / `X-MBX-TIMESTAMP` headers onto an
/// `HttpClient` GET, print status + body.
///
/// What's demonstrated:
///
///   * `eph::net::HmacSha256Key` — RAII-cleared 64-byte normalized key.
///     Copy-deleted so the secret can't sprawl across stack frames.
///   * `eph::net::SignedRequest<BinanceSignTraits>::headers_for_query` —
///     traits-driven sign-into-header. Compile-time venue selection
///     means OKX / Bybit are zero-cost off (they're different traits).
///     Returns a `SignedHeaderBundle` whose owned strings keep the
///     header views alive for the duration of one request.
///   * `HttpClient<KernelTcpStream<RawStreamCodec, true>>` — same client
///     as `coinbase_jwt_rest.cpp`, just a different auth scheme. Highlights
///     that the auth helpers are decoupled from the HTTP plumbing.
///   * Two run modes:
///         dry-run   (default) — fixed timestamp, fixed dummy secret,
///                   prints the signed query + the rendered headers and
///                   exits without network I/O. Asserts the lifecycle
///                   end-to-end.
///         live      (`--live`) — connects to `--host:--port`. Use the
///                   Binance Spot testnet (`testnet.binance.vision`) for
///                   real round-trips; mainnet works too if you trust the
///                   demo with read-only keys. Endpoint defaults to
///                   `/api/v3/account` (read-only).
///
/// What's deliberately out of scope:
///
///   * Order placement (POST `/api/v3/order`). The signing path is the
///     same — `headers_for_body` instead of `headers_for_query` — but
///     wiring a buyable order into a demo invites accidents. Read-only
///     `/api/v3/account` is the conservative pick.
///   * The `X-MBX-APIKEY` header — Binance keeps the public key OUT of
///     the signed material, so it's a plain caller-set header. We add
///     it from `--api-key` (or a placeholder in dry-run).
///   * DNS — `--host` must be an IPv4 literal. Resolve externally:
///         dig +short api.binance.com
///         dig +short testnet.binance.vision
///
/// Usage:
///
///   # Dry-run (no key, no network)
///   ./binance_signed_rest
///
///   # Live read-only against the public testnet (resolve first):
///   ./binance_signed_rest --live
///       --host 13.226.197.3            # testnet.binance.vision
///       --api-key <YOUR_TESTNET_API_KEY>
///       --api-secret <YOUR_TESTNET_API_SECRET>
///       --path /api/v3/account

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/net/hmac.hpp"
#include "eph/net/http_client.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/net/signed_request.hpp"

namespace en = eph::net::kernel;
namespace ec = eph::codec;
using namespace std::chrono_literals;

namespace {

struct AppArgs {
    bool        live        = false;
    std::string host        = "127.0.0.1";   // dry-run never touches this
    uint16_t    port        = 443;
    std::string sni         = "testnet.binance.vision";
    std::string path        = "/api/v3/account";
    std::string api_key     = "DRY-RUN-API-KEY";
    std::string api_secret  = "DRY-RUN-API-SECRET";
    uint32_t    recv_window = 5000;          // Binance default
};

AppArgs parse_args(int argc, char** argv) {
    AppArgs out;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if      (a == "--live")          out.live = true;
        else if (a == "--host"        && i + 1 < argc) out.host        = argv[++i];
        else if (a == "--port"        && i + 1 < argc) out.port        = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--sni"         && i + 1 < argc) out.sni         = argv[++i];
        else if (a == "--path"        && i + 1 < argc) out.path        = argv[++i];
        else if (a == "--api-key"     && i + 1 < argc) out.api_key     = argv[++i];
        else if (a == "--api-secret"  && i + 1 < argc) out.api_secret  = argv[++i];
        else if (a == "--recv-window" && i + 1 < argc) out.recv_window = static_cast<uint32_t>(std::atoi(argv[++i]));
    }
    return out;
}

uint64_t now_ms_unix() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               system_clock::now().time_since_epoch()).count();
}

} // namespace

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::info);
    const AppArgs args = parse_args(argc, argv);

    // ── 1) Build the signed query ────────────────────────────────────────
    // Binance signs the literal querystring including timestamp + recvWindow.
    // Caller controls parameter order — we sign exactly what we send.
    //
    // In dry-run mode we pin the timestamp to a known value so the example's
    // output is reproducible across runs (the signature still varies because
    // it depends on the secret + ts).
    const uint64_t ts_ms = args.live ? now_ms_unix() : 1714867200000ULL;

    std::string query;
    query.reserve(64);
    query.append("recvWindow=");
    query.append(std::to_string(args.recv_window));
    query.append("&timestamp=");
    query.append(std::to_string(ts_ms));

    eph::net::HmacSha256Key key{std::string_view{args.api_secret}};
    eph::net::SignedRequest<eph::net::BinanceSignTraits> signer{std::move(key)};
    auto bundle = signer.headers_for_query(query, ts_ms);

    spdlog::info("binance_signed_rest: signed query: {}", query);
    spdlog::info("binance_signed_rest: signature headers ({} entries):",
                 bundle.headers.size());
    for (const auto& h : bundle.headers) {
        spdlog::info("  {}: {}", std::string(h.name), std::string(h.value));
    }

    if (!args.live) {
        spdlog::info("binance_signed_rest: dry-run complete — pass --live with "
                     "real credentials to issue the actual REST call");
        return 0;
    }

    // ── 2) Live: TLS connect + HttpClient ────────────────────────────────
    auto host_ip = eph::net::Ipv4Addr::parse(args.host);
    if (!host_ip) {
        spdlog::error("binance_signed_rest: --host must be IPv4 literal "
                      "(got '{}'); resolve {} externally and paste the literal",
                      args.host, args.sni);
        return 1;
    }

    auto poller = en::KernelPoller::create({}).value();

    using Stream = en::KernelTcpStream<ec::RawStreamCodec, /*Tls=*/true>;
    en::StreamConfig scfg{};
    scfg.remote                = eph::net::SocketAddr{*host_ip, args.port};
    scfg.connect_timeout       = 5s;
    scfg.reasm_capacity        = 64 * 1024;
    scfg.tls.hostname          = args.sni;        // SNI must be the FQDN
    scfg.tls.handshake_timeout = 5s;

    auto sr = Stream::create(std::move(scfg));
    if (!sr) {
        spdlog::error("binance_signed_rest: TLS connect failed: {}",
                      sr.error().detail);
        return 2;
    }
    auto stream = std::move(*sr);
    if (auto r = poller->add(stream.get()); !r) {
        spdlog::error("binance_signed_rest: poller->add failed: {}",
                      r.error().detail);
        return 3;
    }

    eph::net::HttpClient<Stream> cli{std::move(stream)};

    // Path-with-query: GET /api/v3/account?recvWindow=...&timestamp=...
    std::string path_with_query;
    path_with_query.reserve(args.path.size() + 1 + query.size());
    path_with_query.append(args.path);
    path_with_query.push_back('?');
    path_with_query.append(query);

    // Splice the signer's headers + the public key + Host. The bundle's
    // strings outlive `request()` because the bundle lives on this stack.
    std::vector<eph::net::HttpHeader> headers;
    headers.reserve(bundle.headers.size() + 3);
    headers.push_back({"Host",        args.sni});
    headers.push_back({"X-MBX-APIKEY", args.api_key});
    headers.push_back({"Accept",      "application/json"});
    for (const auto& h : bundle.headers) headers.push_back(h);

    eph::net::HttpClient<Stream>::Request req{
        .method  = "GET",
        .path    = path_with_query,
        .headers = std::move(headers),
        .body    = {},
    };

    auto rsp = cli.request(req,
        [&]() noexcept { (void)poller->poll(); },
        std::chrono::milliseconds{5000});
    if (!rsp) {
        spdlog::error("binance_signed_rest: HttpClient::request failed: {}",
                      rsp.error().detail);
        return 4;
    }
    spdlog::info("binance_signed_rest: HTTP {} ({} bytes body)",
                 rsp->status_code, rsp->body.size());
    const std::size_t print_n = std::min<std::size_t>(rsp->body.size(), 512);
    if (print_n > 0) {
        std::string snippet(reinterpret_cast<const char*>(rsp->body.data()),
                            print_n);
        spdlog::info("binance_signed_rest: body (first {} bytes): {}",
                     print_n, snippet);
    }
    return 0;
}
