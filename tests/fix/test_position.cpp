#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <unordered_map>

#include "eph/fix/position.hpp"

using namespace eph::fix;

// Tolerance for floating-point comparisons.
static constexpr double kEps = 1e-9;

// ---------------------------------------------------------------------------
// Empty tracker returns zero position
// ---------------------------------------------------------------------------
TEST(PositionTracker, EmptyTrackerReturnsZeroPosition)
{
    PositionTracker tracker;

    const auto& pos = tracker.get("AAPL");
    EXPECT_DOUBLE_EQ(pos.qty, 0.0);
    EXPECT_DOUBLE_EQ(pos.avg_price, 0.0);
    EXPECT_DOUBLE_EQ(pos.realized_pnl, 0.0);
    EXPECT_DOUBLE_EQ(pos.notional, 0.0);
    EXPECT_EQ(pos.trade_count, 0u);

    EXPECT_FALSE(tracker.has_position("AAPL"));
    EXPECT_TRUE(tracker.positions().empty());
    EXPECT_DOUBLE_EQ(tracker.total_realized_pnl(), 0.0);
    EXPECT_DOUBLE_EQ(tracker.net_exposure(), 0.0);
}

// ---------------------------------------------------------------------------
// Buy then sell: correct realized PnL
// ---------------------------------------------------------------------------
TEST(PositionTracker, BuyThenSellCorrectPnl)
{
    PositionTracker tracker;

    // Buy 100 @ 50.0
    tracker.on_fill("AAPL", '1', 100.0, 50.0);
    EXPECT_TRUE(tracker.has_position("AAPL"));
    EXPECT_DOUBLE_EQ(tracker.get("AAPL").qty, 100.0);
    EXPECT_DOUBLE_EQ(tracker.get("AAPL").avg_price, 50.0);
    EXPECT_DOUBLE_EQ(tracker.get("AAPL").notional, 5000.0);

    // Sell 100 @ 55.0 -> realized PnL = (55 - 50) * 100 = 500
    tracker.on_fill("AAPL", '2', 100.0, 55.0);
    EXPECT_NEAR(tracker.get("AAPL").qty, 0.0, kEps);
    EXPECT_NEAR(tracker.get("AAPL").realized_pnl, 500.0, kEps);
    EXPECT_EQ(tracker.get("AAPL").trade_count, 2u);
}

// ---------------------------------------------------------------------------
// Multiple buys compute correct average price
// ---------------------------------------------------------------------------
TEST(PositionTracker, MultipleBuysAveragePrice)
{
    PositionTracker tracker;

    // Buy 100 @ 50.0, then buy 200 @ 53.0
    // avg = (100*50 + 200*53) / 300 = 15600 / 300 = 52.0
    tracker.on_fill("TSLA", '1', 100.0, 50.0);
    tracker.on_fill("TSLA", '1', 200.0, 53.0);

    EXPECT_DOUBLE_EQ(tracker.get("TSLA").qty, 300.0);
    EXPECT_NEAR(tracker.get("TSLA").avg_price, 52.0, kEps);
    EXPECT_DOUBLE_EQ(tracker.get("TSLA").notional, 300.0 * 52.0);
    EXPECT_EQ(tracker.get("TSLA").trade_count, 2u);
}

// ---------------------------------------------------------------------------
// Go long then short (cross zero)
// ---------------------------------------------------------------------------
TEST(PositionTracker, CrossZeroLongToShort)
{
    PositionTracker tracker;

    // Buy 100 @ 50.0
    tracker.on_fill("MSFT", '1', 100.0, 50.0);

    // Sell 150 @ 55.0 -> close 100 long for PnL = (55-50)*100 = 500
    //                  -> open 50 short @ 55.0
    tracker.on_fill("MSFT", '2', 150.0, 55.0);

    EXPECT_NEAR(tracker.get("MSFT").qty, -50.0, kEps);
    EXPECT_NEAR(tracker.get("MSFT").avg_price, 55.0, kEps);
    EXPECT_NEAR(tracker.get("MSFT").realized_pnl, 500.0, kEps);
    EXPECT_NEAR(tracker.get("MSFT").notional, 50.0 * 55.0, kEps);
}

// ---------------------------------------------------------------------------
// Short then buy (cross zero, short side)
// ---------------------------------------------------------------------------
TEST(PositionTracker, CrossZeroShortToLong)
{
    PositionTracker tracker;

    // Sell 100 @ 60.0 (open short)
    tracker.on_fill("GOOG", '2', 100.0, 60.0);
    EXPECT_DOUBLE_EQ(tracker.get("GOOG").qty, -100.0);
    EXPECT_DOUBLE_EQ(tracker.get("GOOG").avg_price, 60.0);

    // Buy 150 @ 55.0 -> close 100 short for PnL = (60-55)*100 = 500
    //                 -> open 50 long @ 55.0
    tracker.on_fill("GOOG", '1', 150.0, 55.0);

    EXPECT_NEAR(tracker.get("GOOG").qty, 50.0, kEps);
    EXPECT_NEAR(tracker.get("GOOG").avg_price, 55.0, kEps);
    EXPECT_NEAR(tracker.get("GOOG").realized_pnl, 500.0, kEps);
}

