#include <gtest/gtest.h>

#include <cmath>     // std::nan
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <string_view>

#include "eph/fix.hpp"

using namespace eph::fix;

// Fixed timestamp for deterministic tests: 2024-01-15 10:30:00.123456789 UTC
static constexpr uint64_t kTestTimestamp = 1705311000'123'456'789ULL;

// ---------------------------------------------------------------------------
// NewOrderSingle (MsgType=D)
// ---------------------------------------------------------------------------

TEST(FixOrders, build_new_order_limit_roundtrip) {
    uint8_t buf[512];
    size_t len = build_new_order(
        buf, sizeof(buf),
        "SENDER", "TARGET",
        "ORD001", "AAPL",
        Side::Buy, OrdType::Limit,
        100.0, 150.25,
        TimeInForce::Day,
        kTestTimestamp);

    ASSERT_GT(len, 0u);

    // Parse back
    auto result = parse(buf, len);
    ASSERT_TRUE(result.has_value()) << "parse failed: " << static_cast<int>(result.error());
    auto& msg = *result;

    // Verify all fields
    EXPECT_EQ(msg.get(tag::MsgType), "D");
    EXPECT_EQ(msg.get(tag::SenderCompID), "SENDER");
    EXPECT_EQ(msg.get(tag::TargetCompID), "TARGET");
    EXPECT_EQ(msg.get(tag::ClOrdID), "ORD001");
    EXPECT_EQ(msg.get(tag::Symbol), "AAPL");
    EXPECT_EQ(msg.get_char(tag::Side), '1');           // Buy
    EXPECT_EQ(msg.get_char(tag::OrdType), '2');        // Limit
    EXPECT_EQ(msg.get_char(tag::TimeInForce), '0');    // Day
    EXPECT_EQ(msg.get_char(tag::HandlInst), '1');      // Automated

    // Qty and price
    auto qty = msg.get_double(tag::OrderQty);
    ASSERT_TRUE(qty.has_value());
    EXPECT_DOUBLE_EQ(*qty, 100.0);

    auto price = msg.get_double(tag::Price);
    ASSERT_TRUE(price.has_value());
    EXPECT_DOUBLE_EQ(*price, 150.25);

    // Timestamps present
    EXPECT_TRUE(msg.has(tag::TransactTime));
    EXPECT_TRUE(msg.has(tag::SendingTime));
}

TEST(FixOrders, build_new_order_market_no_price) {
    uint8_t buf[512];
    size_t len = build_new_order(
        buf, sizeof(buf),
        "SENDER", "TARGET",
        "ORD002", "TSLA",
        Side::Sell, OrdType::Market,
        50.0, 0.0,
        TimeInForce::IOC,
        kTestTimestamp);

    ASSERT_GT(len, 0u);

    auto result = parse(buf, len);
    ASSERT_TRUE(result.has_value());
    auto& msg = *result;

    EXPECT_EQ(msg.get(tag::MsgType), "D");
    EXPECT_EQ(msg.get_char(tag::Side), '2');        // Sell
    EXPECT_EQ(msg.get_char(tag::OrdType), '1');     // Market
    EXPECT_EQ(msg.get_char(tag::TimeInForce), '3'); // IOC

    // Market orders must NOT have a Price tag
    EXPECT_FALSE(msg.has(tag::Price));

    auto qty = msg.get_double(tag::OrderQty);
    ASSERT_TRUE(qty.has_value());
    EXPECT_DOUBLE_EQ(*qty, 50.0);
}

TEST(FixOrders, build_new_order_sell_limit_fok) {
    uint8_t buf[512];
    size_t len = build_new_order(
        buf, sizeof(buf),
        "HFT1", "EXCH",
        "C100", "MSFT",
        Side::Sell, OrdType::Limit,
        200.0, 420.50,
        TimeInForce::FOK,
        kTestTimestamp);

    ASSERT_GT(len, 0u);

    auto result = parse(buf, len);
    ASSERT_TRUE(result.has_value());
    auto& msg = *result;

    EXPECT_EQ(msg.get(tag::ClOrdID), "C100");
    EXPECT_EQ(msg.get(tag::Symbol), "MSFT");
    EXPECT_EQ(msg.get_char(tag::Side), '2');
    EXPECT_EQ(msg.get_char(tag::TimeInForce), '4'); // FOK
}

