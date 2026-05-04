#include <gtest/gtest.h>

#include <limits>

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

// ---------------------------------------------------------------------------
// Reducing order: sell when long should reduce exposure, not be rejected
// ---------------------------------------------------------------------------
TEST(RiskChecker, SellWhenLongReducesExposure)
{
    RiskLimits limits;
    limits.max_position_qty      = 500.0;
    limits.max_total_exposure    = 100000.0;

    RiskChecker checker(limits);
    PositionTracker positions;

    // Long 400 @ 150 -> notional = 60000
    positions.on_fill("AAPL", '1', 400.0, 150.0);

    // Selling 200 when long 400 -> projected_qty = abs(400 - 200) = 200
    // This REDUCES the position and should pass, even though
    // the order qty (200) + current qty (400) = 600 > 500 if naively summed.
    EXPECT_EQ(checker.check_order("AAPL", '2', 200.0, 150.0, positions),
              RiskRejectReason::kOk);

    // Projected position notional = 200 * 150 = 30000 (less than current 60000).
    // Projected total exposure = 60000 - 60000 + 30000 = 30000 -> well within 100000.
}

// ---------------------------------------------------------------------------
// Reducing order: buy when short should reduce exposure, not be rejected
// ---------------------------------------------------------------------------
TEST(RiskChecker, BuyWhenShortReducesExposure)
{
    RiskLimits limits;
    limits.max_position_qty      = 500.0;
    limits.max_total_exposure    = 100000.0;

    RiskChecker checker(limits);
    PositionTracker positions;

    // Short 400 @ 200 -> notional = 80000
    positions.on_fill("AAPL", '2', 400.0, 200.0);

    // Buying 300 when short 400 -> projected_qty = abs(-400 + 300) = 100
    // This REDUCES the position and should pass.
    EXPECT_EQ(checker.check_order("AAPL", '1', 300.0, 200.0, positions),
              RiskRejectReason::kOk);
}

// ---------------------------------------------------------------------------
// Projected exposure calculation correctness with reducing order
// ---------------------------------------------------------------------------
TEST(RiskChecker, ProjectedExposureCorrectWithReducingOrder)
{
    RiskLimits limits;
    limits.max_total_exposure = 100000.0;

    RiskChecker checker(limits);
    PositionTracker positions;

    // Long 500 AAPL @ 150 -> notional = 75000
    positions.on_fill("AAPL", '1', 500.0, 150.0);
    // Long 100 TSLA @ 200 -> notional = 20000
    positions.on_fill("TSLA", '1', 100.0, 200.0);
    // Total exposure = 95000

    // Selling 300 AAPL @ 150 -> projected AAPL qty = 200, notional = 30000
    // Projected total = 95000 - 75000 + 30000 = 50000 -> passes
    EXPECT_EQ(checker.check_order("AAPL", '2', 300.0, 150.0, positions),
              RiskRejectReason::kOk);

    // Buying 200 more TSLA @ 200 -> projected TSLA qty = 300, notional = 60000
    // Projected total = 95000 - 20000 + 60000 = 135000 -> exceeds 100000
    EXPECT_EQ(checker.check_order("TSLA", '1', 200.0, 200.0, positions),
              RiskRejectReason::kTotalExposureExceeded);
}

// ---------------------------------------------------------------------------
// Invalid Side handling
//
// Regression for batch-4 protocol bug: RiskChecker silently treated any
// non-'1' side byte as Sell when computing the signed qty for position /
// exposure projections. So a caller that passed Binance-style 'B'/'S'
// (or any garbage byte) would get a Buy projected as a Sell, which can
// hide a real position-qty / exposure breach in a Buy-direction order.
//
// PositionTracker::on_fill already validates side ∈ {'1','2'} and refuses
// to update on anything else; the matching invariant must hold here too,
// otherwise the projected Position used by check_order drifts away from
// the on_fill state machine.
// ---------------------------------------------------------------------------

