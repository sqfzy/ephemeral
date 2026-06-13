#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include "eph/sbe.hpp"

using namespace eph::sbe;

// ---------------------------------------------------------------------------
// Synthetic BestBidAskStreamEvent — stream_1_0.xml id=10001:
//   header(8) | block(50) | varString8 symbol
// ---------------------------------------------------------------------------
namespace {

void put_le16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8));
}
void put_le_i64(std::vector<uint8_t>& b, int64_t v) {
    uint64_t u; std::memcpy(&u, &v, 8);
    for (int i = 0; i < 8; ++i) b.push_back(uint8_t(u >> (8 * i)));
}

std::vector<uint8_t> build_bba(int64_t event_us, int64_t update_id,
                               int8_t price_exp, int8_t qty_exp,
                               int64_t bid_px, int64_t bid_qty,
                               int64_t ask_px, int64_t ask_qty,
                               const std::string& symbol,
                               uint16_t schema_id = 1, uint16_t version = 0) {
    std::vector<uint8_t> b;
    put_le16(b, 50);          // blockLength
    put_le16(b, 10001);       // templateId
    put_le16(b, schema_id);
    put_le16(b, version);
    put_le_i64(b, event_us);
    put_le_i64(b, update_id);
    b.push_back(uint8_t(price_exp));
    b.push_back(uint8_t(qty_exp));
    put_le_i64(b, bid_px);
    put_le_i64(b, bid_qty);
    put_le_i64(b, ask_px);
    put_le_i64(b, ask_qty);
    b.push_back(uint8_t(symbol.size()));
    b.insert(b.end(), symbol.begin(), symbol.end());
    return b;
}

} // namespace

TEST(SbeBestBidAsk, decodes_all_fields) {
    auto buf = build_bba(/*event*/1718000000000000, /*update*/987654321,
                         /*pexp*/-2, /*qexp*/-8,
                         /*bid*/6543210, /*bidq*/1234000,
                         /*ask*/6543220, /*askq*/150000000, "BTCUSDT");
    auto v = parse(buf.data(), buf.size());
    ASSERT_TRUE(v.has_value()) << parse_error_name(v.error());
    EXPECT_EQ(v->template_id, 10001);
    namespace bba = binance::stream::best_bid_ask;
    ASSERT_TRUE(binance::stream::is_supported(*v));
    EXPECT_EQ(bba::event_time_us(*v), 1718000000000000);
    EXPECT_EQ(bba::book_update_id(*v), 987654321);
    EXPECT_DOUBLE_EQ(bba::bid_price(*v), 65432.10);
    EXPECT_DOUBLE_EQ(bba::bid_qty(*v), 0.01234);
    EXPECT_DOUBLE_EQ(bba::ask_price(*v), 65432.20);
    EXPECT_DOUBLE_EQ(bba::ask_qty(*v), 1.5);
    EXPECT_EQ(bba::symbol(*v), "BTCUSDT");
}

TEST(SbeBestBidAsk, short_symbol) {
    auto buf = build_bba(1, 2, 0, 0, 9, 9, 9, 9, "OP");
    auto v = parse(buf.data(), buf.size());
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(binance::stream::best_bid_ask::symbol(*v), "OP");
}

TEST(SbeBestBidAsk, truncated_symbol_returns_empty) {
    auto buf = build_bba(1, 2, 0, 0, 9, 9, 9, 9, "BTCUSDT");
    buf.resize(buf.size() - 3);   // chop part of the symbol
    auto v = parse(buf.data(), buf.size());
    ASSERT_TRUE(v.has_value());
    EXPECT_TRUE(binance::stream::best_bid_ask::symbol(*v).empty());
}

TEST(SbeBestBidAsk, wrong_schema_not_supported) {
    auto buf = build_bba(1, 2, 0, 0, 9, 9, 9, 9, "X", /*schema*/1, /*ver*/9);
    auto v = parse(buf.data(), buf.size());
    ASSERT_TRUE(v.has_value());
    EXPECT_FALSE(binance::stream::is_supported(*v));
}

TEST(SbeBestBidAsk, dispatch_routes_to_best_bid_ask_tag) {
    auto buf = build_bba(1, 2, 0, 0, 9, 9, 9, 9, "X");
    auto v = parse(buf.data(), buf.size());
    ASSERT_TRUE(v.has_value());
    bool routed = false;
    dispatch(*v, [&](auto tag, const MessageView&) {
        if constexpr (std::is_same_v<decltype(tag), msg::BestBidAsk>) routed = true;
    });
    EXPECT_TRUE(routed);
}