// ---------------------------------------------------------------------------
// OrderCancelRequest (MsgType=F)
// ---------------------------------------------------------------------------

TEST(FixOrders, build_cancel_order_roundtrip) {
    uint8_t buf[512];
    size_t len = build_cancel_order(
        buf, sizeof(buf),
        "SENDER", "TARGET",
        "CXL001", "ORD001",
        "AAPL",
        Side::Buy);

    ASSERT_GT(len, 0u);

    auto result = parse(buf, len);
    ASSERT_TRUE(result.has_value());
    auto& msg = *result;

    EXPECT_EQ(msg.get(tag::MsgType), "F");
    EXPECT_EQ(msg.get(tag::SenderCompID), "SENDER");
    EXPECT_EQ(msg.get(tag::TargetCompID), "TARGET");
    EXPECT_EQ(msg.get(tag::ClOrdID), "CXL001");
    EXPECT_EQ(msg.get(tag::OrigClOrdID), "ORD001");
    EXPECT_EQ(msg.get(tag::Symbol), "AAPL");
    EXPECT_EQ(msg.get_char(tag::Side), '1'); // Buy
    EXPECT_TRUE(msg.has(tag::TransactTime));
    EXPECT_TRUE(msg.has(tag::SendingTime));
}

// ---------------------------------------------------------------------------
// OrderCancelReplaceRequest (MsgType=G)
// ---------------------------------------------------------------------------

TEST(FixOrders, build_replace_order_roundtrip) {
    uint8_t buf[512];
    size_t len = build_replace_order(
        buf, sizeof(buf),
        "SENDER", "TARGET",
        "REP001", "ORD001",
        "AAPL",
        Side::Buy, OrdType::Limit,
        200.0, 155.75,
        TimeInForce::GTC);

    ASSERT_GT(len, 0u);

    auto result = parse(buf, len);
    ASSERT_TRUE(result.has_value());
    auto& msg = *result;

    EXPECT_EQ(msg.get(tag::MsgType), "G");
    EXPECT_EQ(msg.get(tag::SenderCompID), "SENDER");
    EXPECT_EQ(msg.get(tag::TargetCompID), "TARGET");
    EXPECT_EQ(msg.get(tag::ClOrdID), "REP001");
    EXPECT_EQ(msg.get(tag::OrigClOrdID), "ORD001");
    EXPECT_EQ(msg.get(tag::Symbol), "AAPL");
    EXPECT_EQ(msg.get_char(tag::Side), '1');
    EXPECT_EQ(msg.get_char(tag::OrdType), '2');      // Limit
    EXPECT_EQ(msg.get_char(tag::TimeInForce), '1');   // GTC
    EXPECT_EQ(msg.get_char(tag::HandlInst), '1');

    auto qty = msg.get_double(tag::OrderQty);
    ASSERT_TRUE(qty.has_value());
    EXPECT_DOUBLE_EQ(*qty, 200.0);

    auto price = msg.get_double(tag::Price);
    ASSERT_TRUE(price.has_value());
    EXPECT_DOUBLE_EQ(*price, 155.75);

    EXPECT_TRUE(msg.has(tag::TransactTime));
    EXPECT_TRUE(msg.has(tag::SendingTime));
}

TEST(FixOrders, build_replace_order_market_no_price) {
    uint8_t buf[512];
    size_t len = build_replace_order(
        buf, sizeof(buf),
        "S", "T",
        "REP002", "ORD002",
        "GOOG",
        Side::Sell, OrdType::Market,
        75.0, 0.0,
        TimeInForce::IOC);

    ASSERT_GT(len, 0u);

    auto result = parse(buf, len);
    ASSERT_TRUE(result.has_value());
    auto& msg = *result;

    EXPECT_EQ(msg.get(tag::MsgType), "G");
    EXPECT_EQ(msg.get_char(tag::OrdType), '1'); // Market
    EXPECT_FALSE(msg.has(tag::Price));           // No price for market
}

// ---------------------------------------------------------------------------
// Buffer overflow returns 0
// ---------------------------------------------------------------------------

