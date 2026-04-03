/// @file test_binance_rest.cpp
/// Unit tests for Binance REST API response parsers (depth snapshot, server time).
///
/// Tests parse_depth_response and parse_server_time_response with hardcoded
/// sample responses. NO live network tests — those are integration tests.

#include <cmath>
#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

#include "eph/json/adapters/binance_rest.hpp"

using namespace eph::json::binance;

// ---------------------------------------------------------------------------
// Sample responses (based on real Binance API format)
// ---------------------------------------------------------------------------

static constexpr std::string_view kDepthResponse = R"({
  "lastUpdateId": 1027024,
  "bids": [
    ["4.00000000", "431.00000000"],
    ["3.99000000", "200.00000000"],
    ["3.98000000", "100.00000000"]
  ],
  "asks": [
    ["4.00000200", "12.00000000"],
    ["5.10000000", "28.00000000"]
  ]
})";

static constexpr std::string_view kDepthResponseMinified =
    R"({"lastUpdateId":1027024,"bids":[["4.00000000","431.00000000"],["3.99000000","200.00000000"]],"asks":[["4.00000200","12.00000000"]]})";

static constexpr std::string_view kServerTimeResponse =
    R"({"serverTime":1711612345678})";

// ---------------------------------------------------------------------------
// parse_depth_response — happy path
// ---------------------------------------------------------------------------

TEST(BinanceRestDepth, ParseFullResponse) {
    auto result = parse_depth_response(kDepthResponse);
    ASSERT_TRUE(result.has_value()) << result.error();

    EXPECT_EQ(result->last_update_id, 1027024);
    ASSERT_EQ(result->bids.size(), 3u);
    ASSERT_EQ(result->asks.size(), 2u);

    // Bids: descending by price (as returned by Binance)
    EXPECT_DOUBLE_EQ(result->bids[0].price, 4.0);
    EXPECT_DOUBLE_EQ(result->bids[0].qty, 431.0);
    EXPECT_DOUBLE_EQ(result->bids[1].price, 3.99);
    EXPECT_DOUBLE_EQ(result->bids[1].qty, 200.0);
    EXPECT_DOUBLE_EQ(result->bids[2].price, 3.98);
    EXPECT_DOUBLE_EQ(result->bids[2].qty, 100.0);

    // Asks: ascending by price (as returned by Binance)
    EXPECT_NEAR(result->asks[0].price, 4.000002, 1e-9);
    EXPECT_DOUBLE_EQ(result->asks[0].qty, 12.0);
    EXPECT_DOUBLE_EQ(result->asks[1].price, 5.1);
    EXPECT_DOUBLE_EQ(result->asks[1].qty, 28.0);
}

TEST(BinanceRestDepth, ParseMinifiedResponse) {
    auto result = parse_depth_response(kDepthResponseMinified);
    ASSERT_TRUE(result.has_value()) << result.error();

    EXPECT_EQ(result->last_update_id, 1027024);
    ASSERT_EQ(result->bids.size(), 2u);
    ASSERT_EQ(result->asks.size(), 1u);

    EXPECT_DOUBLE_EQ(result->bids[0].price, 4.0);
    EXPECT_DOUBLE_EQ(result->bids[0].qty, 431.0);
    EXPECT_NEAR(result->asks[0].price, 4.000002, 1e-9);
}

TEST(BinanceRestDepth, EmptyBidsAndAsks) {
    auto result = parse_depth_response(
        R"({"lastUpdateId":0,"bids":[],"asks":[]})");
    ASSERT_TRUE(result.has_value()) << result.error();

    EXPECT_EQ(result->last_update_id, 0);
    EXPECT_TRUE(result->bids.empty());
    EXPECT_TRUE(result->asks.empty());
}

TEST(BinanceRestDepth, SingleLevel) {
    auto result = parse_depth_response(
        R"({"lastUpdateId":42,"bids":[["100.5","2.3"]],"asks":[["101.0","1.0"]]})");
    ASSERT_TRUE(result.has_value()) << result.error();

    EXPECT_EQ(result->last_update_id, 42);
    ASSERT_EQ(result->bids.size(), 1u);
    ASSERT_EQ(result->asks.size(), 1u);
    EXPECT_DOUBLE_EQ(result->bids[0].price, 100.5);
    EXPECT_DOUBLE_EQ(result->bids[0].qty, 2.3);
    EXPECT_DOUBLE_EQ(result->asks[0].price, 101.0);
    EXPECT_DOUBLE_EQ(result->asks[0].qty, 1.0);
}

