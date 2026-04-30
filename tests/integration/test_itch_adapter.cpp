/// @file test_itch_adapter.cpp
/// Unit tests for the ItchBookBuilder ITCH-to-L2 adapter.
///
/// Covers: AddOrder, OrderExecuted, OrderCancel, OrderDelete, OrderReplace,
/// AddOrderMPID, OrderExecutedWithPrice, multiple orders at the same price
/// level, and clear().

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include "eph/book/itch_adapter.hpp"

using namespace eph::book;
using namespace eph::itch;

// ---------------------------------------------------------------------------
// Helpers — build raw ITCH message bytes for test injection
// ---------------------------------------------------------------------------

static constexpr double kEps = 1e-9;

/// Write a big-endian uint16_t at the given pointer.
static void write_be16(uint8_t* p, uint16_t v) {
    if constexpr (std::endian::native == std::endian::little)
        v = std::byteswap(v);
    std::memcpy(p, &v, 2);
}

/// Write a big-endian uint32_t at the given pointer.
static void write_be32(uint8_t* p, uint32_t v) {
    if constexpr (std::endian::native == std::endian::little)
        v = std::byteswap(v);
    std::memcpy(p, &v, 4);
}

/// Write a big-endian uint64_t at the given pointer.
static void write_be64(uint8_t* p, uint64_t v) {
    if constexpr (std::endian::native == std::endian::little)
        v = std::byteswap(v);
    std::memcpy(p, &v, 8);
}

/// Fill the common ITCH header (type + stock_locate + tracking + timestamp).
/// Returns pointer to byte after the header (offset 11 from msg start).
static void fill_header(uint8_t* msg, uint8_t type) {
    msg[0] = type;
    // stock_locate=1, tracking=0, timestamp=0 — sufficient for adapter tests
    write_be16(msg + 1, 1);
    write_be16(msg + 3, 0);
    // 6-byte timestamp: all zeros
    std::memset(msg + 5, 0, 6);
}

/// Build an AddOrder ('A') message.
/// Layout: type(1) locate(2) tracking(2) timestamp(6)
///         order_ref(8) side(1) shares(4) stock(8) price(4)
/// Total: 36 bytes
struct AddOrderMsg {
    std::array<uint8_t, 36> buf{};

    AddOrderMsg(uint64_t ref, char side, uint32_t shares, double price) {
        fill_header(buf.data(), kAddOrder);
        write_be64(buf.data() + 11, ref);
        buf[19] = static_cast<uint8_t>(side);
        write_be32(buf.data() + 20, shares);
        // stock field: 8 bytes, filled with "TEST    "
        std::memcpy(buf.data() + 24, "TEST    ", 8);
        write_be32(buf.data() + 32, static_cast<uint32_t>(price * 10000.0));
    }

    MessageView view() const {
        return MessageView{kAddOrder, buf.data(), 36};
    }
};

/// Build an AddOrderMPID ('F') message.
/// Layout: type(1) locate(2) tracking(2) timestamp(6)
///         order_ref(8) side(1) shares(4) stock(8) price(4) attribution(4)
/// Total: 40 bytes
struct AddOrderMPIDMsg {
    std::array<uint8_t, 40> buf{};

    AddOrderMPIDMsg(uint64_t ref, char side, uint32_t shares, double price) {
        fill_header(buf.data(), kAddOrderMPID);
        write_be64(buf.data() + 11, ref);
        buf[19] = static_cast<uint8_t>(side);
        write_be32(buf.data() + 20, shares);
        std::memcpy(buf.data() + 24, "TEST    ", 8);
        write_be32(buf.data() + 32, static_cast<uint32_t>(price * 10000.0));
        std::memcpy(buf.data() + 36, "ABCD", 4);
    }

    MessageView view() const {
        return MessageView{kAddOrderMPID, buf.data(), 40};
    }
};

/// Build an OrderExecuted ('E') message.
/// Layout: type(1) locate(2) tracking(2) timestamp(6)
///         order_ref(8) executed_shares(4) match_number(8)
/// Total: 31 bytes
struct OrderExecutedMsg {
    std::array<uint8_t, 31> buf{};

