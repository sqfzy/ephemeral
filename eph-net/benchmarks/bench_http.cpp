/// @file bench_http.cpp
/// Microbenchmarks for `eph::net::parse_http_request` /
/// `parse_http_response` / `build_http_request`.
///
/// Coverage gap (batch 11): HTTP/1.x parsing sits on every REST adapter
/// hot path AND on the WS/443 upgrade handshake (the `cfg.ws.path` flow
/// in `KernelTcpStream::create` / `DpdkTcpStream::create_and_attach`).
/// The parser's incremental contract — return `Ok(None)` on partial
/// input — is hit per-poll cycle until the response is complete; the
/// per-call cost matters for high-frequency reconnect / replay scenarios.
///
/// Why these particular cases:
/// - **ParseRequestSimple**: a 2-header GET — the lower bound (typical
///   exchange-API request).
/// - **ParseRequestRich**: a 12-header POST with body — typical
///   signed-REST shape (auth headers, content-type, accept, user-agent).
/// - **ParseResponseSimple / ParseResponseRich**: the response side
///   (Binance ~12 headers, includes Set-Cookie / Server / Date).
/// - **ParseRequestPartialIncrementalReturn**: the partial-buffer
///   `Ok(None)` fast-return — hit per poll cycle when the response is
///   still arriving. Should be ~tens of ns and not walk the buffer.
/// - **BuildRequest**: the symmetric encoder used by the WS upgrade and
///   by mock servers. Allocation-free; cost is dominated by validation +
///   memcpy.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>
#include <spdlog/spdlog.h>

#include "eph/net/http.hpp"

using eph::net::HttpHeader;
using eph::net::HttpRequest;
using eph::net::HttpResponse;
using eph::net::ParseResult;
using eph::net::build_http_request;
using eph::net::parse_http_request;
using eph::net::parse_http_response;

namespace {

[[nodiscard]] std::vector<uint8_t> bytes_of(std::string_view sv) {
    std::vector<uint8_t> v(sv.size());
    for (std::size_t i = 0; i < sv.size(); ++i) {
        v[i] = static_cast<uint8_t>(sv[i]);
    }
    return v;
}

// 2-header GET — minimal viable request.
constexpr std::string_view kReqSimple =
    "GET /api/v3/ping HTTP/1.1\r\n"
    "Host: api.binance.com\r\n"
    "User-Agent: eph/1.0\r\n"
    "\r\n";

// 12-header POST with a 50-byte body — typical signed-REST shape.
constexpr std::string_view kReqRich =
    "POST /api/v3/order HTTP/1.1\r\n"
    "Host: api.binance.com\r\n"
    "User-Agent: eph/1.0\r\n"
    "Accept: application/json\r\n"
    "Accept-Encoding: gzip, deflate\r\n"
    "Connection: keep-alive\r\n"
    "Content-Type: application/x-www-form-urlencoded\r\n"
    "Content-Length: 50\r\n"
    "X-MBX-APIKEY: 0123456789abcdef0123456789abcdef\r\n"
    "X-MBX-SIGNATURE: deadbeefdeadbeefdeadbeefdeadbeefdeadbeef\r\n"
    "X-MBX-TIMESTAMP: 1733846400000\r\n"
    "X-MBX-RECV-WINDOW: 5000\r\n"
    "X-Forwarded-For: 10.0.0.1\r\n"
    "\r\n"
    "symbol=BTCUSDT&side=BUY&type=MARKET&quantity=0.01";

constexpr std::string_view kRespSimple =
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: 0\r\n"
    "\r\n";

constexpr std::string_view kRespRich =
    "HTTP/1.1 200 OK\r\n"
    "Server: nginx/1.18.0\r\n"
    "Date: Tue, 10 Dec 2025 15:30:00 GMT\r\n"
    "Content-Type: application/json;charset=UTF-8\r\n"
    "Content-Length: 100\r\n"
    "Connection: keep-alive\r\n"
    "X-MBX-USED-WEIGHT: 1\r\n"
    "X-MBX-USED-WEIGHT-1m: 1\r\n"
    "Strict-Transport-Security: max-age=31536000\r\n"
    "X-Frame-Options: SAMEORIGIN\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "X-Xss-Protection: 1; mode=block\r\n"
    "Cache-Control: no-cache, no-store, must-revalidate\r\n"
    "Pragma: no-cache\r\n"
    "\r\n"
    R"({"symbol":"BTCUSDT","price":"50000.00","quantity":"0.01",)"
    R"("status":"NEW","clientOrderId":"order-001"})";

} // namespace

