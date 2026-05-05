/// @file test_ouch.cpp
/// Unit tests for the OUCH 5.0 order entry protocol builders and views.

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <gtest/gtest.h>

#include "eph/itch/ouch.hpp"

using namespace eph::itch::ouch;
using namespace eph::itch;

// ---------------------------------------------------------------------------
// EnterOrder builder tests
// ---------------------------------------------------------------------------

TEST(OuchEnterOrder, BuildVerifyWireBytes) {
    std::array<uint8_t, EnterOrder::kSize> buf{};

    const size_t n = EnterOrder::build(
        buf.data(),
        "ABCDEFGHIJKLMN",  // 14-char token
        'B',               // buy
        100,               // shares
        "AAPL",            // symbol (will be right-padded)
        1500000,           // $150.00
        0,                 // day order
        "FIRM"             // MPID
    );

    ASSERT_EQ(n, EnterOrder::kSize);

    // Type byte
    EXPECT_EQ(buf[0], 'O');

    // Token at offset 1..14
    EXPECT_EQ((std::string_view{reinterpret_cast<const char*>(buf.data() + 1), 14}),
              "ABCDEFGHIJKLMN");

    // Side at offset 15
    EXPECT_EQ(buf[15], 'B');

    // Shares at offset 16..19 (big-endian)
    EXPECT_EQ(read_be32(buf.data() + 16), 100u);

    // Symbol at offset 20..27 (right-padded)
    EXPECT_EQ((std::string_view{reinterpret_cast<const char*>(buf.data() + 20), 8}),
              "AAPL    ");

    // Price at offset 28..31
    EXPECT_EQ(read_be32(buf.data() + 28), 1500000u);

    // TIF at offset 32..35
    EXPECT_EQ(read_be32(buf.data() + 32), 0u);

    // Firm at offset 36..39
    EXPECT_EQ((std::string_view{reinterpret_cast<const char*>(buf.data() + 36), 4}),
              "FIRM");

    // Display at offset 40
    EXPECT_EQ(buf[40], 'Y');
}

TEST(OuchEnterOrder, InvalidSideReturnsZero) {
    std::array<uint8_t, EnterOrder::kSize> buf{};
    EXPECT_EQ(EnterOrder::build(buf.data(), "TOKEN         ", 'X', 100, "AAPL", 100, 0, "FIRM"), 0u);
}

TEST(OuchEnterOrder, NullBufferReturnsZero) {
    EXPECT_EQ(EnterOrder::build(nullptr, "TOKEN         ", 'B', 100, "AAPL", 100, 0, "FIRM"), 0u);
}

TEST(OuchEnterOrder, ShortTokenIsPadded) {
    std::array<uint8_t, EnterOrder::kSize> buf{};
    EnterOrder::build(buf.data(), "TK", 'B', 100, "AAPL", 100, 0, "FIRM");
    // Token should be "TK" followed by 12 spaces
    EXPECT_EQ((std::string_view{reinterpret_cast<const char*>(buf.data() + 1), 14}),
              "TK            ");
}

// Default-constructed std::string_view's data() may legitimately be
// nullptr; write_padded would otherwise hit memcpy(p, nullptr, 0) which
// is technically UB per ISO C and trips UBSan. Verify the guard so the
// empty-string path produces an all-spaces field without UB.
TEST(OuchEnterOrder, EmptyStringViewArgsProduceAllSpaces) {
    std::array<uint8_t, EnterOrder::kSize> buf{};
    auto n = EnterOrder::build(buf.data(),
                               /*token=*/std::string_view{},
                               'B', 100,
                               /*symbol=*/std::string_view{},
                               100, 0,
                               /*firm=*/std::string_view{});
    ASSERT_EQ(n, EnterOrder::kSize);
    // Every text field should be entirely spaces.
    EXPECT_EQ((std::string_view{reinterpret_cast<const char*>(buf.data() + 1), 14}),
              "              ");
    EXPECT_EQ((std::string_view{reinterpret_cast<const char*>(buf.data() + 20), 8}),
              "        ");
    EXPECT_EQ((std::string_view{reinterpret_cast<const char*>(buf.data() + 36), 4}),
              "    ");
}