    OrderExecutedMsg(uint64_t ref, uint32_t exec_shares, uint64_t match_num = 1) {
        fill_header(buf.data(), kOrderExecuted);
        write_be64(buf.data() + 11, ref);
        write_be32(buf.data() + 19, exec_shares);
        write_be64(buf.data() + 23, match_num);
    }

    MessageView view() const {
        return MessageView{kOrderExecuted, buf.data(), 31};
    }
};

/// Build an OrderExecutedWithPrice ('C') message.
/// Layout: type(1) locate(2) tracking(2) timestamp(6)
///         order_ref(8) exec_shares(4) match_num(8) printable(1) exec_price(4)
/// Total: 36 bytes
struct OrderExecutedWithPriceMsg {
    std::array<uint8_t, 36> buf{};

    OrderExecutedWithPriceMsg(uint64_t ref, uint32_t exec_shares,
                              double exec_price, uint64_t match_num = 1) {
        fill_header(buf.data(), kOrderExecutedWithPrice);
        write_be64(buf.data() + 11, ref);
        write_be32(buf.data() + 19, exec_shares);
        write_be64(buf.data() + 23, match_num);
        buf[31] = 'Y';  // printable
        write_be32(buf.data() + 32, static_cast<uint32_t>(exec_price * 10000.0));
    }

    MessageView view() const {
        return MessageView{kOrderExecutedWithPrice, buf.data(), 36};
    }
};

/// Build an OrderCancel ('X') message.
/// Layout: type(1) locate(2) tracking(2) timestamp(6)
///         order_ref(8) cancelled_shares(4)
/// Total: 23 bytes
struct OrderCancelMsg {
    std::array<uint8_t, 23> buf{};

    OrderCancelMsg(uint64_t ref, uint32_t cancel_shares) {
        fill_header(buf.data(), kOrderCancel);
        write_be64(buf.data() + 11, ref);
        write_be32(buf.data() + 19, cancel_shares);
    }

    MessageView view() const {
        return MessageView{kOrderCancel, buf.data(), 23};
    }
};

/// Build an OrderDelete ('D') message.
/// Layout: type(1) locate(2) tracking(2) timestamp(6) order_ref(8)
/// Total: 19 bytes
struct OrderDeleteMsg {
    std::array<uint8_t, 19> buf{};

    explicit OrderDeleteMsg(uint64_t ref) {
        fill_header(buf.data(), kOrderDelete);
        write_be64(buf.data() + 11, ref);
    }

    MessageView view() const {
        return MessageView{kOrderDelete, buf.data(), 19};
    }
};

/// Build an OrderReplace ('U') message.
/// Layout: type(1) locate(2) tracking(2) timestamp(6)
///         orig_order_ref(8) new_order_ref(8) shares(4) price(4)
/// Total: 35 bytes
struct OrderReplaceMsg {
    std::array<uint8_t, 35> buf{};

    OrderReplaceMsg(uint64_t orig_ref, uint64_t new_ref, uint32_t shares, double price) {
        fill_header(buf.data(), kOrderReplace);
        write_be64(buf.data() + 11, orig_ref);
        write_be64(buf.data() + 19, new_ref);
        write_be32(buf.data() + 27, shares);
        write_be32(buf.data() + 31, static_cast<uint32_t>(price * 10000.0));
    }

    MessageView view() const {
        return MessageView{kOrderReplace, buf.data(), 35};
    }
};

// ---------------------------------------------------------------------------
// Test: AddOrder -> book updates
// ---------------------------------------------------------------------------

TEST(ItchBookBuilderTest, AddOrderUpdatesBidAndAsk) {
    ItchBookBuilder<10> builder;

    // Add a bid at 100.50 for 200 shares
    AddOrderMsg bid1(1001, 'B', 200, 100.50);
    EXPECT_TRUE(builder.process(bid1.view()));
    EXPECT_EQ(builder.order_count(), 1u);

    auto bb = builder.book().best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_NEAR(bb->price, 100.50, kEps);
    EXPECT_NEAR(bb->qty, 200.0, kEps);

    // Add an ask at 101.00 for 300 shares
    AddOrderMsg ask1(1002, 'S', 300, 101.00);
    EXPECT_TRUE(builder.process(ask1.view()));
    EXPECT_EQ(builder.order_count(), 2u);

    auto ba = builder.book().best_ask();
    ASSERT_TRUE(ba.has_value());
    EXPECT_NEAR(ba->price, 101.00, kEps);
    EXPECT_NEAR(ba->qty, 300.0, kEps);
}