TEST(FixOrders, build_new_order_buffer_overflow_returns_zero) {
    // A buffer too small to hold even a minimal NewOrderSingle
    uint8_t tiny[32];
    size_t len = build_new_order(
        tiny, sizeof(tiny),
        "SENDER", "TARGET",
        "ORD001", "AAPL",
        Side::Buy, OrdType::Limit,
        100.0, 150.25,
        TimeInForce::Day,
        kTestTimestamp);

    EXPECT_EQ(len, 0u);
}

TEST(FixOrders, build_cancel_order_buffer_overflow_returns_zero) {
    uint8_t tiny[32];
    size_t len = build_cancel_order(
        tiny, sizeof(tiny),
        "SENDER", "TARGET",
        "CXL001", "ORD001",
        "AAPL", Side::Buy);

    EXPECT_EQ(len, 0u);
}

TEST(FixOrders, build_replace_order_buffer_overflow_returns_zero) {
    uint8_t tiny[32];
    size_t len = build_replace_order(
        tiny, sizeof(tiny),
        "SENDER", "TARGET",
        "REP001", "ORD001",
        "AAPL", Side::Buy, OrdType::Limit,
        200.0, 155.75);

    EXPECT_EQ(len, 0u);
}

// ---------------------------------------------------------------------------
// Round-trip: build -> parse -> verify checksum is valid
// ---------------------------------------------------------------------------

TEST(FixOrders, roundtrip_checksum_valid) {
    uint8_t buf[512];
    size_t len = build_new_order(
        buf, sizeof(buf),
        "ALPHA", "OMEGA",
        "RT001", "BTC",
        Side::Buy, OrdType::Limit,
        1.5, 42000.00,
        TimeInForce::GTC,
        kTestTimestamp);

    ASSERT_GT(len, 0u);

    // Manually verify checksum: sum all bytes before "10=" mod 256
    // Find "10=" in the buffer
    std::string_view msg_sv(reinterpret_cast<const char*>(buf), len);
    auto cs_pos = msg_sv.rfind("10=");
    ASSERT_NE(cs_pos, std::string_view::npos);

    // Compute expected checksum over everything before "10="
    uint32_t sum = 0;
    for (size_t i = 0; i < cs_pos; ++i) {
        sum += buf[i];
    }
    uint8_t expected_cs = static_cast<uint8_t>(sum & 0xFF);

    // Extract the checksum value from the message
    // Format: "10=XXX\x01"
    char cs_str[4] = {};
    std::memcpy(cs_str, buf + cs_pos + 3, 3);
    int parsed_cs = std::atoi(cs_str);

    EXPECT_EQ(parsed_cs, static_cast<int>(expected_cs));

    // Also confirm the parser accepts it (it validates checksum internally)
    auto result = parse(buf, len);
    ASSERT_TRUE(result.has_value()) << "parser rejected the checksum";
}

TEST(FixOrders, build_new_order_default_timestamp_uses_current_time) {
    // Build with sending_time_ns=0 (default) to exercise the now() path
    uint8_t buf[512];
    size_t len = build_new_order(
        buf, sizeof(buf),
        "S", "T",
        "ORD_NOW", "SPY",
        Side::Buy, OrdType::Market,
        10.0);

    ASSERT_GT(len, 0u);

    auto result = parse(buf, len);
    ASSERT_TRUE(result.has_value());
    // Just verify timestamp tags are present (value is wall-clock dependent)
    EXPECT_TRUE(result->has(tag::TransactTime));
    EXPECT_TRUE(result->has(tag::SendingTime));
}

// build_new_order with OrdType::Limit + price <= 0 logs a WARN but still emits
// the message — pinning that contract here so a future tightening (return 0
// instead) is an intentional API break rather than an accidental one.
// Operators rely on the message going out so the exchange can produce the
// authoritative reject (e.g. invalid price tag) for compliance audit trails.
TEST(FixOrders, build_new_order_limit_zero_price_emits_with_warning_only) {
    uint8_t buf[512];
    size_t len = build_new_order(
        buf, sizeof(buf),
        "SENDER", "TARGET",
        "ORD_BAD_PX", "AAPL",
        Side::Buy, OrdType::Limit,
        100.0, 0.0,           // <-- price=0 on Limit order
        TimeInForce::Day,
        kTestTimestamp);

    EXPECT_GT(len, 0u) << "build must still emit (warn-only contract)";

    auto result = parse(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get_char(tag::OrdType), '2');  // Limit
    EXPECT_TRUE(result->has(tag::Price));
    auto px = result->get_double(tag::Price);
    ASSERT_TRUE(px.has_value());
    EXPECT_DOUBLE_EQ(*px, 0.0);
}

