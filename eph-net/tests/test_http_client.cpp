/// @file test_http_client.cpp
/// Unit tests for HttpClient request builder, response parser, and header lookup.
///
/// Tests pure functions (build_http_request, parse_http_response, find_header)
/// that don't require network access. Live integration tests are separated
/// into a dedicated test group (HttpClientLive) gated by an environment variable.

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "eph/net/http_client.hpp"

using namespace eph::net;

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time verification of constexpr HttpResponse status methods
// ─────────────────────────────────────────────────────────────────────────────

static_assert(HttpResponse{.status_code = 200}.is_success());
static_assert(!HttpResponse{.status_code = 200}.is_redirect());
static_assert(!HttpResponse{.status_code = 200}.is_client_error());
static_assert(!HttpResponse{.status_code = 200}.is_server_error());
static_assert(!HttpResponse{.status_code = 200}.is_error());

static_assert(HttpResponse{.status_code = 301}.is_redirect());
static_assert(!HttpResponse{.status_code = 301}.is_success());

static_assert(HttpResponse{.status_code = 404}.is_client_error());
static_assert(HttpResponse{.status_code = 404}.is_error());

static_assert(HttpResponse{.status_code = 500}.is_server_error());
static_assert(HttpResponse{.status_code = 500}.is_error());

// =============================================================================
// build_http_request — GET
// =============================================================================

TEST(HttpClientRequest, GetBasic) {
    auto result = build_http_request("GET", "api.binance.com", "/api/v3/ticker/price");
    ASSERT_TRUE(result.has_value()) << result.error();
    auto& req = *result;

    EXPECT_NE(req.find("GET /api/v3/ticker/price HTTP/1.1\r\n"), std::string::npos);
    EXPECT_NE(req.find("Host: api.binance.com\r\n"), std::string::npos);
    EXPECT_NE(req.find("Connection: close\r\n"), std::string::npos);
    // Should NOT have Content-Length for GET without body
    EXPECT_EQ(req.find("Content-Length:"), std::string::npos);
    // Ends with \r\n\r\n
    EXPECT_EQ(req.substr(req.size() - 4), "\r\n\r\n");
}

TEST(HttpClientRequest, GetWithExtraHeaders) {
    auto result = build_http_request("GET", "api.exchange.com", "/v1/balance",
        /*body=*/{}, /*content_type=*/{},
        "Authorization: Bearer tok123\r\nX-MBX-APIKEY: key456\r\n");
    ASSERT_TRUE(result.has_value()) << result.error();
    auto& req = *result;

    EXPECT_NE(req.find("Authorization: Bearer tok123\r\n"), std::string::npos);
    EXPECT_NE(req.find("X-MBX-APIKEY: key456\r\n"), std::string::npos);
}

TEST(HttpClientRequest, GetWithQueryString) {
    auto result = build_http_request("GET", "api.binance.com",
        "/api/v3/depth?symbol=BTCUSDT&limit=5");
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_NE(result->find("GET /api/v3/depth?symbol=BTCUSDT&limit=5 HTTP/1.1"),
              std::string::npos);
}

// =============================================================================
// build_http_request — POST
// =============================================================================

TEST(HttpClientRequest, PostWithJsonBody) {
    std::string body = R"({"symbol":"BTCUSDT","side":"BUY","quantity":"0.01"})";
    auto result = build_http_request("POST", "api.exchange.com", "/api/v1/order",
        body, "application/json");
    ASSERT_TRUE(result.has_value()) << result.error();
    auto& req = *result;

    EXPECT_NE(req.find("POST /api/v1/order HTTP/1.1\r\n"), std::string::npos);
    EXPECT_NE(req.find("Content-Type: application/json\r\n"), std::string::npos);
    EXPECT_NE(req.find(std::format("Content-Length: {}\r\n", body.size())),
              std::string::npos);
    // Body should appear after \r\n\r\n
    auto header_end = req.find("\r\n\r\n");
    ASSERT_NE(header_end, std::string::npos);
    EXPECT_EQ(req.substr(header_end + 4), body);
}

TEST(HttpClientRequest, PostWithFormBody) {
    std::string body = "symbol=BTCUSDT&side=BUY&quantity=0.01";
    auto result = build_http_request("POST", "api.exchange.com", "/order",
        body, "application/x-www-form-urlencoded");
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_NE(result->find("Content-Type: application/x-www-form-urlencoded\r\n"),
              std::string::npos);
    EXPECT_NE(result->find(std::format("Content-Length: {}\r\n", body.size())),
              std::string::npos);
}

