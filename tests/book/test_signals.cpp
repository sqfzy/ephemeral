/// @file test_signals.cpp
/// Unit tests for market microstructure signal calculators.
///
/// Covers: order_imbalance, weighted_mid, microprice, spread_bps, vwap,
/// and depth_ratio — each tested with normal, asymmetric, and empty-book
/// edge cases on both ArrayBook and MapBook.

#include <cmath>
#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

#include "eph/book/array_book.hpp"
#include "eph/book/map_book.hpp"
#include "eph/book/signals.hpp"

using namespace eph::book;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static constexpr double kEps = 1e-9;

/// Build a simple ArrayBook with one bid and one ask level.
static ArrayBook<5> make_simple_array_book(double bid_px, double bid_qty,
                                           double ask_px, double ask_qty) {
    ArrayBook<5> book;
    book.update_bid(bid_px, bid_qty);
    book.update_ask(ask_px, ask_qty);
    return book;
}

/// Build a simple MapBook with one bid and one ask level.
static MapBook make_simple_map_book(double bid_px, double bid_qty,
                                    double ask_px, double ask_qty) {
    MapBook book;
    book.update_bid(bid_px, bid_qty);
    book.update_ask(ask_px, ask_qty);
    return book;
}

// ===========================================================================
// order_imbalance
// ===========================================================================

TEST(SignalsTest, OrderImbalanceSymmetric) {
    auto book = make_simple_array_book(100.0, 10.0, 101.0, 10.0);
    EXPECT_NEAR(order_imbalance(book), 0.0, kEps);
}

TEST(SignalsTest, OrderImbalanceBuyHeavy) {
    auto book = make_simple_array_book(100.0, 30.0, 101.0, 10.0);
    // (30 - 10) / (30 + 10) = 0.5
    EXPECT_NEAR(order_imbalance(book), 0.5, kEps);
}

TEST(SignalsTest, OrderImbalanceSellHeavy) {
    auto book = make_simple_array_book(100.0, 10.0, 101.0, 30.0);
    // (10 - 30) / (10 + 30) = -0.5
    EXPECT_NEAR(order_imbalance(book), -0.5, kEps);
}

TEST(SignalsTest, OrderImbalanceEmptyBook) {
    ArrayBook<5> book;
    EXPECT_NEAR(order_imbalance(book), 0.0, kEps);
}

TEST(SignalsTest, OrderImbalanceOneSideOnly) {
    ArrayBook<5> book;
    book.update_bid(100.0, 10.0);
    // bid only: (10 - 0) / (10 + 0) = 1.0
    EXPECT_NEAR(order_imbalance(book), 1.0, kEps);
}

TEST(SignalsTest, OrderImbalanceMapBook) {
    auto book = make_simple_map_book(100.0, 30.0, 101.0, 10.0);
    EXPECT_NEAR(order_imbalance(book), 0.5, kEps);
}

// ===========================================================================
// weighted_mid / microprice
// ===========================================================================

TEST(SignalsTest, WeightedMidSymmetric) {
    // When sizes are equal, weighted mid == simple mid.
    auto book = make_simple_array_book(100.0, 10.0, 102.0, 10.0);
    auto wm = weighted_mid(book);
    ASSERT_TRUE(wm.has_value());
    EXPECT_NEAR(*wm, 101.0, kEps);
}

TEST(SignalsTest, WeightedMidAsymmetric) {
    // bid=100 qty=10, ask=102 qty=30
    // wm = (100*30 + 102*10) / (10+30) = (3000+1020)/40 = 100.5
    auto book = make_simple_array_book(100.0, 10.0, 102.0, 30.0);
    auto wm = weighted_mid(book);
    ASSERT_TRUE(wm.has_value());
    EXPECT_NEAR(*wm, 100.5, kEps);
}

TEST(SignalsTest, WeightedMidPulledTowardAsk) {
    // Large bid qty pulls weighted mid toward ask price.
    // bid=100 qty=30, ask=102 qty=10
    // wm = (100*10 + 102*30) / (30+10) = (1000+3060)/40 = 101.5
    auto book = make_simple_array_book(100.0, 30.0, 102.0, 10.0);
    auto wm = weighted_mid(book);
    ASSERT_TRUE(wm.has_value());
    EXPECT_NEAR(*wm, 101.5, kEps);
}

