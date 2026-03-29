/// @file test_array_book.cpp
/// Unit tests for the ArrayBook L2 order book.
///
/// Covers: insertion and sort order, BBO extraction, mid price & spread,
/// in-place update, removal (qty=0), overflow/eviction, empty book edge
/// cases, clear(), and a simulated Binance-style depth update sequence.

#include <cmath>
#include <cstddef>

#include <gtest/gtest.h>

#include "eph/book/array_book.hpp"

using namespace eph::book;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static constexpr double kEps = 1e-9;

static void expect_price(double actual, double expected, const char* label) {
    EXPECT_NEAR(actual, expected, kEps) << label;
}

// ---------------------------------------------------------------------------
// Test 1: Empty book edge cases
// ---------------------------------------------------------------------------

TEST(ArrayBookTest, EmptyBookReturnsNullopt) {
    ArrayBook<5> book;
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_FALSE(book.mid_price().has_value());
    EXPECT_FALSE(book.spread().has_value());
    EXPECT_EQ(book.bid_depth(), 0u);
    EXPECT_EQ(book.ask_depth(), 0u);
    EXPECT_TRUE(book.bids().empty());
    EXPECT_TRUE(book.asks().empty());
}

// ---------------------------------------------------------------------------
// Test 2: Insert bids and verify descending sort order
// ---------------------------------------------------------------------------

TEST(ArrayBookTest, BidsSortedDescending) {
    ArrayBook<5> book;
    book.update_bid(100.0, 10.0);
    book.update_bid(102.0, 20.0);
    book.update_bid(101.0, 15.0);

    ASSERT_EQ(book.bid_depth(), 3u);
    auto b = book.bids();
    expect_price(b[0].price, 102.0, "best bid");
    expect_price(b[1].price, 101.0, "second bid");
    expect_price(b[2].price, 100.0, "third bid");
    EXPECT_NEAR(b[0].qty, 20.0, kEps);
    EXPECT_NEAR(b[1].qty, 15.0, kEps);
    EXPECT_NEAR(b[2].qty, 10.0, kEps);
}

// ---------------------------------------------------------------------------
// Test 3: Insert asks and verify ascending sort order
// ---------------------------------------------------------------------------

TEST(ArrayBookTest, AsksSortedAscending) {
    ArrayBook<5> book;
    book.update_ask(105.0, 5.0);
    book.update_ask(103.0, 8.0);
    book.update_ask(104.0, 3.0);

    ASSERT_EQ(book.ask_depth(), 3u);
    auto a = book.asks();
    expect_price(a[0].price, 103.0, "best ask");
    expect_price(a[1].price, 104.0, "second ask");
    expect_price(a[2].price, 105.0, "third ask");
}

// ---------------------------------------------------------------------------
// Test 4: BBO extraction
// ---------------------------------------------------------------------------

TEST(ArrayBookTest, BestBidOffer) {
    ArrayBook<10> book;
    book.update_bid(99.50, 100.0);
    book.update_bid(99.00, 200.0);
    book.update_ask(100.50, 50.0);
    book.update_ask(101.00, 75.0);

    auto bb = book.best_bid();
    auto ba = book.best_ask();
    ASSERT_TRUE(bb.has_value());
    ASSERT_TRUE(ba.has_value());
    expect_price(bb->price, 99.50, "best bid price");
    expect_price(ba->price, 100.50, "best ask price");
}

// ---------------------------------------------------------------------------
// Test 5: Mid price and spread
// ---------------------------------------------------------------------------

TEST(ArrayBookTest, MidPriceAndSpread) {
    ArrayBook<10> book;
    book.update_bid(99.0, 10.0);
    book.update_ask(101.0, 10.0);

    auto mid = book.mid_price();
    auto spr = book.spread();
    ASSERT_TRUE(mid.has_value());
    ASSERT_TRUE(spr.has_value());
    EXPECT_NEAR(*mid, 100.0, kEps);
    EXPECT_NEAR(*spr, 2.0, kEps);
}

TEST(ArrayBookTest, MidPriceNulloptWhenOneSideEmpty) {
    ArrayBook<5> book;
    book.update_bid(99.0, 10.0);
    EXPECT_FALSE(book.mid_price().has_value());
    EXPECT_FALSE(book.spread().has_value());
}

// ---------------------------------------------------------------------------
// Test 6: Update existing level (change qty)
// ---------------------------------------------------------------------------

TEST(ArrayBookTest, UpdateExistingLevel) {
    ArrayBook<5> book;
    book.update_bid(100.0, 10.0);
    book.update_bid(100.0, 25.0); // update qty

    ASSERT_EQ(book.bid_depth(), 1u);
    EXPECT_NEAR(book.bids()[0].qty, 25.0, kEps);
    expect_price(book.bids()[0].price, 100.0, "price unchanged");
}

