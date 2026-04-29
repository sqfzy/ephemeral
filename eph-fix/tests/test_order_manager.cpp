#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "eph/fix.hpp"

using namespace eph::fix;

// ---------------------------------------------------------------------------
// Helper: build a raw FIX message string with correct BodyLength and CheckSum
// ---------------------------------------------------------------------------
static std::vector<uint8_t> make_fix_msg(std::string_view begin_string,
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

// ---------------------------------------------------------------------------
// Build a complete ExecutionReport body with all key fields.
// ---------------------------------------------------------------------------
static std::string make_exec_report_body(
    char exec_type, char ord_status,
    std::string_view cl_ord_id  = "ORD001",
    std::string_view order_id   = "EXCH123",
    std::string_view exec_id    = "EXEC456",
    std::string_view symbol     = "AAPL",
    char side                   = '1',
    std::string_view last_px    = "150.25",
    std::string_view last_qty   = "100",
    std::string_view avg_px     = "150.25",
    std::string_view cum_qty    = "100",
    std::string_view leaves_qty = "0",
    std::string_view text       = "")
{
    std::string body;
    body += "35=8\x01";
    body += "49=SENDER\x01";
    body += "56=TARGET\x01";
    body += "34=1\x01";
    body += std::string("11=") + std::string(cl_ord_id) + '\x01';
    body += std::string("37=") + std::string(order_id) + '\x01';
    body += std::string("17=") + std::string(exec_id) + '\x01';
    body += std::string("150=") + exec_type + '\x01';
    body += std::string("39=") + ord_status + '\x01';
    body += std::string("55=") + std::string(symbol) + '\x01';
    body += std::string("54=") + side + '\x01';
    body += std::string("31=") + std::string(last_px) + '\x01';
    body += std::string("32=") + std::string(last_qty) + '\x01';
    body += std::string("6=") + std::string(avg_px) + '\x01';
    body += std::string("14=") + std::string(cum_qty) + '\x01';
    body += std::string("151=") + std::string(leaves_qty) + '\x01';
    if (!text.empty()) {
        body += std::string("58=") + std::string(text) + '\x01';
    }
    return body;
}

/// Parse a raw FIX message and return an ExecutionReportView.
/// The parsed message is stored in `msg_out` to keep the buffer alive.
static ExecutionReportView<256> make_report(
    const std::vector<uint8_t>& raw,
    BasicMessageView<256>& msg_out)
{
    auto result = parse<256>({raw.data(), raw.size()});
    EXPECT_TRUE(result.has_value());
    msg_out = result.value();
    return ExecutionReportView<256>(msg_out);
}

static constexpr double kEps = 1e-9;

// ===========================================================================
// Full lifecycle: submit -> ack -> fill
// ===========================================================================

TEST(OrderManager, FullLifecycleSubmitAckFill)
{
    OrderManager mgr;

    ASSERT_TRUE(mgr.submit("ORD001", "AAPL", '1', 100.0, 150.0));

    // Verify initial state.
    auto* order = mgr.get("ORD001");
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->state, OrderState::PendingNew);
    EXPECT_DOUBLE_EQ(order->orig_qty, 100.0);
    EXPECT_DOUBLE_EQ(order->leaves_qty, 100.0);
    EXPECT_DOUBLE_EQ(order->filled_qty, 0.0);

    // Exchange ack (ExecType=New).
    auto ack_body = make_exec_report_body('0', '0',
        "ORD001", "EXCH1", "E1", "AAPL", '1',
        "0", "0", "0", "0", "100");
    auto ack_raw = make_fix_msg("FIX.4.4", ack_body);
    BasicMessageView<256> ack_msg;
    auto ack_report = make_report(ack_raw, ack_msg);
    ASSERT_TRUE(mgr.on_execution_report(ack_report));

    order = mgr.get("ORD001");
    EXPECT_EQ(order->state, OrderState::New);

    // Full fill (ExecType=Fill).
    auto fill_body = make_exec_report_body('2', '2',
        "ORD001", "EXCH1", "E2", "AAPL", '1',
        "151.00", "100", "151.00", "100", "0");
    auto fill_raw = make_fix_msg("FIX.4.4", fill_body);
    BasicMessageView<256> fill_msg;
    auto fill_report = make_report(fill_raw, fill_msg);
    ASSERT_TRUE(mgr.on_execution_report(fill_report));

    order = mgr.get("ORD001");
    EXPECT_EQ(order->state, OrderState::Filled);
    EXPECT_NEAR(order->filled_qty, 100.0, kEps);
    EXPECT_NEAR(order->leaves_qty, 0.0, kEps);
    EXPECT_NEAR(order->avg_fill_price, 151.0, kEps);
}