// build_new_order's limit-zero-price WARN-only branch had a parallel
// build_replace_order limit-negative-price test below, but the symmetric
// limit-zero-price case for replace was missing. Pin both shapes so a
// future tightening that turns either into a hard reject is an
// intentional API break rather than an accidental one.
TEST(FixOrders, build_replace_order_limit_zero_price_emits_with_warning_only) {
    uint8_t buf[512];
    size_t len = build_replace_order(
        buf, sizeof(buf),
        "SENDER", "TARGET",
        "ORD_REPL_ZERO", "ORD_ORIG", "AAPL",
        Side::Buy, OrdType::Limit,
        50.0, 0.0,            // <-- price=0 on Limit replace
        TimeInForce::Day);

    EXPECT_GT(len, 0u) << "build_replace_order must still emit (warn-only contract)";

    auto result = parse(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get_char(tag::OrdType), '2');  // Limit
    EXPECT_TRUE(result->has(tag::Price));
    auto px = result->get_double(tag::Price);
    ASSERT_TRUE(px.has_value());
    EXPECT_DOUBLE_EQ(*px, 0.0);
}

// build_replace_order Market with non-zero price must drop the Price
// tag entirely — symmetric to build_new_order_market_with_nonzero_price.
TEST(FixOrders, build_replace_order_market_with_nonzero_price_omits_price_tag) {
    uint8_t buf[512];
    size_t len = build_replace_order(
        buf, sizeof(buf),
        "SENDER", "TARGET",
        "ORD_REPL_MKT_PX", "ORD_ORIG", "TSLA",
        Side::Sell, OrdType::Market,
        50.0,
        9999.99,              // <-- ignored for Market replace
        TimeInForce::FOK);

    ASSERT_GT(len, 0u);

    auto result = parse(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get_char(tag::OrdType), '1');  // Market
    EXPECT_FALSE(result->has(tag::Price))
        << "Market replace must never emit tag 44, regardless of caller value";
}

// Parallel coverage for build_replace_order — same WARN-only contract on
// Limit + non-positive price (line 282 of orders.hpp).
TEST(FixOrders, build_replace_order_limit_negative_price_emits_with_warning_only) {
    uint8_t buf[512];
    size_t len = build_replace_order(
        buf, sizeof(buf),
        "SENDER", "TARGET",
        "ORD_REPL_NEG", "ORD_ORIG", "AAPL",
        Side::Buy, OrdType::Limit,
        50.0, -1.0,           // <-- negative price on Limit
        TimeInForce::Day);

    EXPECT_GT(len, 0u);

    auto result = parse(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->has(tag::Price));
}

// Parity case: Market with non-zero price MUST suppress tag 44 entirely
// (price field is silently dropped). This complements the existing
// build_new_order_market_no_price (which uses price=0).
TEST(FixOrders, build_new_order_market_with_nonzero_price_omits_price_tag) {
    uint8_t buf[512];
    size_t len = build_new_order(
        buf, sizeof(buf),
        "SENDER", "TARGET",
        "ORD_MKT_PX", "TSLA",
        Side::Sell, OrdType::Market,
        50.0,
        12345.67,             // <-- ignored for Market
        TimeInForce::IOC,
        kTestTimestamp);

    ASSERT_GT(len, 0u);

    auto result = parse(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get_char(tag::OrdType), '1');  // Market
    EXPECT_FALSE(result->has(tag::Price))
        << "Market orders must never emit tag 44, regardless of caller value";
}

// ---------------------------------------------------------------------------
// build_new_order / build_replace_order qty validation parity
//
// build_new_order and build_replace_order share the same risk path —
// neither should ever emit a wire message with non-finite or non-positive
// qty (it would be rejected by the venue with BusinessMessageReject after
// the round-trip). Both must surface a build-time failure (return 0) so
// the caller learns immediately that the order was never sent.
// ---------------------------------------------------------------------------

TEST(FixOrders, build_new_order_rejects_nan_qty) {
    uint8_t buf[512];
    size_t len = build_new_order(
        buf, sizeof(buf),
        "SENDER", "TARGET",
        "ORD_NAN", "AAPL",
        Side::Buy, OrdType::Limit,
        std::nan(""), 100.0,
        TimeInForce::Day);
    EXPECT_EQ(len, 0u) << "NaN qty must be rejected at build time";
}

