/// @file test_binance.cpp
/// Unit tests for Binance JSON adapters (BookTicker, CombinedStream, symbol_hash,
/// WebSocket subscription helpers).

#include <array>
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
// BookTicker::parse — fused single-pass parser (contract: byte-for-byte
// identical output to BookTicker::from(parse(data,len))).
// ---------------------------------------------------------------------------

TEST(BinanceBookTickerParse, MatchesFromFlow) {
    // Golden contract: fast path must produce identical BookTicker as the
    // generic parse() + from(view) flow on a representative payload.
    auto data = reinterpret_cast<const uint8_t*>(kBookTickerJson.data());
    auto len  = kBookTickerJson.size();

    auto via_view = BookTicker::from(parse(data, len).value());
    auto via_fast = BookTicker::parse(data, len);

    ASSERT_TRUE(via_view.has_value());
    ASSERT_TRUE(via_fast.has_value());
    EXPECT_EQ(via_fast->symbol,     via_view->symbol);
    EXPECT_EQ(via_fast->bid_price,  via_view->bid_price);
    EXPECT_EQ(via_fast->bid_qty,    via_view->bid_qty);
    EXPECT_EQ(via_fast->ask_price,  via_view->ask_price);
    EXPECT_EQ(via_fast->ask_qty,    via_view->ask_qty);
    EXPECT_EQ(via_fast->update_id,  via_view->update_id);
    EXPECT_EQ(via_fast->event_time, via_view->event_time);
    EXPECT_EQ(via_fast->txn_time,   via_view->txn_time);
}

TEST(BinanceBookTickerParse, OptionalFieldsMissing) {
    constexpr std::string_view only_required =
        R"({"s":"BTCUSDT","b":"100","B":"1","a":"101","A":"2"})";
    auto ticker = BookTicker::parse(
        reinterpret_cast<const uint8_t*>(only_required.data()),
        only_required.size());
    ASSERT_TRUE(ticker.has_value());
    EXPECT_EQ(ticker->symbol, "BTCUSDT");
    EXPECT_EQ(ticker->update_id, 0);
    EXPECT_EQ(ticker->event_time, 0);
    EXPECT_EQ(ticker->txn_time, 0);
}

TEST(BinanceBookTickerParse, MissingSymbolReturnsNullopt) {
    constexpr std::string_view missing_s =
        R"({"b":"1","B":"1","a":"1","A":"1"})";
    EXPECT_FALSE(BookTicker::parse(
        reinterpret_cast<const uint8_t*>(missing_s.data()),
        missing_s.size()).has_value());
}

TEST(BinanceBookTickerParse, MissingBidReturnsNullopt) {
    constexpr std::string_view missing_b =
        R"({"s":"BTC","B":"1","a":"1","A":"1"})";
    EXPECT_FALSE(BookTicker::parse(
        reinterpret_cast<const uint8_t*>(missing_b.data()),
        missing_b.size()).has_value());
}

TEST(BinanceBookTickerParse, UnknownFutureFieldIsSkipped) {
    // Contract: if Binance adds a new 1-char field (e.g. "c" for close price),
    // parse() must ignore it rather than fail. Future-proofs against API
    // additions; known missing fields are still caught by the required check.
    constexpr std::string_view with_future_c =
        R"({"s":"BTC","b":"1","B":"1","a":"2","A":"2","c":"closevalue","T":42})";
    auto ticker = BookTicker::parse(
        reinterpret_cast<const uint8_t*>(with_future_c.data()),
        with_future_c.size());
    ASSERT_TRUE(ticker.has_value());
    EXPECT_EQ(ticker->txn_time, 42);
}