// ===========================================================================
// Partial fill then full fill
// ===========================================================================

TEST(OrderManager, PartialFillThenFill)
{
    OrderManager mgr;
    ASSERT_TRUE(mgr.submit("ORD002", "MSFT", '2', 200.0, 300.0));

    // Partial fill: 80 @ 299.50.
    auto pf_body = make_exec_report_body('1', '1',
        "ORD002", "EXCH2", "E3", "MSFT", '2',
        "299.50", "80", "299.50", "80", "120");
    auto pf_raw = make_fix_msg("FIX.4.4", pf_body);
    BasicMessageView<256> pf_msg;
    auto pf_report = make_report(pf_raw, pf_msg);
    ASSERT_TRUE(mgr.on_execution_report(pf_report));

    auto* order = mgr.get("ORD002");
    EXPECT_EQ(order->state, OrderState::PartiallyFilled);
    EXPECT_NEAR(order->filled_qty, 80.0, kEps);
    EXPECT_NEAR(order->leaves_qty, 120.0, kEps);
    EXPECT_NEAR(order->avg_fill_price, 299.50, kEps);

    // Full fill: remaining 120 @ 300.50.
    auto ff_body = make_exec_report_body('2', '2',
        "ORD002", "EXCH2", "E4", "MSFT", '2',
        "300.50", "120", "300.10", "200", "0");
    auto ff_raw = make_fix_msg("FIX.4.4", ff_body);
    BasicMessageView<256> ff_msg;
    auto ff_report = make_report(ff_raw, ff_msg);
    ASSERT_TRUE(mgr.on_execution_report(ff_report));

    order = mgr.get("ORD002");
    EXPECT_EQ(order->state, OrderState::Filled);
    EXPECT_NEAR(order->filled_qty, 200.0, kEps);
    EXPECT_NEAR(order->leaves_qty, 0.0, kEps);

    // Average fill price: (299.50*80 + 300.50*120) / 200 = 300.10
    EXPECT_NEAR(order->avg_fill_price, 300.10, kEps);
}

// ===========================================================================
// Submit then cancel
// ===========================================================================

TEST(OrderManager, SubmitThenCancel)
{
    OrderManager mgr;
    ASSERT_TRUE(mgr.submit("ORD003", "GOOG", '1', 50.0, 2800.0));

    // Mark pending cancel.
    ASSERT_TRUE(mgr.mark_pending_cancel("ORD003"));
    EXPECT_EQ(mgr.get("ORD003")->state, OrderState::PendingCancel);

    // Exchange confirms cancel.
    auto cxl_body = make_exec_report_body('4', '4',
        "ORD003", "EXCH3", "E5", "GOOG", '1',
        "0", "0", "0", "0", "0");
    auto cxl_raw = make_fix_msg("FIX.4.4", cxl_body);
    BasicMessageView<256> cxl_msg;
    auto cxl_report = make_report(cxl_raw, cxl_msg);
    ASSERT_TRUE(mgr.on_execution_report(cxl_report));

    auto* order = mgr.get("ORD003");
    EXPECT_EQ(order->state, OrderState::Canceled);
    EXPECT_NEAR(order->leaves_qty, 0.0, kEps);
}

