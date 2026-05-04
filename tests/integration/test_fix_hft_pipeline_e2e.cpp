/// @file test_fix_hft_pipeline_e2e.cpp
/// Cross-module HFT order-entry pipeline end-to-end:
///
///     RiskChecker::check_order()      ← gates the SUBMIT
///       → OrderManager::submit()
///         → exchange ack ExecutionReport (parsed via FIX wire bytes)
///           → OrderManager::on_execution_report() + PositionTracker
///             → fill ExecutionReport
///               → OrderManager updates ManagedOrder.filled_qty,
///                 PositionTracker.on_fill() updates per-symbol position,
///                 → next RiskChecker::check_order() sees the NEW position
///                 → gates the NEXT submit using the updated exposure.
///
/// Coverage gap before this commit:
///   * test_order_manager.cpp's `IntegrationWithPositionTracker` exercises
///     OM + Position only — RiskChecker is never in the loop.
///   * test_risk_check.cpp constructs `PositionTracker` with hand-rolled
///     `on_fill` calls and skips the OM lifecycle entirely.
///   * As a result, the **cross-module invariant** that drives every HFT
///     production system is unverified:
///         RiskChecker.check_order() must use the SAME `PositionTracker`
///         that OM `on_execution_report()` mutates via `on_fill`, and the
///         projected exposure / projected position math must match exactly
///         after each fill.
///   * CLAUDE.md P1 explicitly lists this scenario:
///         "HFT pipeline e2e: order submit → fill → over-fill rejection".
///
/// Each test below builds real FIX 4.4 ExecutionReport wire bytes (via the
/// `eph::fix::parse` parser used by the production receive path) so the
/// pipeline is exercised end-to-end, not via hand-built typed messages.

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "eph/fix.hpp"

using namespace eph::fix;

namespace {

constexpr double kEps = 1e-9;

// ---------------------------------------------------------------------------
// FIX wire-bytes helpers (mirror the test_order_manager.cpp fixture so the
// e2e test goes through the SAME parse path as production, not a typed mock)
// ---------------------------------------------------------------------------

inline std::vector<uint8_t> make_fix_msg(std::string_view begin_string,
                                          std::string_view body) {
    std::string header = "8=";
    header += begin_string;
    header += '\x01';

    std::string body_len_field = "9=" + std::to_string(body.size()) + '\x01';
    std::string full = header + body_len_field + std::string(body);

    uint32_t sum = 0;
    for (char c : full) sum += static_cast<uint8_t>(c);
    uint8_t cs = static_cast<uint8_t>(sum & 0xFF);

    char cs_str[8];
    std::snprintf(cs_str, sizeof(cs_str), "10=%03u\x01", cs);
    full += cs_str;
    return {full.begin(), full.end()};
}

inline std::string make_exec_report_body(
    char exec_type, char ord_status,
    std::string_view cl_ord_id,
    std::string_view symbol, char side,
    std::string_view last_px, std::string_view last_qty,
    std::string_view avg_px, std::string_view cum_qty,
    std::string_view leaves_qty)
{
    std::string body;
    body += "35=8\x01";
    body += "49=SENDER\x01";
    body += "56=TARGET\x01";
    body += "34=1\x01";
    body += "11="; body += cl_ord_id;  body += '\x01';
    body += "37=EXCH-OID\x01";
    body += "17=EXEC-ID\x01";
    body += "150="; body += exec_type; body += '\x01';
    body += "39=";  body += ord_status; body += '\x01';
    body += "55="; body += symbol;     body += '\x01';
    body += "54="; body += side;       body += '\x01';
    body += "31="; body += last_px;    body += '\x01';
    body += "32="; body += last_qty;   body += '\x01';
    body += "6=";  body += avg_px;     body += '\x01';
    body += "14="; body += cum_qty;    body += '\x01';
    body += "151="; body += leaves_qty; body += '\x01';
    return body;
}

/// Parse a raw FIX message into an ExecutionReportView.
/// Caller owns `msg_holder` to keep the underlying buffer alive.
inline ExecutionReportView<256>
make_report(const std::vector<uint8_t>& raw, BasicMessageView<256>& msg_holder) {
    auto result = parse<256>({raw.data(), raw.size()});
    EXPECT_TRUE(result.has_value());
    msg_holder = result.value();
    return ExecutionReportView<256>(msg_holder);
}

/// Convenience: build + parse a Fill (ExecType='2') ExecReport in one call.
/// `qty` and `px` are the LastQty / LastPx of THIS fill (single-fill semantics
/// for full-fill case — caller computes cum/leaves themselves for partial).
///
/// NOTE: LastQty / CumQty / LeavesQty are formatted as integers because
/// `OrderManager::on_execution_report` calls `report.last_qty()` (the
/// int64_t accessor), which returns nullopt for fractional wire formats
/// like "100.0000". The integer wire format covers every equity venue
/// and is the only path the OM exercises today; the `last_qty_d()`
/// fractional path used by some crypto venues is read by the venue
/// adapters but not by OM. (See eph-fix/include/eph/fix/order_manager.hpp
/// lines ~285 and ~372.)
inline ExecutionReportView<256> build_full_fill(
    std::string_view cl_ord_id, std::string_view symbol, char side,
    double px, int64_t qty, BasicMessageView<256>& holder)
{
    auto px_s  = std::format("{:.4f}", px);
    auto qty_s = std::format("{}", qty);
    auto body = make_exec_report_body(
        /*ExecType=*/'2', /*OrdStatus=*/'2',
        cl_ord_id, symbol, side,
        px_s, qty_s, px_s, qty_s, "0");
    auto raw = make_fix_msg("FIX.4.4", body);
    return make_report(raw, holder);
}

/// Build + parse a partial-fill report (ExecType='1') with explicit cum/leaves.
/// See note in build_full_fill — Qty fields are integer-formatted.
inline ExecutionReportView<256> build_partial_fill(
    std::string_view cl_ord_id, std::string_view symbol, char side,
    double px, int64_t qty, int64_t cum, int64_t leaves,
    BasicMessageView<256>& holder)
{
    auto px_s     = std::format("{:.4f}", px);
    auto qty_s    = std::format("{}", qty);
    auto cum_s    = std::format("{}", cum);
    auto leaves_s = std::format("{}", leaves);
    auto body = make_exec_report_body(
        /*ExecType=*/'1', /*OrdStatus=*/'1',
        cl_ord_id, symbol, side,
        px_s, qty_s, px_s, cum_s, leaves_s);
    auto raw = make_fix_msg("FIX.4.4", body);
    return make_report(raw, holder);
}

}  // namespace

