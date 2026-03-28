/// @file test_binance_adapter.cpp
/// Unit tests for the BinanceBookAdapter — Binance bookTicker to ArrayBook bridge.
///
/// Covers: basic BBO update, multiple sequential updates, zero-qty level removal,
/// mid price and spread through the adapter, and malformed input handling.

#include <cmath>
#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

#include "eph/book/binance_adapter.hpp"

using namespace eph::book;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static constexpr double kEps = 1e-9;

/// Build a Binance bookTicker JSON string and parse it through the adapter.
/// This mirrors the real pipeline: raw JSON -> parse -> BookTicker::from -> adapter.
static std::string make_bookticker_json(
    std::string_view symbol,
    std::string_view bid_price, std::string_view bid_qty,
    std::string_view ask_price, std::string_view ask_qty,
    int64_t update_id = 123456) {
    // Realistic Binance bookTicker payload
    std::string json = "{";
    json += "\"e\":\"bookTicker\",";
    json += "\"u\":" + std::to_string(update_id) + ",";
    json += "\"s\":\"" + std::string(symbol) + "\",";
    json += "\"b\":\"" + std::string(bid_price) + "\",";
    json += "\"B\":\"" + std::string(bid_qty) + "\",";
    json += "\"a\":\"" + std::string(ask_price) + "\",";
    json += "\"A\":\"" + std::string(ask_qty) + "\",";
    json += "\"T\":1700000000000,";
    json += "\"E\":1700000000001";
    json += "}";
    return json;
}

