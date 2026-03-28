#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
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
