/// @file test_binance.cpp
/// Unit tests for Binance JSON adapters (BookTicker, CombinedStream, symbol_hash).

#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

#include "eph/json/adapters/binance.hpp"
#include "eph/json/parser.hpp"

using namespace eph::json;
using namespace eph::json::binance;

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------

static std::expected<JsonView, ParseError> parse_str(std::string_view sv) {
    return parse(reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
}

// ---------------------------------------------------------------------------
// extract_symbol
// ---------------------------------------------------------------------------

TEST(BinanceSymbol, ExtractFromBookTicker) {
    EXPECT_EQ(extract_symbol("btcusdt@bookTicker"), "btcusdt");
}

TEST(BinanceSymbol, ExtractFromTrade) {
    EXPECT_EQ(extract_symbol("ethusdt@trade"), "ethusdt");
}

TEST(BinanceSymbol, NoAtSign) {
    EXPECT_EQ(extract_symbol("btcusdt"), "btcusdt");
}

TEST(BinanceSymbol, EmptyString) {
    EXPECT_EQ(extract_symbol(""), "");
}

TEST(BinanceSymbol, AtSignOnly) {
    EXPECT_EQ(extract_symbol("@bookTicker"), "");
}

// ---------------------------------------------------------------------------
// symbol_hash
// ---------------------------------------------------------------------------

static uint32_t hash_str(std::string_view sv) {
    return symbol_hash(reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
}

TEST(BinanceSymbolHash, BookTickerPayload) {
    auto h = hash_str(R"({"e":"bookTicker","u":123,"s":"BTCUSDT","b":"87000","B":"1","a":"87001","A":"2","T":999,"E":1000})");
    EXPECT_NE(h, 0u);
}

TEST(BinanceSymbolHash, DifferentSymbolsDifferentHash) {
    auto h1 = hash_str(R"({"s":"BTCUSDT","b":"1"})");
    auto h2 = hash_str(R"({"s":"ETHUSDT","b":"1"})");
    EXPECT_NE(h1, h2);
}

TEST(BinanceSymbolHash, SameSymbolSameHash) {
    auto h1 = hash_str(R"({"s":"BTCUSDT","b":"1"})");
    auto h2 = hash_str(R"({"s":"BTCUSDT","a":"2"})");
    EXPECT_EQ(h1, h2);
}

TEST(BinanceSymbolHash, NoSymbolReturnsZero) {
    auto h = hash_str(R"({"e":"bookTicker","b":"1"})");
    EXPECT_EQ(h, 0u);
}

TEST(BinanceSymbolHash, EmptyPayloadReturnsZero) {
    EXPECT_EQ(symbol_hash(nullptr, 0), 0u);
}

// ---------------------------------------------------------------------------
// BookTicker::from
// ---------------------------------------------------------------------------

static constexpr std::string_view kBookTickerJson =
    R"({"e":"bookTicker","u":4736462646,"s":"BTCUSDT","b":"87245.30","B":"0.500","a":"87245.40","A":"1.200","T":1711612345678,"E":1711612345679})";

TEST(BinanceBookTicker, HappyPath) {
    auto json = parse_str(kBookTickerJson);
    ASSERT_TRUE(json.has_value());

    auto ticker = BookTicker::from(*json);
    ASSERT_TRUE(ticker.has_value());

    EXPECT_EQ(ticker->symbol, "BTCUSDT");
    EXPECT_EQ(ticker->bid_price, "87245.30");
    EXPECT_EQ(ticker->bid_qty, "0.500");
    EXPECT_EQ(ticker->ask_price, "87245.40");
    EXPECT_EQ(ticker->ask_qty, "1.200");
    EXPECT_EQ(ticker->update_id, 4736462646);
    EXPECT_EQ(ticker->event_time, 1711612345679);
    EXPECT_EQ(ticker->txn_time, 1711612345678);
}

TEST(BinanceBookTicker, MidPrice) {
    auto json = parse_str(kBookTickerJson);
    auto ticker = BookTicker::from(*json);
    ASSERT_TRUE(ticker.has_value());

    auto mid = ticker->mid_price();
    ASSERT_TRUE(mid.has_value());
    EXPECT_NEAR(*mid, 87245.35, 0.01);
}

TEST(BinanceBookTicker, Spread) {
    auto json = parse_str(kBookTickerJson);
    auto ticker = BookTicker::from(*json);
    ASSERT_TRUE(ticker.has_value());

    auto sp = ticker->spread();
    ASSERT_TRUE(sp.has_value());
    EXPECT_NEAR(*sp, 0.10, 0.01);
}

TEST(BinanceBookTicker, MissingSymbolReturnsNullopt) {
    auto json = parse_str(R"({"b":"1","B":"1","a":"1","A":"1"})");
    ASSERT_TRUE(json.has_value());
    EXPECT_FALSE(BookTicker::from(*json).has_value());
}

TEST(BinanceBookTicker, MissingBidReturnsNullopt) {
    auto json = parse_str(R"({"s":"BTC","B":"1","a":"1","A":"1"})");
    ASSERT_TRUE(json.has_value());
    EXPECT_FALSE(BookTicker::from(*json).has_value());
}

TEST(BinanceBookTicker, OptionalFieldsMissing) {
    // Only required fields: s, b, B, a, A
    auto json = parse_str(R"({"s":"BTCUSDT","b":"100","B":"1","a":"101","A":"2"})");
    ASSERT_TRUE(json.has_value());

    auto ticker = BookTicker::from(*json);
    ASSERT_TRUE(ticker.has_value());
    EXPECT_EQ(ticker->update_id, 0);
    EXPECT_EQ(ticker->event_time, 0);
    EXPECT_EQ(ticker->txn_time, 0);
}

// ---------------------------------------------------------------------------
// CombinedStream::from
// ---------------------------------------------------------------------------

static constexpr std::string_view kCombinedJson =
    R"({"stream":"btcusdt@bookTicker","data":{"e":"bookTicker","u":123,"s":"BTCUSDT","b":"87000","B":"1","a":"87001","A":"2","T":999,"E":1000}})";

TEST(BinanceCombinedStream, HappyPath) {
    auto json = parse_str(kCombinedJson);
    ASSERT_TRUE(json.has_value());

    auto cs = CombinedStream::from(*json);
    ASSERT_TRUE(cs.has_value());

    EXPECT_EQ(cs->stream, "btcusdt@bookTicker");
    EXPECT_EQ(cs->symbol, "btcusdt");
    EXPECT_TRUE(cs->data_raw.starts_with("{"));
    EXPECT_TRUE(cs->data_raw.ends_with("}"));
}

TEST(BinanceCombinedStream, ParseInnerData) {
    auto json = parse_str(kCombinedJson);
    auto cs = CombinedStream::from(*json);
    ASSERT_TRUE(cs.has_value());

    // Parse the inner data object
    auto inner = parse(
        reinterpret_cast<const uint8_t*>(cs->data_raw.data()),
        cs->data_raw.size());
    ASSERT_TRUE(inner.has_value());

    auto ticker = BookTicker::from(*inner);
    ASSERT_TRUE(ticker.has_value());
    EXPECT_EQ(ticker->symbol, "BTCUSDT");
    EXPECT_EQ(ticker->bid_price, "87000");
}

TEST(BinanceCombinedStream, MissingStreamReturnsNullopt) {
    auto json = parse_str(R"({"data":{"s":"BTC"}})");
    ASSERT_TRUE(json.has_value());
    EXPECT_FALSE(CombinedStream::from(*json).has_value());
}

TEST(BinanceCombinedStream, MissingDataReturnsNullopt) {
    auto json = parse_str(R"({"stream":"btcusdt@bookTicker"})");
    ASSERT_TRUE(json.has_value());
    EXPECT_FALSE(CombinedStream::from(*json).has_value());
}