TEST(HttpClientRequest, PostEmptyBodyOmitsContentType) {
    auto result = build_http_request("POST", "host.com", "/endpoint",
        /*body=*/{}, "application/json");
    ASSERT_TRUE(result.has_value()) << result.error();
    // Empty body should not produce Content-Type or Content-Length
    EXPECT_EQ(result->find("Content-Type:"), std::string::npos);
    EXPECT_EQ(result->find("Content-Length:"), std::string::npos);
}

TEST(HttpClientRequest, PostWithExtraHeaders) {
    auto result = build_http_request("POST", "host.com", "/api",
        R"({"key":"val"})", "application/json",
        "X-Custom: value\r\n");
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_NE(result->find("X-Custom: value\r\n"), std::string::npos);
}

// =============================================================================
// build_http_request — input validation
// =============================================================================

TEST(HttpClientRequest, EmptyMethodReturnsError) {
    auto result = build_http_request("", "host.com", "/path");
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("method"), std::string::npos);
}

TEST(HttpClientRequest, EmptyHostReturnsError) {
    auto result = build_http_request("GET", "", "/path");
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("Host"), std::string::npos);
}

TEST(HttpClientRequest, EmptyPathReturnsError) {
    auto result = build_http_request("GET", "host.com", "");
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("Path"), std::string::npos);
}

TEST(HttpClientRequest, ExtraHeadersWithBlankLineRejectsInjection) {
    // extra_headers containing \r\n\r\n would terminate headers early,
    // enabling HTTP request smuggling. Verify this is rejected.
    auto result = build_http_request("GET", "host.com", "/path",
        {}, {}, "Injected: yes\r\n\r\nGET /evil HTTP/1.1\r\n");
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("injection"), std::string::npos);
}

TEST(HttpClientRequest, ExtraHeadersWithoutBlankLineAccepted) {
    // Normal multi-header extra_headers should work fine
    auto result = build_http_request("GET", "host.com", "/path",
        {}, {}, "X-One: 1\r\nX-Two: 2\r\n");
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_NE(result->find("X-One: 1\r\n"), std::string::npos);
    EXPECT_NE(result->find("X-Two: 2\r\n"), std::string::npos);
}

TEST(HttpClientRequest, LargeBodyIncludesContentLength) {
    std::string body(10000, 'x');
    auto result = build_http_request("POST", "host.com", "/upload",
        body, "application/octet-stream");
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_NE(result->find("Content-Length: 10000\r\n"), std::string::npos);
    // Verify the body is appended
    EXPECT_GE(result->size(), 10000u);
}

// =============================================================================
// build_http_request — other methods
// =============================================================================

TEST(HttpClientRequest, DeleteMethod) {
    auto result = build_http_request("DELETE", "api.exchange.com", "/api/v1/order/123");
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_NE(result->find("DELETE /api/v1/order/123 HTTP/1.1\r\n"), std::string::npos);
}

TEST(HttpClientRequest, PutMethodWithBody) {
    auto result = build_http_request("PUT", "host.com", "/resource",
        R"({"updated":true})", "application/json");
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_NE(result->find("PUT /resource HTTP/1.1\r\n"), std::string::npos);
    EXPECT_NE(result->find("Content-Type: application/json\r\n"), std::string::npos);
}

// =============================================================================
// parse_http_response — success cases
// =============================================================================

TEST(HttpClientResponse, Parse200Ok) {
    std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 27\r\n"
        "\r\n"
        R"({"price":"50000.00","qty":1})";

    auto result = parse_http_response(raw);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->status_code, 200);
    EXPECT_EQ(result->body, R"({"price":"50000.00","qty":1})");
    EXPECT_NE(result->headers_raw.find("Content-Type: application/json"),
              std::string::npos);
}

TEST(HttpClientResponse, Parse404NotFound) {
    std::string raw =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: 9\r\n"
        "\r\n"
        "Not Found";

    auto result = parse_http_response(raw);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->status_code, 404);
    EXPECT_EQ(result->body, "Not Found");
}

TEST(HttpClientResponse, Parse500InternalServerError) {
    std::string raw =
        "HTTP/1.1 500 Internal Server Error\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    auto result = parse_http_response(raw);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->status_code, 500);
    EXPECT_TRUE(result->body.empty());
}

TEST(HttpClientResponse, ParseEmptyBody) {
    std::string raw =
        "HTTP/1.1 204 No Content\r\n"
        "\r\n";

    auto result = parse_http_response(raw);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->status_code, 204);
    EXPECT_TRUE(result->body.empty());
}

