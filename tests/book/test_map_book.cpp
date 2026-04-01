/// @file test_map_book.cpp
/// Unit tests for the MapBook deep L3 order book.
///
/// Covers: BBO extraction, mid price & spread, in-place update, removal
/// (qty=0), empty book edge cases, clear(), is_crossed(), total qty,
/// level_count(), NaN rejection, deep book (100+ levels), top_N extraction,
/// and cross-validation against ArrayBook on identical input sequences.

#include <cmath>
#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

#include "eph/book/array_book.hpp"
#include "eph/book/map_book.hpp"

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

TEST(MapBookTest, EmptyBookReturnsNullopt) {
    MapBook book;
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_FALSE(book.mid_price().has_value());
    EXPECT_FALSE(book.spread().has_value());
    EXPECT_EQ(book.bid_depth(), 0u);
    EXPECT_EQ(book.ask_depth(), 0u);
    EXPECT_EQ(book.level_count(), 0u);
    EXPECT_NEAR(book.total_bid_qty(), 0.0, kEps);
    EXPECT_NEAR(book.total_ask_qty(), 0.0, kEps);
}

// ---------------------------------------------------------------------------
// Test 2: Insert bids and verify BBO (highest price)
// ---------------------------------------------------------------------------

TEST(MapBookTest, BidsReturnHighestAsBest) {
    MapBook book;
    book.update_bid(100.0, 10.0);
    book.update_bid(102.0, 20.0);
    book.update_bid(101.0, 15.0);

    ASSERT_EQ(book.bid_depth(), 3u);
    auto bb = book.best_bid();
    ASSERT_TRUE(bb.has_value());
    expect_price(bb->price, 102.0, "best bid");
    EXPECT_NEAR(bb->qty, 20.0, kEps);
}

// ---------------------------------------------------------------------------
// Test 3: Insert asks and verify BBO (lowest price)
// ---------------------------------------------------------------------------

TEST(MapBookTest, AsksReturnLowestAsBest) {
    MapBook book;
    book.update_ask(105.0, 5.0);
    book.update_ask(103.0, 8.0);
    book.update_ask(104.0, 3.0);

    ASSERT_EQ(book.ask_depth(), 3u);
    auto ba = book.best_ask();
    ASSERT_TRUE(ba.has_value());
    expect_price(ba->price, 103.0, "best ask");
    EXPECT_NEAR(ba->qty, 8.0, kEps);
}

// ---------------------------------------------------------------------------
// Test 4: BBO extraction
// ---------------------------------------------------------------------------

