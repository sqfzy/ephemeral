/// @file bench_http_client.cpp
/// Micro-benchmarks for HttpClient pure functions: request building,
/// response parsing, and header lookup.
///
/// These are off the hot data path but are called for every REST API
/// interaction. Benchmarks establish baselines to catch regressions.

#include <string>
#include <string_view>

#include <benchmark/benchmark.h>

#include "eph/net/http_client.hpp"

using namespace eph::net;

// ---------------------------------------------------------------------------
// build_http_request — request construction
// ---------------------------------------------------------------------------

/// Build a simple GET request (most common case).
static void BM_BuildHttpRequest_Get(benchmark::State& state) {
    for (auto _ : state) {
        auto req = build_http_request("GET", "api.binance.com",
                                       "/api/v3/ticker/price?symbol=BTCUSDT");
        benchmark::DoNotOptimize(req);
    }
}
BENCHMARK(BM_BuildHttpRequest_Get);

/// Build a POST request with JSON body.
static void BM_BuildHttpRequest_Post(benchmark::State& state) {
    std::string_view body = R"({"symbol":"BTCUSDT","side":"BUY","type":"LIMIT","price":"50000","quantity":"0.01"})";
    for (auto _ : state) {
        auto req = build_http_request("POST", "api.binance.com",
                                       "/api/v3/order", body,
                                       "application/json",
                                       "X-MBX-APIKEY: key123\r\n");
        benchmark::DoNotOptimize(req);
    }
}
BENCHMARK(BM_BuildHttpRequest_Post);

// ---------------------------------------------------------------------------
// parse_http_response — response parsing
// ---------------------------------------------------------------------------

/// Parse a typical exchange JSON response.
static void BM_ParseHttpResponse_Json(benchmark::State& state) {
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 39\r\n"
        "X-MBX-USED-WEIGHT-1M: 1\r\n"
        "\r\n"
        R"({"symbol":"BTCUSDT","price":"50000.00"})";

    for (auto _ : state) {
        auto resp = parse_http_response(response);
        benchmark::DoNotOptimize(resp);
    }
}
BENCHMARK(BM_ParseHttpResponse_Json);

/// Parse a larger response (~1KB body).
static void BM_ParseHttpResponse_LargeBody(benchmark::State& state) {
    std::string body(1024, 'x');
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body;

    for (auto _ : state) {
        auto resp = parse_http_response(response);
        benchmark::DoNotOptimize(resp);
    }
}
BENCHMARK(BM_ParseHttpResponse_LargeBody);

// ---------------------------------------------------------------------------
// find_header — header lookup
// ---------------------------------------------------------------------------

/// Find a header in a typical exchange response header block.
static void BM_FindHeader(benchmark::State& state) {
    std::string headers =
        "Content-Type: application/json\r\n"
        "Content-Length: 39\r\n"
        "X-MBX-USED-WEIGHT-1M: 1\r\n"
        "X-MBX-ORDER-COUNT-10S: 3\r\n"
        "Connection: keep-alive\r\n";

    for (auto _ : state) {
        auto val = find_header(headers, "X-MBX-USED-WEIGHT-1M");
        benchmark::DoNotOptimize(val);
    }
}
BENCHMARK(BM_FindHeader);

/// Find the last header in a block (worst-case linear scan).
static void BM_FindHeader_LastHeader(benchmark::State& state) {
    std::string headers =
        "Content-Type: application/json\r\n"
        "Content-Length: 39\r\n"
        "X-MBX-USED-WEIGHT-1M: 1\r\n"
        "X-MBX-ORDER-COUNT-10S: 3\r\n"
        "Connection: keep-alive\r\n";

    for (auto _ : state) {
        auto val = find_header(headers, "Connection");
        benchmark::DoNotOptimize(val);
    }
}
BENCHMARK(BM_FindHeader_LastHeader);

/// find_header_opt — optional variant that distinguishes missing from empty.
static void BM_FindHeaderOpt(benchmark::State& state) {
    std::string headers =
        "Content-Type: application/json\r\n"
        "Content-Length: 39\r\n"
        "X-MBX-USED-WEIGHT-1M: 1\r\n";

    for (auto _ : state) {
        auto val = find_header_opt(headers, "X-MBX-USED-WEIGHT-1M");
        benchmark::DoNotOptimize(val);
    }
}
BENCHMARK(BM_FindHeaderOpt);

