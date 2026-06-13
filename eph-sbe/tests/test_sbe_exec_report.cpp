#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "eph/sbe.hpp"

using namespace eph::sbe;

// ---------------------------------------------------------------------------
// Synthetic ExecutionReportEvent — spot_3_2.xml id=603:
//   header(8) | block(281) | symbol vs8 | clientOrderId vs8
// Only the consumed fields are set; the rest of the block is zero.
// ---------------------------------------------------------------------------
namespace {

void put_le16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
void put_vs8(std::vector<uint8_t>& b, const std::string& s) { b.push_back(uint8_t(s.size())); b.insert(b.end(), s.begin(), s.end()); }

void set_i64(std::vector<uint8_t>& blk, std::size_t off, int64_t v) {
    uint64_t u; std::memcpy(&u, &v, 8);
    for (int i = 0; i < 8; ++i) blk[off + i] = uint8_t(u >> (8 * i));
}

struct ExecSpec {
    int64_t event_us, transact_us, order_id, execution_id, executed_qty, cum_quote_qty, last_price;
    int8_t  price_exp, qty_exp;
    uint8_t execution_type, order_status;
    std::string symbol, client_order_id;
};

std::vector<uint8_t> build_exec(const ExecSpec& s, uint16_t schema = 3, uint16_t ver = 2) {
    std::vector<uint8_t> b;
    put_le16(b, 281); put_le16(b, 603); put_le16(b, schema); put_le16(b, ver);
    std::vector<uint8_t> blk(281, 0);
    set_i64(blk, 0,   s.event_us);
    set_i64(blk, 8,   s.transact_us);
    blk[16] = uint8_t(s.price_exp);
    blk[17] = uint8_t(s.qty_exp);
    set_i64(blk, 35,  s.order_id);
    blk[94] = s.execution_type;
    blk[95] = s.order_status;
    set_i64(blk, 104, s.execution_id);
    set_i64(blk, 112, s.executed_qty);
    set_i64(blk, 120, s.cum_quote_qty);
    set_i64(blk, 136, s.last_price);
    b.insert(b.end(), blk.begin(), blk.end());
    put_vs8(b, s.symbol);
    put_vs8(b, s.client_order_id);
    return b;
}

} // namespace

TEST(SbeExecReport, decodes_fill_fields) {
    // Filled: cum 0.002 @ avg 65000 → executedQty 200000 (exp -8), cumQuote 13000000000 (exp -2... )
    ExecSpec s{
        .event_us = 1718000000111000, .transact_us = 1718000000110000,
        .order_id = 999, .execution_id = 1234567,
        .executed_qty = 200000,            // × 10^-8 = 0.002
        .cum_quote_qty = 13000,            // × 10^-2 = 130.0
        .last_price = 6500000,             // × 10^-2 = 65000.0
        .price_exp = -2, .qty_exp = -8,
        .execution_type = 1 /*TRADE*/, .order_status = 2 /*Filled*/,
        .symbol = "BTCUSDT", .client_order_id = "1001",
    };
    auto buf = build_exec(s);
    auto v = parse(buf.data(), buf.size());
    ASSERT_TRUE(v.has_value()) << parse_error_name(v.error());
    EXPECT_EQ(v->template_id, 603);

    namespace er = binance::execution_report;
    EXPECT_EQ(er::event_time_us(*v), 1718000000111000);
    EXPECT_EQ(er::transact_time_us(*v), 1718000000110000);
    EXPECT_EQ(er::order_id(*v), 999);
    EXPECT_EQ(er::execution_id(*v), 1234567);
    EXPECT_EQ(er::execution_type(*v), 1);
    EXPECT_EQ(er::order_status(*v), uint8_t(binance::OrderStatus::Filled));
    EXPECT_DOUBLE_EQ(er::executed_qty(*v), 0.002);
    EXPECT_DOUBLE_EQ(er::cummulative_quote_qty(*v), 130.0);
    EXPECT_DOUBLE_EQ(er::last_price(*v), 65000.0);
    EXPECT_EQ(er::symbol(*v), "BTCUSDT");
    EXPECT_EQ(er::client_order_id(*v), "1001");
}

TEST(SbeExecReport, truncated_client_order_id_returns_empty) {
    ExecSpec s{ .event_us=1,.transact_us=1,.order_id=1,.execution_id=1,
                .executed_qty=0,.cum_quote_qty=0,.last_price=0,
                .price_exp=0,.qty_exp=0,.execution_type=0,.order_status=0,
                .symbol="BTCUSDT",.client_order_id="1001" };
    auto buf = build_exec(s);
    buf.resize(buf.size() - 2);   // chop the clientOrderId
    auto v = parse(buf.data(), buf.size());
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(binance::execution_report::symbol(*v), "BTCUSDT");
    EXPECT_TRUE(binance::execution_report::client_order_id(*v).empty());
}

TEST(SbeExecReport, dispatch_routes_to_execution_report_tag) {
    ExecSpec s{ .event_us=1,.transact_us=1,.order_id=1,.execution_id=1,
                .executed_qty=0,.cum_quote_qty=0,.last_price=0,
                .price_exp=0,.qty_exp=0,.execution_type=0,.order_status=0,
                .symbol="X",.client_order_id="1" };
    auto buf = build_exec(s);
    auto v = parse(buf.data(), buf.size());
    ASSERT_TRUE(v.has_value());
    bool routed = false;
    dispatch(*v, [&](auto tag, const MessageView&) {
        if constexpr (std::is_same_v<decltype(tag), msg::ExecutionReport>) routed = true;
    });
    EXPECT_TRUE(routed);
}
