/// @file test_signals.cpp
/// Unit tests for market microstructure signal calculators.
///
/// Covers: order_imbalance, weighted_mid, microprice, spread_bps, vwap,
/// and depth_ratio — each tested with normal, asymmetric, and empty-book
/// edge cases on both ArrayBook and MapBook.

#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
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

// vwap takes a raw std::span<const PriceLevel>. While book inserts reject
// non-finite price/qty, a caller can build any span (e.g. snapshot
// reconstruction, replay tools, custom market-data adapter). A single
// NaN/Inf level pre-fix poisoned both running sums; the `sum_q <= 0.0`
// guard then silently returned NaN — propagating into trading-signal
// downstream pipelines. Post-fix: skip non-finite levels, keep aggregating
// the finite ones; only return nullopt if every level is bad / qty<=0.
TEST(SignalsTest, VwapSkipsNonFiniteLevelsInExternallyBuiltSpan) {
    const PriceLevel levels[] = {
        {100.0, 10.0},
        {std::nan(""), 5.0},                              // NaN price
        {99.0, std::numeric_limits<double>::infinity()},  // +Inf qty
        {98.0, 20.0},
    };
    auto v = vwap(std::span<const PriceLevel>{levels});
    ASSERT_TRUE(v.has_value())
        << "Span with mixed finite/non-finite levels must aggregate the "
           "finite subset, not return NaN";
    EXPECT_TRUE(std::isfinite(*v))
        << "Result must be finite — NaN slip would poison downstream signals";
    // Expected: only finite levels {100,10} and {98,20} contribute.
    EXPECT_NEAR(*v, (100.0 * 10.0 + 98.0 * 20.0) / 30.0, kEps);
}

TEST(SignalsTest, VwapAllNonFiniteSpanReturnsNullopt) {
    const PriceLevel levels[] = {
        {std::nan(""), 5.0},
        {std::numeric_limits<double>::infinity(), 10.0},
    };
    EXPECT_FALSE(vwap(std::span<const PriceLevel>{levels}).has_value())
        << "If every level is non-finite, sum_q stays 0 → nullopt, not NaN";
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
    auto r = depth_ratio(book);
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(*r, 1.0, kEps);
}

TEST(SignalsTest, DepthRatioBuyHeavy) {
    auto book = make_simple_array_book(100.0, 30.0, 101.0, 10.0);
    auto r = depth_ratio(book);
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(*r, 3.0, kEps);
}

TEST(SignalsTest, DepthRatioSellHeavy) {
    auto book = make_simple_array_book(100.0, 10.0, 101.0, 30.0);
    auto r = depth_ratio(book);
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(*r, 10.0 / 30.0, kEps);
}

TEST(SignalsTest, DepthRatioEmptyAskReturnsNullopt) {
    ArrayBook<5> book;
    book.update_bid(100.0, 10.0);
    // Ask side empty → ratio is undefined (conceptually +inf, NOT 0.0 — that
    // would invert the buy/sell signal). API returns nullopt to force the
    // caller to handle the undefined case explicitly.
    EXPECT_FALSE(depth_ratio(book).has_value());
}

TEST(SignalsTest, DepthRatioEmptyBookReturnsNullopt) {
    ArrayBook<5> book;
    EXPECT_FALSE(depth_ratio(book).has_value());
}

TEST(SignalsTest, DepthRatioEmptyBidReturnsNullopt) {
    ArrayBook<5> book;
    book.update_ask(101.0, 10.0);
    EXPECT_FALSE(depth_ratio(book).has_value());
}

TEST(SignalsTest, DepthRatioMapBook) {
    auto book = make_simple_map_book(100.0, 30.0, 101.0, 10.0);
    auto r = depth_ratio(book);
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(*r, 3.0, kEps);
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
    auto r = depth_ratio(book);
    ASSERT_TRUE(r.has_value());
    EXPECT_NEAR(*r, 1.5, kEps);
}

// ===========================================================================
// Defensive guards: paths reachable with custom Book types or pathological
// inputs that the ArrayBook/MapBook fixtures never produce in normal use
// (ArrayBook erases on qty=0, so total_qty=0 with non-empty BBO is unreachable
// through update_*; tests below construct the inputs directly).
// ===========================================================================

TEST(SignalsTest, VwapAllZeroQtyLevelsReturnsNullopt) {
    // sum_q == 0 branch in vwap() — span is non-empty but every level
    // has qty 0. Must surface nullopt rather than divide by zero.
    const std::vector<PriceLevel> zero_q{
        {100.0, 0.0}, {101.0, 0.0}, {102.0, 0.0}};
    auto v = vwap(std::span<const PriceLevel>{zero_q});
    EXPECT_FALSE(v.has_value());
}