TEST(RiskChecker, InvalidSideRejectedAsInvalidInput) {
    // Side is the FIX 4.4 single-char code: '1' = Buy, '2' = Sell.
    // Anything else (e.g. Binance 'B'/'S', empty NUL, garbage) is malformed
    // and must be flagged at the risk gate, not silently coerced to Sell.
    RiskLimits limits;
    limits.max_order_qty = 100.0;
    RiskChecker checker(limits);
    PositionTracker positions;

    // 'B' (binance buy) — accidentally passing exchange-native code instead
    // of the FIX code. Must be rejected, not silently treated as Sell.
    EXPECT_EQ(checker.check_order("AAPL", 'B', 50.0, 150.0, positions),
              RiskRejectReason::kInvalidInput)
        << "side='B' is not a valid FIX Side; risk checker must reject "
           "rather than silently treating as Sell";

    // 'S' (binance sell) — same rationale.
    EXPECT_EQ(checker.check_order("AAPL", 'S', 50.0, 150.0, positions),
              RiskRejectReason::kInvalidInput);

    // NUL byte — uninitialized buffer / zeroed struct.
    EXPECT_EQ(checker.check_order("AAPL", '\0', 50.0, 150.0, positions),
              RiskRejectReason::kInvalidInput);

    // ASCII '0' is *not* a side — only '1' (buy) or '2' (sell) are valid.
    EXPECT_EQ(checker.check_order("AAPL", '0', 50.0, 150.0, positions),
              RiskRejectReason::kInvalidInput);
}

TEST(RiskChecker, InvalidSideDoesNotMaskBuyDirectionExposureBreach) {
    // Concrete demonstration of why the lenient ternary is dangerous: a
    // Buy that would breach max_position_qty must be rejected. If 'B' is
    // silently treated as Sell, the projection sees a *short* position
    // and passes — a real production breach.
    RiskLimits limits;
    limits.max_position_qty = 100.0;  // tight cap
    RiskChecker checker(limits);
    PositionTracker positions;
    // Existing long 80; a buy of 30 would project qty 110 — breach.
    positions.on_fill("AAPL", '1', 80.0, 150.0);

    // Sanity: with a *correct* FIX side, the breach is detected.
    EXPECT_EQ(checker.check_order("AAPL", '1', 30.0, 150.0, positions),
              RiskRejectReason::kPositionQtyExceeded);

    // With a Binance-native 'B', the same logical buy must STILL be
    // detected as a breach (the ternary used to map 'B' → -1, projecting
    // qty = |80 - 30| = 50, which slipped under the 100 cap).
    EXPECT_NE(checker.check_order("AAPL", 'B', 30.0, 150.0, positions),
              RiskRejectReason::kOk)
        << "exchange-native 'B' must NOT silently slip a position breach";
}

// ---------------------------------------------------------------------------
// Non-finite (NaN / Inf) qty or price — caller bug class
// ---------------------------------------------------------------------------
//
// The risk checker computes notional, projected qty, projected exposure as
// double arithmetic. NaN inputs propagate through and silently bypass every
// `>` comparison (NaN > x is always false), so the order would pass all
// risk checks despite being malformed. Inf produces a `notional = inf`
// that DOES trip max_order_notional but obscures the original bug.
// `check_order` returns kInvalidInput for both NaN and Inf to surface
// the upstream bug at the gate rather than at a venue 401.

TEST(RiskChecker, NaNQtyRejected) {
    RiskLimits limits;
    limits.max_order_qty      = 100.0;
    limits.max_order_notional = 50000.0;
    RiskChecker checker(limits);
    PositionTracker positions;

    const double nan_qty = std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(checker.check_order("AAPL", '1', nan_qty, 150.0, positions),
              RiskRejectReason::kInvalidInput)
        << "NaN qty must NOT silently bypass risk comparisons";
}

TEST(RiskChecker, NaNPriceRejected) {
    RiskLimits limits;
    limits.max_order_qty      = 100.0;
    limits.max_order_notional = 50000.0;
    RiskChecker checker(limits);
    PositionTracker positions;

    const double nan_px = std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(checker.check_order("AAPL", '1', 50.0, nan_px, positions),
              RiskRejectReason::kInvalidInput);
}

TEST(RiskChecker, InfiniteQtyRejected) {
    RiskLimits limits;
    limits.max_order_qty = 100.0;
    RiskChecker checker(limits);
    PositionTracker positions;

    const double inf_qty = std::numeric_limits<double>::infinity();
    EXPECT_EQ(checker.check_order("AAPL", '1', inf_qty, 150.0, positions),
              RiskRejectReason::kInvalidInput);
}

TEST(RiskChecker, InfinitePriceRejected) {
    RiskLimits limits;
    limits.max_order_notional = 50000.0;
    RiskChecker checker(limits);
    PositionTracker positions;

    const double inf_px = std::numeric_limits<double>::infinity();
    EXPECT_EQ(checker.check_order("AAPL", '1', 50.0, inf_px, positions),
              RiskRejectReason::kInvalidInput);
}

