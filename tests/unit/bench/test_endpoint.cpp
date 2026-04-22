/// @file tests/unit/bench/test_endpoint.cpp
/// Unit coverage for benchmarks/latency/core/endpoint.hpp — pure
/// functions, no sockets, no subprocesses.

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "core/endpoint.hpp"

namespace {

/// Write a minimal TOML config that the new BenchConfig loader accepts
/// (all required networking / cpu keys populated) plus an arbitrary
/// [scenarios.<name>] subtable.
std::string write_toml(std::string_view server_ip,
                       std::string_view scenario_body,
                       std::string_view scenario_name = "lat_ex_market") {
    const std::string path = "/tmp/test_endpoint_" +
                             std::to_string(::getpid()) + ".toml";
    std::ofstream f(path);
    f << "[networking]\n"
      << "nic_a      = \"ens34\"\n"
      << "nic_b      = \"ens35\"\n"
      << "server_ip  = \"" << server_ip << "\"\n"
      << "client_ip  = \"10.0.0.99\"\n"
      << "gateway_ip = \"10.0.0.254\"\n"
      << "\n[cpu]\ncpu_client = 0\ncpu_mock = 1\n\n"
      << "[scenarios." << scenario_name << "]\n"
      << scenario_body;
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

TEST(ResolveEndpoint, DefaultsToServerIpPortPath) {
    const auto p = write_toml("10.0.0.1",
        "port             = 20003\n"
        "ws_path          = \"/ws/book\"\n");
    auto cfg = bench::load_bench_conf(p).value();
    const auto* sc = cfg.scenario("lat_ex_market");
    ASSERT_NE(sc, nullptr);
    auto r = bench::resolve_endpoint(cfg, *sc).value();
    EXPECT_EQ(r.host, "10.0.0.1");
    EXPECT_EQ(r.port, 20003u);
    EXPECT_EQ(r.ws_path, "/ws/book");
    EXPECT_FALSE(r.is_tls);
    EXPECT_FALSE(r.is_real_server);
    std::remove(p.c_str());
}

TEST(ResolveEndpoint, ExplicitMockSameAsAbsent) {
    const auto p = write_toml("10.0.0.1",
        "port     = 20003\n"
        "ws_path  = \"/ws/book\"\n"
        "endpoint = \"mock\"\n");
    auto cfg = bench::load_bench_conf(p).value();
    const auto* sc = cfg.scenario("lat_ex_market");
    ASSERT_NE(sc, nullptr);
    auto r = bench::resolve_endpoint(cfg, *sc).value();
    EXPECT_EQ(r.host, "10.0.0.1");
    EXPECT_FALSE(r.is_real_server);
    std::remove(p.c_str());
}

TEST(ResolveEndpoint, WssUrlOverridesServerIp) {
    const auto p = write_toml("10.0.0.1",
        "port     = 20003\n"
        "ws_path  = \"/ignored\"\n"
        "endpoint = \"wss://stream.binance.com:9443/ws/btcusdt@bookTicker\"\n");
    auto cfg = bench::load_bench_conf(p).value();
    const auto* sc = cfg.scenario("lat_ex_market");
    ASSERT_NE(sc, nullptr);
    auto r = bench::resolve_endpoint(cfg, *sc).value();
    EXPECT_EQ(r.host, "stream.binance.com");
    EXPECT_EQ(r.port, 9443u);
    EXPECT_EQ(r.ws_path, "/ws/btcusdt@bookTicker");
    EXPECT_TRUE(r.is_tls);
    EXPECT_TRUE(r.is_real_server);
    std::remove(p.c_str());
}

TEST(ResolveEndpoint, BadEndpointSchemeIsError) {
    const auto p = write_toml("10.0.0.1",
        "port     = 20003\n"
        "endpoint = \"http://x\"\n");
    auto cfg = bench::load_bench_conf(p).value();
    const auto* sc = cfg.scenario("lat_ex_market");
    ASSERT_NE(sc, nullptr);
    auto r = bench::resolve_endpoint(cfg, *sc);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("'mock'"), std::string::npos);
    std::remove(p.c_str());
}