// ---------------------------------------------------------------------------
// Total realized PnL across multiple symbols
// ---------------------------------------------------------------------------
TEST(PositionTracker, TotalRealizedPnl)
{
    PositionTracker tracker;

    // AAPL: buy 100 @ 50, sell 100 @ 55 -> PnL = 500
    tracker.on_fill("AAPL", '1', 100.0, 50.0);
    tracker.on_fill("AAPL", '2', 100.0, 55.0);

    // TSLA: sell 50 @ 200, buy 50 @ 190 -> PnL = (200-190)*50 = 500
    tracker.on_fill("TSLA", '2', 50.0, 200.0);
    tracker.on_fill("TSLA", '1', 50.0, 190.0);

    EXPECT_NEAR(tracker.total_realized_pnl(), 1000.0, kEps);
}

// ---------------------------------------------------------------------------
// Unrealized PnL at market price
// ---------------------------------------------------------------------------
TEST(PositionTracker, UnrealizedPnlAtMarketPrice)
{
    PositionTracker tracker;

    // Long 100 AAPL @ 50
    tracker.on_fill("AAPL", '1', 100.0, 50.0);
    // Short 200 TSLA @ 300
    tracker.on_fill("TSLA", '2', 200.0, 300.0);

    std::unordered_map<std::string, double> market_prices = {
        {"AAPL", 55.0},   // unrealized = (55-50)*100 = 500
        {"TSLA", 290.0},  // unrealized = (290-300)*(-200) = 2000
    };

    // Total = 500 + 2000 = 2500
    EXPECT_NEAR(tracker.total_unrealized_pnl(market_prices), 2500.0, kEps);
}

// ---------------------------------------------------------------------------
// Unrealized PnL: missing market price skips symbol
// ---------------------------------------------------------------------------
TEST(PositionTracker, UnrealizedPnlMissingPriceSkipsSymbol)
{
    PositionTracker tracker;
    tracker.on_fill("AAPL", '1', 100.0, 50.0);

    // Empty market prices -- AAPL is skipped.
    std::unordered_map<std::string, double> empty_prices;
    EXPECT_DOUBLE_EQ(tracker.total_unrealized_pnl(empty_prices), 0.0);
}

// ---------------------------------------------------------------------------
// Net exposure
// ---------------------------------------------------------------------------
TEST(PositionTracker, NetExposure)
{
    PositionTracker tracker;

    // Long 100 AAPL @ 50  -> notional = 5000
    tracker.on_fill("AAPL", '1', 100.0, 50.0);
    // Short 200 TSLA @ 300 -> notional = 60000
    tracker.on_fill("TSLA", '2', 200.0, 300.0);

    EXPECT_NEAR(tracker.net_exposure(), 65000.0, kEps);
}

// ---------------------------------------------------------------------------
// Partial close preserves average price
// ---------------------------------------------------------------------------
TEST(PositionTracker, PartialClosePreservesAvgPrice)
{
    PositionTracker tracker;

    tracker.on_fill("AAPL", '1', 100.0, 50.0);
    // Sell only 40 @ 55 -> realized = (55-50)*40 = 200, remaining 60 @ avg 50
    tracker.on_fill("AAPL", '2', 40.0, 55.0);

    EXPECT_NEAR(tracker.get("AAPL").qty, 60.0, kEps);
    EXPECT_NEAR(tracker.get("AAPL").avg_price, 50.0, kEps);
    EXPECT_NEAR(tracker.get("AAPL").realized_pnl, 200.0, kEps);
    EXPECT_NEAR(tracker.get("AAPL").notional, 60.0 * 50.0, kEps);
}

// ---------------------------------------------------------------------------
// Clear resets everything
// ---------------------------------------------------------------------------
TEST(PositionTracker, ClearResetsAll)
{
    PositionTracker tracker;
    tracker.on_fill("AAPL", '1', 100.0, 50.0);
    EXPECT_FALSE(tracker.positions().empty());

    tracker.clear();
    EXPECT_TRUE(tracker.positions().empty());
    EXPECT_DOUBLE_EQ(tracker.total_realized_pnl(), 0.0);
    EXPECT_DOUBLE_EQ(tracker.net_exposure(), 0.0);
}

// ---------------------------------------------------------------------------
// Invalid fills are rejected
// ---------------------------------------------------------------------------
TEST(PositionTracker, InvalidFillsRejected)
{
    PositionTracker tracker;

    // Negative qty
    tracker.on_fill("AAPL", '1', -10.0, 50.0);
    EXPECT_FALSE(tracker.has_position("AAPL"));

    // Zero price
    tracker.on_fill("AAPL", '1', 10.0, 0.0);
    EXPECT_FALSE(tracker.has_position("AAPL"));

    // Bad side
    tracker.on_fill("AAPL", 'X', 10.0, 50.0);
    EXPECT_FALSE(tracker.has_position("AAPL"));
}
