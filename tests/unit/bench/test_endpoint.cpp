/// @file tests/unit/bench/test_endpoint.cpp
/// Unit coverage for benchmarks/latency/core/endpoint.hpp — pure
/// functions, no sockets, no subprocesses.

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "core/endpoint.hpp"

namespace {

std::string write_conf(std::string_view global, std::string_view section_body,
                       std::string_view section_name = "lat_ex_market") {
    const std::string path = "/tmp/test_endpoint_" +
                             std::to_string(::getpid()) + ".conf";
    std::ofstream f(path);
    f << global;
    f << "[" << section_name << "]\n";
    f << section_body;
    return path;
}

} // namespace

TEST(ParseWsUrl, PlainWsWithExplicitPort) {
    auto r = bench::parse_ws_url("ws://example.com:8080/ws/x").value();
    EXPECT_EQ(r.host, "example.com");
    EXPECT_EQ(r.port, 8080u);
    EXPECT_EQ(r.path, "/ws/x");
    EXPECT_FALSE(r.is_tls);
}

TEST(ParseWsUrl, WssDefaultPortAndPath) {
    auto r = bench::parse_ws_url("wss://stream.binance.com").value();
    EXPECT_EQ(r.host, "stream.binance.com");
    EXPECT_EQ(r.port, 443u);       // default for wss
    EXPECT_EQ(r.path, "/");         // default when no path given
    EXPECT_TRUE(r.is_tls);
}

TEST(ParseWsUrl, WssRealisticBinanceUrl) {
    auto r = bench::parse_ws_url(
        "wss://stream.binance.com:9443/ws/btcusdt@bookTicker").value();
    EXPECT_EQ(r.host, "stream.binance.com");
    EXPECT_EQ(r.port, 9443u);
    EXPECT_EQ(r.path, "/ws/btcusdt@bookTicker");
    EXPECT_TRUE(r.is_tls);
}

TEST(ParseWsUrl, RejectsUnknownScheme) {
    EXPECT_FALSE(bench::parse_ws_url("http://x/y").has_value());
    EXPECT_FALSE(bench::parse_ws_url("x").has_value());
    EXPECT_FALSE(bench::parse_ws_url("").has_value());
}

TEST(ParseWsUrl, RejectsNonNumericPort) {
    EXPECT_FALSE(bench::parse_ws_url("wss://x:abc/y").has_value());
}

TEST(ParseWsUrl, RejectsOutOfRangePort) {
    EXPECT_FALSE(bench::parse_ws_url("wss://x:65536/y").has_value());
}

TEST(ResolveEndpoint, DefaultsToMockIpPortPath) {
    const auto p = write_conf(
        "mock_ip = 10.0.0.1\n",
        "port = 20003\nws_path = /ws/book\n");
    auto globals = bench::ScenarioConfig::load_globals(p).value();
    auto section = bench::ScenarioConfig::load(p, "lat_ex_market").value();
    auto r = bench::resolve_endpoint(globals, section).value();
    EXPECT_EQ(r.host, "10.0.0.1");
    EXPECT_EQ(r.port, 20003u);
    EXPECT_EQ(r.ws_path, "/ws/book");
    EXPECT_FALSE(r.is_tls);
    EXPECT_FALSE(r.is_real_server);
    std::remove(p.c_str());
}

TEST(ResolveEndpoint, ExplicitMockSameAsAbsent) {
    const auto p = write_conf(
        "mock_ip = 10.0.0.1\n",
        "port = 20003\nws_path = /ws/book\nendpoint = mock\n");
    auto globals = bench::ScenarioConfig::load_globals(p).value();
    auto section = bench::ScenarioConfig::load(p, "lat_ex_market").value();
    auto r = bench::resolve_endpoint(globals, section).value();
    EXPECT_EQ(r.host, "10.0.0.1");
    EXPECT_FALSE(r.is_real_server);
    std::remove(p.c_str());
}

TEST(ResolveEndpoint, WssUrlOverridesGlobals) {
    const auto p = write_conf(
        "mock_ip = 10.0.0.1\n",
        "port = 20003\nws_path = /ignored\n"
        "endpoint = wss://stream.binance.com:9443/ws/btcusdt@bookTicker\n");
    auto globals = bench::ScenarioConfig::load_globals(p).value();
    auto section = bench::ScenarioConfig::load(p, "lat_ex_market").value();
    auto r = bench::resolve_endpoint(globals, section).value();
    EXPECT_EQ(r.host, "stream.binance.com");
    EXPECT_EQ(r.port, 9443u);
    EXPECT_EQ(r.ws_path, "/ws/btcusdt@bookTicker");
    EXPECT_TRUE(r.is_tls);
    EXPECT_TRUE(r.is_real_server);
    std::remove(p.c_str());
}

TEST(ResolveEndpoint, BadEndpointSchemeIsError) {
    const auto p = write_conf(
        "mock_ip = 10.0.0.1\n",
        "port = 20003\nendpoint = http://x\n");
    auto globals = bench::ScenarioConfig::load_globals(p).value();
    auto section = bench::ScenarioConfig::load(p, "lat_ex_market").value();
    auto r = bench::resolve_endpoint(globals, section);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("'mock'"), std::string::npos);
    std::remove(p.c_str());
}