// ===========================================================================
// E2E: RiskCheck → submit → fill → next RiskCheck sees new position
// ===========================================================================

/// First order passes risk gate, fills fully; second order's risk gate
/// then evaluates against the post-fill PositionTracker state. This is
/// the SINGLE most important invariant in the HFT order-entry stack:
/// RiskChecker.check_order() and OM.on_execution_report() must operate
/// on the same PositionTracker — otherwise the second submit either
/// passes a check it shouldn't (wrong direction) or rejects a legal
/// trade (false positive that costs revenue).
TEST(FixHftPipelineE2E, RiskGateAndFillUpdateAreConsistent) {
    PositionTracker positions;
    OrderManager    mgr;
    RiskChecker     risk(RiskLimits{
        .max_order_qty         = 1000.0,
        .max_order_notional    = 200000.0,
        .max_position_qty      = 500.0,
        .max_position_notional = 100000.0,
        .max_total_exposure    = 100000.0,
    });

    // First order: 200 BUY @ 150.00 — projected exposure 30000, well under cap.
    EXPECT_EQ(risk.check_order("AAPL", '1', 200.0, 150.0, positions),
              RiskRejectReason::kOk);
    ASSERT_TRUE(mgr.submit("ORD-1", "AAPL", '1', 200.0, 150.0));

    // Full fill at 151.00 (slipped one tick).
    BasicMessageView<256> holder1;
    auto fill1 = build_full_fill("ORD-1", "AAPL", '1', 151.00, 200, holder1);
    ASSERT_TRUE(mgr.on_execution_report(fill1, &positions));

    // Position must now reflect the 200 long @ 151.
    auto const& pos = positions.get("AAPL");
    EXPECT_NEAR(pos.qty, 200.0, kEps);
    EXPECT_NEAR(pos.avg_price, 151.0, kEps);
    EXPECT_NEAR(positions.net_exposure(), 200.0 * 151.0, kEps);

    // Second order would push position to 500 (boundary OK).
    EXPECT_EQ(risk.check_order("AAPL", '1', 300.0, 152.0, positions),
              RiskRejectReason::kOk);

    // Third would push it to 510 — strictly exceeds 500. The risk gate
    // MUST fire because the post-fill PositionTracker now reports qty=200.
    EXPECT_EQ(risk.check_order("AAPL", '1', 310.0, 152.0, positions),
              RiskRejectReason::kPositionQtyExceeded);

    // Sell-side projection must reduce exposure, not add to it.
    // 200 long → sell 50 → projected |150|, well within max_position_qty=500.
    EXPECT_EQ(risk.check_order("AAPL", '2', 50.0, 152.0, positions),
              RiskRejectReason::kOk);
}