// Oversize text fields must be rejected (HFT correctness): silent truncation
// would let two distinct tokens collide on the wire and mis-route the order.
TEST(OuchEnterOrder, OversizeTokenReturnsZero) {
    std::array<uint8_t, EnterOrder::kSize> buf{};
    EXPECT_EQ(EnterOrder::build(buf.data(), "TOKEN_FIFTEENXY", 'B', 100, "AAPL", 100, 0, "FIRM"), 0u);
}

TEST(OuchEnterOrder, OversizeSymbolReturnsZero) {
    std::array<uint8_t, EnterOrder::kSize> buf{};
    EXPECT_EQ(EnterOrder::build(buf.data(), "TOKEN", 'B', 100, "TOOLONGSYM", 100, 0, "FIRM"), 0u);
}

TEST(OuchEnterOrder, OversizeFirmReturnsZero) {
    std::array<uint8_t, EnterOrder::kSize> buf{};
    EXPECT_EQ(EnterOrder::build(buf.data(), "TOKEN", 'B', 100, "AAPL", 100, 0, "FIRM5"), 0u);
}

// 14-char token boundary: the longest accepted token must succeed and copy
// verbatim — guards against an off-by-one in the bound check.
TEST(OuchEnterOrder, ExactBoundaryTokenAccepted) {
    std::array<uint8_t, EnterOrder::kSize> buf{};
    EXPECT_EQ(EnterOrder::build(buf.data(), "TOKEN_FOURTEEN", 'B', 100, "AAPL", 100, 0, "FIRM"),
              EnterOrder::kSize);
    EXPECT_EQ((std::string_view{reinterpret_cast<const char*>(buf.data() + 1), 14}),
              "TOKEN_FOURTEEN");
}

// ---------------------------------------------------------------------------
// ReplaceOrder builder tests
// ---------------------------------------------------------------------------

TEST(OuchReplaceOrder, BuildVerifyWireBytes) {
    std::array<uint8_t, ReplaceOrder::kSize> buf{};

    const size_t n = ReplaceOrder::build(
        buf.data(),
        "EXISTING_TOK  ",   // existing token
        "REPLACEMNT_TOK",   // replacement token
        200,                 // shares
        2000000,             // $200.00
        300                  // 5 min TIF
    );

    ASSERT_EQ(n, ReplaceOrder::kSize);
    EXPECT_EQ(buf[0], 'U');

    // Existing token at offset 1..14
    EXPECT_EQ((std::string_view{reinterpret_cast<const char*>(buf.data() + 1), 14}),
              "EXISTING_TOK  ");

    // Replacement token at offset 15..28
    EXPECT_EQ((std::string_view{reinterpret_cast<const char*>(buf.data() + 15), 14}),
              "REPLACEMNT_TOK");

    // Shares at offset 29..32
    EXPECT_EQ(read_be32(buf.data() + 29), 200u);

    // Price at offset 33..36
    EXPECT_EQ(read_be32(buf.data() + 33), 2000000u);

    // TIF at offset 37..40
    EXPECT_EQ(read_be32(buf.data() + 37), 300u);
}

TEST(OuchReplaceOrder, OversizeExistingTokenReturnsZero) {
    std::array<uint8_t, ReplaceOrder::kSize> buf{};
    EXPECT_EQ(ReplaceOrder::build(buf.data(), "EXISTING_FIFTNX", "REPLACEMNT_TOK", 100, 100, 0), 0u);
}

TEST(OuchReplaceOrder, OversizeReplacementTokenReturnsZero) {
    std::array<uint8_t, ReplaceOrder::kSize> buf{};
    EXPECT_EQ(ReplaceOrder::build(buf.data(), "EXISTING_TOK", "REPLACEMNT_TOKX", 100, 100, 0), 0u);
}

