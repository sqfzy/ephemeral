/// @file fix_trading_demo.cpp
/// FIX protocol end-to-end demo — build orders, parse execution reports.
///
/// Demonstrates the FIX 4.4 builder and parser without a network connection.
/// Shows the complete lifecycle: build NewOrderSingle, build
/// ExecutionReport (simulating server response), parse both messages,
/// and extract typed field values.
///
/// This is a standalone demo — no server required. For sending FIX
/// over WebSocket, combine with Transport (see production_client.cpp).
///
/// Usage:
///   xmake build fix_trading_demo && xmake run fix_trading_demo

#include <cstdint>
#include <format>
#include <iostream>

#include <spdlog/spdlog.h>

#include "eph/fix.hpp"
#include "eph/utils/time.hpp"

using namespace eph::fix;

/// Build a FIX 4.4 NewOrderSingle message.
/// Returns total message length, or 0 on failure.
static size_t build_new_order(uint8_t* buf, size_t capacity,
                               int seq_num, uint64_t timestamp_ns) {
    MessageBuilder b(buf, capacity);

    // Session-level fields
    b.set(tag::MsgType, "D");                       // NewOrderSingle
    b.set(tag::SenderCompID, "MY_ALGO");
    b.set(tag::TargetCompID, "EXCHANGE");
    b.set_int(tag::MsgSeqNum, seq_num);
    b.set_timestamp(tag::SendingTime, timestamp_ns);

    // Order fields
    b.set(tag::ClOrdID, "ORD-20260327-001");
    b.set(tag::Symbol, "AAPL");
    b.set_int(tag::Side, 1);                        // 1 = Buy
    b.set_int(tag::OrderQty, 100);
    b.set_int(tag::OrdType, 2);                     // 2 = Limit
    b.set_decimal(tag::Price, "185.50");             // Exact decimal, no float precision loss
    b.set_int(tag::TimeInForce, 0);                 // 0 = Day
    b.set_timestamp(tag::TransactTime, timestamp_ns);
    b.set_int(tag::HandlInst, 1);                   // 1 = Automated, no intervention
    b.set(tag::Account, "ACCT001");

    return b.finish("FIX.4.4");
}

/// Build a simulated ExecutionReport (server response).
static size_t build_exec_report(uint8_t* buf, size_t capacity,
                                 int seq_num, uint64_t timestamp_ns) {
    MessageBuilder b(buf, capacity);

    // Session-level
    b.set(tag::MsgType, "8");                       // ExecutionReport
    b.set(tag::SenderCompID, "EXCHANGE");
    b.set(tag::TargetCompID, "MY_ALGO");
    b.set_int(tag::MsgSeqNum, seq_num);
    b.set_timestamp(tag::SendingTime, timestamp_ns);

    // Execution fields
    b.set(tag::OrderID, "EX-99887766");
    b.set(tag::ExecID, "FILL-001");
    b.set(tag::ClOrdID, "ORD-20260327-001");
    b.set(tag::ExecType, "F");                      // F = Trade (fill)
    b.set(tag::OrdStatus, "2");                     // 2 = Filled
    b.set(tag::Symbol, "AAPL");
    b.set_int(tag::Side, 1);
    b.set_int(tag::OrderQty, 100);
    b.set_decimal(tag::LastPx, "185.50");
    b.set_int(tag::LastQty, 100);
    b.set_int(tag::CumQty, 100);
    b.set_int(tag::LeavesQty, 0);
    b.set_decimal(tag::AvgPx, "185.50");
    b.set_timestamp(tag::TransactTime, timestamp_ns);

    return b.finish("FIX.4.4");
}