TEST(HttpClientResponse, ParseBodyLargerThanContentLength) {
    // Body has extra data — parser should include all data after headers
    // (Content-Length enforcement is done at the I/O layer, not the parser)
    std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "helloextra";

    auto result = parse_http_response(raw);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->status_code, 200);
    // Parser returns everything after headers
    EXPECT_EQ(result->body, "helloextra");
}

TEST(HttpClientResponse, ParseMultipleHeaders) {
    std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "X-RateLimit-Remaining: 1199\r\n"
        "X-RateLimit-Reset: 1640000000\r\n"
        "Content-Length: 2\r\n"
        "\r\n"
        "{}";

    auto result = parse_http_response(raw);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->status_code, 200);
    EXPECT_EQ(result->body, "{}");
    // All headers should be in headers_raw
    EXPECT_NE(result->headers_raw.find("X-RateLimit-Remaining: 1199"),
              std::string::npos);
    EXPECT_NE(result->headers_raw.find("X-RateLimit-Reset: 1640000000"),
              std::string::npos);
}

// =============================================================================
// parse_http_response — error cases
// =============================================================================

TEST(HttpClientResponse, ParseIncompleteNoHeaderEnd) {
    std::string raw = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n";
    auto result = parse_http_response(raw);
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("Incomplete"), std::string::npos);
}

TEST(HttpClientResponse, ParseMalformedStatusLine) {
    std::string raw = "GARBAGE\r\n\r\n";
    auto result = parse_http_response(raw);
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("Malformed"), std::string::npos);
}

TEST(HttpClientResponse, ParseInvalidStatusCode) {
    std::string raw = "HTTP/1.1 XYZ Bad\r\n\r\n";
    auto result = parse_http_response(raw);
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("Invalid"), std::string::npos);
}

TEST(HttpClientResponse, ParseEmptyInput) {
    auto result = parse_http_response("");
    EXPECT_FALSE(result.has_value());
}

// =============================================================================
// parse_http_response — edge cases
// =============================================================================

TEST(HttpClientResponse, ParseStatusCodeOnly) {
    // Some servers return status code without reason phrase
    std::string raw = "HTTP/1.1 200\r\n\r\n";
    auto result = parse_http_response(raw);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->status_code, 200);
}

TEST(HttpClientResponse, ParseHeadersRawExcludesStatusLine) {
    std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n";

    auto result = parse_http_response(raw);
    ASSERT_TRUE(result.has_value()) << result.error();
    // headers_raw should NOT contain the status line
    EXPECT_EQ(result->headers_raw.find("HTTP/1.1"), std::string::npos);
    EXPECT_NE(result->headers_raw.find("Content-Type: text/plain"),
              std::string::npos);
}

TEST(HttpClientResponse, ParseBinaryBody) {
    // Body with null bytes and binary data
    std::string raw =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 8\r\n"
        "\r\n";
    raw.append("\x00\x01\x02\x03\x04\x05\x06\x07", 8);

    auto result = parse_http_response(raw);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->body.size(), 8u);
}

// =============================================================================
// find_header — case-insensitive header lookup
// =============================================================================

TEST(HttpClientFindHeader, BasicLookup) {
    std::string headers =
        "Content-Type: application/json\r\n"
        "Content-Length: 42\r\n"
        "X-Custom: value123\r\n";

    EXPECT_EQ(find_header(headers, "Content-Type"), "application/json");
    EXPECT_EQ(find_header(headers, "Content-Length"), "42");
    EXPECT_EQ(find_header(headers, "X-Custom"), "value123");
}

TEST(HttpClientFindHeader, CaseInsensitive) {
    std::string headers = "content-type: text/html\r\n";

    EXPECT_EQ(find_header(headers, "Content-Type"), "text/html");
    EXPECT_EQ(find_header(headers, "CONTENT-TYPE"), "text/html");
    EXPECT_EQ(find_header(headers, "content-type"), "text/html");
}

TEST(HttpClientFindHeader, TrimsWhitespace) {
    std::string headers = "Content-Type:   application/json  \r\n";
    EXPECT_EQ(find_header(headers, "Content-Type"), "application/json");
}

TEST(HttpClientFindHeader, NotFoundReturnsEmpty) {
    std::string headers = "Content-Type: text/html\r\n";
    EXPECT_EQ(find_header(headers, "X-Missing"), "");
}

