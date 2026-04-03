/// @file test_okx.cpp
/// Unit tests for OKX JSON adapters (OkxBookTicker, OkxPushMessage,
/// inst_id_hash, WebSocket subscription helpers).

#include <array>
#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

#include "eph/json/adapters/okx.hpp"
#include "eph/json/parser.hpp"

using namespace eph::json;
using namespace eph::json::okx;

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------

static std::expected<JsonView, ParseError> parse_str(std::string_view sv) {
    return parse(reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
}

// ---------------------------------------------------------------------------
// Full OKX bookTicker payload used across tests
// ---------------------------------------------------------------------------

static constexpr std::string_view kOkxBookTickerJson =
    R"({"arg":{"channel":"bbo-tbt","instId":"BTC-USDT"},"data":[{"askPx":"87001.0","askSz":"0.5","bidPx":"87000.0","bidSz":"1.2","ts":"1711612345678","instId":"BTC-USDT"}]})";

// ---------------------------------------------------------------------------
// OkxBookTicker::from
// ---------------------------------------------------------------------------

TEST(OkxBookTicker, HappyPath) {
    auto json = parse_str(kOkxBookTickerJson);
    ASSERT_TRUE(json.has_value());

    auto ticker = OkxBookTicker::from(*json);
    ASSERT_TRUE(ticker.has_value());

    EXPECT_EQ(ticker->inst_id, "BTC-USDT");
    EXPECT_EQ(ticker->bid_price, "87000.0");
    EXPECT_EQ(ticker->bid_qty, "1.2");
    EXPECT_EQ(ticker->ask_price, "87001.0");
    EXPECT_EQ(ticker->ask_qty, "0.5");
    EXPECT_EQ(ticker->timestamp_ms, 1711612345678);
}

TEST(OkxBookTicker, MidPrice) {
    auto json = parse_str(kOkxBookTickerJson);
    auto ticker = OkxBookTicker::from(*json);
    ASSERT_TRUE(ticker.has_value());

    auto mid = ticker->mid_price();
    ASSERT_TRUE(mid.has_value());
    EXPECT_NEAR(*mid, 87000.5, 0.01);
}

TEST(OkxBookTicker, Spread) {
    auto json = parse_str(kOkxBookTickerJson);
    auto ticker = OkxBookTicker::from(*json);
    ASSERT_TRUE(ticker.has_value());

    auto sp = ticker->spread();
    ASSERT_TRUE(sp.has_value());
    EXPECT_NEAR(*sp, 1.0, 0.01);
}

TEST(OkxBookTicker, MissingDataFieldReturnsNullopt) {
    // No "data" field at all
    auto json = parse_str(R"({"arg":{"channel":"bbo-tbt","instId":"BTC-USDT"}})");
    ASSERT_TRUE(json.has_value());
    EXPECT_FALSE(OkxBookTicker::from(*json).has_value());
}

TEST(OkxBookTicker, EmptyDataArrayReturnsNullopt) {
    auto json = parse_str(R"({"arg":{"channel":"bbo-tbt","instId":"BTC-USDT"},"data":[]})");
    ASSERT_TRUE(json.has_value());
    EXPECT_FALSE(OkxBookTicker::from(*json).has_value());
}

TEST(OkxBookTicker, MissingBidPxReturnsNullopt) {
    auto json = parse_str(
        R"({"arg":{"channel":"bbo-tbt","instId":"BTC-USDT"},"data":[{"bidSz":"1.2","askPx":"87001.0","askSz":"0.5","ts":"123","instId":"BTC-USDT"}]})");
    ASSERT_TRUE(json.has_value());
    EXPECT_FALSE(OkxBookTicker::from(*json).has_value());
}

TEST(OkxBookTicker, MissingTimestampStillParses) {
    // ts is optional — ticker should parse with timestamp_ms = 0
    auto json = parse_str(
        R"({"arg":{"channel":"bbo-tbt","instId":"BTC-USDT"},"data":[{"bidPx":"100","bidSz":"1","askPx":"101","askSz":"2","instId":"BTC-USDT"}]})");
    ASSERT_TRUE(json.has_value());

    auto ticker = OkxBookTicker::from(*json);
    ASSERT_TRUE(ticker.has_value());
    EXPECT_EQ(ticker->timestamp_ms, 0);
    EXPECT_EQ(ticker->bid_price, "100");
}

// ---------------------------------------------------------------------------
// OkxPushMessage::from
// ---------------------------------------------------------------------------

TEST(OkxPushMessage, HappyPath) {
    auto json = parse_str(kOkxBookTickerJson);
    ASSERT_TRUE(json.has_value());

    auto msg = OkxPushMessage::from(*json);
    ASSERT_TRUE(msg.has_value());

    EXPECT_EQ(msg->channel, "bbo-tbt");
    EXPECT_EQ(msg->inst_id, "BTC-USDT");
    EXPECT_TRUE(msg->data_raw.starts_with("["));
    EXPECT_TRUE(msg->data_raw.ends_with("]"));
}

TEST(OkxPushMessage, TradesChannel) {
    auto json = parse_str(
        R"({"arg":{"channel":"trades","instId":"ETH-USDT"},"data":[{"px":"3500.0","sz":"0.1","ts":"999"}]})");
    ASSERT_TRUE(json.has_value());

    auto msg = OkxPushMessage::from(*json);
    ASSERT_TRUE(msg.has_value());

    EXPECT_EQ(msg->channel, "trades");
    EXPECT_EQ(msg->inst_id, "ETH-USDT");
}

TEST(OkxPushMessage, MissingArgReturnsNullopt) {
    auto json = parse_str(R"({"data":[{"px":"100"}]})");
    ASSERT_TRUE(json.has_value());
    EXPECT_FALSE(OkxPushMessage::from(*json).has_value());
}

TEST(OkxPushMessage, MissingDataReturnsNullopt) {
    auto json = parse_str(R"({"arg":{"channel":"bbo-tbt","instId":"BTC-USDT"}})");
    ASSERT_TRUE(json.has_value());
    EXPECT_FALSE(OkxPushMessage::from(*json).has_value());
}

TEST(OkxPushMessage, MissingChannelInArgReturnsNullopt) {
    auto json = parse_str(
        R"({"arg":{"instId":"BTC-USDT"},"data":[{"px":"100"}]})");
    ASSERT_TRUE(json.has_value());
    EXPECT_FALSE(OkxPushMessage::from(*json).has_value());
}

TEST(OkxPushMessage, MissingInstIdInArgReturnsNullopt) {
    auto json = parse_str(
        R"({"arg":{"channel":"bbo-tbt"},"data":[{"px":"100"}]})");
    ASSERT_TRUE(json.has_value());
    EXPECT_FALSE(OkxPushMessage::from(*json).has_value());
}

// ---------------------------------------------------------------------------
// inst_id_hash
// ---------------------------------------------------------------------------

static uint32_t hash_str(std::string_view sv) {
    return inst_id_hash(reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
}

TEST(OkxInstIdHash, BookTickerPayload) {
    auto h = hash_str(kOkxBookTickerJson);
    EXPECT_NE(h, 0u);
}

TEST(OkxInstIdHash, DifferentInstrumentsDifferentHash) {
    auto h1 = hash_str(
        R"({"arg":{"channel":"bbo-tbt","instId":"BTC-USDT"},"data":[{"instId":"BTC-USDT","bidPx":"1"}]})");
    auto h2 = hash_str(
        R"({"arg":{"channel":"bbo-tbt","instId":"ETH-USDT"},"data":[{"instId":"ETH-USDT","bidPx":"1"}]})");
    EXPECT_NE(h1, h2);
}

TEST(OkxInstIdHash, SameInstrumentSameHash) {
    auto h1 = hash_str(
        R"({"arg":{"channel":"bbo-tbt","instId":"BTC-USDT"},"data":[{"instId":"BTC-USDT","bidPx":"1"}]})");
    auto h2 = hash_str(
        R"({"arg":{"channel":"bbo-tbt","instId":"BTC-USDT"},"data":[{"instId":"BTC-USDT","askPx":"2"}]})");
    EXPECT_EQ(h1, h2);
}

TEST(OkxInstIdHash, NoInstIdReturnsZero) {
    auto h = hash_str(R"({"arg":{"channel":"bbo-tbt"},"data":[{"bidPx":"1"}]})");
    EXPECT_EQ(h, 0u);
}

TEST(OkxInstIdHash, EmptyPayloadReturnsZero) {
    EXPECT_EQ(inst_id_hash(nullptr, 0), 0u);
}

// ---------------------------------------------------------------------------
// subscribe_message
// ---------------------------------------------------------------------------

TEST(OkxSubscribe, SingleInstrument) {
    std::array<std::string_view, 1> insts = {"BTC-USDT"};
    auto msg = subscribe_message("bbo-tbt", insts, 1);

    // Parse back to verify it's valid JSON
    auto json = parse_str(msg);
    ASSERT_TRUE(json.has_value()) << "subscribe_message produced invalid JSON: " << msg;

    auto op = json->get_string("op");
    ASSERT_TRUE(op.has_value());
    EXPECT_EQ(*op, "subscribe");

    auto id = json->get_string("id");
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, "1");

    // Verify args contain the channel and instId
    EXPECT_NE(msg.find("bbo-tbt"), std::string::npos);
    EXPECT_NE(msg.find("BTC-USDT"), std::string::npos);
}

TEST(OkxSubscribe, MultipleInstruments) {
    std::array<std::string_view, 2> insts = {"BTC-USDT", "ETH-USDT"};
    auto msg = subscribe_message("bbo-tbt", insts, 42);

    auto json = parse_str(msg);
    ASSERT_TRUE(json.has_value()) << "subscribe_message produced invalid JSON: " << msg;

    auto id = json->get_string("id");
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, "42");

    EXPECT_NE(msg.find("BTC-USDT"), std::string::npos);
    EXPECT_NE(msg.find("ETH-USDT"), std::string::npos);
}