// Null-buffer guard parity with EnterOrder/CancelOrder — the early return
// at line 231 was previously only exercised on the other two builders.
TEST(OuchReplaceOrder, NullBufferReturnsZero) {
    EXPECT_EQ(ReplaceOrder::build(nullptr, "TOK1", "TOK2", 100, 100, 0), 0u);
}

// 14-char exact-boundary on both tokens — guards against an off-by-one
// flipping the comparison in either guard. Both must succeed and the
// padded fields must contain the exact 14 bytes.
TEST(OuchReplaceOrder, ExactBoundaryBothTokensAccepted) {
    std::array<uint8_t, ReplaceOrder::kSize> buf{};
    const size_t n = ReplaceOrder::build(
        buf.data(),
        "EXISTING14CHRS",   // exactly 14
        "REPLACEMNT_NEW",   // exactly 14
        100, 100, 0);
    ASSERT_EQ(n, ReplaceOrder::kSize);
    EXPECT_EQ((std::string_view{reinterpret_cast<const char*>(buf.data() + 1), 14}),
              "EXISTING14CHRS");
    EXPECT_EQ((std::string_view{reinterpret_cast<const char*>(buf.data() + 15), 14}),
              "REPLACEMNT_NEW");
}

// ---------------------------------------------------------------------------
// CancelOrder builder tests
// ---------------------------------------------------------------------------

TEST(OuchCancelOrder, BuildVerifyWireBytes) {
    std::array<uint8_t, CancelOrder::kSize> buf{};

    const size_t n = CancelOrder::build(
        buf.data(),
        "CANCEL_TOKEN  ",  // 14-char token
        0                   // cancel all
    );

    ASSERT_EQ(n, CancelOrder::kSize);

    // Type byte
    EXPECT_EQ(buf[0], 'X');

    // Token at offset 1..14
    EXPECT_EQ((std::string_view{reinterpret_cast<const char*>(buf.data() + 1), 14}),
              "CANCEL_TOKEN  ");

    // Shares at offset 15..18 (0 = cancel all)
    EXPECT_EQ(read_be32(buf.data() + 15), 0u);
}

TEST(OuchCancelOrder, PartialCancel) {
    std::array<uint8_t, CancelOrder::kSize> buf{};
    CancelOrder::build(buf.data(), "PARTIAL_CANCEL", 50);
    EXPECT_EQ(read_be32(buf.data() + 15), 50u);
}

TEST(OuchCancelOrder, NullBufferReturnsZero) {
    EXPECT_EQ(CancelOrder::build(nullptr, "TOKEN         ", 0), 0u);
}

TEST(OuchCancelOrder, OversizeTokenReturnsZero) {
    std::array<uint8_t, CancelOrder::kSize> buf{};
    EXPECT_EQ(CancelOrder::build(buf.data(), "FIFTEEN_CHARTKN", 0), 0u);
}

// ---------------------------------------------------------------------------
// AcceptedView tests
// ---------------------------------------------------------------------------

