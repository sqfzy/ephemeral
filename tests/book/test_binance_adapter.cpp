/// @file test_binance_adapter.cpp
/// Unit tests for the BinanceBookAdapter — Binance bookTicker to ArrayBook bridge.
///
/// Covers: basic BBO update, multiple sequential updates, zero-qty level removal,
/// mid price and spread through the adapter, malformed input handling, and
/// depth snapshot loading for reconnection recovery.

#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

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

// ===========================================================================
// Depth snapshot tests — reconnection recovery path
// ===========================================================================

/// Helper: build a DepthSnapshot with given bid/ask levels.
static eph::json::binance::DepthSnapshot make_snapshot(
    int64_t update_id,
    std::vector<eph::json::binance::DepthLevel> bids,
    std::vector<eph::json::binance::DepthLevel> asks) {
    eph::json::binance::DepthSnapshot snap;
    snap.last_update_id = update_id;
    snap.bids = std::move(bids);
    snap.asks = std::move(asks);
    return snap;
}

/// Load snapshot with 5 bid + 5 ask levels, verify BBO and depth.
TEST(BinanceBookAdapter, LoadSnapshot5x5VerifyBbo) {
    BinanceBookAdapter<20> adapter;

    auto snap = make_snapshot(100000, {
        {50005.0, 1.0},
        {50004.0, 2.0},
        {50003.0, 3.0},
        {50002.0, 4.0},
        {50001.0, 5.0},
    }, {
        {50006.0, 1.5},
        {50007.0, 2.5},
        {50008.0, 3.5},
        {50009.0, 4.5},
        {50010.0, 5.5},
    });

    auto loaded = adapter.load_snapshot(snap);
    EXPECT_EQ(loaded, 10u);

    const auto& book = adapter.book();
    EXPECT_EQ(book.bid_depth(), 5u);
    EXPECT_EQ(book.ask_depth(), 5u);

    // Best bid is highest price
    auto bid = book.best_bid();
    ASSERT_TRUE(bid.has_value());
    EXPECT_NEAR(bid->price, 50005.0, kEps);
    EXPECT_NEAR(bid->qty, 1.0, kEps);

    // Best ask is lowest price
    auto ask = book.best_ask();
    ASSERT_TRUE(ask.has_value());
    EXPECT_NEAR(ask->price, 50006.0, kEps);
    EXPECT_NEAR(ask->qty, 1.5, kEps);

    // Mid and spread
    auto mid = book.mid_price();
    ASSERT_TRUE(mid.has_value());
    EXPECT_NEAR(*mid, 50005.5, kEps);

    auto spread = book.spread();
    ASSERT_TRUE(spread.has_value());
    EXPECT_NEAR(*spread, 1.0, kEps);
}

/// Load an empty snapshot — book should be empty.
TEST(BinanceBookAdapter, LoadEmptySnapshotEmptyBook) {
    BinanceBookAdapter<20> adapter;

    // Pre-populate book with a ticker update
    auto json = make_bookticker_json("BTCUSDT", "50000.0", "1.0", "50001.0", "2.0");
    ASSERT_TRUE(feed_ticker(adapter, json));
    EXPECT_EQ(adapter.book().bid_depth(), 1u);

    // Load empty snapshot — clears the book
    auto snap = make_snapshot(200000, {}, {});
    auto loaded = adapter.load_snapshot(snap);
    EXPECT_EQ(loaded, 0u);

    EXPECT_FALSE(adapter.book().best_bid().has_value());
    EXPECT_FALSE(adapter.book().best_ask().has_value());
    EXPECT_EQ(adapter.book().bid_depth(), 0u);
    EXPECT_EQ(adapter.book().ask_depth(), 0u);
}

/// Load snapshot then apply incremental update — verifies composite book state.
TEST(BinanceBookAdapter, LoadSnapshotThenIncrementalUpdate) {
    BinanceBookAdapter<20> adapter;

    // Load snapshot with 3 levels per side
    auto snap = make_snapshot(300000, {
        {40003.0, 1.0},
        {40002.0, 2.0},
        {40001.0, 3.0},
    }, {
        {40004.0, 1.0},
        {40005.0, 2.0},
        {40006.0, 3.0},
    });
    adapter.load_snapshot(snap);
    EXPECT_EQ(adapter.book().bid_depth(), 3u);
    EXPECT_EQ(adapter.book().ask_depth(), 3u);

    // Apply incremental bookTicker update — new best bid
    auto json = make_bookticker_json("BTCUSDT",
        "40003.50", "0.5",
        "40003.80", "0.8");
    ASSERT_TRUE(feed_ticker(adapter, json));

    const auto& book = adapter.book();

    // New best bid should be 40003.50 (above old best 40003.0)
    auto bid = book.best_bid();
    ASSERT_TRUE(bid.has_value());
    EXPECT_NEAR(bid->price, 40003.50, kEps);
    EXPECT_NEAR(bid->qty, 0.5, kEps);

    // New best ask should be 40003.80 (below old best 40004.0)
    auto ask = book.best_ask();
    ASSERT_TRUE(ask.has_value());
    EXPECT_NEAR(ask->price, 40003.80, kEps);
    EXPECT_NEAR(ask->qty, 0.8, kEps);

    // Depth increased: 3 snapshot + 1 incremental per side
    EXPECT_EQ(book.bid_depth(), 4u);
    EXPECT_EQ(book.ask_depth(), 4u);
}

/// Verify last_update_id tracking across snapshots.
TEST(BinanceBookAdapter, LastUpdateIdTracking) {
    BinanceBookAdapter<20> adapter;

    // Initially zero
    EXPECT_EQ(adapter.last_update_id(), 0);

    // Load first snapshot
    auto snap1 = make_snapshot(12345, {
        {100.0, 1.0},
    }, {
        {101.0, 1.0},
    });
    adapter.load_snapshot(snap1);
    EXPECT_EQ(adapter.last_update_id(), 12345);

    // Ticker updates do not change last_update_id
    auto json = make_bookticker_json("BTCUSDT", "100.0", "2.0", "101.0", "2.0", 99999);
    ASSERT_TRUE(feed_ticker(adapter, json));
    EXPECT_EQ(adapter.last_update_id(), 12345);

    // Load second snapshot — update_id advances
    auto snap2 = make_snapshot(67890, {
        {200.0, 1.0},
    }, {
        {201.0, 1.0},
    });
    adapter.load_snapshot(snap2);
    EXPECT_EQ(adapter.last_update_id(), 67890);

    // Book reflects second snapshot only
    EXPECT_EQ(adapter.book().bid_depth(), 1u);
    EXPECT_NEAR(adapter.book().best_bid()->price, 200.0, kEps);
}