TEST(OkxSubscribe, EmptyInstrumentsList) {
    std::span<const std::string_view> empty;
    auto msg = subscribe_message("bbo-tbt", empty, 1);

    auto json = parse_str(msg);
    ASSERT_TRUE(json.has_value()) << "subscribe_message with empty insts produced invalid JSON: " << msg;

    auto op = json->get_string("op");
    ASSERT_TRUE(op.has_value());
    EXPECT_EQ(*op, "subscribe");
}

TEST(OkxSubscribe, RoundTrip) {
    // Build a subscribe message and verify all fields can be parsed back
    std::array<std::string_view, 1> insts = {"SOL-USDT"};
    auto msg = subscribe_message("trades", insts, 7);

    auto json = parse_str(msg);
    ASSERT_TRUE(json.has_value());

    EXPECT_EQ(*json->get_string("op"), "subscribe");
    EXPECT_EQ(*json->get_string("id"), "7");

    // The args array contains our channel+instId pair
    auto args_raw = json->get("args");
    EXPECT_FALSE(args_raw.empty());

    // Extract first element from args array and verify fields
    auto elem = okx::detail::first_array_element(args_raw);
    EXPECT_FALSE(elem.empty());

    auto inner = parse(
        reinterpret_cast<const uint8_t*>(elem.data()), elem.size());
    ASSERT_TRUE(inner.has_value());
    EXPECT_EQ(*inner->get_string("channel"), "trades");
    EXPECT_EQ(*inner->get_string("instId"), "SOL-USDT");
}