// ITCH wire format does not validate the side byte. A corrupt feed (or
// adversarial sender) sending side='X' would fall through qty_map's
// `side == 'B' ? bid : ask` ternary into the ASK side, silently routing
// the rogue order onto the wrong book side and corrupting every BBO read
// after that point. Verify the adapter rejects up-front.
TEST(ItchBookBuilderTest, AddOrderInvalidSideRejected) {
    ItchBookBuilder<10> builder;

    AddOrderMsg bad(1099, 'X', 100, 100.0);
    EXPECT_FALSE(builder.process(bad.view()));
    EXPECT_EQ(builder.order_count(), 0u);
    EXPECT_FALSE(builder.book().best_bid().has_value());
    EXPECT_FALSE(builder.book().best_ask().has_value());
}

TEST(ItchBookBuilderTest, AddOrderMPIDInvalidSideRejected) {
    ItchBookBuilder<10> builder;

    AddOrderMPIDMsg bad(1199, 0x00, 100, 100.0);
    EXPECT_FALSE(builder.process(bad.view()));
    EXPECT_EQ(builder.order_count(), 0u);
}

// ---------------------------------------------------------------------------
// Test: AddOrderMPID works identically to AddOrder
// ---------------------------------------------------------------------------

TEST(ItchBookBuilderTest, AddOrderMPIDUpdatesBid) {
    ItchBookBuilder<10> builder;

    AddOrderMPIDMsg bid(2001, 'B', 500, 99.25);
    EXPECT_TRUE(builder.process(bid.view()));
    EXPECT_EQ(builder.order_count(), 1u);

    auto bb = builder.book().best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_NEAR(bb->price, 99.25, kEps);
    EXPECT_NEAR(bb->qty, 500.0, kEps);
}

// ---------------------------------------------------------------------------
// Test: OrderExecuted -> qty decreases
// ---------------------------------------------------------------------------

TEST(ItchBookBuilderTest, OrderExecutedReducesQty) {
    ItchBookBuilder<10> builder;

    AddOrderMsg bid(3001, 'B', 500, 100.00);
    builder.process(bid.view());

    // Execute 200 shares
    OrderExecutedMsg exec(3001, 200);
    EXPECT_TRUE(builder.process(exec.view()));
    EXPECT_EQ(builder.order_count(), 1u);

    auto bb = builder.book().best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_NEAR(bb->qty, 300.0, kEps);
}

TEST(ItchBookBuilderTest, OrderExecutedFullyRemovesOrder) {
    ItchBookBuilder<10> builder;

    AddOrderMsg bid(3002, 'B', 100, 100.00);
    builder.process(bid.view());

    // Execute all 100 shares
    OrderExecutedMsg exec(3002, 100);
    EXPECT_TRUE(builder.process(exec.view()));
    EXPECT_EQ(builder.order_count(), 0u);
    EXPECT_FALSE(builder.book().best_bid().has_value());
}

TEST(ItchBookBuilderTest, OrderExecutedUnknownRefReturnsFalse) {
    ItchBookBuilder<10> builder;

    OrderExecutedMsg exec(9999, 100);
    EXPECT_FALSE(builder.process(exec.view()));
}

// ---------------------------------------------------------------------------
// Test: OrderExecutedWithPrice -> qty decreases
// ---------------------------------------------------------------------------

TEST(ItchBookBuilderTest, OrderExecutedWithPriceReducesQty) {
    ItchBookBuilder<10> builder;

    AddOrderMsg ask(3010, 'S', 400, 105.00);
    builder.process(ask.view());

    // Execute 150 shares at a different execution price (informational only)
    OrderExecutedWithPriceMsg exec(3010, 150, 104.50);
    EXPECT_TRUE(builder.process(exec.view()));
    EXPECT_EQ(builder.order_count(), 1u);

    auto ba = builder.book().best_ask();
    ASSERT_TRUE(ba.has_value());
    // The book level price stays at the resting price, not the execution price.
    EXPECT_NEAR(ba->price, 105.00, kEps);
    EXPECT_NEAR(ba->qty, 250.0, kEps);
}