TEST(HttpClientFindHeader, EmptyHeaders) {
    EXPECT_EQ(find_header("", "Content-Type"), "");
}

TEST(HttpClientFindHeader, HeaderWithColonInValue) {
    std::string headers = "Location: https://example.com:8080/path\r\n";
    EXPECT_EQ(find_header(headers, "Location"), "https://example.com:8080/path");
}

TEST(HttpClientFindHeader, MultipleHeadersReturnFirst) {
    // Duplicate headers — returns the first one found
    std::string headers =
        "Set-Cookie: a=1\r\n"
        "Set-Cookie: b=2\r\n";
    EXPECT_EQ(find_header(headers, "Set-Cookie"), "a=1");
}

// =============================================================================
// is_response_complete — Content-Length detection
// =============================================================================

TEST(HttpClientComplete, CompleteWithContentLength) {
    std::string buf =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";
    EXPECT_TRUE(HttpClient::is_response_complete(buf));
}

TEST(HttpClientComplete, IncompleteBody) {
    std::string buf =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 10\r\n"
        "\r\n"
        "hello";
    EXPECT_FALSE(HttpClient::is_response_complete(buf));
}

TEST(HttpClientComplete, NoContentLength) {
    std::string buf =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "\r\n"
        "some data";
    // Without Content-Length, we can't know if complete — returns false
    EXPECT_FALSE(HttpClient::is_response_complete(buf));
}

TEST(HttpClientComplete, IncompleteHeaders) {
    std::string buf = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n";
    EXPECT_FALSE(HttpClient::is_response_complete(buf));
}

TEST(HttpClientComplete, ZeroContentLength) {
    std::string buf =
        "HTTP/1.1 204 No Content\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    EXPECT_TRUE(HttpClient::is_response_complete(buf));
}

TEST(HttpClientComplete, LowercaseContentLength) {
    std::string buf =
        "HTTP/1.1 200 OK\r\n"
        "content-length: 3\r\n"
        "\r\n"
        "abc";
    EXPECT_TRUE(HttpClient::is_response_complete(buf));
}

// =============================================================================
// HttpClient::Config
// =============================================================================

TEST(HttpClientConfig, DefaultValues) {
    HttpClient::Config cfg;
    cfg.host = "api.exchange.com";
    EXPECT_EQ(cfg.port, 443);
    EXPECT_TRUE(cfg.use_tls);
    EXPECT_EQ(cfg.timeout, std::chrono::milliseconds{5000});
    EXPECT_TRUE(cfg.ca_cert_path.empty());
}

TEST(HttpClientConfig, DumpContainsFields) {
    HttpClient::Config cfg{
        .host = "api.binance.com",
        .port = 443,
        .use_tls = true,
        .timeout = std::chrono::milliseconds{3000},
    };
    auto dump = cfg.dump();
    EXPECT_NE(dump.find("api.binance.com"), std::string::npos);
    EXPECT_NE(dump.find("443"), std::string::npos);
    EXPECT_NE(dump.find("3000"), std::string::npos);
}

// =============================================================================
// HttpClient construction
// =============================================================================

TEST(HttpClient, ConstructWithConfig) {
    HttpClient client(HttpClient::Config{
        .host = "api.binance.com",
        .port = 443,
        .use_tls = true,
        .timeout = std::chrono::milliseconds{3000},
    });

    EXPECT_EQ(client.config().host, "api.binance.com");
    EXPECT_EQ(client.config().port, 443);
    EXPECT_TRUE(client.config().use_tls);
}

// =============================================================================
// Round-trip: build request -> parse response
// =============================================================================

TEST(HttpClientRoundTrip, BuildAndParseSimulatedExchange) {
    // Simulate building a request and parsing a typical exchange response
    auto req = build_http_request("GET", "api.binance.com",
        "/api/v3/ticker/price?symbol=BTCUSDT");
    ASSERT_TRUE(req.has_value()) << req.error();

    // Simulate server response
    std::string server_response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 39\r\n"
        "X-MBX-USED-WEIGHT-1M: 1\r\n"
        "\r\n"
        R"({"symbol":"BTCUSDT","price":"50000.00"})";

    auto resp = parse_http_response(server_response);
    ASSERT_TRUE(resp.has_value()) << resp.error();
    EXPECT_EQ(resp->status_code, 200);
    EXPECT_EQ(resp->body, R"({"symbol":"BTCUSDT","price":"50000.00"})");
    EXPECT_EQ(find_header(resp->headers_raw, "Content-Type"), "application/json");
    EXPECT_EQ(find_header(resp->headers_raw, "X-MBX-USED-WEIGHT-1M"), "1");
}