TEST(BinanceRestDepth, LargeUpdateId) {
    auto result = parse_depth_response(
        R"({"lastUpdateId":54876532109,"bids":[["87000.01","0.001"]],"asks":[["87000.02","0.002"]]})");
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->last_update_id, 54876532109LL);
}

// ---------------------------------------------------------------------------
// parse_depth_response — error cases
// ---------------------------------------------------------------------------

TEST(BinanceRestDepth, MalformedJsonReturnsError) {
    auto result = parse_depth_response("not json at all");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("JSON parse failed"), std::string::npos);
}

TEST(BinanceRestDepth, EmptyBodyReturnsError) {
    auto result = parse_depth_response("");
    ASSERT_FALSE(result.has_value());
}

TEST(BinanceRestDepth, MissingLastUpdateIdReturnsError) {
    auto result = parse_depth_response(
        R"({"bids":[["1","2"]],"asks":[["3","4"]]})");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("lastUpdateId"), std::string::npos);
}

TEST(BinanceRestDepth, MissingBidsReturnsError) {
    auto result = parse_depth_response(
        R"({"lastUpdateId":1,"asks":[["3","4"]]})");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("bids"), std::string::npos);
}

TEST(BinanceRestDepth, MissingAsksReturnsError) {
    auto result = parse_depth_response(
        R"({"lastUpdateId":1,"bids":[["1","2"]]})");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("asks"), std::string::npos);
}

TEST(BinanceRestDepth, InvalidPriceInBidsReturnsError) {
    auto result = parse_depth_response(
        R"({"lastUpdateId":1,"bids":[["not_a_number","2"]],"asks":[["3","4"]]})");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("bids"), std::string::npos);
}

TEST(BinanceRestDepth, InvalidQtyInAsksReturnsError) {
    auto result = parse_depth_response(
        R"({"lastUpdateId":1,"bids":[["1","2"]],"asks":[["3","xyz"]]})");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("asks"), std::string::npos);
}

// ---------------------------------------------------------------------------
// parse_server_time_response — happy path
// ---------------------------------------------------------------------------

TEST(BinanceRestServerTime, ParseResponse) {
    auto result = parse_server_time_response(kServerTimeResponse);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->server_time_ms, 1711612345678LL);
}

TEST(BinanceRestServerTime, LargeTimestamp) {
    auto result = parse_server_time_response(R"({"serverTime":9999999999999})");
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->server_time_ms, 9999999999999LL);
}

// ---------------------------------------------------------------------------
// parse_server_time_response — error cases
// ---------------------------------------------------------------------------

TEST(BinanceRestServerTime, MalformedJsonReturnsError) {
    auto result = parse_server_time_response("{broken");
    ASSERT_FALSE(result.has_value());
}

TEST(BinanceRestServerTime, EmptyBodyReturnsError) {
    auto result = parse_server_time_response("");
    ASSERT_FALSE(result.has_value());
}

TEST(BinanceRestServerTime, MissingFieldReturnsError) {
    auto result = parse_server_time_response(R"({"otherField":123})");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("serverTime"), std::string::npos);
}

TEST(BinanceRestServerTime, StringValueReturnsError) {
    // serverTime should be an integer, not a string
    auto result = parse_server_time_response(R"({"serverTime":"not_a_number"})");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("serverTime"), std::string::npos);
}

// ---------------------------------------------------------------------------
// detail::parse_depth_levels — edge cases
// ---------------------------------------------------------------------------

TEST(BinanceRestDepthLevels, EmptyString) {
    auto result = detail::parse_depth_levels("");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
}

TEST(BinanceRestDepthLevels, EmptyArray) {
    auto result = detail::parse_depth_levels("[]");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
}

TEST(BinanceRestDepthLevels, MultipleLevels) {
    auto result = detail::parse_depth_levels(
        R"([["100.5","2.3"],["99.5","5.0"],["98.0","10.0"]])");
    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->size(), 3u);
    EXPECT_DOUBLE_EQ((*result)[0].price, 100.5);
    EXPECT_DOUBLE_EQ((*result)[0].qty, 2.3);
    EXPECT_DOUBLE_EQ((*result)[1].price, 99.5);
    EXPECT_DOUBLE_EQ((*result)[1].qty, 5.0);
    EXPECT_DOUBLE_EQ((*result)[2].price, 98.0);
    EXPECT_DOUBLE_EQ((*result)[2].qty, 10.0);
}