// ---------------------------------------------------------------------------
// Test 7: Remove level (qty=0)
// ---------------------------------------------------------------------------

TEST(ArrayBookTest, RemoveLevelWithZeroQty) {
    ArrayBook<5> book;
    book.update_ask(100.0, 5.0);
    book.update_ask(101.0, 8.0);
    book.update_ask(102.0, 3.0);

    // Remove the best ask
    book.update_ask(100.0, 0.0);
    ASSERT_EQ(book.ask_depth(), 2u);
    expect_price(book.asks()[0].price, 101.0, "new best ask after removal");
    expect_price(book.asks()[1].price, 102.0, "second ask after removal");
}

TEST(ArrayBookTest, RemoveNonExistentLevelIsNoop) {
    ArrayBook<5> book;
    book.update_bid(100.0, 10.0);
    book.update_bid(999.0, 0.0); // price not in book

    ASSERT_EQ(book.bid_depth(), 1u);
    expect_price(book.bids()[0].price, 100.0, "unchanged");
}

TEST(ArrayBookTest, RemoveMiddleLevel) {
    ArrayBook<5> book;
    book.update_bid(100.0, 10.0);
    book.update_bid(102.0, 20.0);
    book.update_bid(101.0, 15.0);

    book.update_bid(101.0, 0.0); // remove middle
    ASSERT_EQ(book.bid_depth(), 2u);
    expect_price(book.bids()[0].price, 102.0, "best bid");
    expect_price(book.bids()[1].price, 100.0, "second bid");
}

TEST(ArrayBookTest, RemoveLastRemainingLevel) {
    ArrayBook<5> book;
    book.update_bid(100.0, 10.0);
    book.update_bid(100.0, 0.0);
    EXPECT_EQ(book.bid_depth(), 0u);
    EXPECT_FALSE(book.best_bid().has_value());
}

// ---------------------------------------------------------------------------
// Test 8: Overflow — more than MaxLevels
// ---------------------------------------------------------------------------

TEST(ArrayBookTest, OverflowEvictsWorstLevel) {
    ArrayBook<3> book;

    // Fill bids (descending: best = highest)
    book.update_bid(100.0, 1.0);
    book.update_bid(101.0, 1.0);
    book.update_bid(102.0, 1.0);
    ASSERT_EQ(book.bid_depth(), 3u);

    // Insert a better bid — worst (100.0) should be evicted
    book.update_bid(103.0, 1.0);
    ASSERT_EQ(book.bid_depth(), 3u);
    expect_price(book.bids()[0].price, 103.0, "new best bid");
    expect_price(book.bids()[1].price, 102.0, "second bid");
    expect_price(book.bids()[2].price, 101.0, "third bid (100.0 evicted)");
}

TEST(ArrayBookTest, OverflowDropsWorsePrice) {
    ArrayBook<3> book;

    book.update_bid(101.0, 1.0);
    book.update_bid(102.0, 1.0);
    book.update_bid(103.0, 1.0);

    // Insert a worse bid — should be silently dropped
    book.update_bid(100.0, 1.0);
    ASSERT_EQ(book.bid_depth(), 3u);
    expect_price(book.bids()[2].price, 101.0, "worst tracked unchanged");
}

TEST(ArrayBookTest, OverflowAskSide) {
    ArrayBook<3> book;

    // Fill asks (ascending: best = lowest)
    book.update_ask(103.0, 1.0);
    book.update_ask(102.0, 1.0);
    book.update_ask(101.0, 1.0);
    ASSERT_EQ(book.ask_depth(), 3u);

    // Insert a better ask — worst (103.0) should be evicted
    book.update_ask(100.0, 1.0);
    ASSERT_EQ(book.ask_depth(), 3u);
    expect_price(book.asks()[0].price, 100.0, "new best ask");
    expect_price(book.asks()[2].price, 102.0, "worst tracked (103.0 evicted)");

    // Insert a worse ask — should be dropped
    book.update_ask(200.0, 1.0);
    ASSERT_EQ(book.ask_depth(), 3u);
    expect_price(book.asks()[2].price, 102.0, "worst tracked unchanged");
}

// ---------------------------------------------------------------------------
// Test 9: Clear
// ---------------------------------------------------------------------------

TEST(ArrayBookTest, ClearResetsBook) {
    ArrayBook<5> book;
    book.update_bid(100.0, 10.0);
    book.update_ask(101.0, 5.0);
    book.clear();

    EXPECT_EQ(book.bid_depth(), 0u);
    EXPECT_EQ(book.ask_depth(), 0u);
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
}

// ---------------------------------------------------------------------------
// Test 10: max_levels compile-time constant
// ---------------------------------------------------------------------------

TEST(ArrayBookTest, MaxLevelsConstant) {
    EXPECT_EQ(ArrayBook<10>::max_levels, 10u);
    EXPECT_EQ(ArrayBook<1>::max_levels, 1u);
}