TEST(HttpClientRoundTrip, PostOrderAndParseResponse) {
    std::string order_body =
        R"({"symbol":"ETHUSDT","side":"BUY","type":"LIMIT","price":"3000","quantity":"1"})";
    auto req = build_http_request("POST", "api.exchange.com", "/api/v1/order",
        order_body, "application/json",
        "X-API-KEY: mykey123\r\n");
    ASSERT_TRUE(req.has_value()) << req.error();

    // Verify request has all parts
    EXPECT_NE(req->find("POST /api/v1/order HTTP/1.1"), std::string::npos);
    EXPECT_NE(req->find("X-API-KEY: mykey123"), std::string::npos);
    EXPECT_NE(req->find(order_body), std::string::npos);

    // Simulate response
    std::string server_response =
        "HTTP/1.1 201 Created\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 25\r\n"
        "\r\n"
        R"({"orderId":"12345","ok":1})";

    auto resp = parse_http_response(server_response);
    ASSERT_TRUE(resp.has_value()) << resp.error();
    EXPECT_EQ(resp->status_code, 201);
    EXPECT_EQ(resp->body, R"({"orderId":"12345","ok":1})");
}

// =============================================================================
// HttpResponse — status category helpers
// =============================================================================

TEST(HttpResponse, IsSuccessFor2xx) {
    EXPECT_TRUE((HttpResponse{.status_code = 200}).is_success());
    EXPECT_TRUE((HttpResponse{.status_code = 201}).is_success());
    EXPECT_TRUE((HttpResponse{.status_code = 299}).is_success());
    EXPECT_FALSE((HttpResponse{.status_code = 199}).is_success());
    EXPECT_FALSE((HttpResponse{.status_code = 300}).is_success());
}

TEST(HttpResponse, IsClientErrorFor4xx) {
    EXPECT_TRUE((HttpResponse{.status_code = 400}).is_client_error());
    EXPECT_TRUE((HttpResponse{.status_code = 404}).is_client_error());
    EXPECT_TRUE((HttpResponse{.status_code = 499}).is_client_error());
    EXPECT_FALSE((HttpResponse{.status_code = 399}).is_client_error());
    EXPECT_FALSE((HttpResponse{.status_code = 500}).is_client_error());
}

TEST(HttpResponse, IsServerErrorFor5xx) {
    EXPECT_TRUE((HttpResponse{.status_code = 500}).is_server_error());
    EXPECT_TRUE((HttpResponse{.status_code = 503}).is_server_error());
    EXPECT_TRUE((HttpResponse{.status_code = 599}).is_server_error());
    EXPECT_FALSE((HttpResponse{.status_code = 499}).is_server_error());
    EXPECT_FALSE((HttpResponse{.status_code = 600}).is_server_error());
}

TEST(HttpResponse, ZeroStatusCodeIsNoneOfCategories) {
    HttpResponse resp;
    EXPECT_FALSE(resp.is_success());
    EXPECT_FALSE(resp.is_client_error());
    EXPECT_FALSE(resp.is_server_error());
    EXPECT_FALSE(resp.is_redirect());
    EXPECT_FALSE(resp.is_error());
}

TEST(HttpResponse, IsRedirectFor3xx) {
    EXPECT_TRUE((HttpResponse{.status_code = 301}).is_redirect());
    EXPECT_TRUE((HttpResponse{.status_code = 302}).is_redirect());
    EXPECT_TRUE((HttpResponse{.status_code = 307}).is_redirect());
    EXPECT_TRUE((HttpResponse{.status_code = 399}).is_redirect());
    EXPECT_FALSE((HttpResponse{.status_code = 200}).is_redirect());
    EXPECT_FALSE((HttpResponse{.status_code = 299}).is_redirect());
    EXPECT_FALSE((HttpResponse{.status_code = 400}).is_redirect());
}

TEST(HttpResponse, IsErrorFor4xxAnd5xx) {
    EXPECT_TRUE((HttpResponse{.status_code = 400}).is_error());
    EXPECT_TRUE((HttpResponse{.status_code = 404}).is_error());
    EXPECT_TRUE((HttpResponse{.status_code = 500}).is_error());
    EXPECT_TRUE((HttpResponse{.status_code = 503}).is_error());
    EXPECT_TRUE((HttpResponse{.status_code = 599}).is_error());
    EXPECT_FALSE((HttpResponse{.status_code = 200}).is_error());
    EXPECT_FALSE((HttpResponse{.status_code = 301}).is_error());
    EXPECT_FALSE((HttpResponse{.status_code = 399}).is_error());
    EXPECT_FALSE((HttpResponse{.status_code = 600}).is_error());
}