TEST(OuchAcceptedView, ParseAllFields) {
    std::array<uint8_t, AcceptedView::kSize> buf{};
    std::memset(buf.data(), ' ', buf.size());

    // type
    buf[0] = 'A';

    // timestamp at offset 1..8 (nanoseconds since midnight)
    const uint64_t ts = 34200000000000ULL;  // 09:30:00.000000000
    write_be64(buf.data() + 1, ts);

    // token at offset 9..22
    std::memcpy(buf.data() + 9, "ORDER_TOKEN_01", 14);

    // side at offset 23
    buf[23] = 'B';

    // shares at offset 24..27
    write_be32(buf.data() + 24, 500);

    // symbol at offset 28..35
    std::memcpy(buf.data() + 28, "MSFT    ", 8);

    // price at offset 36..39
    write_be32(buf.data() + 36, 3500000);  // $350.00

    // tif at offset 40..43
    write_be32(buf.data() + 40, 0);

    // firm at offset 44..47
    std::memcpy(buf.data() + 44, "ABCD", 4);

    // display at offset 48
    buf[48] = 'Y';

    // order_reference at offset 49..56
    write_be64(buf.data() + 49, 123456789ULL);

    // capacity at offset 57
    buf[57] = 'O';

    // int_mkt_sweep at offset 58
    buf[58] = 'Y';

    // cross_type at offset 59
    buf[59] = 'N';  // continuous

    // order_state at offset 60
    buf[60] = 'L';  // live

    // bbo_weight at offset 61
    buf[61] = '1';  // example weight code

    AcceptedView view(buf.data(), buf.size());
    ASSERT_TRUE(view.valid());

    EXPECT_EQ(view.msg_type(), 'A');
    EXPECT_EQ(view.timestamp(), ts);
    EXPECT_EQ(view.token(), "ORDER_TOKEN_01");
    EXPECT_EQ(view.side(), 'B');
    EXPECT_EQ(view.shares(), 500u);
    EXPECT_EQ(view.symbol(), "MSFT    ");
    EXPECT_EQ(trim(view.symbol()), "MSFT");
    EXPECT_EQ(view.price(), 3500000u);
    EXPECT_EQ(view.time_in_force(), 0u);
    EXPECT_EQ(view.firm(), "ABCD");
    EXPECT_EQ(view.display(), 'Y');
    EXPECT_EQ(view.order_reference(), 123456789ULL);
    EXPECT_EQ(view.capacity(), 'O');
    EXPECT_EQ(view.int_mkt_sweep(), 'Y');
    EXPECT_EQ(view.cross_type(), 'N');
    EXPECT_EQ(view.order_state(), 'L');
    EXPECT_EQ(view.bbo_weight(), '1');
}

TEST(OuchAcceptedView, TooSmallBufferIsInvalid) {
    std::array<uint8_t, 10> buf{};
    AcceptedView view(buf.data(), buf.size());
    EXPECT_FALSE(view.valid());
}

// ---------------------------------------------------------------------------
// ExecutedView tests
// ---------------------------------------------------------------------------

TEST(OuchExecutedView, ParseAllFields) {
    std::array<uint8_t, ExecutedView::kSize> buf{};
    std::memset(buf.data(), 0, buf.size());

    // type
    buf[0] = 'E';

    // timestamp at offset 1..8
    const uint64_t ts = 34200500000000ULL;
    write_be64(buf.data() + 1, ts);

    // token at offset 9..22
    std::memcpy(buf.data() + 9, "EXEC_TOKEN_001", 14);

    // executed_shares at offset 23..26
    write_be32(buf.data() + 23, 100);

    // execution_price at offset 27..30
    write_be32(buf.data() + 27, 1505000);  // $150.50

    // liquidity_flag at offset 31
    buf[31] = 'A';  // added liquidity

    // match_number at offset 32..39
    write_be64(buf.data() + 32, 987654321ULL);

    ExecutedView view(buf.data(), buf.size());
    ASSERT_TRUE(view.valid());

    EXPECT_EQ(view.msg_type(), 'E');
    EXPECT_EQ(view.timestamp(), ts);
    EXPECT_EQ(view.token(), "EXEC_TOKEN_001");
    EXPECT_EQ(view.executed_shares(), 100u);
    EXPECT_EQ(view.execution_price(), 1505000u);
    EXPECT_EQ(view.liquidity_flag(), 'A');
    EXPECT_EQ(view.match_number(), 987654321ULL);
}

TEST(OuchExecutedView, TooSmallBufferIsInvalid) {
    std::array<uint8_t, 5> buf{};
    ExecutedView view(buf.data(), buf.size());
    EXPECT_FALSE(view.valid());
}