// ---------------------------------------------------------------------------
// Test: OrderCancel -> qty decreases
// ---------------------------------------------------------------------------

TEST(ItchBookBuilderTest, OrderCancelReducesQty) {
    ItchBookBuilder<10> builder;

    AddOrderMsg bid(4001, 'B', 500, 100.00);
    builder.process(bid.view());

    // Cancel 300 shares
    OrderCancelMsg cancel(4001, 300);
    EXPECT_TRUE(builder.process(cancel.view()));
    EXPECT_EQ(builder.order_count(), 1u);

    auto bb = builder.book().best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_NEAR(bb->qty, 200.0, kEps);
}

TEST(ItchBookBuilderTest, OrderCancelFullyRemovesOrder) {
    ItchBookBuilder<10> builder;

    AddOrderMsg bid(4002, 'B', 100, 100.00);
    builder.process(bid.view());

    OrderCancelMsg cancel(4002, 100);
    EXPECT_TRUE(builder.process(cancel.view()));
    EXPECT_EQ(builder.order_count(), 0u);
    EXPECT_FALSE(builder.book().best_bid().has_value());
}

TEST(ItchBookBuilderTest, OrderCancelUnknownRefReturnsFalse) {
    ItchBookBuilder<10> builder;

    OrderCancelMsg cancel(9999, 100);
    EXPECT_FALSE(builder.process(cancel.view()));
}

// ---------------------------------------------------------------------------
// Test: OrderDelete -> level removed
// ---------------------------------------------------------------------------

TEST(ItchBookBuilderTest, OrderDeleteRemovesLevel) {
    ItchBookBuilder<10> builder;

    AddOrderMsg bid(5001, 'B', 500, 100.00);
    builder.process(bid.view());

    OrderDeleteMsg del(5001);
    EXPECT_TRUE(builder.process(del.view()));
    EXPECT_EQ(builder.order_count(), 0u);
    EXPECT_FALSE(builder.book().best_bid().has_value());
}

TEST(ItchBookBuilderTest, OrderDeleteUnknownRefReturnsFalse) {
    ItchBookBuilder<10> builder;

    OrderDeleteMsg del(9999);
    EXPECT_FALSE(builder.process(del.view()));
}

// ---------------------------------------------------------------------------
// Test: OrderReplace -> level moves
// ---------------------------------------------------------------------------

TEST(ItchBookBuilderTest, OrderReplaceMovesPriceLevel) {
    ItchBookBuilder<10> builder;

    // Add bid at 100.00 for 500 shares
    AddOrderMsg bid(6001, 'B', 500, 100.00);
    builder.process(bid.view());

    // Replace: new ref 6002, 300 shares at 100.50
    OrderReplaceMsg replace(6001, 6002, 300, 100.50);
    EXPECT_TRUE(builder.process(replace.view()));
    EXPECT_EQ(builder.order_count(), 1u);

    // Old level at 100.00 should be gone, new level at 100.50
    auto bb = builder.book().best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_NEAR(bb->price, 100.50, kEps);
    EXPECT_NEAR(bb->qty, 300.0, kEps);
    EXPECT_EQ(builder.book().bid_depth(), 1u);
}

TEST(ItchBookBuilderTest, OrderReplacePreservesSide) {
    ItchBookBuilder<10> builder;

    // Add ask at 105.00
    AddOrderMsg ask(6010, 'S', 200, 105.00);
    builder.process(ask.view());

    // Replace to 104.00
    OrderReplaceMsg replace(6010, 6011, 150, 104.00);
    EXPECT_TRUE(builder.process(replace.view()));

    // Should still be an ask
    auto ba = builder.book().best_ask();
    ASSERT_TRUE(ba.has_value());
    EXPECT_NEAR(ba->price, 104.00, kEps);
    EXPECT_NEAR(ba->qty, 150.0, kEps);
    EXPECT_FALSE(builder.book().best_bid().has_value());
}

TEST(ItchBookBuilderTest, OrderReplaceUnknownRefReturnsFalse) {
    ItchBookBuilder<10> builder;

    OrderReplaceMsg replace(9999, 10000, 100, 100.00);
    EXPECT_FALSE(builder.process(replace.view()));
}