// ===========================================================================
// Submit then reject
// ===========================================================================

TEST(OrderManager, SubmitThenReject)
{
    OrderManager mgr;
    ASSERT_TRUE(mgr.submit("ORD004", "TSLA", '2', 10.0, 900.0));

    auto rej_body = make_exec_report_body('8', '8',
        "ORD004", "NONE", "E6", "TSLA", '2',
        "0", "0", "0", "0", "0", "Insufficient margin");
    auto rej_raw = make_fix_msg("FIX.4.4", rej_body);
    BasicMessageView<256> rej_msg;
    auto rej_report = make_report(rej_raw, rej_msg);
    ASSERT_TRUE(mgr.on_execution_report(rej_report));

    auto* order = mgr.get("ORD004");
    EXPECT_EQ(order->state, OrderState::Rejected);
    EXPECT_NEAR(order->leaves_qty, 0.0, kEps);
}

// ===========================================================================
// Unknown order returns false
// ===========================================================================

TEST(OrderManager, UnknownOrderReturnsFalse)
{
    OrderManager mgr;

    auto body = make_exec_report_body('0', '0',
        "UNKNOWN", "EXCH1", "E1", "AAPL", '1',
        "0", "0", "0", "0", "0");
    auto raw = make_fix_msg("FIX.4.4", body);
    BasicMessageView<256> msg;
    auto report = make_report(raw, msg);
    EXPECT_FALSE(mgr.on_execution_report(report));
}

// ===========================================================================
// Duplicate submit returns false
// ===========================================================================

TEST(OrderManager, DuplicateSubmitReturnsFalse)
{
    OrderManager mgr;
    ASSERT_TRUE(mgr.submit("ORD005", "AAPL", '1', 100.0, 150.0));
    EXPECT_FALSE(mgr.submit("ORD005", "AAPL", '1', 100.0, 150.0));
}

// ===========================================================================
// Active count tracks non-terminal orders
// ===========================================================================

TEST(OrderManager, ActiveCount)
{
    OrderManager mgr;
    EXPECT_EQ(mgr.active_count(), 0u);

    mgr.submit("A1", "AAPL", '1', 100.0, 150.0);
    mgr.submit("A2", "MSFT", '2', 200.0, 300.0);
    EXPECT_EQ(mgr.active_count(), 2u);

    // Reject A1.
    auto rej_body = make_exec_report_body('8', '8',
        "A1", "EXCH", "E1", "AAPL", '1',
        "0", "0", "0", "0", "0");
    auto rej_raw = make_fix_msg("FIX.4.4", rej_body);
    BasicMessageView<256> rej_msg;
    auto rej_report = make_report(rej_raw, rej_msg);
    mgr.on_execution_report(rej_report);
    EXPECT_EQ(mgr.active_count(), 1u);

    // Fill A2.
    auto fill_body = make_exec_report_body('2', '2',
        "A2", "EXCH", "E2", "MSFT", '2',
        "300.0", "200", "300.0", "200", "0");
    auto fill_raw = make_fix_msg("FIX.4.4", fill_body);
    BasicMessageView<256> fill_msg;
    auto fill_report = make_report(fill_raw, fill_msg);
    mgr.on_execution_report(fill_report);
    EXPECT_EQ(mgr.active_count(), 0u);
}

// ===========================================================================
// Purge terminal removes only terminal orders
// ===========================================================================