// ---------------------------------------------------------------------------
// Test 11: Single-level book (MaxLevels = 1)
// ---------------------------------------------------------------------------

TEST(ArrayBookTest, SingleLevelBook) {
    ArrayBook<1> book;
    book.update_bid(100.0, 10.0);
    ASSERT_EQ(book.bid_depth(), 1u);

    // Replace with a better bid
    book.update_bid(101.0, 5.0);
    ASSERT_EQ(book.bid_depth(), 1u);
    expect_price(book.bids()[0].price, 101.0, "replaced by better");

    // Worse bid is dropped
    book.update_bid(99.0, 20.0);
    ASSERT_EQ(book.bid_depth(), 1u);
    expect_price(book.bids()[0].price, 101.0, "worse bid dropped");
}

// ---------------------------------------------------------------------------
// Test 12: Simulated Binance-style depth update sequence
// ---------------------------------------------------------------------------

TEST(ArrayBookTest, BinanceDepthUpdateSequence) {
    // Simulate a typical Binance @depth snapshot + incremental updates
    ArrayBook<10> book;

    // --- Initial snapshot ---
    book.update_bid(50000.00, 1.5);
    book.update_bid(49999.50, 2.0);
    book.update_bid(49999.00, 3.0);
    book.update_bid(49998.50, 1.0);
    book.update_bid(49998.00, 0.5);

    book.update_ask(50000.50, 1.0);
    book.update_ask(50001.00, 2.5);
    book.update_ask(50001.50, 1.8);
    book.update_ask(50002.00, 3.0);
    book.update_ask(50002.50, 0.7);

    // Verify initial BBO
    ASSERT_TRUE(book.best_bid().has_value());
    ASSERT_TRUE(book.best_ask().has_value());
    expect_price(book.best_bid()->price, 50000.00, "initial best bid");
    expect_price(book.best_ask()->price, 50000.50, "initial best ask");
    EXPECT_NEAR(*book.spread(), 0.50, kEps);

    // --- Incremental update 1: qty change on existing bid ---
    book.update_bid(49999.50, 5.0);
    EXPECT_NEAR(book.bids()[1].qty, 5.0, kEps);

    // --- Incremental update 2: remove best ask (filled) ---
    book.update_ask(50000.50, 0.0);
    expect_price(book.best_ask()->price, 50001.00, "new best ask after fill");

    // --- Incremental update 3: new best bid comes in ---
    book.update_bid(50000.25, 0.3);
    expect_price(book.best_bid()->price, 50000.25, "new best bid");

    // --- Verify final state ---
    EXPECT_NEAR(*book.mid_price(), (50000.25 + 50001.00) / 2.0, kEps);
    EXPECT_NEAR(*book.spread(), 50001.00 - 50000.25, kEps);
    EXPECT_EQ(book.bid_depth(), 6u);  // 5 original + 1 new
    EXPECT_EQ(book.ask_depth(), 4u);  // 5 original - 1 removed
}

// ---------------------------------------------------------------------------
// Test 13: Rapid insert-remove cycles preserve invariants
// ---------------------------------------------------------------------------

TEST(ArrayBookTest, InsertRemoveCyclesBidSide) {
    ArrayBook<5> book;

    for (int i = 0; i < 100; ++i) {
        double price = 100.0 + static_cast<double>(i % 7);
        book.update_bid(price, 1.0);
        // Occasionally remove
        if (i % 3 == 0) {
            book.update_bid(price, 0.0);
        }
    }

    // Just verify the book is consistent: sorted descending
    auto b = book.bids();
    for (std::size_t i = 1; i < b.size(); ++i) {
        EXPECT_GT(b[i - 1].price, b[i].price)
            << "bid sort violated at index " << i;
    }
}

// ---------------------------------------------------------------------------
// Test 14: Update at full capacity (existing level, no eviction)
// ---------------------------------------------------------------------------

TEST(ArrayBookTest, UpdateAtFullCapacityNoEviction) {
    ArrayBook<3> book;
    book.update_bid(100.0, 1.0);
    book.update_bid(101.0, 1.0);
    book.update_bid(102.0, 1.0);

    // Update qty of existing level when book is full
    book.update_bid(101.0, 99.0);
    ASSERT_EQ(book.bid_depth(), 3u);
    EXPECT_NEAR(book.bids()[1].qty, 99.0, kEps);
}

// ---------------------------------------------------------------------------
// Test 15: is_crossed()
// ---------------------------------------------------------------------------

TEST(ArrayBookTest, IsCrossedReturnsFalseWhenEmpty) {
    ArrayBook<5> book;
    EXPECT_FALSE(book.is_crossed());
}