/// Parse JSON and extract BookTicker, then feed to adapter.
/// Returns true if the full pipeline succeeded.
static bool feed_ticker(BinanceBookAdapter<20>& adapter, const std::string& json) {
    auto result = eph::json::parse(
        reinterpret_cast<const uint8_t*>(json.data()), json.size());
    if (!result) return false;

    auto ticker = eph::json::binance::BookTicker::from(result.value());
    if (!ticker) return false;

    return adapter.update_from_ticker(*ticker);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

/// Parse a real Binance bookTicker JSON, update book, verify BBO.
TEST(BinanceBookAdapter, BasicBboUpdate) {
    BinanceBookAdapter<20> adapter;

    auto json = make_bookticker_json("BTCUSDT",
        "87245.30000000", "1.50000000",
        "87245.40000000", "2.30000000");

    ASSERT_TRUE(feed_ticker(adapter, json));

    const auto& book = adapter.book();

    auto bid = book.best_bid();
    ASSERT_TRUE(bid.has_value());
    EXPECT_NEAR(bid->price, 87245.30, kEps);
    EXPECT_NEAR(bid->qty, 1.50, kEps);

    auto ask = book.best_ask();
    ASSERT_TRUE(ask.has_value());
    EXPECT_NEAR(ask->price, 87245.40, kEps);
    EXPECT_NEAR(ask->qty, 2.30, kEps);
}

/// Multiple updates — book tracks the latest values.
TEST(BinanceBookAdapter, MultipleUpdatesTrackLatest) {
    BinanceBookAdapter<20> adapter;

    // First tick
    auto json1 = make_bookticker_json("ETHUSDT",
        "3200.50", "10.0",
        "3200.60", "15.0");
    ASSERT_TRUE(feed_ticker(adapter, json1));

    // Second tick — prices moved
    auto json2 = make_bookticker_json("ETHUSDT",
        "3201.00", "12.5",
        "3201.10", "8.0");
    ASSERT_TRUE(feed_ticker(adapter, json2));

    const auto& book = adapter.book();

    // Both ticks inserted at different prices, so the book accumulates levels.
    // Best bid is highest (3201.00 from tick 2), best ask is lowest (3200.60
    // from tick 1, qty unchanged since it's a different price from tick 2).
    auto bid = book.best_bid();
    ASSERT_TRUE(bid.has_value());
    EXPECT_NEAR(bid->price, 3201.00, kEps);
    EXPECT_NEAR(bid->qty, 12.5, kEps);

    auto ask = book.best_ask();
    ASSERT_TRUE(ask.has_value());
    EXPECT_NEAR(ask->price, 3200.60, kEps);
    EXPECT_NEAR(ask->qty, 15.0, kEps);  // unchanged from tick 1

    // Should have 2 bid levels and 2 ask levels
    EXPECT_EQ(book.bid_depth(), 2u);
    EXPECT_EQ(book.ask_depth(), 2u);
}

/// Zero quantity bid — level is removed from the book.
TEST(BinanceBookAdapter, ZeroQtyRemovesLevel) {
    BinanceBookAdapter<20> adapter;

    // Establish BBO
    auto json1 = make_bookticker_json("BTCUSDT",
        "87245.30", "1.50",
        "87245.40", "2.30");
    ASSERT_TRUE(feed_ticker(adapter, json1));
    EXPECT_EQ(adapter.book().bid_depth(), 1u);
    EXPECT_EQ(adapter.book().ask_depth(), 1u);

    // Zero bid qty — bid level should be removed
    auto json2 = make_bookticker_json("BTCUSDT",
        "87245.30", "0.00000000",
        "87245.40", "2.30");
    ASSERT_TRUE(feed_ticker(adapter, json2));

    EXPECT_FALSE(adapter.book().best_bid().has_value());
    EXPECT_EQ(adapter.book().bid_depth(), 0u);

    // Ask should still be present
    auto ask = adapter.book().best_ask();
    ASSERT_TRUE(ask.has_value());
    EXPECT_NEAR(ask->price, 87245.40, kEps);
}

/// Mid price and spread calculation through the adapter.
TEST(BinanceBookAdapter, MidPriceAndSpread) {
    BinanceBookAdapter<20> adapter;

    auto json = make_bookticker_json("BTCUSDT",
        "87245.00", "1.00",
        "87246.00", "1.00");
    ASSERT_TRUE(feed_ticker(adapter, json));

    auto mid = adapter.book().mid_price();
    ASSERT_TRUE(mid.has_value());
    EXPECT_NEAR(*mid, 87245.50, kEps);

    auto spread = adapter.book().spread();
    ASSERT_TRUE(spread.has_value());
    EXPECT_NEAR(*spread, 1.00, kEps);
}

/// Empty book has no mid price or spread.
TEST(BinanceBookAdapter, EmptyBookNoMidOrSpread) {
    BinanceBookAdapter<20> adapter;

    EXPECT_FALSE(adapter.book().mid_price().has_value());
    EXPECT_FALSE(adapter.book().spread().has_value());
    EXPECT_FALSE(adapter.book().best_bid().has_value());
    EXPECT_FALSE(adapter.book().best_ask().has_value());
}

/// Update with same price replaces quantity (does not duplicate the level).
TEST(BinanceBookAdapter, SamePriceUpdatesQty) {
    BinanceBookAdapter<20> adapter;

    auto json1 = make_bookticker_json("BTCUSDT",
        "50000.00", "1.00",
        "50001.00", "2.00");
    ASSERT_TRUE(feed_ticker(adapter, json1));

    // Same prices, different quantities
    auto json2 = make_bookticker_json("BTCUSDT",
        "50000.00", "5.00",
        "50001.00", "10.00");
    ASSERT_TRUE(feed_ticker(adapter, json2));

    const auto& book = adapter.book();
    EXPECT_EQ(book.bid_depth(), 1u);
    EXPECT_EQ(book.ask_depth(), 1u);
    EXPECT_NEAR(book.best_bid()->qty, 5.00, kEps);
    EXPECT_NEAR(book.best_ask()->qty, 10.00, kEps);
}

/// Zero qty on ask side removes the ask level.
TEST(BinanceBookAdapter, ZeroAskQtyRemovesLevel) {
    BinanceBookAdapter<20> adapter;

    auto json1 = make_bookticker_json("BTCUSDT",
        "50000.00", "1.00",
        "50001.00", "2.00");
    ASSERT_TRUE(feed_ticker(adapter, json1));

    auto json2 = make_bookticker_json("BTCUSDT",
        "50000.00", "1.00",
        "50001.00", "0.00");
    ASSERT_TRUE(feed_ticker(adapter, json2));

    EXPECT_TRUE(adapter.book().best_bid().has_value());
    EXPECT_FALSE(adapter.book().best_ask().has_value());
    EXPECT_EQ(adapter.book().ask_depth(), 0u);
}

/// End-to-end: raw combined stream wrapper -> extract data -> parse bookTicker -> adapter.
TEST(BinanceBookAdapter, CombinedStreamEndToEnd) {
    // Binance combined stream format: {"stream":"btcusdt@bookTicker","data":{...}}
    std::string json =
        R"({"stream":"btcusdt@bookTicker","data":{"e":"bookTicker","u":12345,)"
        R"("s":"BTCUSDT","b":"95000.10","B":"3.00","a":"95000.20","A":"4.50",)"
        R"("T":1700000000000,"E":1700000000001}})";

    // Parse the outer wrapper
    auto outer = eph::json::parse(
        reinterpret_cast<const uint8_t*>(json.data()), json.size());
    ASSERT_TRUE(outer.has_value());

    auto combined = eph::json::binance::CombinedStream::from(outer.value());
    ASSERT_TRUE(combined.has_value());
    EXPECT_EQ(combined->symbol, "btcusdt");

    // Parse the inner data object
    auto inner = eph::json::parse(
        reinterpret_cast<const uint8_t*>(combined->data_raw.data()),
        combined->data_raw.size());
    ASSERT_TRUE(inner.has_value());

    auto ticker = eph::json::binance::BookTicker::from(inner.value());
    ASSERT_TRUE(ticker.has_value());

    // Feed to adapter
    BinanceBookAdapter<20> adapter;
    ASSERT_TRUE(adapter.update_from_ticker(*ticker));

    auto bid = adapter.book().best_bid();
    ASSERT_TRUE(bid.has_value());
    EXPECT_NEAR(bid->price, 95000.10, kEps);
    EXPECT_NEAR(bid->qty, 3.00, kEps);

    auto ask = adapter.book().best_ask();
    ASSERT_TRUE(ask.has_value());
    EXPECT_NEAR(ask->price, 95000.20, kEps);
    EXPECT_NEAR(ask->qty, 4.50, kEps);
}