// ---------------------------------------------------------------------------
// CanceledView tests
// ---------------------------------------------------------------------------

TEST(OuchCanceledView, ParseAllFields) {
    std::array<uint8_t, CanceledView::kSize> buf{};
    std::memset(buf.data(), 0, buf.size());

    buf[0] = 'C';
    const uint64_t ts = 50000000000ULL;
    write_be64(buf.data() + 1, ts);
    std::memcpy(buf.data() + 9, "CNCL_TOKEN_001", 14);
    write_be32(buf.data() + 23, 250);
    buf[27] = 'U';  // user requested

    CanceledView view(buf.data(), buf.size());
    ASSERT_TRUE(view.valid());

    EXPECT_EQ(view.msg_type(), 'C');
    EXPECT_EQ(view.timestamp(), ts);
    EXPECT_EQ(view.token(), "CNCL_TOKEN_001");
    EXPECT_EQ(view.decrement_shares(), 250u);
    EXPECT_EQ(view.reason(), 'U');
}

// ---------------------------------------------------------------------------
// Round-trip test: EnterOrder -> simulated Accepted -> verify matching token
// ---------------------------------------------------------------------------

TEST(OuchRoundTrip, EnterOrderToAccepted) {
    // Step 1: Build an EnterOrder
    std::array<uint8_t, EnterOrder::kSize> enter_buf{};
    const std::string_view token = "ROUND_TRIP_001";

    const size_t n = EnterOrder::build(
        enter_buf.data(), token, 'S', 1000, "GOOG", 2800000, 0, "MYMP");
    ASSERT_EQ(n, EnterOrder::kSize);

    // Verify the token in the enter message
    const std::string_view enter_token{
        reinterpret_cast<const char*>(enter_buf.data() + 1), 14};
    EXPECT_EQ(enter_token, token);

    // Step 2: Simulate an Accepted response from Nasdaq
    // (exchange echoes back the token and adds its own fields)
    std::array<uint8_t, AcceptedView::kSize> accepted_buf{};
    std::memset(accepted_buf.data(), ' ', accepted_buf.size());

    accepted_buf[0] = 'A';
    write_be64(accepted_buf.data() + 1, 34200000000000ULL);

    // Copy the token from the enter order into the accepted message
    std::memcpy(accepted_buf.data() + 9, enter_buf.data() + 1, 14);

    // Copy side, shares, symbol, price from the enter order
    accepted_buf[23] = enter_buf[15];  // side
    std::memcpy(accepted_buf.data() + 24, enter_buf.data() + 16, 4);   // shares
    std::memcpy(accepted_buf.data() + 28, enter_buf.data() + 20, 8);   // symbol
    std::memcpy(accepted_buf.data() + 36, enter_buf.data() + 28, 4);   // price
    std::memcpy(accepted_buf.data() + 40, enter_buf.data() + 32, 4);   // tif
    std::memcpy(accepted_buf.data() + 44, enter_buf.data() + 36, 4);   // firm

    accepted_buf[48] = 'Y';  // display
    write_be64(accepted_buf.data() + 49, 999888777ULL);  // order reference
    accepted_buf[57] = 'O';  // capacity
    accepted_buf[60] = 'L';  // live

    // Step 3: Parse the Accepted and verify token matches
    AcceptedView view(accepted_buf.data(), accepted_buf.size());
    ASSERT_TRUE(view.valid());

    EXPECT_EQ(view.token(), token);
    EXPECT_EQ(view.side(), 'S');
    EXPECT_EQ(view.shares(), 1000u);
    EXPECT_EQ(trim(view.symbol()), "GOOG");
    EXPECT_EQ(view.price(), 2800000u);
    EXPECT_EQ(view.firm(), "MYMP");
    EXPECT_EQ(view.order_reference(), 999888777ULL);
}

