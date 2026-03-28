/// @file test_bybit.cpp
/// Unit tests for Bybit JSON adapters (BybitBookTicker, BybitPushMessage,
/// symbol_hash, WebSocket subscription helpers).

#include <array>
#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

#include "eph/json/adapters/bybit.hpp"
#include "eph/json/parser.hpp"

using namespace eph::json;
using namespace eph::json::bybit;

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------

static std::expected<JsonView, ParseError> parse_str(std::string_view sv) {
    return parse(reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
}

// ---------------------------------------------------------------------------
// Full Bybit bookTicker payload used across tests
// ---------------------------------------------------------------------------

static constexpr std::string_view kBybitBookTickerJson =
    R"({"topic":"tickers.BTCUSDT","type":"snapshot","data":{"symbol":"BTCUSDT","bid1Price":"87000.0","bid1Size":"1.2","ask1Price":"87001.0","ask1Size":"0.5","lastPrice":"87000.5","ts":"1711612345678"},"ts":1711612345679})";

// ---------------------------------------------------------------------------
// BybitBookTicker::from
// ---------------------------------------------------------------------------

TEST(BybitBookTicker, HappyPath) {
    auto json = parse_str(kBybitBookTickerJson);
    ASSERT_TRUE(json.has_value());

    auto ticker = BybitBookTicker::from(*json);
    ASSERT_TRUE(ticker.has_value());

    EXPECT_EQ(ticker->symbol, "BTCUSDT");
    EXPECT_EQ(ticker->bid_price, "87000.0");
    EXPECT_EQ(ticker->bid_qty, "1.2");
    EXPECT_EQ(ticker->ask_price, "87001.0");
    EXPECT_EQ(ticker->ask_qty, "0.5");
    EXPECT_EQ(ticker->last_price, "87000.5");
    EXPECT_EQ(ticker->timestamp_ms, 1711612345678);
}

TEST(BybitBookTicker, MidPrice) {
    auto json = parse_str(kBybitBookTickerJson);
    auto ticker = BybitBookTicker::from(*json);
    ASSERT_TRUE(ticker.has_value());

    auto mid = ticker->mid_price();
    ASSERT_TRUE(mid.has_value());
    EXPECT_NEAR(*mid, 87000.5, 0.01);
}

TEST(BybitBookTicker, Spread) {
    auto json = parse_str(kBybitBookTickerJson);
    auto ticker = BybitBookTicker::from(*json);
    ASSERT_TRUE(ticker.has_value());

    auto sp = ticker->spread();
    ASSERT_TRUE(sp.has_value());
    EXPECT_NEAR(*sp, 1.0, 0.01);
}

TEST(BybitBookTicker, MissingDataFieldReturnsNullopt) {
    auto json = parse_str(R"({"topic":"tickers.BTCUSDT","type":"snapshot","ts":123})");
    ASSERT_TRUE(json.has_value());
    EXPECT_FALSE(BybitBookTicker::from(*json).has_value());
}

TEST(BybitBookTicker, MissingBid1PriceReturnsNullopt) {
    auto json = parse_str(
        R"({"topic":"tickers.BTCUSDT","type":"snapshot","data":{"symbol":"BTCUSDT","bid1Size":"1.2","ask1Price":"87001.0","ask1Size":"0.5","ts":"123"},"ts":123})");
    ASSERT_TRUE(json.has_value());
    EXPECT_FALSE(BybitBookTicker::from(*json).has_value());
}

TEST(BybitBookTicker, MissingSymbolReturnsNullopt) {
    auto json = parse_str(
        R"({"topic":"tickers.BTCUSDT","type":"snapshot","data":{"bid1Price":"100","bid1Size":"1","ask1Price":"101","ask1Size":"2","ts":"123"},"ts":123})");
    ASSERT_TRUE(json.has_value());
    EXPECT_FALSE(BybitBookTicker::from(*json).has_value());
}

TEST(BybitBookTicker, MissingTimestampStillParses) {
    // ts is optional — ticker should parse with timestamp_ms = 0
    auto json = parse_str(
        R"({"topic":"tickers.BTCUSDT","type":"snapshot","data":{"symbol":"BTCUSDT","bid1Price":"100","bid1Size":"1","ask1Price":"101","ask1Size":"2"},"ts":123})");
    ASSERT_TRUE(json.has_value());

    auto ticker = BybitBookTicker::from(*json);
    ASSERT_TRUE(ticker.has_value());
    EXPECT_EQ(ticker->timestamp_ms, 0);
    EXPECT_EQ(ticker->bid_price, "100");
}

TEST(BybitBookTicker, MissingLastPriceStillParses) {
    // lastPrice is optional (e.g., delta updates may omit it)
    auto json = parse_str(
        R"({"topic":"tickers.BTCUSDT","type":"delta","data":{"symbol":"BTCUSDT","bid1Price":"100","bid1Size":"1","ask1Price":"101","ask1Size":"2","ts":"999"},"ts":999})");
    ASSERT_TRUE(json.has_value());

    auto ticker = BybitBookTicker::from(*json);
    ASSERT_TRUE(ticker.has_value());
    EXPECT_TRUE(ticker->last_price.empty());
    EXPECT_EQ(ticker->timestamp_ms, 999);
}

// ---------------------------------------------------------------------------
// BybitPushMessage::from
// ---------------------------------------------------------------------------

TEST(BybitPushMessage, HappyPath) {
    auto json = parse_str(kBybitBookTickerJson);
    ASSERT_TRUE(json.has_value());

    auto msg = BybitPushMessage::from(*json);
    ASSERT_TRUE(msg.has_value());

    EXPECT_EQ(msg->topic, "tickers.BTCUSDT");
    EXPECT_EQ(msg->type, "snapshot");
    EXPECT_TRUE(msg->data_raw.starts_with("{"));
    EXPECT_TRUE(msg->data_raw.ends_with("}"));
}

TEST(BybitPushMessage, DeltaType) {
    auto json = parse_str(
        R"({"topic":"tickers.ETHUSDT","type":"delta","data":{"symbol":"ETHUSDT","bid1Price":"3500.0","bid1Size":"10"},"ts":999})");
    ASSERT_TRUE(json.has_value());

    auto msg = BybitPushMessage::from(*json);
    ASSERT_TRUE(msg.has_value());

    EXPECT_EQ(msg->topic, "tickers.ETHUSDT");
    EXPECT_EQ(msg->type, "delta");
}

TEST(BybitPushMessage, OrderbookTopic) {
    auto json = parse_str(
        R"({"topic":"orderbook.1.BTCUSDT","type":"snapshot","data":{"s":"BTCUSDT","b":[["87000","1.2"]],"a":[["87001","0.5"]]},"ts":123})");
    ASSERT_TRUE(json.has_value());

    auto msg = BybitPushMessage::from(*json);
    ASSERT_TRUE(msg.has_value());

    EXPECT_EQ(msg->topic, "orderbook.1.BTCUSDT");
    EXPECT_EQ(msg->type, "snapshot");
}

TEST(BybitPushMessage, MissingTopicReturnsNullopt) {
    auto json = parse_str(R"({"type":"snapshot","data":{"symbol":"BTCUSDT"},"ts":123})");
    ASSERT_TRUE(json.has_value());
    EXPECT_FALSE(BybitPushMessage::from(*json).has_value());
}

TEST(BybitPushMessage, MissingTypeReturnsNullopt) {
    auto json = parse_str(R"({"topic":"tickers.BTCUSDT","data":{"symbol":"BTCUSDT"},"ts":123})");
    ASSERT_TRUE(json.has_value());
    EXPECT_FALSE(BybitPushMessage::from(*json).has_value());
}

TEST(BybitPushMessage, MissingDataReturnsNullopt) {
    auto json = parse_str(R"({"topic":"tickers.BTCUSDT","type":"snapshot","ts":123})");
    ASSERT_TRUE(json.has_value());
    EXPECT_FALSE(BybitPushMessage::from(*json).has_value());
}

// ---------------------------------------------------------------------------
// symbol_hash
// ---------------------------------------------------------------------------

static uint32_t hash_str(std::string_view sv) {
    return symbol_hash(reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
}

TEST(BybitSymbolHash, BookTickerPayload) {
    auto h = hash_str(kBybitBookTickerJson);
    EXPECT_NE(h, 0u);
}

TEST(BybitSymbolHash, DifferentSymbolsDifferentHash) {
    auto h1 = hash_str(
        R"({"topic":"tickers.BTCUSDT","type":"snapshot","data":{"symbol":"BTCUSDT","bid1Price":"1"},"ts":1})");
    auto h2 = hash_str(
        R"({"topic":"tickers.ETHUSDT","type":"snapshot","data":{"symbol":"ETHUSDT","bid1Price":"1"},"ts":1})");
    EXPECT_NE(h1, h2);
}

TEST(BybitSymbolHash, SameSymbolSameHash) {
    auto h1 = hash_str(
        R"({"topic":"tickers.BTCUSDT","type":"snapshot","data":{"symbol":"BTCUSDT","bid1Price":"1"},"ts":1})");
    auto h2 = hash_str(
        R"({"topic":"tickers.BTCUSDT","type":"delta","data":{"symbol":"BTCUSDT","ask1Price":"2"},"ts":2})");
    EXPECT_EQ(h1, h2);
}

TEST(BybitSymbolHash, NoSymbolReturnsZero) {
    auto h = hash_str(R"({"topic":"tickers.BTCUSDT","type":"snapshot","data":{"bid1Price":"1"},"ts":1})");
    EXPECT_EQ(h, 0u);
}

TEST(BybitSymbolHash, EmptyPayloadReturnsZero) {
    EXPECT_EQ(symbol_hash(nullptr, 0), 0u);
}

// ---------------------------------------------------------------------------
// subscribe_message
// ---------------------------------------------------------------------------

TEST(BybitSubscribe, SingleSymbol) {
    std::array<std::string_view, 1> syms = {"BTCUSDT"};
    auto msg = subscribe_message("tickers", syms, 1);

    // Parse back to verify it's valid JSON
    auto json = parse_str(msg);
    ASSERT_TRUE(json.has_value()) << "subscribe_message produced invalid JSON: " << msg;

    auto op = json->get_string("op");
    ASSERT_TRUE(op.has_value());
    EXPECT_EQ(*op, "subscribe");

    auto id = json->get_string("req_id");
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, "1");

    // Verify args contain the channel.symbol pair
    EXPECT_NE(msg.find("tickers.BTCUSDT"), std::string::npos);
}

TEST(BybitSubscribe, MultipleSymbols) {
    std::array<std::string_view, 2> syms = {"BTCUSDT", "ETHUSDT"};
    auto msg = subscribe_message("tickers", syms, 42);

    auto json = parse_str(msg);
    ASSERT_TRUE(json.has_value()) << "subscribe_message produced invalid JSON: " << msg;

    auto id = json->get_string("req_id");
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, "42");

    EXPECT_NE(msg.find("tickers.BTCUSDT"), std::string::npos);
    EXPECT_NE(msg.find("tickers.ETHUSDT"), std::string::npos);
}

TEST(BybitSubscribe, EmptySymbolsList) {
    std::span<const std::string_view> empty;
    auto msg = subscribe_message("tickers", empty, 1);

    auto json = parse_str(msg);
    ASSERT_TRUE(json.has_value()) << "subscribe_message with empty syms produced invalid JSON: " << msg;

    auto op = json->get_string("op");
    ASSERT_TRUE(op.has_value());
    EXPECT_EQ(*op, "subscribe");
}

TEST(BybitSubscribe, OrderbookChannel) {
    std::array<std::string_view, 1> syms = {"SOLUSDT"};
    auto msg = subscribe_message("orderbook.1", syms, 7);

    auto json = parse_str(msg);
    ASSERT_TRUE(json.has_value());

    EXPECT_EQ(*json->get_string("op"), "subscribe");
    EXPECT_EQ(*json->get_string("req_id"), "7");

    // Should contain "orderbook.1.SOLUSDT"
    EXPECT_NE(msg.find("orderbook.1.SOLUSDT"), std::string::npos);
}