TEST(HttpResponse, StatusCategoriesMutuallyExclusive) {
    // For each major status range, verify exactly one category matches
    auto check = [](int code) {
        HttpResponse r{.status_code = code};
        int count = (r.is_success() ? 1 : 0)
                  + (r.is_redirect() ? 1 : 0)
                  + (r.is_client_error() ? 1 : 0)
                  + (r.is_server_error() ? 1 : 0);
        return count;
    };
    EXPECT_EQ(check(200), 1);
    EXPECT_EQ(check(301), 1);
    EXPECT_EQ(check(404), 1);
    EXPECT_EQ(check(500), 1);
    EXPECT_EQ(check(0), 0);   // none match
    EXPECT_EQ(check(100), 0); // 1xx not classified
}

// =============================================================================
// HttpClient::Config::validate()
// =============================================================================

TEST(HttpClientConfig, DefaultConstructedIsInvalid) {
    HttpClient::Config cfg;
    EXPECT_FALSE(cfg.validate().empty());  // host is empty
}

TEST(HttpClientConfig, ValidConfigPasses) {
    HttpClient::Config cfg{.host = "api.binance.com"};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(HttpClientConfig, EmptyHostFails) {
    HttpClient::Config cfg{.host = "", .port = 443};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("host"), std::string_view::npos);
}

TEST(HttpClientConfig, HostWithControlCharsFails) {
    HttpClient::Config cfg{.host = "evil\r\nhost"};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("control characters"), std::string_view::npos);
}

TEST(HttpClientConfig, ZeroPortFails) {
    HttpClient::Config cfg{.host = "example.com", .port = 0};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("port"), std::string_view::npos);
}

TEST(HttpClientConfig, ZeroTimeoutFails) {
    HttpClient::Config cfg{.host = "example.com", .timeout = std::chrono::milliseconds{0}};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("timeout"), std::string_view::npos);
}

TEST(HttpClientConfig, ZeroMaxResponseSizeFails) {
    HttpClient::Config cfg{.host = "example.com", .max_response_size = 0};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("max_response_size"), std::string_view::npos);
}