// ---------------------------------------------------------------------------
// ReplacedView tests (the previous batch had zero coverage for this view —
// every field accessor went untested and there was no too-small boundary
// guard. ReplacedView is the only OUCH outbound view that exposes
// `previous_token`, which downstream order-state-machine consumers use to
// reconcile Replace echoes back to the original order.)
// ---------------------------------------------------------------------------

TEST(OuchReplacedView, ParseAllFields) {
    std::array<uint8_t, ReplacedView::kSize> buf{};
    // Pre-fill with spaces so the string fields decode cleanly without
    // relying on uninitialized memory landing on a printable byte.
    std::memset(buf.data(), ' ', buf.size());

    buf[0] = 'U';
    const uint64_t ts = 34200000000000ULL;
    write_be64(buf.data() + 1, ts);

    // Replacement token at offset 9..22
    std::memcpy(buf.data() + 9, "REPL_TOKEN_001", 14);

    buf[23] = 'B';                                   // side
    write_be32(buf.data() + 24, 750);                // shares
    std::memcpy(buf.data() + 28, "MSFT    ", 8);     // symbol (right-padded)
    write_be32(buf.data() + 36, 4205000);            // $420.50
    write_be32(buf.data() + 40, 60);                 // tif (60s)
    std::memcpy(buf.data() + 44, "MPID", 4);         // firm
    buf[48] = 'Y';                                   // display
    write_be64(buf.data() + 49, 246813579ULL);       // order_reference
    buf[57] = 'P';                                   // capacity (principal)
    buf[58] = 'Y';                                   // intermarket sweep
    buf[59] = 'N';                                   // cross_type
    buf[60] = 'L';                                   // order_state (live)

    // Previous token at offset 61..74
    std::memcpy(buf.data() + 61, "PREV_TOKEN_001", 14);

    ReplacedView view(buf.data(), buf.size());
    ASSERT_TRUE(view.valid());

    EXPECT_EQ(view.msg_type(), 'U');
    EXPECT_EQ(view.timestamp(), ts);
    EXPECT_EQ(view.replacement_token(), "REPL_TOKEN_001");
    EXPECT_EQ(view.side(), 'B');
    EXPECT_EQ(view.shares(), 750u);
    EXPECT_EQ(trim(view.symbol()), "MSFT");
    EXPECT_EQ(view.price(), 4205000u);
    EXPECT_EQ(view.time_in_force(), 60u);
    EXPECT_EQ(view.firm(), "MPID");
    EXPECT_EQ(view.display(), 'Y');
    EXPECT_EQ(view.order_reference(), 246813579ULL);
    EXPECT_EQ(view.capacity(), 'P');
    EXPECT_EQ(view.int_mkt_sweep(), 'Y');
    EXPECT_EQ(view.cross_type(), 'N');
    EXPECT_EQ(view.order_state(), 'L');
    EXPECT_EQ(view.previous_token(), "PREV_TOKEN_001");
}

TEST(OuchReplacedView, TooSmallBufferIsInvalid) {
    // 79 bytes is one short of kSize (80) — caller passing a partially-
    // received frame must not be tricked into thinking it has data.
    std::array<uint8_t, 79> buf{};
    ReplacedView view(buf.data(), buf.size());
    EXPECT_FALSE(view.valid());
}

TEST(OuchReplacedView, ExactBoundarySizeAccepted) {
    // Exactly kSize must be valid — the boundary is `>=` not `>`.
    std::array<uint8_t, ReplacedView::kSize> buf{};
    ReplacedView view(buf.data(), buf.size());
    EXPECT_TRUE(view.valid());
}

TEST(OuchCanceledView, TooSmallBufferIsInvalid) {
    // CanceledView previously lacked a too-small guard test even though
    // every other outbound view had one. Pin the contract.
    std::array<uint8_t, 27> buf{};  // one byte shy of kSize (28)
    CanceledView view(buf.data(), buf.size());
    EXPECT_FALSE(view.valid());
}