// ---------------------------------------------------------------------------
// Test: Multiple orders at same price -> qty sums
// ---------------------------------------------------------------------------

TEST(ItchBookBuilderTest, MultipleOrdersAtSamePriceSumQty) {
    ItchBookBuilder<10> builder;

    AddOrderMsg bid1(7001, 'B', 200, 100.00);
    AddOrderMsg bid2(7002, 'B', 300, 100.00);
    AddOrderMsg bid3(7003, 'B', 150, 100.00);
    builder.process(bid1.view());
    builder.process(bid2.view());
    builder.process(bid3.view());

    EXPECT_EQ(builder.order_count(), 3u);

    auto bb = builder.book().best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_NEAR(bb->price, 100.00, kEps);
    EXPECT_NEAR(bb->qty, 650.0, kEps);

    // Only one price level despite three orders
    EXPECT_EQ(builder.book().bid_depth(), 1u);
}

TEST(ItchBookBuilderTest, DeleteOneOfMultipleOrdersAtSamePrice) {
    ItchBookBuilder<10> builder;

    AddOrderMsg bid1(7101, 'B', 200, 100.00);
    AddOrderMsg bid2(7102, 'B', 300, 100.00);
    builder.process(bid1.view());
    builder.process(bid2.view());

    // Delete one order — level should still exist with remaining qty
    OrderDeleteMsg del(7101);
    builder.process(del.view());

    EXPECT_EQ(builder.order_count(), 1u);
    auto bb = builder.book().best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_NEAR(bb->price, 100.00, kEps);
    EXPECT_NEAR(bb->qty, 300.0, kEps);
}

TEST(ItchBookBuilderTest, PartialExecuteWithMultipleOrdersAtSamePrice) {
    ItchBookBuilder<10> builder;

    AddOrderMsg bid1(7201, 'B', 200, 100.00);
    AddOrderMsg bid2(7202, 'B', 300, 100.00);
    builder.process(bid1.view());
    builder.process(bid2.view());

    // Execute 100 shares from bid1
    OrderExecutedMsg exec(7201, 100);
    builder.process(exec.view());

    auto bb = builder.book().best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_NEAR(bb->qty, 400.0, kEps);  // 100 + 300
    EXPECT_EQ(builder.order_count(), 2u);
}

// ---------------------------------------------------------------------------
// Test: Ignoring non-order messages
// ---------------------------------------------------------------------------

TEST(ItchBookBuilderTest, IgnoresNonOrderMessages) {
    ItchBookBuilder<10> builder;

    // Build a SystemEvent message ('S', 12 bytes)
    std::array<uint8_t, 12> sys_buf{};
    fill_header(sys_buf.data(), kSystemEvent);
    sys_buf[11] = 'O';  // start-of-messages

    MessageView sys_view{kSystemEvent, sys_buf.data(), 12};
    EXPECT_FALSE(builder.process(sys_view));
    EXPECT_EQ(builder.order_count(), 0u);
}

// ---------------------------------------------------------------------------
// Test: clear()
// ---------------------------------------------------------------------------

TEST(ItchBookBuilderTest, ClearResetsState) {
    ItchBookBuilder<10> builder;

    AddOrderMsg bid(8001, 'B', 500, 100.00);
    AddOrderMsg ask(8002, 'S', 300, 101.00);
    builder.process(bid.view());
    builder.process(ask.view());

    EXPECT_EQ(builder.order_count(), 2u);
    EXPECT_TRUE(builder.book().best_bid().has_value());

    builder.clear();

    EXPECT_EQ(builder.order_count(), 0u);
    EXPECT_FALSE(builder.book().best_bid().has_value());
    EXPECT_FALSE(builder.book().best_ask().has_value());
    EXPECT_EQ(builder.book().level_count(), 0u);
}

// ---------------------------------------------------------------------------
// Test: Multi-level book scenario
// ---------------------------------------------------------------------------