TEST(SignalsTest, WeightedMidEmptyBook) {
    ArrayBook<5> book;
    EXPECT_FALSE(weighted_mid(book).has_value());
}

TEST(SignalsTest, WeightedMidOneSideEmpty) {
    ArrayBook<5> book;
    book.update_bid(100.0, 10.0);
    EXPECT_FALSE(weighted_mid(book).has_value());
}

TEST(SignalsTest, MicropriceMatchesWeightedMid) {
    auto book = make_simple_array_book(100.0, 10.0, 102.0, 30.0);
    auto mp = microprice(book);
    auto wm = weighted_mid(book);
    ASSERT_TRUE(mp.has_value());
    ASSERT_TRUE(wm.has_value());
    EXPECT_NEAR(*mp, *wm, kEps);
}

TEST(SignalsTest, MicropriceEmptyBook) {
    ArrayBook<5> book;
    EXPECT_FALSE(microprice(book).has_value());
}

TEST(SignalsTest, WeightedMidMapBook) {
    auto book = make_simple_map_book(100.0, 10.0, 102.0, 30.0);
    auto wm = weighted_mid(book);
    ASSERT_TRUE(wm.has_value());
    EXPECT_NEAR(*wm, 100.5, kEps);
}

// ===========================================================================
// spread_bps
// ===========================================================================

TEST(SignalsTest, SpreadBpsNormal) {
    // bid=100, ask=101, mid=100.5
    // spread_bps = (101-100)/100.5 * 10000 = 99.5025...
    auto book = make_simple_array_book(100.0, 10.0, 101.0, 10.0);
    auto s = spread_bps(book);
    ASSERT_TRUE(s.has_value());
    EXPECT_NEAR(*s, 1.0 / 100.5 * 10000.0, kEps);
}

TEST(SignalsTest, SpreadBpsTight) {
    // bid=100.00, ask=100.01, mid=100.005
    // spread_bps = 0.01/100.005 * 10000 ≈ 0.9999500
    auto book = make_simple_array_book(100.00, 10.0, 100.01, 10.0);
    auto s = spread_bps(book);
    ASSERT_TRUE(s.has_value());
    EXPECT_NEAR(*s, 0.01 / 100.005 * 10000.0, kEps);
}

TEST(SignalsTest, SpreadBpsEmptyBook) {
    ArrayBook<5> book;
    EXPECT_FALSE(spread_bps(book).has_value());
}

TEST(SignalsTest, SpreadBpsMapBook) {
    auto book = make_simple_map_book(100.0, 10.0, 101.0, 10.0);
    auto s = spread_bps(book);
    ASSERT_TRUE(s.has_value());
    EXPECT_NEAR(*s, 1.0 / 100.5 * 10000.0, kEps);
}

// ===========================================================================
// vwap (free function taking span<const PriceLevel>)
// ===========================================================================

TEST(SignalsTest, VwapSingleLevel) {
    ArrayBook<5> book;
    book.update_bid(100.0, 10.0);
    auto v = vwap(book.bids());
    ASSERT_TRUE(v.has_value());
    EXPECT_NEAR(*v, 100.0, kEps);
}

TEST(SignalsTest, VwapMultipleLevels) {
    ArrayBook<5> book;
    book.update_bid(100.0, 10.0);
    book.update_bid(99.0, 20.0);
    book.update_bid(98.0, 30.0);
    // vwap = (100*10 + 99*20 + 98*30) / (10+20+30)
    //      = (1000 + 1980 + 2940) / 60 = 5920/60 = 98.6667
    auto v = vwap(book.bids());
    ASSERT_TRUE(v.has_value());
    EXPECT_NEAR(*v, 5920.0 / 60.0, kEps);
}