TEST(FixOrders, build_new_order_rejects_zero_qty) {
    uint8_t buf[512];
    size_t len = build_new_order(
        buf, sizeof(buf),
        "SENDER", "TARGET",
        "ORD_ZERO", "AAPL",
        Side::Buy, OrdType::Limit,
        0.0, 100.0,
        TimeInForce::Day);
    EXPECT_EQ(len, 0u) << "qty=0 must be rejected at build time";
}

TEST(FixOrders, build_new_order_rejects_negative_qty) {
    uint8_t buf[512];
    size_t len = build_new_order(
        buf, sizeof(buf),
        "SENDER", "TARGET",
        "ORD_NEG", "AAPL",
        Side::Buy, OrdType::Limit,
        -50.0, 100.0,
        TimeInForce::Day);
    EXPECT_EQ(len, 0u) << "negative qty must be rejected at build time";
}

// Limit order with NaN/+Inf price reaches MessageBuilder.set_double which
// marks overflow and finish() returns 0 — but only if the orders.hpp
// guard runs BEFORE set_double poisons the buffer. The previous WARN
// path used `<= 0.0`, which silently slipped past NaN (every NaN
// comparison is false), producing a confusing "(overflow or error)"
// log line WITHOUT the actionable "non-positive price" diagnostic.
// The fixed guard surfaces both finite-non-positive and non-finite
// in the same warn so log readers know which knob to fix.
TEST(FixOrders, build_new_order_limit_with_nan_price_returns_zero) {
    uint8_t buf[512];
    size_t len = build_new_order(
        buf, sizeof(buf),
        "SENDER", "TARGET",
        "ORD_NAN_PX", "AAPL",
        Side::Buy, OrdType::Limit,
        100.0, std::nan(""),
        TimeInForce::Day);
    EXPECT_EQ(len, 0u)
        << "NaN price on Limit order must surface as build failure "
           "(set_double marks overflow, finish returns 0)";
}

TEST(FixOrders, build_new_order_limit_with_inf_price_returns_zero) {
    uint8_t buf[512];
    size_t len = build_new_order(
        buf, sizeof(buf),
        "SENDER", "TARGET",
        "ORD_INF_PX", "AAPL",
        Side::Buy, OrdType::Limit,
        100.0, std::numeric_limits<double>::infinity(),
        TimeInForce::Day);
    EXPECT_EQ(len, 0u);
}

TEST(FixOrders, build_replace_order_rejects_nan_qty) {
    uint8_t buf[512];
    size_t len = build_replace_order(
        buf, sizeof(buf),
        "SENDER", "TARGET",
        "ORD_REPL_NAN", "ORD_ORIG", "AAPL",
        Side::Buy, OrdType::Limit,
        std::nan(""), 100.0,
        TimeInForce::Day);
    EXPECT_EQ(len, 0u) << "NaN qty must be rejected at build time";
}

TEST(FixOrders, build_replace_order_rejects_zero_qty) {
    uint8_t buf[512];
    size_t len = build_replace_order(
        buf, sizeof(buf),
        "SENDER", "TARGET",
        "ORD_REPL_ZERO", "ORD_ORIG", "AAPL",
        Side::Buy, OrdType::Limit,
        0.0, 100.0,
        TimeInForce::Day);
    EXPECT_EQ(len, 0u) << "qty=0 must be rejected at build time";
}

TEST(FixOrders, build_replace_order_rejects_negative_qty) {
    uint8_t buf[512];
    size_t len = build_replace_order(
        buf, sizeof(buf),
        "SENDER", "TARGET",
        "ORD_REPL_NEG", "ORD_ORIG", "AAPL",
        Side::Buy, OrdType::Limit,
        -50.0, 100.0,
        TimeInForce::Day);
    EXPECT_EQ(len, 0u) << "negative qty must be rejected at build time";
}

// Mirror of build_new_order_limit_with_nan_price_returns_zero — same
// NaN-slip pattern and same fix for the replace path.
TEST(FixOrders, build_replace_order_limit_with_nan_price_returns_zero) {
    uint8_t buf[512];
    size_t len = build_replace_order(
        buf, sizeof(buf),
        "SENDER", "TARGET",
        "ORD_REPL_NAN_PX", "ORD_ORIG", "AAPL",
        Side::Buy, OrdType::Limit,
        100.0, std::nan(""),
        TimeInForce::Day);
    EXPECT_EQ(len, 0u);
}