TEST(ItchBookBuilderTest, MultiLevelBookMaintainsSortOrder) {
    ItchBookBuilder<10> builder;

    // Add bids at three different prices
    AddOrderMsg bid1(9001, 'B', 100, 100.00);
    AddOrderMsg bid2(9002, 'B', 200, 99.50);
    AddOrderMsg bid3(9003, 'B', 300, 100.25);
    builder.process(bid1.view());
    builder.process(bid2.view());
    builder.process(bid3.view());

    // Best bid should be 100.25 (highest)
    auto bb = builder.book().best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_NEAR(bb->price, 100.25, kEps);
    EXPECT_NEAR(bb->qty, 300.0, kEps);
    EXPECT_EQ(builder.book().bid_depth(), 3u);

    // Add asks at two different prices
    AddOrderMsg ask1(9004, 'S', 150, 101.00);
    AddOrderMsg ask2(9005, 'S', 250, 100.75);
    builder.process(ask1.view());
    builder.process(ask2.view());

    // Best ask should be 100.75 (lowest)
    auto ba = builder.book().best_ask();
    ASSERT_TRUE(ba.has_value());
    EXPECT_NEAR(ba->price, 100.75, kEps);
    EXPECT_NEAR(ba->qty, 250.0, kEps);
    EXPECT_EQ(builder.book().ask_depth(), 2u);

    // Verify spread
    auto spread = builder.book().spread();
    ASSERT_TRUE(spread.has_value());
    EXPECT_NEAR(*spread, 0.50, kEps);  // 100.75 - 100.25
}

// ---------------------------------------------------------------------------
// Test: Replace at same price
// ---------------------------------------------------------------------------

TEST(ItchBookBuilderTest, OrderReplaceAtSamePriceUpdatesQty) {
    ItchBookBuilder<10> builder;

    AddOrderMsg bid(6100, 'B', 500, 100.00);
    builder.process(bid.view());

    // Replace at the same price but different qty
    OrderReplaceMsg replace(6100, 6101, 200, 100.00);
    builder.process(replace.view());

    auto bb = builder.book().best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_NEAR(bb->price, 100.00, kEps);
    EXPECT_NEAR(bb->qty, 200.0, kEps);
    EXPECT_EQ(builder.order_count(), 1u);
}

// ---------------------------------------------------------------------------
// Test: Duplicate AddOrder ref must not leak phantom qty into the book.
//
// Regression for batch-4 / round-2 protocol bug:
//   Each AddOrder handler unconditionally overwrote `orders_[ref]` and
//   then called `add_qty(...)` on the new (price, shares). The old
//   level's qty was NEVER subtracted, so a duplicate ref left a
//   "phantom" sum on the price-aggregation map that no subsequent
//   delete/cancel could ever drain.
//
// ITCH 5.0 spec §4 says order_ref is a session-unique 64-bit identifier,
// but real Nasdaq ITCH-TV / ITCH-FOP feeds occasionally retransmit an
// AddOrder for the same ref during gap-recovery / cancel-replace races.
// A market-maker that trusts the book over the wire (e.g., for a
// `qty / depth_at_price` size-check) gets stale levels until the entire
// session restarts. Treat re-add of a known ref as "remove old then add
// new" so the book stays consistent with `orders_`.
// ---------------------------------------------------------------------------
TEST(ItchBookBuilderTest, DuplicateAddOrderRefDoesNotLeakPhantomQty) {
    ItchBookBuilder<10> builder;

    // First AddOrder at 100.00, 500 shares.
    AddOrderMsg first(7100, 'B', 500, 100.00);
    ASSERT_TRUE(builder.process(first.view()));

    auto bb1 = builder.book().best_bid();
    ASSERT_TRUE(bb1.has_value());
    EXPECT_NEAR(bb1->qty, 500.0, kEps);
    EXPECT_EQ(builder.order_count(), 1u);

    // Same ref re-added at a *different* price/qty (replay / race).
    // Correct behavior: the old (100.00, 500) level shrinks back to 0
    // and the new (101.00, 700) level becomes the BBO with qty 700,
    // not 1200 (= old phantom + new).
    AddOrderMsg dup(7100, 'B', 700, 101.00);
    ASSERT_TRUE(builder.process(dup.view()));

    EXPECT_EQ(builder.order_count(), 1u)
        << "duplicate ref must replace, not double-track";

    // Best bid is now the new price.
    auto bb2 = builder.book().best_bid();
    ASSERT_TRUE(bb2.has_value());
    EXPECT_NEAR(bb2->price, 101.00, kEps);
    EXPECT_NEAR(bb2->qty, 700.0, kEps)
        << "new level must hold only the new qty, not old+new";

    // Old price level must be gone (qty 0 → removed from book).
    bool found_old = false;
    for (const auto& lvl : builder.book().bids()) {
        if (std::abs(lvl.price - 100.00) < kEps) found_old = true;
    }
    EXPECT_FALSE(found_old)
        << "phantom level at old price must be cleared";

    // Now delete the order — both levels must drain to zero.
    OrderDeleteMsg del(7100);
    ASSERT_TRUE(builder.process(del.view()));
    EXPECT_EQ(builder.order_count(), 0u);
    EXPECT_EQ(builder.book().bid_depth(), 0u)
        << "after delete, book must be empty — any residual depth is a "
           "phantom-qty leak from the duplicate AddOrder";
}

