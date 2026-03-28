#include <gtest/gtest.h>

#include "eph/fix/risk_check.hpp"

using namespace eph::fix;

// ---------------------------------------------------------------------------
// Order within all limits passes
// ---------------------------------------------------------------------------
TEST(RiskChecker, OrderWithinLimitsPasses)
{
    RiskLimits limits;
    limits.max_order_qty         = 1000.0;
    limits.max_order_notional    = 100000.0;
    limits.max_position_qty      = 5000.0;
    limits.max_position_notional = 500000.0;
    limits.max_total_exposure    = 1000000.0;

    RiskChecker checker(limits);
    PositionTracker positions;

    // Small order well within all limits.
    auto result = checker.check_order("AAPL", '1', 100.0, 150.0, positions);
    EXPECT_EQ(result, RiskRejectReason::kOk);
}

// ---------------------------------------------------------------------------
// Order qty exceeded
// ---------------------------------------------------------------------------
TEST(RiskChecker, OrderQtyExceeded)
{
    RiskLimits limits;
    limits.max_order_qty = 500.0;

    RiskChecker checker(limits);
    PositionTracker positions;

    // Exactly at limit should pass.
    EXPECT_EQ(checker.check_order("AAPL", '1', 500.0, 100.0, positions),
              RiskRejectReason::kOk);

    // Over limit should be rejected.
    EXPECT_EQ(checker.check_order("AAPL", '1', 501.0, 100.0, positions),
              RiskRejectReason::kOrderQtyExceeded);
}

// ---------------------------------------------------------------------------
// Order notional exceeded
// ---------------------------------------------------------------------------
TEST(RiskChecker, OrderNotionalExceeded)
{
    RiskLimits limits;
    limits.max_order_notional = 50000.0;

    RiskChecker checker(limits);
    PositionTracker positions;

    // 100 * 499 = 49900 -> pass
    EXPECT_EQ(checker.check_order("AAPL", '1', 100.0, 499.0, positions),
              RiskRejectReason::kOk);

    // 100 * 501 = 50100 -> reject
    EXPECT_EQ(checker.check_order("AAPL", '1', 100.0, 501.0, positions),
              RiskRejectReason::kOrderNotionalExceeded);
}

// ---------------------------------------------------------------------------
// Position qty would be exceeded
// ---------------------------------------------------------------------------
TEST(RiskChecker, PositionQtyExceeded)
{
    RiskLimits limits;
    limits.max_position_qty = 500.0;

    RiskChecker checker(limits);
    PositionTracker positions;

    // Existing position: long 400.
    positions.on_fill("AAPL", '1', 400.0, 150.0);

    // Buy 100 more -> projected 500 -> at limit, should pass.
    EXPECT_EQ(checker.check_order("AAPL", '1', 100.0, 150.0, positions),
              RiskRejectReason::kOk);

    // Buy 101 more -> projected 501 -> reject.
    EXPECT_EQ(checker.check_order("AAPL", '1', 101.0, 150.0, positions),
              RiskRejectReason::kPositionQtyExceeded);
}

// ---------------------------------------------------------------------------
// Position qty check works for sells (short side)
// ---------------------------------------------------------------------------
TEST(RiskChecker, PositionQtyExceededShortSide)
{
    RiskLimits limits;
    limits.max_position_qty = 500.0;

    RiskChecker checker(limits);
    PositionTracker positions;

    // Existing position: short 400.
    positions.on_fill("AAPL", '2', 400.0, 150.0);

    // Sell 101 more -> projected abs(-400 + -101) = 501 -> reject.
    EXPECT_EQ(checker.check_order("AAPL", '2', 101.0, 150.0, positions),
              RiskRejectReason::kPositionQtyExceeded);
}