TEST(FixOrders, build_replace_order_limit_with_inf_price_returns_zero) {
    uint8_t buf[512];
    size_t len = build_replace_order(
        buf, sizeof(buf),
        "SENDER", "TARGET",
        "ORD_REPL_INF_PX", "ORD_ORIG", "AAPL",
        Side::Buy, OrdType::Limit,
        100.0, std::numeric_limits<double>::infinity(),
        TimeInForce::Day);
    EXPECT_EQ(len, 0u);
}

// ---------------------------------------------------------------------------
// FIX enum formatter tests
// ---------------------------------------------------------------------------

TEST(FixEnumFormatters, SideFormatsCorrectly) {
    EXPECT_EQ(std::format("{}", Side::Buy), "Buy");
    EXPECT_EQ(std::format("{}", Side::Sell), "Sell");
}

TEST(FixEnumFormatters, OrdTypeFormatsCorrectly) {
    EXPECT_EQ(std::format("{}", OrdType::Market), "Market");
    EXPECT_EQ(std::format("{}", OrdType::Limit), "Limit");
}

TEST(FixEnumFormatters, TimeInForceFormatsCorrectly) {
    EXPECT_EQ(std::format("{}", TimeInForce::Day), "Day");
    EXPECT_EQ(std::format("{}", TimeInForce::GTC), "GTC");
    EXPECT_EQ(std::format("{}", TimeInForce::IOC), "IOC");
    EXPECT_EQ(std::format("{}", TimeInForce::FOK), "FOK");
}

TEST(FixEnumFormatters, ExecTypeFormatsCorrectly) {
    EXPECT_EQ(std::format("{}", ExecType::New), "New");
    EXPECT_EQ(std::format("{}", ExecType::Fill), "Fill");
    EXPECT_EQ(std::format("{}", ExecType::Canceled), "Canceled");
    EXPECT_EQ(std::format("{}", ExecType::Rejected), "Rejected");
    EXPECT_EQ(std::format("{}", ExecType::Trade), "Trade");
}

TEST(FixEnumFormatters, OrdStatusFormatsCorrectly) {
    EXPECT_EQ(std::format("{}", OrdStatus::New), "New");
    EXPECT_EQ(std::format("{}", OrdStatus::Filled), "Filled");
    EXPECT_EQ(std::format("{}", OrdStatus::Canceled), "Canceled");
    EXPECT_EQ(std::format("{}", OrdStatus::Rejected), "Rejected");
    EXPECT_EQ(std::format("{}", OrdStatus::PartiallyFilled), "PartiallyFilled");
}

// Direct ord_status_name() coverage — the std::format specialisation
// already round-trips through this function, but pinning the function
// itself lets a future refactor that swaps the formatter to a different
// path (e.g. integer-stringify) catch missing renames here. Also covers
// the Unknown sentinel branch which the format test cannot reach.
TEST(FixEnumFormatters, OrdStatusNameFunction) {
    EXPECT_EQ(ord_status_name(OrdStatus::New),             "New");
    EXPECT_EQ(ord_status_name(OrdStatus::PartiallyFilled), "PartiallyFilled");
    EXPECT_EQ(ord_status_name(OrdStatus::Filled),          "Filled");
    EXPECT_EQ(ord_status_name(OrdStatus::DoneForDay),      "DoneForDay");
    EXPECT_EQ(ord_status_name(OrdStatus::Canceled),        "Canceled");
    EXPECT_EQ(ord_status_name(OrdStatus::Replaced),        "Replaced");
    EXPECT_EQ(ord_status_name(OrdStatus::PendingCancel),   "PendingCancel");
    EXPECT_EQ(ord_status_name(OrdStatus::Stopped),         "Stopped");
    EXPECT_EQ(ord_status_name(OrdStatus::Rejected),        "Rejected");
    EXPECT_EQ(ord_status_name(OrdStatus::Suspended),       "Suspended");
    EXPECT_EQ(ord_status_name(OrdStatus::PendingNew),      "PendingNew");
    EXPECT_EQ(ord_status_name(OrdStatus::Calculated),      "Calculated");
    EXPECT_EQ(ord_status_name(OrdStatus::Expired),         "Expired");
    EXPECT_EQ(ord_status_name(OrdStatus::PendingReplace),  "PendingReplace");
}