/// Find a non-existent header (full scan, miss).
static void BM_FindHeader_Miss(benchmark::State& state) {
    std::string headers =
        "Content-Type: application/json\r\n"
        "Content-Length: 39\r\n"
        "X-MBX-USED-WEIGHT-1M: 1\r\n";

    for (auto _ : state) {
        auto val = find_header(headers, "X-Nonexistent");
        benchmark::DoNotOptimize(val);
    }
}
BENCHMARK(BM_FindHeader_Miss);

// ---------------------------------------------------------------------------
// HttpResponse::to_json() — monitoring serialization
// ---------------------------------------------------------------------------

static void BM_HttpResponse_ToJson(benchmark::State& state) {
    HttpResponse resp{.status_code = 200, .body = R"({"ok":true,"data":"value"})"};

    for (auto _ : state) {
        auto j = resp.to_json();
        benchmark::DoNotOptimize(j);
    }
}
BENCHMARK(BM_HttpResponse_ToJson);

// ---------------------------------------------------------------------------
// HttpClient::Config::validate()
// ---------------------------------------------------------------------------

static void BM_HttpClientConfig_Validate(benchmark::State& state) {
    HttpClient::Config cfg{.host = "api.binance.com", .port = 443, .use_tls = true};

    for (auto _ : state) {
        auto err = cfg.validate();
        benchmark::DoNotOptimize(err);
    }
}
BENCHMARK(BM_HttpClientConfig_Validate);

// ---------------------------------------------------------------------------
// HttpClient::Config::from_url() — URL parsing
// ---------------------------------------------------------------------------

/// Parse a simple HTTPS URL (most common case for exchange APIs).
static void BM_HttpClientConfig_FromUrl_Https(benchmark::State& state) {
    for (auto _ : state) {
        auto cfg = HttpClient::Config::from_url("https://api.binance.com");
        benchmark::DoNotOptimize(cfg);
    }
}
BENCHMARK(BM_HttpClientConfig_FromUrl_Https);

/// Parse an HTTPS URL with explicit port and path (stripped).
static void BM_HttpClientConfig_FromUrl_HttpsWithPortAndPath(benchmark::State& state) {
    for (auto _ : state) {
        auto cfg = HttpClient::Config::from_url("https://api.exchange.com:9443/api/v3/ticker");
        benchmark::DoNotOptimize(cfg);
    }
}
BENCHMARK(BM_HttpClientConfig_FromUrl_HttpsWithPortAndPath);

/// Parse an HTTP URL (plaintext, used for local dev/testing).
static void BM_HttpClientConfig_FromUrl_Http(benchmark::State& state) {
    for (auto _ : state) {
        auto cfg = HttpClient::Config::from_url("http://localhost:8080");
        benchmark::DoNotOptimize(cfg);
    }
}
BENCHMARK(BM_HttpClientConfig_FromUrl_Http);

/// Parse an IPv6 URL with bracket notation.
static void BM_HttpClientConfig_FromUrl_Ipv6(benchmark::State& state) {
    for (auto _ : state) {
        auto cfg = HttpClient::Config::from_url("https://[::1]:8443");
        benchmark::DoNotOptimize(cfg);
    }
}
BENCHMARK(BM_HttpClientConfig_FromUrl_Ipv6);

// ---------------------------------------------------------------------------
// HttpClient::Config::to_url() — URL serialization
// ---------------------------------------------------------------------------

/// Serialize HTTPS config with default port (port elided).
static void BM_HttpClientConfig_ToUrl_DefaultPort(benchmark::State& state) {
    HttpClient::Config cfg{.host = "api.binance.com", .port = 443, .use_tls = true};
    for (auto _ : state) {
        auto url = cfg.to_url();
        benchmark::DoNotOptimize(url);
    }
}
BENCHMARK(BM_HttpClientConfig_ToUrl_DefaultPort);

/// Serialize config with non-default port (port included).
static void BM_HttpClientConfig_ToUrl_NonDefaultPort(benchmark::State& state) {
    HttpClient::Config cfg{.host = "api.exchange.com", .port = 9443, .use_tls = true};
    for (auto _ : state) {
        auto url = cfg.to_url();
        benchmark::DoNotOptimize(url);
    }
}
BENCHMARK(BM_HttpClientConfig_ToUrl_NonDefaultPort);

/// Serialize IPv6 config (bracket-enclosed).
static void BM_HttpClientConfig_ToUrl_Ipv6(benchmark::State& state) {
    HttpClient::Config cfg{.host = "::1", .port = 8443, .use_tls = true};
    for (auto _ : state) {
        auto url = cfg.to_url();
        benchmark::DoNotOptimize(url);
    }
}
BENCHMARK(BM_HttpClientConfig_ToUrl_Ipv6);

BENCHMARK_MAIN();