// ---------------------------------------------------------------------------
// Test: Duplicate AddOrderMPID ref — same invariant as the plain AddOrder
//       handler, since both use orders_[ref] = ... unconditionally.
// ---------------------------------------------------------------------------
TEST(ItchBookBuilderTest, DuplicateAddOrderMPIDRefDoesNotLeakPhantomQty) {
    ItchBookBuilder<10> builder;

    AddOrderMPIDMsg first(8100, 'S', 300, 200.00);
    ASSERT_TRUE(builder.process(first.view()));
    EXPECT_EQ(builder.order_count(), 1u);

    AddOrderMPIDMsg dup(8100, 'S', 400, 201.00);
    ASSERT_TRUE(builder.process(dup.view()));
    EXPECT_EQ(builder.order_count(), 1u);

    auto ba = builder.book().best_ask();
    ASSERT_TRUE(ba.has_value());
    EXPECT_NEAR(ba->price, 201.00, kEps);
    EXPECT_NEAR(ba->qty, 400.0, kEps)
        << "MPID variant must not leak phantom qty either";

    OrderDeleteMsg del(8100);
    ASSERT_TRUE(builder.process(del.view()));
    EXPECT_EQ(builder.book().ask_depth(), 0u);
}

// ---------------------------------------------------------------------------
// Test: OrderReplace where new_ref collides with an *existing* live ref.
//
// Same root cause as duplicate AddOrder: the replace handler does
//   orders_[new_ref] = Order{...};
// unconditionally, so if `new_ref` was already a live order from a
// prior AddOrder, that order's level qty is silently doubled in the
// price-aggregation map.
//
// Realistic scenario: a buggy upstream feed retransmits a Replace
// whose new_ref happens to alias an existing unrelated order.
// ---------------------------------------------------------------------------
TEST(ItchBookBuilderTest, OrderReplaceCollidingNewRefDoesNotLeakPhantomQty) {
    ItchBookBuilder<10> builder;

    // Add two independent orders.
    AddOrderMsg a(9100, 'B', 100, 50.00);
    AddOrderMsg b(9200, 'B', 200, 60.00);
    builder.process(a.view());
    builder.process(b.view());
    EXPECT_EQ(builder.order_count(), 2u);

    // Replace 9100 → new_ref=9200 (ref collision with an existing order).
    OrderReplaceMsg replace(9100, 9200, 50, 70.00);
    ASSERT_TRUE(builder.process(replace.view()));

    // After the replace there is exactly one live order with ref 9200.
    EXPECT_EQ(builder.order_count(), 1u)
        << "new_ref colliding with existing must not leave double entries";

    // BBO is the replace target price.
    auto bb = builder.book().best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_NEAR(bb->price, 70.00, kEps);
    EXPECT_NEAR(bb->qty, 50.0, kEps)
        << "level at new price must reflect only the replace qty, "
           "not collision-leaked old qty";

    // The 60.00 level (where the *displaced* old 9200 used to sit)
    // must drain to zero — otherwise we leaked phantom qty there.
    bool found_old = false;
    for (const auto& lvl : builder.book().bids()) {
        if (std::abs(lvl.price - 60.00) < kEps) found_old = true;
    }
    EXPECT_FALSE(found_old)
        << "old 9200 level at 60.00 must be gone after the collision-replace";

    OrderDeleteMsg del(9200);
    ASSERT_TRUE(builder.process(del.view()));
    EXPECT_EQ(builder.book().bid_depth(), 0u);
}