TEST(SignalsTest, VwapNegativeQtyLevelTreatedAsInvalid) {
    // sum_q goes <= 0 from a single negative qty cancelling positives.
    // Documents the contract: signals refuse to compute on inputs where
    // the denominator would not be strictly positive.
    const std::vector<PriceLevel> neg{{100.0, 5.0}, {101.0, -5.0}};
    auto v = vwap(std::span<const PriceLevel>{neg});
    EXPECT_FALSE(v.has_value());
}

namespace {
// Minimal stub book matching the duck-typed signal API. Lets us drive the
// `total <= 0.0` and `mid <= 0.0` guards in weighted_mid / spread_bps that
// ArrayBook/MapBook cannot reach via their normal update_* paths.
struct StubBook {
    std::optional<PriceLevel> bid_;
    std::optional<PriceLevel> ask_;
    double bid_total_ = 0.0;
    double ask_total_ = 0.0;
    [[nodiscard]] std::optional<PriceLevel> best_bid() const noexcept { return bid_; }
    [[nodiscard]] std::optional<PriceLevel> best_ask() const noexcept { return ask_; }
    [[nodiscard]] double total_bid_qty() const noexcept { return bid_total_; }
    [[nodiscard]] double total_ask_qty() const noexcept { return ask_total_; }
};
} // namespace

TEST(SignalsTest, WeightedMidZeroQtyBothSidesReturnsNullopt) {
    // total = bid.qty + ask.qty == 0 — the second-stage guard at line 69
    // of signals.hpp. Unreachable through ArrayBook/MapBook because qty=0
    // erases the level, so we use a stub.
    StubBook book;
    book.bid_ = PriceLevel{100.0, 0.0};
    book.ask_ = PriceLevel{101.0, 0.0};
    EXPECT_FALSE(weighted_mid(book).has_value());
    EXPECT_FALSE(microprice(book).has_value()); // shares the formula
}

TEST(SignalsTest, SpreadBpsNonPositiveMidReturnsNullopt) {
    // mid = (bid + ask) / 2 <= 0 — possible only with a zero-or-negative
    // mid (e.g. derivative books quoted with negative prices). Confirms
    // the divide-by-mid guard surfaces nullopt rather than producing inf.
    StubBook book;
    book.bid_ = PriceLevel{-1.0, 1.0};
    book.ask_ = PriceLevel{1.0, 1.0}; // mid = 0
    EXPECT_FALSE(spread_bps(book).has_value());
}

// All four signals (order_imbalance, weighted_mid, spread_bps,
// depth_ratio) carry an explicit "templated-book defensive" comment
// explaining that a user-supplied book without insert-time NaN
// validation could feed NaN into the math and silently poison every
// downstream consumer (NaN > threshold == false → buy-pressure tests
// never trip). The defense uses the `!(x > 0.0)` shape which folds
// NaN into the empty-book branch. ArrayBook/MapBook reject NaN at
// insert, so this is unreachable through them — the StubBook path is
// the only way to verify the guards actually fire.

TEST(SignalsTest, OrderImbalanceNanTotalReturnsZero) {
    StubBook book;
    book.bid_total_ = std::nan("");
    book.ask_total_ = 5.0;
    // total = NaN; `!(NaN > 0.0)` → return 0.0 (not NaN).
    EXPECT_DOUBLE_EQ(order_imbalance(book), 0.0)
        << "NaN total must fold into the empty-book early-return, not "
           "propagate into trading-signal pipelines";
}

TEST(SignalsTest, WeightedMidNanQtyReturnsNullopt) {
    StubBook book;
    book.bid_ = PriceLevel{100.0, std::nan("")};
    book.ask_ = PriceLevel{101.0, 1.0};
    EXPECT_FALSE(weighted_mid(book).has_value())
        << "NaN qty must fold into nullopt, not produce NaN price";
    // Microprice shares the formula — also nullopt.
    EXPECT_FALSE(microprice(book).has_value());
}

TEST(SignalsTest, SpreadBpsNanPriceReturnsNullopt) {
    StubBook book;
    // NaN bid price → mid = NaN → guard `!(NaN > 0.0)` returns nullopt.
    book.bid_ = PriceLevel{std::nan(""), 1.0};
    book.ask_ = PriceLevel{101.0, 1.0};
    EXPECT_FALSE(spread_bps(book).has_value());
}

TEST(SignalsTest, DepthRatioNanQtyReturnsNullopt) {
    StubBook book;
    // NaN bid_total → guard `!(NaN > 0.0)` returns nullopt instead of NaN.
    book.bid_total_ = std::nan("");
    book.ask_total_ = 5.0;
    EXPECT_FALSE(depth_ratio(book).has_value())
        << "NaN bid_qty must produce nullopt, not NaN/inf — otherwise "
           "callers' `if (depth_ratio > threshold)` tests silently break";

    // Symmetric: NaN ask_total
    book.bid_total_ = 5.0;
    book.ask_total_ = std::nan("");
    EXPECT_FALSE(depth_ratio(book).has_value());
}