TEST(BinanceRestDepthLevels, NotAnArrayReturnsError) {
    auto result = detail::parse_depth_levels("not_an_array");
    ASSERT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// BinanceRestConfig API tests
// ---------------------------------------------------------------------------

TEST(BinanceRestConfig, ValidateAcceptsDefaults) {
    BinanceRestConfig cfg{};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(BinanceRestConfig, ValidateRejectsEmptyHost) {
    BinanceRestConfig cfg{.host = ""};
    EXPECT_FALSE(cfg.validate().empty());
    EXPECT_NE(cfg.validate().find("host"), std::string_view::npos);
}

TEST(BinanceRestConfig, ValidateRejectsZeroPort) {
    BinanceRestConfig cfg{.port = 0};
    EXPECT_FALSE(cfg.validate().empty());
}

TEST(BinanceRestConfig, ValidateRejectsNonPositiveTimeout) {
    BinanceRestConfig cfg{.timeout = std::chrono::milliseconds{0}};
    EXPECT_FALSE(cfg.validate().empty());
}

TEST(BinanceRestConfig, DumpContainsAllFields) {
    BinanceRestConfig cfg{.host = "test.example.com", .port = 8443,
                           .timeout = std::chrono::milliseconds{3000}};
    auto d = cfg.dump();
    EXPECT_NE(d.find("test.example.com"), std::string::npos);
    EXPECT_NE(d.find("8443"), std::string::npos);
    EXPECT_NE(d.find("3000"), std::string::npos);
}

TEST(BinanceRestConfig, ToJsonIsValidStructure) {
    BinanceRestConfig cfg{};
    auto j = cfg.to_json();
    EXPECT_TRUE(j.starts_with("{"));
    EXPECT_TRUE(j.ends_with("}"));
    EXPECT_NE(j.find("\"host\":\"api.binance.com\""), std::string::npos);
    EXPECT_NE(j.find("\"port\":443"), std::string::npos);
    EXPECT_NE(j.find("\"timeout_ms\":5000"), std::string::npos);
}

TEST(BinanceRestConfig, ToJsonEscapesSpecialChars) {
    BinanceRestConfig cfg{.host = "test\"host"};
    auto j = cfg.to_json();
    EXPECT_NE(j.find("test\\\"host"), std::string::npos);
}

TEST(BinanceRestConfig, EqualityMatchesIdentical) {
    BinanceRestConfig a{}, b{};
    EXPECT_EQ(a, b);
}

TEST(BinanceRestConfig, EqualityDetectsDifferences) {
    BinanceRestConfig a{}, b{};
    b.port = 8080;
    EXPECT_NE(a, b);
}

TEST(BinanceRestConfig, WarningsEmptyForDefaults) {
    BinanceRestConfig cfg{};
    EXPECT_TRUE(cfg.warnings().empty());
}

TEST(BinanceRestConfig, WarningsDetectsNonStandardPort) {
    BinanceRestConfig cfg{.port = 8080};
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w)
        if (msg.find("443") != std::string::npos) found = true;
    EXPECT_TRUE(found);
}

TEST(BinanceRestConfig, WarningsDetectsShortTimeout) {
    BinanceRestConfig cfg{.timeout = std::chrono::milliseconds{500}};
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w)
        if (msg.find("very short") != std::string::npos) found = true;
    EXPECT_TRUE(found);
}

TEST(BinanceRestConfig, WarningsDetectsUnknownHost) {
    BinanceRestConfig cfg{.host = "custom.host.com"};
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w)
        if (msg.find("not a recognized") != std::string::npos) found = true;
    EXPECT_TRUE(found);
}

TEST(BinanceRestConfig, WarningsAcceptsTestnet) {
    BinanceRestConfig cfg{.host = "testnet.binance.vision"};
    auto w = cfg.warnings();
    // Should not warn about testnet host
    bool found_host_warn = false;
    for (const auto& msg : w)
        if (msg.find("not a recognized") != std::string::npos) found_host_warn = true;
    EXPECT_FALSE(found_host_warn);
}

TEST(BinanceRestConfig, FormatterProducesDump) {
    BinanceRestConfig cfg{};
    auto formatted = std::format("{}", cfg);
    EXPECT_NE(formatted.find("BinanceRestConfig"), std::string::npos);
    EXPECT_NE(formatted.find("api.binance.com"), std::string::npos);
}