TEST(MapBookTest, BestBidOffer) {
    MapBook book;
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

TEST(MapBookTest, MidPriceAndSpread) {
    MapBook book;
    book.update_bid(99.0, 10.0);
    book.update_ask(101.0, 10.0);

    auto mid = book.mid_price();
    auto spr = book.spread();
    ASSERT_TRUE(mid.has_value());
    ASSERT_TRUE(spr.has_value());
    EXPECT_NEAR(*mid, 100.0, kEps);
    EXPECT_NEAR(*spr, 2.0, kEps);
}

TEST(MapBookTest, MidPriceNulloptWhenOneSideEmpty) {
    MapBook book;
    book.update_bid(99.0, 10.0);
    EXPECT_FALSE(book.mid_price().has_value());
    EXPECT_FALSE(book.spread().has_value());
}

// ---------------------------------------------------------------------------
// Test 6: Update existing level (change qty)
// ---------------------------------------------------------------------------

TEST(MapBookTest, UpdateExistingLevel) {
    MapBook book;
    book.update_bid(100.0, 10.0);
    book.update_bid(100.0, 25.0); // update qty

    ASSERT_EQ(book.bid_depth(), 1u);
    EXPECT_NEAR(book.best_bid()->qty, 25.0, kEps);
    expect_price(book.best_bid()->price, 100.0, "price unchanged");
}

// ---------------------------------------------------------------------------
// Test 7: Remove level (qty=0)
// ---------------------------------------------------------------------------

TEST(MapBookTest, RemoveLevelWithZeroQty) {
    MapBook book;
    book.update_ask(100.0, 5.0);
    book.update_ask(101.0, 8.0);
    book.update_ask(102.0, 3.0);

    // Remove the best ask.
    book.update_ask(100.0, 0.0);
    ASSERT_EQ(book.ask_depth(), 2u);
    expect_price(book.best_ask()->price, 101.0, "new best ask after removal");
}

TEST(MapBookTest, RemoveNonExistentLevelIsNoop) {
    MapBook book;
    book.update_bid(100.0, 10.0);
    book.update_bid(999.0, 0.0); // price not in book

    ASSERT_EQ(book.bid_depth(), 1u);
    expect_price(book.best_bid()->price, 100.0, "unchanged");
}

TEST(MapBookTest, RemoveMiddleLevel) {
    MapBook book;
    book.update_bid(100.0, 10.0);
    book.update_bid(102.0, 20.0);
    book.update_bid(101.0, 15.0);

    book.update_bid(101.0, 0.0); // remove middle
    ASSERT_EQ(book.bid_depth(), 2u);
    expect_price(book.best_bid()->price, 102.0, "best bid");
}

TEST(MapBookTest, RemoveLastRemainingLevel) {
    MapBook book;
    book.update_bid(100.0, 10.0);
    book.update_bid(100.0, 0.0);
    EXPECT_EQ(book.bid_depth(), 0u);
    EXPECT_FALSE(book.best_bid().has_value());
}

// ---------------------------------------------------------------------------
// Test 8: Clear
// ---------------------------------------------------------------------------

TEST(MapBookTest, ClearResetsBook) {
    MapBook book;
    book.update_bid(100.0, 10.0);
    book.update_ask(101.0, 5.0);
    book.clear();

    EXPECT_EQ(book.bid_depth(), 0u);
    EXPECT_EQ(book.ask_depth(), 0u);
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
}

// ---------------------------------------------------------------------------
// Test 9: is_crossed()
// ---------------------------------------------------------------------------

TEST(MapBookTest, IsCrossedReturnsFalseWhenEmpty) {
    MapBook book;
    EXPECT_FALSE(book.is_crossed());
}

TEST(MapBookTest, IsCrossedReturnsFalseWhenOneSideEmpty) {
    MapBook book;
    book.update_bid(100.0, 1.0);
    EXPECT_FALSE(book.is_crossed());
}

TEST(MapBookTest, IsCrossedReturnsFalseForNormalBook) {
    MapBook book;
    book.update_bid(99.0, 1.0);
    book.update_ask(101.0, 1.0);
    EXPECT_FALSE(book.is_crossed());
}

TEST(MapBookTest, IsLockedReturnsTrueWhenBidEqualsAsk) {
    MapBook book;
    book.update_bid(100.0, 1.0);
    book.update_ask(100.0, 1.0);
    // bid == ask is "locked", not "crossed" — matches ArrayBook semantics
    EXPECT_FALSE(book.is_crossed());
    EXPECT_TRUE(book.is_locked());
}

TEST(MapBookTest, IsLockedReturnsFalseForNormalBook) {
    MapBook book;
    book.update_bid(99.0, 1.0);
    book.update_ask(101.0, 1.0);
    EXPECT_FALSE(book.is_locked());
}

TEST(MapBookTest, IsCrossedReturnsTrueWhenBidExceedsAsk) {
    MapBook book;
    book.update_bid(101.0, 1.0);
    book.update_ask(100.0, 1.0);
    EXPECT_TRUE(book.is_crossed());
}

// ---------------------------------------------------------------------------
// Test 10: total_bid_qty() and total_ask_qty()
// ---------------------------------------------------------------------------

TEST(MapBookTest, TotalBidQtyEmptyBook) {
    MapBook book;
    EXPECT_NEAR(book.total_bid_qty(), 0.0, kEps);
}

TEST(MapBookTest, TotalBidQtySumsAllLevels) {
    MapBook book;
    book.update_bid(100.0, 10.0);
    book.update_bid(99.0, 20.0);
    book.update_bid(98.0, 30.0);
    EXPECT_NEAR(book.total_bid_qty(), 60.0, kEps);
}

TEST(MapBookTest, TotalAskQtySumsAllLevels) {
    MapBook book;
    book.update_ask(101.0, 5.0);
    book.update_ask(102.0, 15.0);
    EXPECT_NEAR(book.total_ask_qty(), 20.0, kEps);
}

TEST(MapBookTest, TotalQtyUpdatesAfterRemoval) {
    MapBook book;
    book.update_bid(100.0, 10.0);
    book.update_bid(99.0, 20.0);
    book.update_bid(100.0, 0.0); // remove
    EXPECT_NEAR(book.total_bid_qty(), 20.0, kEps);
}

// ---------------------------------------------------------------------------
// Test 11: level_count()
// ---------------------------------------------------------------------------

TEST(MapBookTest, LevelCountEmptyBook) {
    MapBook book;
    EXPECT_EQ(book.level_count(), 0u);
}

TEST(MapBookTest, LevelCountBothSides) {
    MapBook book;
    book.update_bid(100.0, 1.0);
    book.update_bid(99.0, 1.0);
    book.update_ask(101.0, 1.0);
    EXPECT_EQ(book.level_count(), 3u);
}

TEST(MapBookTest, LevelCountAfterClear) {
    MapBook book;
    book.update_bid(100.0, 1.0);
    book.update_ask(101.0, 1.0);
    book.clear();
    EXPECT_EQ(book.level_count(), 0u);
}

// ---------------------------------------------------------------------------
// Test 12: NaN price is rejected
// ---------------------------------------------------------------------------

TEST(MapBookTest, NanPriceIsIgnored) {
    MapBook book;
    book.update_bid(std::nan(""), 10.0);
    EXPECT_EQ(book.bid_depth(), 0u);

    book.update_ask(std::nan(""), 5.0);
    EXPECT_EQ(book.ask_depth(), 0u);

    // NaN removal also doesn't crash.
    book.update_bid(100.0, 10.0);
    book.update_bid(std::nan(""), 0.0);
    EXPECT_EQ(book.bid_depth(), 1u);
}

// ---------------------------------------------------------------------------
// Test 13: Simulated Binance-style depth update sequence
// ---------------------------------------------------------------------------

TEST(MapBookTest, BinanceDepthUpdateSequence) {
    MapBook book;

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

    // Verify initial BBO.
    ASSERT_TRUE(book.best_bid().has_value());
    ASSERT_TRUE(book.best_ask().has_value());
    expect_price(book.best_bid()->price, 50000.00, "initial best bid");
    expect_price(book.best_ask()->price, 50000.50, "initial best ask");
    EXPECT_NEAR(*book.spread(), 0.50, kEps);

    // --- Incremental update 1: qty change on existing bid ---
    book.update_bid(49999.50, 5.0);
    // Verify the update took effect (check via top_bids).
    auto bids = book.top_bids(5);
    // 49999.50 is the second level.
    ASSERT_GE(bids.size(), 2u);
    expect_price(bids[1].price, 49999.50, "second bid price");
    EXPECT_NEAR(bids[1].qty, 5.0, kEps);

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
// Test 14: Rapid insert-remove cycles preserve invariants
// ---------------------------------------------------------------------------

TEST(MapBookTest, InsertRemoveCyclesBidSide) {
    MapBook book;

    for (int i = 0; i < 100; ++i) {
        double price = 100.0 + static_cast<double>(i % 7);
        book.update_bid(price, 1.0);
        // Occasionally remove.
        if (i % 3 == 0) {
            book.update_bid(price, 0.0);
        }
    }

    // Verify the book is consistent: top bids are sorted descending.
    auto bids = book.top_bids(book.bid_depth());
    for (std::size_t i = 1; i < bids.size(); ++i) {
        EXPECT_GT(bids[i - 1].price, bids[i].price)
            << "bid sort violated at index " << i;
    }
}

// ---------------------------------------------------------------------------
// Test 15: Deep book (100+ levels)
// ---------------------------------------------------------------------------

TEST(MapBookTest, DeepBookHundredPlusLevels) {
    MapBook book;

    // Insert 500 bid levels and 500 ask levels.
    for (int i = 0; i < 500; ++i) {
        double bid_price = 10000.0 - static_cast<double>(i) * 0.01;
        double ask_price = 10000.01 + static_cast<double>(i) * 0.01;
        book.update_bid(bid_price, 1.0 + static_cast<double>(i) * 0.1);
        book.update_ask(ask_price, 1.0 + static_cast<double>(i) * 0.1);
    }

    EXPECT_EQ(book.bid_depth(), 500u);
    EXPECT_EQ(book.ask_depth(), 500u);
    EXPECT_EQ(book.level_count(), 1000u);

    // BBO should be the tightest prices.
    expect_price(book.best_bid()->price, 10000.0, "best bid in deep book");
    expect_price(book.best_ask()->price, 10000.01, "best ask in deep book");
    EXPECT_NEAR(*book.spread(), 0.01, kEps);

    // Remove best bid; next level becomes best.
    book.update_bid(10000.0, 0.0);
    EXPECT_EQ(book.bid_depth(), 499u);
    expect_price(book.best_bid()->price, 9999.99, "new best bid after removal");
}

// ---------------------------------------------------------------------------
// Test 16: top_bids / top_asks extraction
// ---------------------------------------------------------------------------

TEST(MapBookTest, TopBidsExtraction) {
    MapBook book;
    book.update_bid(100.0, 1.0);
    book.update_bid(99.0, 2.0);
    book.update_bid(98.0, 3.0);
    book.update_bid(97.0, 4.0);
    book.update_bid(96.0, 5.0);

    // Request fewer than available.
    auto top3 = book.top_bids(3);
    ASSERT_EQ(top3.size(), 3u);
    expect_price(top3[0].price, 100.0, "top bid 1");
    expect_price(top3[1].price, 99.0, "top bid 2");
    expect_price(top3[2].price, 98.0, "top bid 3");
    EXPECT_NEAR(top3[0].qty, 1.0, kEps);
    EXPECT_NEAR(top3[1].qty, 2.0, kEps);
    EXPECT_NEAR(top3[2].qty, 3.0, kEps);

    // Request more than available.
    auto all = book.top_bids(100);
    ASSERT_EQ(all.size(), 5u);
}

TEST(MapBookTest, TopAsksExtraction) {
    MapBook book;
    book.update_ask(101.0, 1.0);
    book.update_ask(102.0, 2.0);
    book.update_ask(103.0, 3.0);
    book.update_ask(104.0, 4.0);

    auto top2 = book.top_asks(2);
    ASSERT_EQ(top2.size(), 2u);
    expect_price(top2[0].price, 101.0, "top ask 1");
    expect_price(top2[1].price, 102.0, "top ask 2");
    EXPECT_NEAR(top2[0].qty, 1.0, kEps);
    EXPECT_NEAR(top2[1].qty, 2.0, kEps);

    // Request more than available.
    auto all = book.top_asks(100);
    ASSERT_EQ(all.size(), 4u);
}

TEST(MapBookTest, TopNOnEmptyBookReturnsEmpty) {
    MapBook book;
    EXPECT_TRUE(book.top_bids(5).empty());
    EXPECT_TRUE(book.top_asks(5).empty());
}

// ---------------------------------------------------------------------------
// Test 17: Cross-validation — MapBook and ArrayBook agree on same input
// ---------------------------------------------------------------------------

TEST(MapBookTest, CrossValidationWithArrayBook) {
    MapBook map_book;
    ArrayBook<20> arr_book;

    // Feed identical sequence to both books.
    struct Update { bool is_bid; double price; double qty; };
    const Update updates[] = {
        {true,  100.0, 10.0},
        {true,  99.0,  20.0},
        {true,  98.0,  30.0},
        {false, 101.0, 5.0},
        {false, 102.0, 15.0},
        {false, 103.0, 25.0},
        // Update existing levels.
        {true,  99.0,  50.0},
        {false, 102.0, 8.0},
        // Remove levels.
        {true,  98.0,  0.0},
        {false, 103.0, 0.0},
        // Add new best.
        {true,  100.5, 7.0},
        {false, 100.8, 3.0},
    };

    for (const auto& u : updates) {
        if (u.is_bid) {
            map_book.update_bid(u.price, u.qty);
            arr_book.update_bid(u.price, u.qty);
        } else {
            map_book.update_ask(u.price, u.qty);
            arr_book.update_ask(u.price, u.qty);
        }
    }

    // Compare BBO.
    ASSERT_TRUE(map_book.best_bid().has_value());
    ASSERT_TRUE(arr_book.best_bid().has_value());
    expect_price(map_book.best_bid()->price, arr_book.best_bid()->price,
                 "best bid matches");
    expect_price(map_book.best_bid()->qty, arr_book.best_bid()->qty,
                 "best bid qty matches");

    ASSERT_TRUE(map_book.best_ask().has_value());
    ASSERT_TRUE(arr_book.best_ask().has_value());
    expect_price(map_book.best_ask()->price, arr_book.best_ask()->price,
                 "best ask matches");
    expect_price(map_book.best_ask()->qty, arr_book.best_ask()->qty,
                 "best ask qty matches");

    // Compare derived values.
    EXPECT_NEAR(*map_book.mid_price(), *arr_book.mid_price(), kEps);
    EXPECT_NEAR(*map_book.spread(), *arr_book.spread(), kEps);
    EXPECT_EQ(map_book.bid_depth(), arr_book.bid_depth());
    EXPECT_EQ(map_book.ask_depth(), arr_book.ask_depth());
    EXPECT_EQ(map_book.level_count(), arr_book.level_count());
    EXPECT_NEAR(map_book.total_bid_qty(), arr_book.total_bid_qty(), kEps);
    EXPECT_NEAR(map_book.total_ask_qty(), arr_book.total_ask_qty(), kEps);
    EXPECT_EQ(map_book.is_crossed(), arr_book.is_crossed());
}