TEST(ArrayBookTest, IsCrossedReturnsFalseWhenOneSideEmpty) {
    ArrayBook<5> book;
    book.update_bid(100.0, 1.0);
    EXPECT_FALSE(book.is_crossed());
}

TEST(ArrayBookTest, IsCrossedReturnsFalseForNormalBook) {
    ArrayBook<5> book;
    book.update_bid(99.0, 1.0);
    book.update_ask(101.0, 1.0);
    EXPECT_FALSE(book.is_crossed());
}

// bid == ask is a "locked" market, not strictly crossed.
// is_crossed() now only returns true for bid > ask; use is_locked() for bid == ask.
TEST(ArrayBookTest, IsCrossedReturnsFalseWhenBidEqualsAsk) {
    ArrayBook<5> book;
    book.update_bid(100.0, 1.0);
    book.update_ask(100.0, 1.0);
    EXPECT_FALSE(book.is_crossed());
    EXPECT_TRUE(book.is_locked());
}

TEST(ArrayBookTest, IsLockedReturnsTrueWhenBidEqualsAsk) {
    ArrayBook<5> book;
    book.update_bid(100.0, 1.0);
    book.update_ask(100.0, 1.0);
    EXPECT_TRUE(book.is_locked());
}

TEST(ArrayBookTest, IsLockedReturnsFalseForNormalBook) {
    ArrayBook<5> book;
    book.update_bid(99.0, 1.0);
    book.update_ask(101.0, 1.0);
    EXPECT_FALSE(book.is_locked());
}

TEST(ArrayBookTest, IsLockedReturnsFalseWhenCrossed) {
    ArrayBook<5> book;
    book.update_bid(102.0, 1.0);
    book.update_ask(100.0, 1.0);
    EXPECT_FALSE(book.is_locked());
    EXPECT_TRUE(book.is_crossed());
}

TEST(ArrayBookTest, IsCrossedReturnsTrueWhenBidExceedsAsk) {
    ArrayBook<5> book;
    book.update_bid(101.0, 1.0);
    book.update_ask(100.0, 1.0);
    EXPECT_TRUE(book.is_crossed());
}

// ---------------------------------------------------------------------------
// Test 16: total_bid_qty() and total_ask_qty()
// ---------------------------------------------------------------------------

TEST(ArrayBookTest, TotalBidQtyEmptyBook) {
    ArrayBook<5> book;
    EXPECT_NEAR(book.total_bid_qty(), 0.0, kEps);
}

TEST(ArrayBookTest, TotalBidQtySumsAllLevels) {
    ArrayBook<5> book;
    book.update_bid(100.0, 10.0);
    book.update_bid(99.0, 20.0);
    book.update_bid(98.0, 30.0);
    EXPECT_NEAR(book.total_bid_qty(), 60.0, kEps);
}

TEST(ArrayBookTest, TotalAskQtySumsAllLevels) {
    ArrayBook<5> book;
    book.update_ask(101.0, 5.0);
    book.update_ask(102.0, 15.0);
    EXPECT_NEAR(book.total_ask_qty(), 20.0, kEps);
}

TEST(ArrayBookTest, TotalQtyUpdatesAfterRemoval) {
    ArrayBook<5> book;
    book.update_bid(100.0, 10.0);
    book.update_bid(99.0, 20.0);
    book.update_bid(100.0, 0.0); // remove
    EXPECT_NEAR(book.total_bid_qty(), 20.0, kEps);
}

// ---------------------------------------------------------------------------
// Test 17: level_count()
// ---------------------------------------------------------------------------

TEST(ArrayBookTest, LevelCountEmptyBook) {
    ArrayBook<5> book;
    EXPECT_EQ(book.level_count(), 0u);
}

TEST(ArrayBookTest, LevelCountBothSides) {
    ArrayBook<5> book;
    book.update_bid(100.0, 1.0);
    book.update_bid(99.0, 1.0);
    book.update_ask(101.0, 1.0);
    EXPECT_EQ(book.level_count(), 3u);
}

TEST(ArrayBookTest, LevelCountAfterClear) {
    ArrayBook<5> book;
    book.update_bid(100.0, 1.0);
    book.update_ask(101.0, 1.0);
    book.clear();
    EXPECT_EQ(book.level_count(), 0u);
}

// ---------------------------------------------------------------------------
// Test 18: NaN price is rejected
// ---------------------------------------------------------------------------

TEST(ArrayBookTest, NanPriceIsIgnored) {
    ArrayBook<5> book;
    book.update_bid(std::nan(""), 10.0);
    EXPECT_EQ(book.bid_depth(), 0u);

    book.update_ask(std::nan(""), 5.0);
    EXPECT_EQ(book.ask_depth(), 0u);

    // NaN removal also doesn't crash
    book.update_bid(100.0, 10.0);
    book.update_bid(std::nan(""), 0.0);
    EXPECT_EQ(book.bid_depth(), 1u);
}