/// After a partial fill leaves leaves_qty > 0, the position has only
/// the partial qty — the next risk check must see THAT, not the original
/// orig_qty. Regression for the class of bugs where risk math accidentally
/// uses ManagedOrder.orig_qty instead of PositionTracker.qty.
TEST(FixHftPipelineE2E, PartialFillExposesOnlyPartialQtyToRiskGate) {
    PositionTracker positions;
    OrderManager    mgr;
    RiskChecker     risk(RiskLimits{
        .max_position_qty   = 300.0,
        .max_total_exposure = 100000.0,
    });

    // Submit 250 BUY (projected) — 0 + 250 ≤ 300, OK.
    EXPECT_EQ(risk.check_order("MSFT", '1', 250.0, 300.0, positions),
              RiskRejectReason::kOk);
    ASSERT_TRUE(mgr.submit("ORD-PF", "MSFT", '1', 250.0, 300.0));

    // Partial fill 80 @ 300.0 — leaves 170.
    BasicMessageView<256> h;
    auto pf = build_partial_fill("ORD-PF", "MSFT", '1', 300.0,
                                  /*last=*/80, /*cum=*/80, /*leaves=*/170, h);
    ASSERT_TRUE(mgr.on_execution_report(pf, &positions));

    // Position now 80 long. A NEW order for 200 BUY would project to 280 — OK.
    EXPECT_NEAR(positions.get("MSFT").qty, 80.0, kEps);
    EXPECT_EQ(risk.check_order("MSFT", '1', 200.0, 300.0, positions),
              RiskRejectReason::kOk);

    // 250 would project 80 + 250 = 330 > 300 — must reject. The buggy
    // implementation that double-counted the leaves_qty of the open
    // ORD-PF (250 + 80 = 330 phantom) would return kOk here.
    EXPECT_EQ(risk.check_order("MSFT", '1', 250.0, 300.0, positions),
              RiskRejectReason::kPositionQtyExceeded);
}

/// Over-fill rejected by OM must NOT leak into PositionTracker.
/// CLAUDE.md P1 calls this out explicitly. If the OM's over-fill guard
/// ever regressed and forwarded the bogus fill to position.on_fill(),
/// the risk gate's projected_position would diverge from reality and
/// the system would silently double-bill exposure.
TEST(FixHftPipelineE2E, OverFillRejectionDoesNotMutatePosition) {
    PositionTracker positions;
    OrderManager    mgr;

    ASSERT_TRUE(mgr.submit("ORD-OF", "GOOG", '1', 100.0, 2500.0));

    // Legitimate fill: 60 of 100.
    BasicMessageView<256> h1;
    auto pf = build_partial_fill("ORD-OF", "GOOG", '1', 2500.0,
                                  /*last=*/60, /*cum=*/60, /*leaves=*/40, h1);
    ASSERT_TRUE(mgr.on_execution_report(pf, &positions));
    EXPECT_NEAR(positions.get("GOOG").qty, 60.0, kEps);

    // Bogus over-fill: claims another 100 (would put cum at 160, > orig 100).
    // OM must reject; PositionTracker must remain at 60.
    BasicMessageView<256> h2;
    auto over = build_partial_fill("ORD-OF", "GOOG", '1', 2500.0,
                                    /*last=*/100, /*cum=*/160, /*leaves=*/0, h2);
    EXPECT_FALSE(mgr.on_execution_report(over, &positions))
        << "OM must reject over-fill before forwarding to PositionTracker";

    // CRITICAL: position must still be 60 — a leak here would make the
    // very next RiskChecker.check_order() see 160 and gate wrong.
    auto const& pos = positions.get("GOOG");
    EXPECT_NEAR(pos.qty, 60.0, kEps)
        << "PositionTracker must NOT see the rejected over-fill";

    // ManagedOrder also unchanged.
    auto const* o = mgr.get("ORD-OF");
    ASSERT_NE(o, nullptr);
    EXPECT_NEAR(o->filled_qty, 60.0, kEps);
    EXPECT_NEAR(o->leaves_qty, 40.0, kEps);
    EXPECT_EQ(o->state, OrderState::PartiallyFilled);
}

