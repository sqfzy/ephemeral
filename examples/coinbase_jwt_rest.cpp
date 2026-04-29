/// @file coinbase_jwt_rest.cpp
///
/// Coinbase Advanced Trade REST example: build an ES256 JWT via
/// `eph::net::build_coinbase_jwt`, attach it as `Authorization: Bearer
/// <jwt>` to a request issued by `eph::net::HttpClient<KernelTcpStream
/// <RawStreamCodec, true>>`, print the response status + body.
///
/// What's demonstrated:
///
///   * `Es256PrivateKey::from_pem` — RAII-loaded P-256 key. Auto-detects
///     PKCS#8 (`BEGIN PRIVATE KEY`) and SEC1 (`BEGIN EC PRIVATE KEY`)
///     PEM labels; rejects RSA / Ed25519 / non-P-256 curves with
///     `Error::InvalidConfig`.
///   * `build_coinbase_jwt` — assembles `header.payload.signature` per
///     RFC 7519 + Coinbase's documented kid/sub/iss/nbf/exp/uri claim
///     shape. Internally converts aws-lc's DER ECDSA output to the JOSE-
///     mandated IEEE P-1363 r||s form (the "decodes locally but venue
///     rejects it" gotcha).
///   * `HttpClient<KernelTcpStream<RawStreamCodec, true>>` — TLS 1.3 +
///     keep-alive HTTP/1.1 over the kernel backend. The same client
///     issues a second `request()` over the same socket to demonstrate
///     keep-alive reuse.
///   * Two run modes:
///         dry-run   (default, no real key needed) — uses a throwaway
///                   committed PEM, prints the produced JWT, decodes
///                   the three b64u parts, and exits without network I/O.
///                   Useful as a "does this build / link" smoke test.
///         live      (`--live`) — connects to `--host:--port` (default
///                   `api.coinbase.com:443`), issues GET `--path` with
///                   the Bearer token, prints status + first 512 bytes
///                   of body. Needs valid Coinbase Cloud credentials
///                   for a 200 OK; without them you'll see 401 (which
///                   is also informative — it means the JWT round-trip
///                   reached Coinbase intact).
///
/// What's deliberately out of scope:
///
///   * DNS resolution — `--host` must be an IPv4 literal. Resolve once
///     externally (`dig +short api.coinbase.com`) or paste the literal.
///     This matches `binance_book.cpp` and avoids dragging in the
///     resolver path.
///   * POST / PUT / DELETE — only GET is shown. The signing path is
///     identical for any method; just change `req.method` and pass a
///     non-empty `req.body`. The `uri` JWT claim must reflect the same
///     METHOD + path the wire request carries.
///
/// Usage:
///
///   # Dry-run (no key, no network) — proves the JWT path links
///   ./coinbase_jwt_rest
///
///   # Live with your own Coinbase Cloud key
///   ./coinbase_jwt_rest --live
///       --key-pem-file /etc/coinbase/key.pem
///       --key-id 'organizations/<org-uuid>/apiKeys/<key-uuid>'
///       --api-key-name 'my-trading-bot/v1'
///       --host 18.155.118.43         # api.coinbase.com (resolve externally)
///       --path /api/v3/brokerage/accounts

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/net/http_client.hpp"
#include "eph/net/jwt_signed_request.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"

namespace en = eph::net::kernel;
namespace ec = eph::codec;
using namespace std::chrono_literals;

// Throwaway P-256 key for dry-run mode. Generated via
// `openssl ecparam -genkey -name prime256v1 -noout`. Has no production
// lifetime; matches the test fixture in test_jwt_signed_request.cpp.
constexpr std::string_view kDryRunPem =
    "-----BEGIN EC PRIVATE KEY-----\n"
    "MHcCAQEEINOpmaBE6cuLkglCisJtB93Y4yJ2RGC4HSHdUJZesfueoAoGCCqGSM49\n"
    "AwEHoUQDQgAEipxCkur6xELXaT83IyfmFcCIETWrJCXZRs+en43AvHt+Lu0i15E9\n"
    "90z0OphnjtVyoeuhbuMPChrOEkZbGyZUMw==\n"
    "-----END EC PRIVATE KEY-----\n";