// ---------------------------------------------------------------------------
// RiskLimits Config API tests
// ---------------------------------------------------------------------------

TEST(RiskLimits, ValidateAcceptsDefaults) {
    RiskLimits limits{};
    EXPECT_TRUE(limits.validate().empty());
}

TEST(RiskLimits, ValidateAcceptsPositiveValues) {
    RiskLimits limits{
        .max_order_qty = 100.0,
        .max_order_notional = 50000.0,
        .max_position_qty = 1000.0,
        .max_position_notional = 500000.0,
        .max_total_exposure = 1000000.0,
        .max_orders_per_second = 10,
    };
    EXPECT_TRUE(limits.validate().empty());
}

TEST(RiskLimits, ValidateRejectsNegativeOrderQty) {
    RiskLimits limits{.max_order_qty = -1.0};
    EXPECT_FALSE(limits.validate().empty());
}

TEST(RiskLimits, ValidateRejectsNegativeNotional) {
    RiskLimits limits{.max_order_notional = -100.0};
    EXPECT_FALSE(limits.validate().empty());
}

TEST(RiskLimits, ValidateRejectsNegativeRateLimit) {
    RiskLimits limits{.max_orders_per_second = -1};
    EXPECT_FALSE(limits.validate().empty());
}

// Cover the three field-validation branches that were not exercised
// individually — each branch carries a distinct error message that callers
// surface to operators.  Pinning the exact message text catches accidental
// reordering of the validate() chain.
TEST(RiskLimits, ValidateRejectsNegativePositionQty) {
    RiskLimits limits{.max_position_qty = -1.0};
    EXPECT_EQ(limits.validate(), "max_position_qty must be >= 0 (0 disables)");
}

TEST(RiskLimits, ValidateRejectsNegativePositionNotional) {
    RiskLimits limits{.max_position_notional = -1.0};
    EXPECT_EQ(limits.validate(),
              "max_position_notional must be >= 0 (0 disables)");
}

TEST(RiskLimits, ValidateRejectsNegativeTotalExposure) {
    RiskLimits limits{.max_total_exposure = -1.0};
    EXPECT_EQ(limits.validate(),
              "max_total_exposure must be >= 0 (0 disables)");
}

// validate() is short-circuit — only the first violated field's message is
// returned. Mixing two negatives must surface only the earlier one.
// Encodes the documented evaluation order; if someone reorders the chain
// without updating callers, the test will flag it.
TEST(RiskLimits, ValidateReportsFirstViolationOnly) {
    RiskLimits limits{.max_order_qty = -1.0, .max_position_notional = -2.0};
    EXPECT_EQ(limits.validate(), "max_order_qty must be >= 0 (0 disables)");
}

TEST(RiskLimits, DumpContainsFieldNames) {
    RiskLimits limits{.max_order_qty = 100.0};
    auto d = limits.dump();
    EXPECT_NE(d.find("RiskLimits"), std::string::npos);
    EXPECT_NE(d.find("100.00"), std::string::npos);
    EXPECT_NE(d.find("disabled"), std::string::npos);
}

TEST(RiskLimits, DumpShowsDisabledForZero) {
    RiskLimits limits{};
    auto d = limits.dump();
    // All fields should show "disabled"
    auto count_disabled = [&] {
        size_t count = 0, pos = 0;
        while ((pos = d.find("disabled", pos)) != std::string::npos) {
            ++count; pos += 8;
        }
        return count;
    }();
    EXPECT_EQ(count_disabled, 6u); // 6 fields
}

TEST(RiskLimits, ToJsonIsValidStructure) {
    RiskLimits limits{.max_order_qty = 100.5, .max_orders_per_second = 10};
    auto j = limits.to_json();
    EXPECT_TRUE(j.starts_with("{"));
    EXPECT_TRUE(j.ends_with("}"));
    EXPECT_NE(j.find("\"max_order_qty\":100.5"), std::string::npos);
    EXPECT_NE(j.find("\"max_orders_per_second\":10"), std::string::npos);
}

TEST(RiskLimits, EqualityMatchesIdentical) {
    RiskLimits a{.max_order_qty = 100.0, .max_orders_per_second = 5};
    RiskLimits b = a;
    EXPECT_EQ(a, b);
}

TEST(RiskLimits, EqualityDetectsDifferences) {
    RiskLimits a{.max_order_qty = 100.0};
    RiskLimits b{.max_order_qty = 200.0};
    EXPECT_NE(a, b);
}