TEST(SignalsTest, VwapSubspan) {
    // Only compute VWAP over top 2 of 3 levels.
    ArrayBook<5> book;
    book.update_bid(100.0, 10.0);
    book.update_bid(99.0, 20.0);
    book.update_bid(98.0, 30.0);
    auto top2 = book.bids().subspan(0, 2);
    // vwap = (100*10 + 99*20) / (10+20) = 2980/30 = 99.3333
    auto v = vwap(top2);
    ASSERT_TRUE(v.has_value());
    EXPECT_NEAR(*v, 2980.0 / 30.0, kEps);
}

TEST(SignalsTest, VwapEmptySpan) {
    ArrayBook<5> book;
    EXPECT_FALSE(vwap(book.bids()).has_value());
}

TEST(SignalsTest, VwapAskSide) {
    ArrayBook<5> book;
    book.update_ask(101.0, 5.0);
    book.update_ask(102.0, 15.0);
    // vwap = (101*5 + 102*15) / (5+15) = (505+1530)/20 = 101.75
    auto v = vwap(book.asks());
    ASSERT_TRUE(v.has_value());
    EXPECT_NEAR(*v, 2035.0 / 20.0, kEps);
}

TEST(SignalsTest, VwapFromMapBookTopBids) {
    // MapBook returns vector from top_bids(); convert to span.
    MapBook book;
    book.update_bid(100.0, 10.0);
    book.update_bid(99.0, 20.0);
    auto levels = book.top_bids(2);
    auto v = vwap(std::span<const PriceLevel>{levels});
    ASSERT_TRUE(v.has_value());
    EXPECT_NEAR(*v, (100.0 * 10.0 + 99.0 * 20.0) / 30.0, kEps);
}

// ===========================================================================
// depth_ratio
// ===========================================================================

TEST(SignalsTest, DepthRatioSymmetric) {
    auto book = make_simple_array_book(100.0, 10.0, 101.0, 10.0);
    EXPECT_NEAR(depth_ratio(book), 1.0, kEps);
}

TEST(SignalsTest, DepthRatioBuyHeavy) {
    auto book = make_simple_array_book(100.0, 30.0, 101.0, 10.0);
    EXPECT_NEAR(depth_ratio(book), 3.0, kEps);
}

TEST(SignalsTest, DepthRatioSellHeavy) {
    auto book = make_simple_array_book(100.0, 10.0, 101.0, 30.0);
    EXPECT_NEAR(depth_ratio(book), 10.0 / 30.0, kEps);
}

TEST(SignalsTest, DepthRatioEmptyAsk) {
    ArrayBook<5> book;
    book.update_bid(100.0, 10.0);
    // Ask side empty → returns 0.0.
    EXPECT_NEAR(depth_ratio(book), 0.0, kEps);
}

TEST(SignalsTest, DepthRatioEmptyBook) {
    ArrayBook<5> book;
    EXPECT_NEAR(depth_ratio(book), 0.0, kEps);
}

TEST(SignalsTest, DepthRatioMapBook) {
    auto book = make_simple_map_book(100.0, 30.0, 101.0, 10.0);
    EXPECT_NEAR(depth_ratio(book), 3.0, kEps);
}

// ===========================================================================
// Multi-level book: signals with deeper books
// ===========================================================================

TEST(SignalsTest, OrderImbalanceMultiLevel) {
    ArrayBook<5> book;
    book.update_bid(100.0, 10.0);
    book.update_bid(99.0, 20.0);
    book.update_ask(101.0, 5.0);
    book.update_ask(102.0, 15.0);
    // total_bid=30, total_ask=20 → imbalance = 10/50 = 0.2
    EXPECT_NEAR(order_imbalance(book), 0.2, kEps);
}

TEST(SignalsTest, DepthRatioMultiLevel) {
    ArrayBook<5> book;
    book.update_bid(100.0, 10.0);
    book.update_bid(99.0, 20.0);
    book.update_ask(101.0, 5.0);
    book.update_ask(102.0, 15.0);
    // 30 / 20 = 1.5
    EXPECT_NEAR(depth_ratio(book), 1.5, kEps);
}