namespace {

struct AppArgs {
    bool        live          = false;
    std::string key_pem_file;
    std::string key_id        = "organizations/dry-run-org/apiKeys/dry-run-kid";
    std::string api_key_name  = "eph-coinbase-jwt-rest-demo/v1";
    std::string host          = "127.0.0.1";   // dry-run never touches this
    uint16_t    port          = 443;
    std::string path          = "/api/v3/brokerage/accounts";
};

AppArgs parse_args(int argc, char** argv) {
    AppArgs out;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if      (a == "--live")          out.live = true;
        else if (a == "--key-pem-file" && i + 1 < argc) out.key_pem_file = argv[++i];
        else if (a == "--key-id"       && i + 1 < argc) out.key_id        = argv[++i];
        else if (a == "--api-key-name" && i + 1 < argc) out.api_key_name  = argv[++i];
        else if (a == "--host"         && i + 1 < argc) out.host          = argv[++i];
        else if (a == "--port"         && i + 1 < argc) out.port          = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--path"         && i + 1 < argc) out.path          = argv[++i];
    }
    return out;
}

std::string read_file(const std::string& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

} // namespace

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::info);
    const AppArgs args = parse_args(argc, argv);

    // ── 1) Load the ES256 key (real PEM in live mode, throwaway otherwise) ─
    std::string pem;
    if (args.live && !args.key_pem_file.empty()) {
        pem = read_file(args.key_pem_file);
        if (pem.empty()) {
            spdlog::error("coinbase_jwt_rest: failed to read --key-pem-file '{}'",
                          args.key_pem_file);
            return 1;
        }
    } else {
        if (args.live) {
            spdlog::warn("coinbase_jwt_rest: --live without --key-pem-file — "
                         "demo will sign with the throwaway PEM and Coinbase "
                         "will return 401. Pass --key-pem-file to authenticate.");
        }
        pem = std::string(kDryRunPem);
    }

    auto key_r = eph::net::Es256PrivateKey::from_pem(pem);
    if (!key_r) {
        spdlog::error("coinbase_jwt_rest: Es256PrivateKey::from_pem failed: {}",
                      key_r.error().detail);
        return 2;
    }
    spdlog::info("coinbase_jwt_rest: P-256 key loaded ({} PEM source)",
                 args.key_pem_file.empty() ? "throwaway" : args.key_pem_file);

    // ── 2) Build the JWT ─────────────────────────────────────────────────
    // The `uri` claim must match the wire request: METHOD + " " + host + path.
    // Coinbase uses host (not full URL with scheme) followed by a single
    // space then the request path.
    const auto now = static_cast<uint64_t>(std::time(nullptr));
    std::string uri_claim;
    uri_claim.reserve(args.host.size() + args.path.size());
    uri_claim.append(args.host);
    uri_claim.append(args.path);

    eph::net::CoinbaseJwtParams jp{
        .key_id        = args.key_id,
        .api_key_name  = args.api_key_name,
        .method        = "GET",
        .uri           = uri_claim,
        .now_unix_secs = now,
        .ttl_secs      = 120,
    };
    auto jwt_r = eph::net::build_coinbase_jwt(*key_r, jp);
    if (!jwt_r) {
        spdlog::error("coinbase_jwt_rest: build_coinbase_jwt failed: {}",
                      jwt_r.error().detail);
        return 3;
    }
    const std::string jwt = std::move(*jwt_r);
    spdlog::info("coinbase_jwt_rest: JWT built ({} bytes)", jwt.size());
    // Print the three-part split — useful for sanity-eyeballing the JWT
    // structure (header.payload.signature). Don't log the full sig in
    // production; we do it here because the demo key is throwaway.
    const auto dot1 = jwt.find('.');
    const auto dot2 = (dot1 == std::string::npos) ? std::string::npos
                                                  : jwt.find('.', dot1 + 1);
    if (dot1 != std::string::npos && dot2 != std::string::npos) {
        spdlog::info("  header_b64u  = {}", jwt.substr(0, dot1));
        spdlog::info("  payload_b64u = {}", jwt.substr(dot1 + 1, dot2 - dot1 - 1));
        spdlog::info("  sig_b64u     = {} ({} bytes)",
                     jwt.substr(dot2 + 1), jwt.size() - dot2 - 1);
    }

    if (!args.live) {
        spdlog::info("coinbase_jwt_rest: dry-run complete — pass --live to "
                     "issue the actual REST call");
        return 0;
    }

    // ── 3) Live: TLS connect + HttpClient + keep-alive request ────────────
    auto host_ip = eph::net::Ipv4Addr::parse(args.host);
    if (!host_ip) {
        spdlog::error("coinbase_jwt_rest: --host must be an IPv4 literal "
                      "(got '{}'); resolve api.coinbase.com externally and "
                      "paste the literal", args.host);
        return 4;
    }

    auto poller = en::KernelPoller::create({}).value();

    using Stream = en::KernelTcpStream<ec::RawStreamCodec, /*Tls=*/true>;
    en::StreamConfig scfg{};
    scfg.remote               = eph::net::SocketAddr{*host_ip, args.port};
    scfg.connect_timeout      = 5s;
    scfg.reasm_capacity       = 64 * 1024;
    // SNI hostname — must match the cert CN/SAN. Use the public hostname
    // even when --host is an IP literal.
    scfg.tls.hostname         = "api.coinbase.com";
    scfg.tls.handshake_timeout = 5s;

    auto sr = Stream::create(std::move(scfg));
    if (!sr) {
        spdlog::error("coinbase_jwt_rest: TLS connect failed: {}",
                      sr.error().detail);
        return 5;
    }
    auto stream = std::move(*sr);
    if (auto r = poller->add(stream.get()); !r) {
        spdlog::error("coinbase_jwt_rest: poller->add failed: {}",
                      r.error().detail);
        return 6;
    }

    eph::net::HttpClient<Stream> cli{std::move(stream)};

    // The Bearer header lives until request() returns; std::string-stored
    // and then exposed as a string_view through HttpHeader.
    const std::string auth_value = "Bearer " + jwt;
    eph::net::HttpClient<Stream>::Request req{
        .method  = "GET",
        .path    = args.path,
        .headers = {
            {"Host",          args.host},
            {"Authorization", auth_value},
            {"Accept",        "application/json"},
            {"User-Agent",    "eph-coinbase-jwt-rest-demo/1.0"},
        },
        .body = {},
    };

    auto rsp = cli.request(req,
        [&]() noexcept { (void)poller->poll(); },
        std::chrono::milliseconds{5000});
    if (!rsp) {
        spdlog::error("coinbase_jwt_rest: HttpClient::request failed: {}",
                      rsp.error().detail);
        return 7;
    }
    spdlog::info("coinbase_jwt_rest: HTTP {} ({} bytes body)",
                 rsp->status_code, rsp->body.size());
    // Print up to 512 bytes of the body so a 401 / error JSON is visible
    // without flooding the console on a successful 200 with a long
    // accounts list.
    const std::size_t print_n = std::min<std::size_t>(rsp->body.size(), 512);
    if (print_n > 0) {
        std::string snippet(reinterpret_cast<const char*>(rsp->body.data()),
                            print_n);
        spdlog::info("coinbase_jwt_rest: body (first {} bytes): {}",
                     print_n, snippet);
    }

    // ── 4) Demonstrate keep-alive: issue a /time probe on the same socket ─
    // HttpClient leaves the stream connected after request() returns
    // (default `send_connection_keep_alive=true`). A second request reuses
    // the same TLS session — no new handshake.
    eph::net::HttpClient<Stream>::Request req2{
        .method  = "GET",
        .path    = "/api/v3/brokerage/time",
        .headers = {
            {"Host",          args.host},
            {"Authorization", auth_value},
            {"Accept",        "application/json"},
        },
        .body = {},
    };
    auto rsp2 = cli.request(req2,
        [&]() noexcept { (void)poller->poll(); },
        std::chrono::milliseconds{3000});
    if (!rsp2) {
        spdlog::warn("coinbase_jwt_rest: keep-alive probe failed: {}",
                     rsp2.error().detail);
    } else {
        spdlog::info("coinbase_jwt_rest: keep-alive probe HTTP {} ({} bytes)",
                     rsp2->status_code, rsp2->body.size());
    }

    return 0;
}
