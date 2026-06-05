#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "eph/sbe.hpp"

using namespace eph::sbe;

// ---------------------------------------------------------------------------
// Synthetic BookTickerResponse builder — emits bytes per spot_3_2.xml id=212:
//   header(8) | group hdr(4) | [ block(34) + varString8 symbol ] * N
// ---------------------------------------------------------------------------

namespace {

struct TickerSpec {
    int8_t      price_exp;
    int8_t      qty_exp;
    int64_t     bid_price;   // mantissa; INT64_MIN == null
    int64_t     bid_qty;
    int64_t     ask_price;   // mantissa; INT64_MIN == null
    int64_t     ask_qty;
    std::string symbol;
};

void put_le16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void put_i8(std::vector<uint8_t>& b, int8_t v) {
    b.push_back(static_cast<uint8_t>(v));
}

void put_le_i64(std::vector<uint8_t>& b, int64_t v) {
    uint64_t u;
    std::memcpy(&u, &v, 8);
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>((u >> (8 * i)) & 0xFF));
}

std::vector<uint8_t> build_book_ticker(const std::vector<TickerSpec>& tickers,
                                       uint16_t schema_id = 3,
                                       uint16_t version = 2,
                                       uint16_t group_block_len = 34) {
    std::vector<uint8_t> b;
    // messageHeader: blockLength=0 (empty root), templateId=212.
    put_le16(b, 0);
    put_le16(b, 212);
    put_le16(b, schema_id);
    put_le16(b, version);
    // groupSize16Encoding: blockLength, numInGroup.
    put_le16(b, group_block_len);
    put_le16(b, static_cast<uint16_t>(tickers.size()));
    for (const auto& t : tickers) {
        const std::size_t block_start = b.size();
        put_i8(b, t.price_exp);
        put_i8(b, t.qty_exp);
        put_le_i64(b, t.bid_price);
        put_le_i64(b, t.bid_qty);
        put_le_i64(b, t.ask_price);
        put_le_i64(b, t.ask_qty);
        // Pad the fixed block to group_block_len if the schema declares it larger.
        while (b.size() - block_start < group_block_len) b.push_back(0);
        // varString8 symbol.
        b.push_back(static_cast<uint8_t>(t.symbol.size()));
        b.insert(b.end(), t.symbol.begin(), t.symbol.end());
    }
    return b;
}

constexpr int64_t kNull = std::numeric_limits<int64_t>::min();

} // namespace

// ===========================================================================
// Single ticker
// ===========================================================================

TEST(SbeBookTicker, single_ticker_decodes_all_fields) {
    // bid 65432.10, qty 0.01234, ask 65432.20, qty 1.5 — price exp -2, qty exp -8.
    auto buf = build_book_ticker({{
        .price_exp = -2, .qty_exp = -8,
        .bid_price = 6543210, .bid_qty = 1234000,
        .ask_price = 6543220, .ask_qty = 150000000,
        .symbol = "BTCUSDT",
    }});

    auto v = parse(buf.data(), buf.size());
    ASSERT_TRUE(v.has_value()) << parse_error_name(v.error());
    EXPECT_EQ(v->template_id, 212);

    int count = 0;
    auto n = binance::for_each_ticker(*v, [&](const uint8_t* t) {
        ++count;
        namespace bt = binance::book_ticker;
        ASSERT_TRUE(bt::bid_price(t).has_value());
        EXPECT_DOUBLE_EQ(*bt::bid_price(t), 65432.10);
        EXPECT_DOUBLE_EQ(bt::bid_qty(t), 0.01234);
        ASSERT_TRUE(bt::ask_price(t).has_value());
        EXPECT_DOUBLE_EQ(*bt::ask_price(t), 65432.20);
        EXPECT_DOUBLE_EQ(bt::ask_qty(t), 1.5);
        EXPECT_EQ(bt::symbol(t), "BTCUSDT");
    });
    ASSERT_TRUE(n.has_value()) << parse_error_name(n.error());
    EXPECT_EQ(*n, 1u);
    EXPECT_EQ(count, 1);
}

// ===========================================================================
// Multiple tickers with different symbol lengths (var-data iteration)
// ===========================================================================

TEST(SbeBookTicker, multi_ticker_variable_symbol_lengths) {
    auto buf = build_book_ticker({
        {.price_exp = -1, .qty_exp = -1, .bid_price = 100, .bid_qty = 5,
         .ask_price = 101, .ask_qty = 6, .symbol = "ETHUSDT"},
        {.price_exp = -2, .qty_exp = -2, .bid_price = 200, .bid_qty = 7,
         .ask_price = 201, .ask_qty = 8, .symbol = "OP"},
        {.price_exp = 0, .qty_exp = 0, .bid_price = 9, .bid_qty = 9,
         .ask_price = 9, .ask_qty = 9, .symbol = "1000SHIBUSDT"},
    });

    auto v = parse(buf.data(), buf.size());
    ASSERT_TRUE(v.has_value());

    std::vector<std::string> syms;
    std::vector<double> bids;
    auto n = binance::for_each_ticker(*v, [&](const uint8_t* t) {
        namespace bt = binance::book_ticker;
        syms.emplace_back(bt::symbol(t));
        bids.push_back(bt::bid_price(t).value());
    });
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(*n, 3u);
    ASSERT_EQ(syms.size(), 3u);
    EXPECT_EQ(syms[0], "ETHUSDT");
    EXPECT_EQ(syms[1], "OP");
    EXPECT_EQ(syms[2], "1000SHIBUSDT");
    EXPECT_DOUBLE_EQ(bids[0], 10.0);   // 100 × 10^-1
    EXPECT_DOUBLE_EQ(bids[1], 2.0);    // 200 × 10^-2
    EXPECT_DOUBLE_EQ(bids[2], 9.0);    // 9 × 10^0
}