TEST(BinanceBookTickerParse, MalformedJsonReturnsNullopt) {
    // Various malformed inputs must cleanly return nullopt, never crash or
    // read past the buffer end.
    auto test_case = [](std::string_view sv) {
        return BookTicker::parse(
            reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
    };
    EXPECT_FALSE(test_case(R"()").has_value());             // empty
    EXPECT_FALSE(test_case(R"({)").has_value());            // unterminated
    EXPECT_FALSE(test_case(R"({"s":)").has_value());        // no value
    EXPECT_FALSE(test_case(R"({"s":"BTC)").has_value());    // unterminated string
    EXPECT_FALSE(test_case(R"(not json)").has_value());     // no {
    EXPECT_FALSE(test_case(R"({"s":42,"b":"1","B":"1","a":"1","A":"1"})").has_value());
    //                              ^-- s as number, required string
}

TEST(BinanceBookTickerParse, FieldReorderingPreservesValues) {
    // Contract: field order must not affect output. Binance documented order
    // is {e,u,s,b,B,a,A,T,E}; verify we handle any permutation.
    constexpr std::string_view reordered =
        R"({"T":1,"A":"2.0","a":"1.5","B":"0.5","b":"1.0","s":"ETH","E":2,"u":99})";
    auto ticker = BookTicker::parse(
        reinterpret_cast<const uint8_t*>(reordered.data()),
        reordered.size());
    ASSERT_TRUE(ticker.has_value());
    EXPECT_EQ(ticker->symbol, "ETH");
    EXPECT_EQ(ticker->bid_price, "1.0");
    EXPECT_EQ(ticker->ask_price, "1.5");
    EXPECT_EQ(ticker->update_id, 99);
    EXPECT_EQ(ticker->event_time, 2);
    EXPECT_EQ(ticker->txn_time, 1);
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

// ---------------------------------------------------------------------------
// ws_path
// ---------------------------------------------------------------------------

TEST(BinanceWsPath, SingleStream) {
    EXPECT_EQ(ws_path("btcusdt", "bookTicker"), "/ws/btcusdt@bookTicker");
}

TEST(BinanceWsPath, TradeStream) {
    EXPECT_EQ(ws_path("ethusdt", "trade"), "/ws/ethusdt@trade");
}

TEST(BinanceWsPath, DepthStream) {
    EXPECT_EQ(ws_path("solusdt", "depth@100ms"), "/ws/solusdt@depth@100ms");
}

// ---------------------------------------------------------------------------
// combined_ws_path
// ---------------------------------------------------------------------------

TEST(BinanceCombinedWsPath, SingleSymbol) {
    std::array<std::string_view, 1> syms = {"btcusdt"};
    EXPECT_EQ(combined_ws_path(syms, "bookTicker"),
              "/stream?streams=btcusdt@bookTicker");
}

TEST(BinanceCombinedWsPath, TwoSymbols) {
    std::array<std::string_view, 2> syms = {"btcusdt", "ethusdt"};
    EXPECT_EQ(combined_ws_path(syms, "bookTicker"),
              "/stream?streams=btcusdt@bookTicker/ethusdt@bookTicker");
}

TEST(BinanceCombinedWsPath, ThreeSymbols) {
    std::array<std::string_view, 3> syms = {"btcusdt", "ethusdt", "solusdt"};
    EXPECT_EQ(combined_ws_path(syms, "trade"),
              "/stream?streams=btcusdt@trade/ethusdt@trade/solusdt@trade");
}

TEST(BinanceCombinedWsPath, EmptySymbolsList) {
    std::span<const std::string_view> empty;
    EXPECT_EQ(combined_ws_path(empty, "bookTicker"), "/stream?streams=");
}

// ---------------------------------------------------------------------------
// subscribe_message
// ---------------------------------------------------------------------------

TEST(BinanceSubscribe, SingleSymbol) {
    std::array<std::string_view, 1> syms = {"btcusdt"};
    auto msg = subscribe_message(syms, "bookTicker", 1);

    // Parse back to verify it's valid JSON
    auto json = parse_str(msg);
    ASSERT_TRUE(json.has_value()) << "subscribe_message produced invalid JSON: " << msg;

    auto method = json->get_string("method");
    ASSERT_TRUE(method.has_value());
    EXPECT_EQ(*method, "SUBSCRIBE");

    auto id = json->get_int("id");
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, 1);
}

TEST(BinanceSubscribe, MultipleSymbols) {
    std::array<std::string_view, 2> syms = {"btcusdt", "ethusdt"};
    auto msg = subscribe_message(syms, "bookTicker", 42);

    auto json = parse_str(msg);
    ASSERT_TRUE(json.has_value()) << "subscribe_message produced invalid JSON: " << msg;

    auto id = json->get_int("id");
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, 42);

    // Verify the params array contains the expected streams by checking raw content
    // (our parser doesn't expose array iteration, but we can check the raw string)
    EXPECT_NE(msg.find("btcusdt@bookTicker"), std::string::npos);
    EXPECT_NE(msg.find("ethusdt@bookTicker"), std::string::npos);
}

TEST(BinanceSubscribe, EmptySymbolsList) {
    std::span<const std::string_view> empty;
    auto msg = subscribe_message(empty, "bookTicker", 1);

    auto json = parse_str(msg);
    ASSERT_TRUE(json.has_value()) << "subscribe_message with empty symbols produced invalid JSON: " << msg;

    auto method = json->get_string("method");
    ASSERT_TRUE(method.has_value());
    EXPECT_EQ(*method, "SUBSCRIBE");
}

// ---------------------------------------------------------------------------
// unsubscribe_message
// ---------------------------------------------------------------------------

TEST(BinanceUnsubscribe, SingleSymbol) {
    std::array<std::string_view, 1> syms = {"btcusdt"};
    auto msg = unsubscribe_message(syms, "bookTicker", 2);

    auto json = parse_str(msg);
    ASSERT_TRUE(json.has_value()) << "unsubscribe_message produced invalid JSON: " << msg;

    auto method = json->get_string("method");
    ASSERT_TRUE(method.has_value());
    EXPECT_EQ(*method, "UNSUBSCRIBE");

    auto id = json->get_int("id");
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, 2);

    EXPECT_NE(msg.find("btcusdt@bookTicker"), std::string::npos);
}

TEST(BinanceUnsubscribe, EmptySymbolsList) {
    std::span<const std::string_view> empty;
    auto msg = unsubscribe_message(empty, "trade", 99);

    auto json = parse_str(msg);
    ASSERT_TRUE(json.has_value()) << "unsubscribe_message with empty symbols produced invalid JSON: " << msg;

    auto method = json->get_string("method");
    ASSERT_TRUE(method.has_value());
    EXPECT_EQ(*method, "UNSUBSCRIBE");

    auto id = json->get_int("id");
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(*id, 99);
}