TEST(OrderManager, PurgeTerminal)
{
    OrderManager mgr;
    mgr.submit("T1", "AAPL", '1', 100.0, 150.0);
    mgr.submit("T2", "MSFT", '2', 200.0, 300.0);
    mgr.submit("T3", "GOOG", '1', 50.0, 2800.0);

    // Fill T1.
    auto fill_body = make_exec_report_body('2', '2',
        "T1", "EXCH", "E1", "AAPL", '1',
        "150.0", "100", "150.0", "100", "0");
    auto fill_raw = make_fix_msg("FIX.4.4", fill_body);
    BasicMessageView<256> fill_msg;
    auto fill_report = make_report(fill_raw, fill_msg);
    mgr.on_execution_report(fill_report);

    // Reject T3.
    auto rej_body = make_exec_report_body('8', '8',
        "T3", "NONE", "E2", "GOOG", '1',
        "0", "0", "0", "0", "0");
    auto rej_raw = make_fix_msg("FIX.4.4", rej_body);
    BasicMessageView<256> rej_msg;
    auto rej_report = make_report(rej_raw, rej_msg);
    mgr.on_execution_report(rej_report);

    // T2 still active, T1 filled, T3 rejected.
    EXPECT_EQ(mgr.orders().size(), 3u);

    mgr.purge_terminal();
    EXPECT_EQ(mgr.orders().size(), 1u);
    EXPECT_NE(mgr.get("T2"), nullptr);
    EXPECT_EQ(mgr.get("T1"), nullptr);
    EXPECT_EQ(mgr.get("T3"), nullptr);
}

// ===========================================================================
// Integration with PositionTracker
// ===========================================================================

TEST(OrderManager, IntegrationWithPositionTracker)
{
    OrderManager mgr;
    PositionTracker positions;

    mgr.submit("POS1", "AAPL", '1', 100.0, 150.0);

    // Partial fill: 60 @ 150.25.
    auto pf_body = make_exec_report_body('1', '1',
        "POS1", "EXCH", "E1", "AAPL", '1',
        "150.25", "60", "150.25", "60", "40");
    auto pf_raw = make_fix_msg("FIX.4.4", pf_body);
    BasicMessageView<256> pf_msg;
    auto pf_report = make_report(pf_raw, pf_msg);
    ASSERT_TRUE(mgr.on_execution_report(pf_report, &positions));

    // Position tracker should reflect the fill.
    EXPECT_NEAR(positions.get("AAPL").qty, 60.0, kEps);
    EXPECT_NEAR(positions.get("AAPL").avg_price, 150.25, kEps);

    // Full fill: remaining 40 @ 150.75.
    auto ff_body = make_exec_report_body('2', '2',
        "POS1", "EXCH", "E2", "AAPL", '1',
        "150.75", "40", "150.45", "100", "0");
    auto ff_raw = make_fix_msg("FIX.4.4", ff_body);
    BasicMessageView<256> ff_msg;
    auto ff_report = make_report(ff_raw, ff_msg);
    ASSERT_TRUE(mgr.on_execution_report(ff_report, &positions));

    // Position tracker should reflect total position.
    EXPECT_NEAR(positions.get("AAPL").qty, 100.0, kEps);
    // avg = (60*150.25 + 40*150.75) / 100 = 150.45
    EXPECT_NEAR(positions.get("AAPL").avg_price, 150.45, kEps);
    EXPECT_EQ(positions.get("AAPL").trade_count, 2u);

    // Order should be fully filled.
    auto* order = mgr.get("POS1");
    EXPECT_EQ(order->state, OrderState::Filled);
    EXPECT_NEAR(order->filled_qty, 100.0, kEps);
}

// ===========================================================================
// mark_pending_cancel on unknown order returns false
// ===========================================================================

TEST(OrderManager, MarkPendingCancelUnknownReturnsFalse)
{
    OrderManager mgr;
    EXPECT_FALSE(mgr.mark_pending_cancel("NOEXIST"));
}

// ===========================================================================
// mark_pending_cancel on terminal order returns false
// ===========================================================================