TEST(RiskLimits, WarningsAllDisabled) {
    RiskLimits limits{};
    auto w = limits.warnings();
    EXPECT_FALSE(w.empty());
    bool found = false;
    for (const auto& msg : w)
        if (msg.find("all risk limits are disabled") != std::string::npos) found = true;
    EXPECT_TRUE(found);
}

TEST(RiskLimits, WarningsNotionalWithoutExposure) {
    RiskLimits limits{.max_order_notional = 50000.0};
    auto w = limits.warnings();
    bool found = false;
    for (const auto& msg : w)
        if (msg.find("aggregate exposure") != std::string::npos) found = true;
    EXPECT_TRUE(found);
}

TEST(RiskLimits, WarningsPositionWithoutOrderQty) {
    RiskLimits limits{.max_position_qty = 1000.0};
    auto w = limits.warnings();
    bool found = false;
    for (const auto& msg : w)
        if (msg.find("single large order") != std::string::npos) found = true;
    EXPECT_TRUE(found);
}

TEST(RiskLimits, WarningsEmptyForCompleteConfig) {
    // max_orders_per_second is intentionally OMITTED here — RiskChecker does
    // not enforce it (see WarningsRateLimitNotEnforced below), so configuring
    // it would deliberately add a warning. "Complete config" for this gate's
    // purposes therefore means every threshold actually enforced by
    // check_order().
    RiskLimits limits{
        .max_order_qty = 100.0,
        .max_order_notional = 50000.0,
        .max_position_qty = 1000.0,
        .max_position_notional = 500000.0,
        .max_total_exposure = 1000000.0,
    };
    EXPECT_TRUE(limits.warnings().empty());
}

// Regression for the "silently accepted but unimplemented" knob: the
// max_orders_per_second field stores a threshold, but `RiskChecker::
// check_order()` deliberately leaves rate-limit enforcement to the caller
// (see comment at the bottom of check_order()). Without a warning, an
// operator who sets the field reasonably assumes the gate is enforcing
// it -- and ships without the external rate limiter, then the venue
// throttles or bans on first burst. The warnings() helper must surface
// the gap so production deployments cannot silently rely on a no-op gate.
TEST(RiskLimits, WarningsRateLimitNotEnforced) {
    RiskLimits limits{
        .max_order_qty         = 100.0,
        .max_order_notional    = 50000.0,
        .max_position_qty      = 1000.0,
        .max_position_notional = 500000.0,
        .max_total_exposure    = 1000000.0,
        .max_orders_per_second = 10,  // <-- the hazardous knob
    };
    auto w = limits.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("max_orders_per_second") != std::string::npos &&
            msg.find("does NOT enforce") != std::string::npos) {
            found = true;
        }
    }
    EXPECT_TRUE(found)
        << "warnings() must surface the rate-limit non-enforcement so "
           "operators don't ship without an external TokenBucket";
}

// And conversely: if the operator does NOT configure rate-limit, no
// warning is emitted on that axis. (Keeps the WarningsAllDisabled
// baseline behaviour unchanged.)
TEST(RiskLimits, WarningsRateLimitSilentWhenZero) {
    RiskLimits limits{
        .max_order_qty         = 100.0,
        .max_order_notional    = 50000.0,
        .max_position_qty      = 1000.0,
        .max_position_notional = 500000.0,
        .max_total_exposure    = 1000000.0,
        // max_orders_per_second omitted -> defaults to 0 -> no warning
    };
    auto w = limits.warnings();
    for (const auto& msg : w) {
        EXPECT_EQ(msg.find("max_orders_per_second"), std::string::npos)
            << "rate-limit warning must not fire when the field is 0";
    }
}

TEST(RiskLimits, FormatterProducesDump) {
    RiskLimits limits{.max_order_qty = 42.0};
    auto formatted = std::format("{}", limits);
    EXPECT_NE(formatted.find("RiskLimits"), std::string::npos);
    EXPECT_NE(formatted.find("42.00"), std::string::npos);
}

TEST(RiskLimits, RiskRejectReasonFormatterProducesName) {
    auto s = std::format("{}", RiskRejectReason::kOrderQtyExceeded);
    EXPECT_EQ(s, "OrderQtyExceeded");
    auto s2 = std::format("{}", RiskRejectReason::kOk);
    EXPECT_EQ(s2, "Ok");
}