/// Parse and display a FIX message.
static void parse_and_display(const uint8_t* buf, size_t len,
                               std::string_view label) {
    auto result = parse(buf, len);
    if (!result) {
        spdlog::error("{}: parse failed: {}", label,
                      parse_error_name(result.error()));
        return;
    }

    auto& msg = *result;
    spdlog::info("=== {} ===", label);
    spdlog::info("  MsgType: {} ({})",
                 msg.msg_type().value_or("?"),
                 msg.msg_type() ? tag::msg_type_name(*msg.msg_type()) : "unknown");

    // Extract typed values
    if (auto sym = msg.get(tag::Symbol))
        spdlog::info("  Symbol:   {}", *sym);
    if (auto side = msg.get_int(tag::Side))
        spdlog::info("  Side:     {} ({})", *side, *side == 1 ? "Buy" : "Sell");
    if (auto qty = msg.get_int(tag::OrderQty))
        spdlog::info("  Qty:      {}", *qty);
    if (auto px = msg.get(tag::Price))
        spdlog::info("  Price:    {}", *px);

    // Execution-specific fields
    if (auto status = msg.get(tag::OrdStatus)) {
        const char* name = "Unknown";
        if (*status == "0") name = "New";
        else if (*status == "1") name = "PartiallyFilled";
        else if (*status == "2") name = "Filled";
        else if (*status == "4") name = "Canceled";
        else if (*status == "8") name = "Rejected";
        spdlog::info("  Status:   {} ({})", *status, name);
    }
    if (auto last_px = msg.get(tag::LastPx))
        spdlog::info("  LastPx:   {}", *last_px);
    if (auto last_qty = msg.get_int(tag::LastQty))
        spdlog::info("  LastQty:  {}", *last_qty);
    if (auto leaves = msg.get_int(tag::LeavesQty))
        spdlog::info("  Leaves:   {}", *leaves);

    // Full message dump (for debugging)
    spdlog::debug("\n{}", msg.dump());
}

int main() {
    spdlog::set_level(spdlog::level::info);

    // Use a fixed timestamp for reproducible output
    uint64_t now_ns = 1774828800000000000ULL; // 2026-03-27 00:00:00 UTC

    // --- Step 1: Build a NewOrderSingle ---
    uint8_t order_buf[512];
    size_t order_len = build_new_order(order_buf, sizeof(order_buf), 1, now_ns);
    if (order_len == 0) {
        spdlog::error("Failed to build NewOrderSingle");
        return 1;
    }
    spdlog::info("Built NewOrderSingle: {} bytes", order_len);
    parse_and_display(order_buf, order_len, "NewOrderSingle (Client -> Exchange)");

    // --- Step 2: Build a simulated ExecutionReport (fill) ---
    uint8_t exec_buf[512];
    size_t exec_len = build_exec_report(exec_buf, sizeof(exec_buf), 1, now_ns);
    if (exec_len == 0) {
        spdlog::error("Failed to build ExecutionReport");
        return 1;
    }
    spdlog::info("Built ExecutionReport: {} bytes", exec_len);
    parse_and_display(exec_buf, exec_len, "ExecutionReport (Exchange -> Client)");

    // --- Step 3: Demonstrate set_price() for exact decimal encoding ---
    spdlog::info("");
    spdlog::info("=== Price Encoding Demo ===");

    uint8_t price_buf[256];
    MessageBuilder pb(price_buf, sizeof(price_buf));
    pb.set(tag::MsgType, "D");
    pb.set(tag::Symbol, "BTC-USD");

    // set_price: integer mantissa avoids float precision issues
    // 5432100 with 2 decimals = "54321.00"
    pb.set_price(tag::Price, 5432100, 2);
    // set_decimal: string input, zero float math
    pb.set_decimal(tag::StopPx, "54000.99");

    size_t price_len = pb.finish();
    if (price_len > 0) {
        auto result = parse(price_buf, price_len);
        if (result) {
            spdlog::info("  set_price(54321, 2) → Price={}",
                         result->get(tag::Price).value_or("?"));
            spdlog::info("  set_decimal(\"54000.99\") → StopPx={}",
                         result->get(tag::StopPx).value_or("?"));
        }
    }

    // --- Step 4: Demonstrate JSON export ---
    spdlog::info("");
    spdlog::info("=== JSON Export ===");
    auto result = parse(order_buf, order_len);
    if (result) {
        spdlog::info("{}", result->to_json());
    }

    return 0;
}