TEST(HttpResponse, EqualityOperator) {
    HttpResponse a{200, "ok", "Content-Type: text/plain\r\n"};
    HttpResponse b{200, "ok", "Content-Type: text/plain\r\n"};
    HttpResponse c{404, "not found", ""};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

// =============================================================================
// std::formatter<HttpResponse>
// =============================================================================

TEST(HttpResponse, FormatterContainsStatusAndSize) {
    HttpResponse resp{.status_code = 200, .body = "hello world"};
    auto s = std::format("{}", resp);
    EXPECT_NE(s.find("200"), std::string::npos);
    EXPECT_NE(s.find("11B"), std::string::npos);  // "hello world" = 11 bytes
}

TEST(HttpResponse, FormatterCompositeFormat) {
    HttpResponse resp{.status_code = 404, .body = ""};
    auto s = std::format("result={}", resp);
    EXPECT_NE(s.find("result="), std::string::npos);
    EXPECT_NE(s.find("404"), std::string::npos);
}

// =============================================================================
// HttpResponse::to_json()
// =============================================================================

TEST(HttpResponse, ToJsonContainsKeyFields) {
    HttpResponse resp{.status_code = 200, .body = R"({"ok":true})"};
    auto j = resp.to_json();
    EXPECT_EQ(j.front(), '{');
    EXPECT_EQ(j.back(), '}');
    EXPECT_NE(j.find("\"status_code\":200"), std::string::npos);
    EXPECT_NE(j.find("\"is_success\":true"), std::string::npos);
}

TEST(HttpResponse, ToJsonTruncatesLargeBody) {
    HttpResponse resp{.status_code = 200, .body = std::string(500, 'x')};
    auto j = resp.to_json();
    EXPECT_NE(j.find("\"body_size\":500"), std::string::npos);
    EXPECT_NE(j.find("..."), std::string::npos);  // truncation marker
}

TEST(HttpResponse, ToJsonErrorResponse) {
    HttpResponse resp{.status_code = 503, .body = "Service Unavailable"};
    auto j = resp.to_json();
    EXPECT_NE(j.find("\"is_success\":false"), std::string::npos);
    EXPECT_NE(j.find("503"), std::string::npos);
}

TEST(HttpResponse, ToJsonEscapesBodyWithQuotes) {
    // Body containing quotes and backslashes must be properly escaped
    // to produce valid JSON (RFC 8259 section 7).
    HttpResponse resp{.status_code = 200, .body = R"({"key":"value"})"};
    auto j = resp.to_json();
    // The body_preview should have escaped quotes so the JSON is valid
    EXPECT_EQ(j.front(), '{');
    EXPECT_EQ(j.back(), '}');
    // The raw quote characters in body should be escaped
    EXPECT_NE(j.find("\\\"key\\\""), std::string::npos)
        << "body_preview should escape embedded quotes; got: " << j;
}

TEST(HttpResponse, ToJsonEscapesBodyWithBackslash) {
    HttpResponse resp{.status_code = 200, .body = "path\\to\\file"};
    auto j = resp.to_json();
    EXPECT_NE(j.find("path\\\\to\\\\file"), std::string::npos)
        << "body_preview should escape backslashes; got: " << j;
}

TEST(HttpResponse, ToJsonEscapesBodyWithControlChars) {
    HttpResponse resp{.status_code = 200, .body = "line1\nline2\ttab"};
    auto j = resp.to_json();
    // Control characters should be escaped (no raw \n or \t in JSON string)
    EXPECT_NE(j.find("\\n"), std::string::npos)
        << "body_preview should escape newlines; got: " << j;
}

// =============================================================================
// std::formatter<HttpClient::Config>
// =============================================================================

TEST(HttpClientConfig, FormatterContainsKeyFields) {
    HttpClient::Config cfg{
        .host = "api.binance.com", .port = 443,
        .use_tls = true, .timeout = std::chrono::milliseconds{5000}};
    auto s = std::format("{}", cfg);
    EXPECT_NE(s.find("api.binance.com"), std::string::npos);
    EXPECT_NE(s.find("443"), std::string::npos);
    EXPECT_NE(s.find("5000ms"), std::string::npos);
}

TEST(HttpClientConfig, ToJsonValidStructure) {
    HttpClient::Config cfg{.host = "api.binance.com", .port = 443};
    auto j = cfg.to_json();
    EXPECT_EQ(j.front(), '{');
    EXPECT_EQ(j.back(), '}');
    EXPECT_NE(j.find("\"host\":\"api.binance.com\""), std::string::npos);
    EXPECT_NE(j.find("\"port\":443"), std::string::npos);
}

TEST(HttpClientConfig, ToJsonEscapesHostWithQuotes) {
    // Host containing quotes should be escaped in JSON output
    HttpClient::Config cfg{.host = "evil\"host", .port = 443};
    auto j = cfg.to_json();
    EXPECT_NE(j.find("evil\\\"host"), std::string::npos)
        << "host with quotes should be escaped; got: " << j;
}

TEST(HttpClientConfig, ToJsonEscapesCaCertPath) {
    // ca_cert_path with backslashes should be escaped
    HttpClient::Config cfg{.host = "api.io", .port = 443,
                           .ca_cert_path = "C:\\certs\\ca.pem"};
    auto j = cfg.to_json();
    EXPECT_NE(j.find("C:\\\\certs\\\\ca.pem"), std::string::npos)
        << "ca_cert_path backslashes should be escaped; got: " << j;
}

TEST(HttpClientConfig, Equality) {
    HttpClient::Config a{.host = "api.io", .port = 443};
    HttpClient::Config b{.host = "api.io", .port = 443};
    HttpClient::Config c{.host = "other.io", .port = 443};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST(HttpClientConfig, WarningsEmptyForReasonableConfig) {
    HttpClient::Config cfg{.host = "api.io"};
    EXPECT_TRUE(cfg.warnings().empty());
}

TEST(HttpClientConfig, WarnsOnPlaintextHttp) {
    HttpClient::Config cfg{.host = "api.io", .use_tls = false};
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& s : w) {
        if (s.find("plaintext") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(HttpClientConfig, WarnsOnShortTimeout) {
    HttpClient::Config cfg{.host = "api.io",
                           .timeout = std::chrono::milliseconds{500}};
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& s : w) {
        if (s.find("short") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(HttpClientConfig, WarnsOnExcessiveMaxResponseSize) {
    HttpClient::Config cfg{.host = "api.io",
                           .max_response_size = 128 * 1024 * 1024}; // 128 MB
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& s : w) {
        if (s.find("64MB") != std::string::npos || s.find("memory") != std::string::npos)
            found = true;
    }
    EXPECT_TRUE(found) << "expected warning about excessive max_response_size";
}

TEST(HttpClientConfig, NoWarningsWhenAllReasonable) {
    // Full config with all fields set to reasonable values
    HttpClient::Config cfg{
        .host = "api.binance.com",
        .port = 443,
        .use_tls = true,
        .timeout = std::chrono::milliseconds{5000},
        .max_response_size = 8 * 1024 * 1024,
    };
    EXPECT_TRUE(cfg.warnings().empty());
}

// =============================================================================
// find_header edge cases
// =============================================================================

TEST(HttpClientFindHeader, PartialNameMatchDoesNotFalsePositive) {
    // "Content-Typ" should not match "Content-Type"
    std::string headers = "Content-Type: application/json\r\n";
    EXPECT_EQ(find_header(headers, "Content-Typ"), "");
}

TEST(HttpClientFindHeader, EmptyValueReturnsEmpty) {
    // Header with empty value after colon
    std::string headers = "X-Empty:\r\n";
    EXPECT_EQ(find_header(headers, "X-Empty"), "");
}

TEST(HttpClientFindHeader, WhitespaceOnlyValueReturnsEmpty) {
    // Header with only whitespace value (should be trimmed to empty)
    std::string headers = "X-Blank:   \t  \r\n";
    EXPECT_EQ(find_header(headers, "X-Blank"), "");
}

TEST(HttpClientFindHeader, ValueWithLeadingAndTrailingOWS) {
    // RFC 7230: OWS (optional whitespace) should be trimmed
    std::string headers = "X-Val:  \t hello  \r\n";
    EXPECT_EQ(find_header(headers, "X-Val"), "hello");
}

// =============================================================================
// find_header_opt — distinguishes missing from empty
// =============================================================================

// =============================================================================
// HttpResponse::header() and has_header() convenience methods
// =============================================================================

TEST(HttpResponse, HeaderConvenienceReturnsValue) {
    HttpResponse resp{
        .status_code = 200, .body = "ok",
        .headers_raw = "Content-Type: application/json\r\nX-Rate: 5\r\n"};
    EXPECT_EQ(resp.header("Content-Type"), "application/json");
    EXPECT_EQ(resp.header("X-Rate"), "5");
    EXPECT_EQ(resp.header("X-Missing"), "");
}

TEST(HttpResponse, HasHeaderReturnsTrueForPresent) {
    HttpResponse resp{
        .status_code = 200, .body = "",
        .headers_raw = "Content-Type: text/plain\r\nX-Empty:\r\n"};
    EXPECT_TRUE(resp.has_header("Content-Type"));
    EXPECT_TRUE(resp.has_header("X-Empty"));  // present even with empty value
    EXPECT_FALSE(resp.has_header("X-Missing"));
}

TEST(HttpClientFindHeaderOpt, ReturnsValueWhenPresent) {
    std::string headers = "Content-Type: application/json\r\n";
    auto result = find_header_opt(headers, "Content-Type");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "application/json");
}

TEST(HttpClientFindHeaderOpt, ReturnsNulloptWhenMissing) {
    std::string headers = "Content-Type: application/json\r\n";
    auto result = find_header_opt(headers, "X-Nonexistent");
    EXPECT_FALSE(result.has_value());
}

TEST(HttpClientFindHeaderOpt, ReturnsEmptyStringForEmptyValue) {
    // Header present but with empty value: should return "" not nullopt
    std::string headers = "X-Empty:\r\n";
    auto result = find_header_opt(headers, "X-Empty");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "");
}

TEST(HttpClientFindHeaderOpt, DistinguishesMissingFromEmpty) {
    // This is the key use case: missing vs empty
    std::string headers = "X-Present:\r\nX-Valued: 42\r\n";
    auto present = find_header_opt(headers, "X-Present");
    auto valued = find_header_opt(headers, "X-Valued");
    auto missing = find_header_opt(headers, "X-Missing");

    ASSERT_TRUE(present.has_value());
    EXPECT_EQ(*present, "");

    ASSERT_TRUE(valued.has_value());
    EXPECT_EQ(*valued, "42");

    EXPECT_FALSE(missing.has_value());
}

TEST(HttpClientFindHeaderOpt, CaseInsensitive) {
    std::string headers = "Content-Type: text/plain\r\n";
    ASSERT_TRUE(find_header_opt(headers, "content-type").has_value());
    ASSERT_TRUE(find_header_opt(headers, "CONTENT-TYPE").has_value());
}