TEST(OrderManager, MarkPendingCancelTerminalReturnsFalse)
{
    OrderManager mgr;
    mgr.submit("TC1", "AAPL", '1', 100.0, 150.0);

    // Reject it.
    auto rej_body = make_exec_report_body('8', '8',
        "TC1", "NONE", "E1", "AAPL", '1',
        "0", "0", "0", "0", "0");
    auto rej_raw = make_fix_msg("FIX.4.4", rej_body);
    BasicMessageView<256> rej_msg;
    auto rej_report = make_report(rej_raw, rej_msg);
    mgr.on_execution_report(rej_report);

    EXPECT_FALSE(mgr.mark_pending_cancel("TC1"));
}

// ===========================================================================
// Get returns nullptr for nonexistent order
// ===========================================================================

TEST(OrderManager, GetReturnsNullptrForNonexistent)
{
    OrderManager mgr;
    EXPECT_EQ(mgr.get("NOEXIST"), nullptr);
}

// ===========================================================================
// is_terminal free function
// ===========================================================================

TEST(OrderManager, IsTerminalFreeFunction)
{
    EXPECT_FALSE(is_terminal(OrderState::PendingNew));
    EXPECT_FALSE(is_terminal(OrderState::New));
    EXPECT_FALSE(is_terminal(OrderState::PartiallyFilled));
    EXPECT_FALSE(is_terminal(OrderState::PendingCancel));
    EXPECT_TRUE(is_terminal(OrderState::Filled));
    EXPECT_TRUE(is_terminal(OrderState::Canceled));
    EXPECT_TRUE(is_terminal(OrderState::Rejected));
}

// ---------------------------------------------------------------------------
// ManagedOrder dump/to_json tests
// ---------------------------------------------------------------------------

TEST(ManagedOrder, DumpContainsAllFields) {
    ManagedOrder order{
        .cl_ord_id = "ORD001", .symbol = "AAPL", .side = '1',
        .orig_qty = 100.0, .price = 150.25, .filled_qty = 50.0,
        .avg_fill_price = 150.10, .leaves_qty = 50.0,
        .state = OrderState::PartiallyFilled,
    };
    auto d = order.dump();
    EXPECT_NE(d.find("ORD001"), std::string::npos);
    EXPECT_NE(d.find("AAPL"), std::string::npos);
    EXPECT_NE(d.find("Buy"), std::string::npos);
    EXPECT_NE(d.find("PartiallyFilled"), std::string::npos);
    EXPECT_NE(d.find("150.25"), std::string::npos);
}

TEST(ManagedOrder, DumpShowsSellSide) {
    // Designated initializers must initialize earlier fields too — explicit
    // empty strings silence -Wmissing-field-initializers without changing
    // semantics (std::string default-constructs to empty either way).
    ManagedOrder order{
        .cl_ord_id = {}, .symbol = {},
        .side = '2', .state = OrderState::New,
    };
    auto d = order.dump();
    EXPECT_NE(d.find("Sell"), std::string::npos);
}

TEST(ManagedOrder, ToJsonIsValidStructure) {
    ManagedOrder order{
        .cl_ord_id = "ORD002", .symbol = "TSLA", .side = '2',
        .orig_qty = 200.0, .state = OrderState::Filled,
    };
    auto j = order.to_json();
    EXPECT_TRUE(j.starts_with("{"));
    EXPECT_TRUE(j.ends_with("}"));
    EXPECT_NE(j.find("\"cl_ord_id\":\"ORD002\""), std::string::npos);
    EXPECT_NE(j.find("\"symbol\":\"TSLA\""), std::string::npos);
    EXPECT_NE(j.find("\"state\":\"Filled\""), std::string::npos);
}

TEST(ManagedOrder, ToJsonEscapesSpecialChars) {
    ManagedOrder order{.cl_ord_id = "test\"id", .symbol = "back\\slash"};
    auto j = order.to_json();
    EXPECT_NE(j.find("test\\\"id"), std::string::npos);
    EXPECT_NE(j.find("back\\\\slash"), std::string::npos);
}