// ---------------------------------------------------------------------------
// Parse — request (2-header simple, 12-header rich)
// ---------------------------------------------------------------------------

static void BM_HttpParseRequestSimple(benchmark::State& state) {
    auto buf = bytes_of(kReqSimple);
    std::array<HttpHeader, 16> hdrs{};
    for (auto _ : state) {
        auto r = parse_http_request(
            std::span<const uint8_t>{buf.data(), buf.size()},
            std::span<HttpHeader>{hdrs});
        benchmark::DoNotOptimize(r);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(buf.size()));
}
BENCHMARK(BM_HttpParseRequestSimple);

static void BM_HttpParseRequestRich(benchmark::State& state) {
    auto buf = bytes_of(kReqRich);
    std::array<HttpHeader, 32> hdrs{};
    for (auto _ : state) {
        auto r = parse_http_request(
            std::span<const uint8_t>{buf.data(), buf.size()},
            std::span<HttpHeader>{hdrs});
        benchmark::DoNotOptimize(r);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(buf.size()));
}
BENCHMARK(BM_HttpParseRequestRich);

// ---------------------------------------------------------------------------
// Parse — response (the production flow shape — what every REST poller hits)
// ---------------------------------------------------------------------------

static void BM_HttpParseResponseSimple(benchmark::State& state) {
    auto buf = bytes_of(kRespSimple);
    std::array<HttpHeader, 16> hdrs{};
    for (auto _ : state) {
        auto r = parse_http_response(
            std::span<const uint8_t>{buf.data(), buf.size()},
            std::span<HttpHeader>{hdrs});
        benchmark::DoNotOptimize(r);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(buf.size()));
}
BENCHMARK(BM_HttpParseResponseSimple);

static void BM_HttpParseResponseRich(benchmark::State& state) {
    auto buf = bytes_of(kRespRich);
    std::array<HttpHeader, 32> hdrs{};
    for (auto _ : state) {
        auto r = parse_http_response(
            std::span<const uint8_t>{buf.data(), buf.size()},
            std::span<HttpHeader>{hdrs});
        benchmark::DoNotOptimize(r);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(buf.size()));
}
BENCHMARK(BM_HttpParseResponseRich);

// ---------------------------------------------------------------------------
// Parse — partial (Ok(None) fast return)
// ---------------------------------------------------------------------------

static void BM_HttpParseRequestPartial(benchmark::State& state) {
    // First half of the rich request — header block clearly incomplete.
    auto buf = bytes_of(kReqRich);
    const auto half = buf.size() / 2;
    std::array<HttpHeader, 32> hdrs{};
    for (auto _ : state) {
        auto r = parse_http_request(
            std::span<const uint8_t>{buf.data(), half},
            std::span<HttpHeader>{hdrs});
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_HttpParseRequestPartial);

// ---------------------------------------------------------------------------
// Build — encoder used by WS upgrade + mocks
// ---------------------------------------------------------------------------

static void BM_HttpBuildRequest(benchmark::State& state) {
    std::array<uint8_t, 1024> out{};
    constexpr std::array<HttpHeader, 4> hdrs{{
        {"Host", "api.binance.com"},
        {"User-Agent", "eph/1.0"},
        {"Accept", "application/json"},
        {"Connection", "keep-alive"},
    }};
    for (auto _ : state) {
        auto r = build_http_request(
            out.data(), out.size(),
            "GET", "/api/v3/ping",
            std::span<const HttpHeader>{hdrs});
        benchmark::DoNotOptimize(r);
        benchmark::DoNotOptimize(out);
    }
}
BENCHMARK(BM_HttpBuildRequest);

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::off);
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