TEST(FixEnumFormatters, OrdStatusNameUnknownReturnsSentinel) {
    auto bogus = static_cast<OrdStatus>(static_cast<char>('X'));
    EXPECT_EQ(ord_status_name(bogus), "Unknown");
}

TEST(FixEnumFormatters, OrderStateFormatsCorrectly) {
    EXPECT_EQ(std::format("{}", OrderState::PendingNew), "PendingNew");
    EXPECT_EQ(std::format("{}", OrderState::New), "New");
    EXPECT_EQ(std::format("{}", OrderState::Filled), "Filled");
    EXPECT_EQ(std::format("{}", OrderState::Canceled), "Canceled");
    EXPECT_EQ(std::format("{}", OrderState::Rejected), "Rejected");
}

TEST(FixEnumFormatters, SideNameFunction) {
    EXPECT_EQ(side_name(Side::Buy), "Buy");
    EXPECT_EQ(side_name(Side::Sell), "Sell");
}

TEST(FixEnumFormatters, OrdTypeNameFunction) {
    EXPECT_EQ(ord_type_name(OrdType::Market), "Market");
    EXPECT_EQ(ord_type_name(OrdType::Limit), "Limit");
}

TEST(FixEnumFormatters, ExecTypeNameFunction) {
    EXPECT_EQ(exec_type_name(ExecType::PartialFill), "PartialFill");
    EXPECT_EQ(exec_type_name(ExecType::PendingNew), "PendingNew");
    EXPECT_EQ(exec_type_name(ExecType::Expired), "Expired");
}

TEST(FixEnumFormatters, OrderStateNameFunction) {
    EXPECT_EQ(order_state_name(OrderState::PartiallyFilled), "PartiallyFilled");
    EXPECT_EQ(order_state_name(OrderState::PendingCancel), "PendingCancel");
}

// ─────────────────────────────────────────────────────────────────────────────
// Unknown-value fallbacks — every *_name() returns "Unknown" sentinel for a
// value reinterpret_cast'd outside the defined enum range. Without these
// tests, a future refactor that drops the trailing `return "Unknown";` would
// silently turn the function into UB territory (control falls off the end
// of a non-void function) — only Clang's -Wreturn-type would catch it, and
// only in optimised builds.
// ─────────────────────────────────────────────────────────────────────────────

TEST(FixEnumFormatters, SideNameUnknownReturnsSentinel) {
    auto bogus = static_cast<Side>(static_cast<char>(0x7E));
    EXPECT_EQ(side_name(bogus), "Unknown");
}

TEST(FixEnumFormatters, OrdTypeNameUnknownReturnsSentinel) {
    auto bogus = static_cast<OrdType>(static_cast<char>('X'));
    EXPECT_EQ(ord_type_name(bogus), "Unknown");
}

TEST(FixEnumFormatters, TimeInForceNameUnknownReturnsSentinel) {
    auto bogus = static_cast<TimeInForce>(static_cast<char>('Z'));
    EXPECT_EQ(time_in_force_name(bogus), "Unknown");
}

TEST(FixEnumFormatters, ExecTypeNameUnknownReturnsSentinel) {
    auto bogus = static_cast<ExecType>(static_cast<char>(0xFE));
    EXPECT_EQ(exec_type_name(bogus), "Unknown");
}

TEST(FixEnumFormatters, OrderStateNameUnknownReturnsSentinel) {
    auto bogus = static_cast<OrderState>(static_cast<uint8_t>(99));
    EXPECT_EQ(order_state_name(bogus), "Unknown");
}

// time_in_force_name happy path also worth pinning — it had no test until now.
TEST(FixEnumFormatters, TimeInForceNameFunction) {
    EXPECT_EQ(time_in_force_name(TimeInForce::Day), "Day");
    EXPECT_EQ(time_in_force_name(TimeInForce::GTC), "GTC");
    EXPECT_EQ(time_in_force_name(TimeInForce::IOC), "IOC");
    EXPECT_EQ(time_in_force_name(TimeInForce::FOK), "FOK");
}