// ---------------------------------------------------------------------------
// Position notional exceeded
// ---------------------------------------------------------------------------
TEST(RiskChecker, PositionNotionalExceeded)
{
    RiskLimits limits;
    limits.max_position_notional = 100000.0;

    RiskChecker checker(limits);
    PositionTracker positions;

    // Existing position: long 200 @ 150.
    positions.on_fill("AAPL", '1', 200.0, 150.0);

    // Buy 500 more @ 150 -> projected qty = 700, notional = 700*150 = 105000 -> reject.
    EXPECT_EQ(checker.check_order("AAPL", '1', 500.0, 150.0, positions),
              RiskRejectReason::kPositionNotionalExceeded);

    // Buy 100 more @ 150 -> projected qty = 300, notional = 300*150 = 45000 -> pass.
    EXPECT_EQ(checker.check_order("AAPL", '1', 100.0, 150.0, positions),
              RiskRejectReason::kOk);
}

// ---------------------------------------------------------------------------
// Total exposure exceeded
// ---------------------------------------------------------------------------
TEST(RiskChecker, TotalExposureExceeded)
{
    RiskLimits limits;
    limits.max_total_exposure = 100000.0;

    RiskChecker checker(limits);
    PositionTracker positions;

    // Build up exposure: long 500 AAPL @ 150 = 75000 notional.
    positions.on_fill("AAPL", '1', 500.0, 150.0);

    // New order: buy 200 TSLA @ 200 = 40000 notional.
    // Total = 75000 + 40000 = 115000 -> reject.
    EXPECT_EQ(checker.check_order("TSLA", '1', 200.0, 200.0, positions),
              RiskRejectReason::kTotalExposureExceeded);

    // Smaller order: buy 100 TSLA @ 200 = 20000.
    // Total = 75000 + 20000 = 95000 -> pass.
    EXPECT_EQ(checker.check_order("TSLA", '1', 100.0, 200.0, positions),
              RiskRejectReason::kOk);
}

// ---------------------------------------------------------------------------
// All limits disabled (0) passes everything
// ---------------------------------------------------------------------------
TEST(RiskChecker, AllLimitsDisabledPassesEverything)
{
    RiskLimits limits;  // All zeros by default.

    RiskChecker checker(limits);
    PositionTracker positions;

    // Even absurd orders should pass when all limits are disabled.
    positions.on_fill("AAPL", '1', 1e9, 1e6);
    EXPECT_EQ(checker.check_order("AAPL", '1', 1e9, 1e6, positions),
              RiskRejectReason::kOk);
}

// ---------------------------------------------------------------------------
// Reject reason names
// ---------------------------------------------------------------------------
TEST(RiskChecker, RejectReasonNames)
{
    EXPECT_EQ(risk_reject_name(RiskRejectReason::kOk), "Ok");
    EXPECT_EQ(risk_reject_name(RiskRejectReason::kOrderQtyExceeded), "OrderQtyExceeded");
    EXPECT_EQ(risk_reject_name(RiskRejectReason::kOrderNotionalExceeded),
              "OrderNotionalExceeded");
    EXPECT_EQ(risk_reject_name(RiskRejectReason::kPositionQtyExceeded),
              "PositionQtyExceeded");
    EXPECT_EQ(risk_reject_name(RiskRejectReason::kPositionNotionalExceeded),
              "PositionNotionalExceeded");
    EXPECT_EQ(risk_reject_name(RiskRejectReason::kTotalExposureExceeded),
              "TotalExposureExceeded");
    EXPECT_EQ(risk_reject_name(RiskRejectReason::kRateLimitExceeded),
              "RateLimitExceeded");
}

// ---------------------------------------------------------------------------
// set_limits updates at runtime
// ---------------------------------------------------------------------------
TEST(RiskChecker, SetLimitsUpdatesAtRuntime)
{
    RiskLimits limits;
    limits.max_order_qty = 100.0;
    RiskChecker checker(limits);
    PositionTracker positions;

    // Rejected with original limit.
    EXPECT_EQ(checker.check_order("AAPL", '1', 200.0, 50.0, positions),
              RiskRejectReason::kOrderQtyExceeded);

    // Raise the limit.
    RiskLimits new_limits;
    new_limits.max_order_qty = 500.0;
    checker.set_limits(new_limits);

    // Now passes.
    EXPECT_EQ(checker.check_order("AAPL", '1', 200.0, 50.0, positions),
              RiskRejectReason::kOk);

    // Verify accessor.
    EXPECT_DOUBLE_EQ(checker.limits().max_order_qty, 500.0);
}