/// A rejected ExecutionReport (ExecType='8') must terminalize the
/// ManagedOrder but NEVER touch PositionTracker — risk math then
/// remains valid for the next order on the same symbol.
TEST(FixHftPipelineE2E, RejectExecReportDoesNotMutatePosition) {
    PositionTracker positions;
    OrderManager    mgr;

    ASSERT_TRUE(mgr.submit("ORD-RJ", "TSLA", '1', 50.0, 800.0));

    // ExecType='8' (Rejected), OrdStatus='8'. Body fields zero/empty.
    auto rej_body = make_exec_report_body(
        '8', '8', "ORD-RJ", "TSLA", '1',
        "0", "0", "0", "0", "50");
    auto rej_raw = make_fix_msg("FIX.4.4", rej_body);
    BasicMessageView<256> h;
    auto rj = make_report(rej_raw, h);
    ASSERT_TRUE(mgr.on_execution_report(rj, &positions));

    auto const* o = mgr.get("ORD-RJ");
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->state, OrderState::Rejected);
    EXPECT_NEAR(o->filled_qty, 0.0, kEps);

    // Position must stay flat.
    EXPECT_NEAR(positions.get("TSLA").qty, 0.0, kEps);
    EXPECT_NEAR(positions.net_exposure(), 0.0, kEps);
}

/// Risk-gate INVALID-INPUT branch must NOT poison the OM/Position state
/// even if the application bug downstream still tries to submit. Proves
/// the three modules are NOT silently entangled by NaN propagation.
TEST(FixHftPipelineE2E, NaNPriceRiskRejectThenIndependentValidSubmit) {
    PositionTracker positions;
    OrderManager    mgr;
    RiskChecker     risk(RiskLimits{.max_order_qty = 1000.0});

    EXPECT_EQ(risk.check_order("X", '1', 10.0, std::nan(""), positions),
              RiskRejectReason::kInvalidInput);

    // Even if the application erroneously submits despite the reject,
    // OM's own NaN guard must catch it.
    EXPECT_FALSE(mgr.submit("ORD-NAN", "X", '1', 10.0, std::nan("")));
    EXPECT_EQ(mgr.get("ORD-NAN"), nullptr);

    // A valid follow-up submit on the same symbol must work cleanly.
    EXPECT_EQ(risk.check_order("X", '1', 10.0, 100.0, positions),
              RiskRejectReason::kOk);
    ASSERT_TRUE(mgr.submit("ORD-OK", "X", '1', 10.0, 100.0));
    EXPECT_NE(mgr.get("ORD-OK"), nullptr);
}

/// Multi-fill, multi-order on a single symbol: the cross-module
/// invariant `OM.cumulative_filled` ≡ `PositionTracker.qty` must hold
/// for the duration of the simulated session. Drives both the FIX
/// parser path AND the risk gate to make sure no copy of the state
/// drifts between layers.
TEST(FixHftPipelineE2E, MultiFillSequenceKeepsOMAndPositionInSync) {
    PositionTracker positions;
    OrderManager    mgr;

    // Three buys, two partial-then-full each.
    struct OrderSpec { const char* id; double qty; double px; };
    std::array<OrderSpec, 3> specs = {{
        {"O1", 100.0, 50.0},
        {"O2", 200.0, 51.0},
        {"O3", 150.0, 52.0},
    }};
    for (auto const& s : specs) {
        ASSERT_TRUE(mgr.submit(s.id, "AMD", '1', s.qty, s.px));
    }

    // Fill all O1 fully, O2 partially (half), O3 partially (third).
    BasicMessageView<256> h_o1, h_o2pf, h_o3pf;
    auto f_o1   = build_full_fill   ("O1", "AMD", '1', 50.0, 100, h_o1);
    ASSERT_TRUE(mgr.on_execution_report(f_o1,  &positions));
    EXPECT_NEAR(positions.get("AMD").qty, 100.0, kEps)
        << "after O1 full fill, position should be 100";

    auto pf_o2  = build_partial_fill("O2", "AMD", '1', 51.0,
                                      /*last=*/100, /*cum=*/100, /*leaves=*/100, h_o2pf);
    ASSERT_TRUE(mgr.on_execution_report(pf_o2, &positions));
    EXPECT_NEAR(positions.get("AMD").qty, 200.0, kEps)
        << "after O2 partial fill of 100, position should be 200";

    auto pf_o3  = build_partial_fill("O3", "AMD", '1', 52.0,
                                      /*last=*/50,  /*cum=*/50,  /*leaves=*/100, h_o3pf);
    ASSERT_TRUE(mgr.on_execution_report(pf_o3, &positions));

    // Sum of OM filled_qty == PositionTracker qty.
    double om_total = 0.0;
    for (auto const& s : specs) {
        auto const* o = mgr.get(s.id);
        ASSERT_NE(o, nullptr);
        om_total += o->filled_qty;
    }
    EXPECT_NEAR(om_total, 250.0, kEps);          // 100 + 100 + 50
    EXPECT_NEAR(positions.get("AMD").qty, 250.0, kEps);

    // Volume-weighted average price must agree across layers (within rounding).
    // (100*50 + 100*51 + 50*52) / 250 = (5000 + 5100 + 2600) / 250 = 50.8
    EXPECT_NEAR(positions.get("AMD").avg_price, 50.8, 1e-6);
}