TEST(ManagedOrder, OrderStateFormatterInFormat) {
    auto s = std::format("{}", OrderState::PendingNew);
    EXPECT_EQ(s, "PendingNew");
}

// ===========================================================================
// Regression: negative LastQty must be rejected.
//
// FIX 4.4 LastQty (tag 32) is defined as a Qty (non-negative). A malformed or
// hostile ExecutionReport with LastQty < 0 must not be allowed to corrupt the
// tracked order: it would otherwise drive filled_qty negative, push leaves_qty
// above orig_qty, and break avg_fill_price (potentially producing NaN/inf).
//
// Without the guard, on_execution_report() returned true and silently mutated
// the order's filled_qty/leaves_qty into impossible values.
// ===========================================================================
TEST(OrderManager, NegativeLastQtyRejected)
{
    OrderManager mgr;
    ASSERT_TRUE(mgr.submit("ORDNEG", "AAPL", '1', 100.0, 150.0));

    // Hostile / corrupted ExecutionReport with LastQty=-50.
    auto body = make_exec_report_body('1', '1',
        "ORDNEG", "EXCHN", "EXECN", "AAPL", '1',
        "150.00", "-50", "150.00", "-50", "150");
    auto raw = make_fix_msg("FIX.4.4", body);
    BasicMessageView<256> msg;
    auto report = make_report(raw, msg);

    // Must reject the report (return false) and leave the order untouched.
    EXPECT_FALSE(mgr.on_execution_report(report));

    auto* order = mgr.get("ORDNEG");
    ASSERT_NE(order, nullptr);
    // Order state and quantities must remain at their pre-report values.
    EXPECT_EQ(order->state, OrderState::PendingNew);
    EXPECT_NEAR(order->filled_qty, 0.0, kEps);
    EXPECT_NEAR(order->leaves_qty, 100.0, kEps);
    EXPECT_NEAR(order->avg_fill_price, 0.0, kEps);
}

// Same guard for ExecType=Trade ('F') and full-Fill ('2') paths.
TEST(OrderManager, NegativeLastQtyOnFullFillRejected)
{
    OrderManager mgr;
    ASSERT_TRUE(mgr.submit("ORDFNEG", "MSFT", '2', 50.0, 300.0));

    auto body = make_exec_report_body('2', '2',
        "ORDFNEG", "EXCHN", "EXECN", "MSFT", '2',
        "300.00", "-25", "300.00", "-25", "0");
    auto raw = make_fix_msg("FIX.4.4", body);
    BasicMessageView<256> msg;
    auto report = make_report(raw, msg);

    EXPECT_FALSE(mgr.on_execution_report(report));

    auto* order = mgr.get("ORDFNEG");
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->state, OrderState::PendingNew);
    EXPECT_NEAR(order->filled_qty, 0.0, kEps);
    EXPECT_NEAR(order->leaves_qty, 50.0, kEps);
}

// Negative LastPx must also be rejected — FIX Px is non-negative and a
// negative price would drive avg_fill_price downward in nonsensical ways.
TEST(OrderManager, NegativeLastPxRejected)
{
    OrderManager mgr;
    ASSERT_TRUE(mgr.submit("ORDPNEG", "AAPL", '1', 100.0, 150.0));

    auto body = make_exec_report_body('1', '1',
        "ORDPNEG", "EXCHN", "EXECN", "AAPL", '1',
        "-150.00", "50", "-150.00", "50", "50");
    auto raw = make_fix_msg("FIX.4.4", body);
    BasicMessageView<256> msg;
    auto report = make_report(raw, msg);

    EXPECT_FALSE(mgr.on_execution_report(report));

    auto* order = mgr.get("ORDPNEG");
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->state, OrderState::PendingNew);
    EXPECT_NEAR(order->filled_qty, 0.0, kEps);
    EXPECT_NEAR(order->leaves_qty, 100.0, kEps);
}