// ===========================================================================
// Optional bid/ask null sentinel
// ===========================================================================

TEST(SbeBookTicker, optional_price_null_returns_nullopt) {
    auto buf = build_book_ticker({{
        .price_exp = -2, .qty_exp = -2,
        .bid_price = kNull, .bid_qty = 5,   // no bid
        .ask_price = 6543220, .ask_qty = 6, // has ask
        .symbol = "BTCUSDT",
    }});
    auto v = parse(buf.data(), buf.size());
    ASSERT_TRUE(v.has_value());

    auto n = binance::for_each_ticker(*v, [&](const uint8_t* t) {
        namespace bt = binance::book_ticker;
        EXPECT_FALSE(bt::bid_price(t).has_value());     // null → nullopt
        ASSERT_TRUE(bt::ask_price(t).has_value());
        EXPECT_DOUBLE_EQ(*bt::ask_price(t), 65432.20);
        EXPECT_DOUBLE_EQ(bt::bid_qty(t), 0.05);          // qty still present
    });
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(*n, 1u);
}

// ===========================================================================
// Empty group (numInGroup == 0)
// ===========================================================================

TEST(SbeBookTicker, empty_group_delivers_zero) {
    auto buf = build_book_ticker({});
    auto v = parse(buf.data(), buf.size());
    ASSERT_TRUE(v.has_value());

    int count = 0;
    auto n = binance::for_each_ticker(*v, [&](const uint8_t*) { ++count; });
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(*n, 0u);
    EXPECT_EQ(count, 0);
}

// ===========================================================================
// Truncation — a partial entry must error, not over-read
// ===========================================================================

TEST(SbeBookTicker, truncated_entry_returns_truncated) {
    auto buf = build_book_ticker({{
        .price_exp = -2, .qty_exp = -2, .bid_price = 1, .bid_qty = 1,
        .ask_price = 1, .ask_qty = 1, .symbol = "BTCUSDT"}});
    // Lop off the last few bytes of the symbol.
    buf.resize(buf.size() - 4);

    auto v = parse(buf.data(), buf.size());
    ASSERT_TRUE(v.has_value());
    auto n = binance::for_each_ticker(*v, [](const uint8_t*) {});
    ASSERT_FALSE(n.has_value());
    EXPECT_EQ(n.error(), ParseError::kTruncated);
}

TEST(SbeBookTicker, missing_group_header_returns_truncated) {
    // Header only, no group dimension bytes.
    auto buf = build_book_ticker({});
    buf.resize(kHeaderSize + 2); // only 2 of the 4 group-header bytes

    auto v = parse(buf.data(), buf.size());
    ASSERT_TRUE(v.has_value());
    auto n = binance::for_each_ticker(*v, [](const uint8_t*) {});
    ASSERT_FALSE(n.has_value());
    EXPECT_EQ(n.error(), ParseError::kTruncated);
}

// ===========================================================================
// Schema guard — wrong schema/version is refused, not silently mis-decoded
// ===========================================================================

TEST(SbeBookTicker, unsupported_schema_is_rejected) {
    auto buf = build_book_ticker({{.price_exp = -2, .qty_exp = -2, .bid_price = 1,
                                    .bid_qty = 1, .ask_price = 1, .ask_qty = 1,
                                    .symbol = "BTCUSDT"}},
                                 /*schema_id=*/3, /*version=*/99);
    auto v = parse(buf.data(), buf.size());
    ASSERT_TRUE(v.has_value());
    EXPECT_FALSE(binance::is_supported(*v));
    auto n = binance::for_each_ticker(*v, [](const uint8_t*) {});
    ASSERT_FALSE(n.has_value());
    EXPECT_EQ(n.error(), ParseError::kMalformedGroup);
}

// ===========================================================================
// dispatch() routes template id 212 to msg::BookTicker
// ===========================================================================

TEST(SbeBookTicker, dispatch_routes_to_book_ticker_tag) {
    auto buf = build_book_ticker({{.price_exp = 0, .qty_exp = 0, .bid_price = 1,
                                   .bid_qty = 1, .ask_price = 1, .ask_qty = 1,
                                   .symbol = "X"}});
    auto v = parse(buf.data(), buf.size());
    ASSERT_TRUE(v.has_value());

    bool routed = false;
    dispatch(*v, [&](auto tag, const MessageView&) {
        if constexpr (std::is_same_v<decltype(tag), msg::BookTicker>) routed = true;
    });
    EXPECT_TRUE(routed);
}
