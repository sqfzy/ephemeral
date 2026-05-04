/// @file test_itch_mold_book_e2e.cpp
/// Cross-module end-to-end test for the HFT market-data ingestion pipeline:
///
///     UDP payload (MoldUDP64 wire bytes)
///       → eph::itch::parse_moldudp64()                  // batch container
///         → eph::itch::parse_all() + dispatch           // ITCH 5.0 parser
///           → eph::book::ItchBookBuilder::process()     // adapter
///             → eph::book::ArrayBook                    // L2 book
///
/// Existing `test_itch_adapter` exercises the adapter in isolation by
/// constructing `MessageView` objects directly, completely bypassing the
/// MoldUDP64 framing and the ITCH parser stage. That is fine for the
/// adapter's unit-level invariants (phantom qty, side validation, …) but
/// leaves the **full HFT market-data path** uncovered — exactly the path
/// that runs in production every microsecond.
///
/// This file fills that gap. Each test builds a real on-the-wire MoldUDP64
/// UDP datagram (ten-byte session id, big-endian sequence number, count,
/// message-length-prefixed bodies), then drives it through the full
/// pipeline and asserts the resulting `ArrayBook` BBO state.
///
/// Coverage targets:
///   - Multi-message MoldUDP64 packet → book BBO matches expected.
///   - Sequence numbers per delivered message line up across the batch.
///   - Truncated trailing message in a batch → first N delivered, book is
///     a strict prefix (no partial / phantom updates).
///   - End-of-session sentinel (0xFFFF) → no callbacks, no book mutation.
///   - Two consecutive packets — sequence wraps continue, book mutations
///     accumulate.

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "eph/book/itch_adapter.hpp"
#include "eph/itch/messages.hpp"
#include "eph/itch/moldudp64.hpp"
#include "eph/itch/parser.hpp"

using namespace eph::itch;
using namespace eph::book;

namespace {

constexpr double kEps = 1e-9;

// ---------------------------------------------------------------------------
// Big-endian writers — match the on-wire layout of MoldUDP64 / ITCH 5.0
// ---------------------------------------------------------------------------

inline void write_be16(uint8_t* p, uint16_t v) {
    if constexpr (std::endian::native == std::endian::little)
        v = std::byteswap(v);
    std::memcpy(p, &v, 2);
}
inline void write_be32(uint8_t* p, uint32_t v) {
    if constexpr (std::endian::native == std::endian::little)
        v = std::byteswap(v);
    std::memcpy(p, &v, 4);
}
inline void write_be64(uint8_t* p, uint64_t v) {
    if constexpr (std::endian::native == std::endian::little)
        v = std::byteswap(v);
    std::memcpy(p, &v, 8);
}

// ---------------------------------------------------------------------------
// ITCH message builders — produce raw on-wire bytes (NO framing).
// Layouts mirror ITCH 5.0 §4 and the adapter's existing helpers.
// ---------------------------------------------------------------------------

/// AddOrder ('A'): 36 bytes — type(1) locate(2) tracking(2) ts(6)
///                 ref(8) side(1) shares(4) stock(8) price(4)
inline std::vector<uint8_t> build_add_order(uint64_t ref, char side,
                                             uint32_t shares, double price) {
    std::vector<uint8_t> b(kAddOrderSize, 0);
    b[0] = kAddOrder;
    write_be16(b.data() + 1, /*locate*/ 1);
    // tracking + 6-byte timestamp left zero
    write_be64(b.data() + 11, ref);
    b[19] = static_cast<uint8_t>(side);
    write_be32(b.data() + 20, shares);
    std::memcpy(b.data() + 24, "TEST    ", 8);
    write_be32(b.data() + 32, static_cast<uint32_t>(price * 10000.0));
    return b;
}

/// OrderDelete ('D'): 19 bytes — type(1) locate(2) tracking(2) ts(6) ref(8)
inline std::vector<uint8_t> build_order_delete(uint64_t ref) {
    std::vector<uint8_t> b(kOrderDeleteSize, 0);
    b[0] = kOrderDelete;
    write_be16(b.data() + 1, /*locate*/ 1);
    write_be64(b.data() + 11, ref);
    return b;
}

/// OrderExecuted ('E'): 31 bytes — type(1) locate(2) tracking(2) ts(6)
///                                  ref(8) exec_shares(4) match_num(8)
inline std::vector<uint8_t> build_order_executed(uint64_t ref,
                                                  uint32_t exec_shares,
                                                  uint64_t match_num = 1) {
    std::vector<uint8_t> b(kOrderExecutedSize, 0);
    b[0] = kOrderExecuted;
    write_be16(b.data() + 1, /*locate*/ 1);
    write_be64(b.data() + 11, ref);
    write_be32(b.data() + 19, exec_shares);
    write_be64(b.data() + 23, match_num);
    return b;
}

/// OrderCancel ('X'): 23 bytes — type(1) locate(2) tracking(2) ts(6)
///                                ref(8) cancelled_shares(4)
inline std::vector<uint8_t> build_order_cancel(uint64_t ref,
                                                uint32_t cancel_shares) {
    std::vector<uint8_t> b(kOrderCancelSize, 0);
    b[0] = kOrderCancel;
    write_be16(b.data() + 1, /*locate*/ 1);
    write_be64(b.data() + 11, ref);
    write_be32(b.data() + 19, cancel_shares);
    return b;
}

// ---------------------------------------------------------------------------
// MoldUDP64 packet builder
//
// Wire layout:
//   [10 byte session][8 byte BE seq][2 byte BE count]
//   then for each message: [2 byte BE length][body bytes]
// ---------------------------------------------------------------------------

inline std::vector<uint8_t>
build_mold_packet(std::string_view session, uint64_t seq,
                  std::vector<std::vector<uint8_t>> const& msgs) {
    std::vector<uint8_t> pkt;
    pkt.reserve(moldudp64::kHeaderLen + 64 * msgs.size());
    pkt.resize(moldudp64::kHeaderLen);

    // Session id — pad / truncate to exactly 10 bytes.
    std::memset(pkt.data(), 0, moldudp64::kSessionLen);
    std::memcpy(pkt.data(), session.data(),
                std::min<size_t>(session.size(), moldudp64::kSessionLen));

    write_be64(pkt.data() + moldudp64::kSessionLen, seq);
    write_be16(pkt.data() + moldudp64::kSessionLen + 8,
               static_cast<uint16_t>(msgs.size()));

    for (auto const& body : msgs) {
        uint8_t lp[2];
        write_be16(lp, static_cast<uint16_t>(body.size()));
        pkt.insert(pkt.end(), lp, lp + 2);
        pkt.insert(pkt.end(), body.begin(), body.end());
    }
    return pkt;
}

/// Build an end-of-session marker MoldUDP64 packet (count == 0xFFFF, no body).
inline std::vector<uint8_t>
build_mold_eos(std::string_view session, uint64_t seq) {
    std::vector<uint8_t> pkt(moldudp64::kHeaderLen, 0);
    std::memcpy(pkt.data(), session.data(),
                std::min<size_t>(session.size(), moldudp64::kSessionLen));
    write_be64(pkt.data() + moldudp64::kSessionLen, seq);
    write_be16(pkt.data() + moldudp64::kSessionLen + 8, moldudp64::kEndOfSession);
    return pkt;
}

/// Drive one MoldUDP64 packet through the entire MD pipeline.
/// Returns the count of ITCH messages that survived parse + were processed.
inline size_t feed_packet(std::vector<uint8_t> const& pkt,
                          ItchBookBuilder<10>& builder,
                          std::vector<uint64_t>* delivered_seqs = nullptr) {
    size_t processed = 0;
    parse_moldudp64(pkt.data(), pkt.size(),
        [&](const uint8_t* msg_data, uint16_t msg_len, uint64_t seq) {
            if (delivered_seqs) delivered_seqs->push_back(seq);
            // Each MoldUDP64 message body is exactly one ITCH message;
            // parse() validates type + length and returns a MessageView.
            auto v = parse(msg_data, msg_len);
            if (!v) return;
            if (builder.process(*v)) ++processed;
        });
    return processed;
}

}  // namespace

// ===========================================================================
// Pipeline: MoldUDP64 → ITCH parse → ItchBookBuilder → ArrayBook
// ===========================================================================

/// Single-packet smoke test: 4 messages produce the expected BBO.
TEST(ItchMoldBookE2E, SinglePacketBuildsExpectedBBO) {
    ItchBookBuilder<10> builder;

    auto pkt = build_mold_packet("SESS000001", /*seq=*/1, {
        build_add_order(/*ref=*/100, 'B',  500, 100.50),
        build_add_order(/*ref=*/101, 'B',  300, 100.25),
        build_add_order(/*ref=*/200, 'S',  200, 101.00),
        build_add_order(/*ref=*/201, 'S',  150, 101.25),
    });

    std::vector<uint64_t> seqs;
    EXPECT_EQ(feed_packet(pkt, builder, &seqs), 4u);

    // MoldUDP64 spec §4: each message in a packet has seq = base + index.
    ASSERT_EQ(seqs.size(), 4u);
    EXPECT_EQ(seqs[0], 1u);
    EXPECT_EQ(seqs[1], 2u);
    EXPECT_EQ(seqs[2], 3u);
    EXPECT_EQ(seqs[3], 4u);

    auto bb = builder.book().best_bid();
    auto ba = builder.book().best_ask();
    ASSERT_TRUE(bb.has_value());
    ASSERT_TRUE(ba.has_value());
    EXPECT_NEAR(bb->price, 100.50, kEps);
    EXPECT_NEAR(bb->qty,   500.0,  kEps);
    EXPECT_NEAR(ba->price, 101.00, kEps);
    EXPECT_NEAR(ba->qty,   200.0,  kEps);
    EXPECT_EQ(builder.order_count(), 4u);
}

/// End-to-end execute / cancel / delete flow inside one packet.
/// Verifies the adapter handles a realistic mixed-event burst as it would
/// arrive on a single MoldUDP64 datagram in production.
TEST(ItchMoldBookE2E, MixedEventBurstUpdatesBook) {
    ItchBookBuilder<10> builder;

    auto p1 = build_mold_packet("SESS000002", 100, {
        build_add_order(1, 'B', 1000, 99.00),
        build_add_order(2, 'B',  500, 99.50),
        build_add_order(3, 'S',  800, 100.00),
    });
    EXPECT_EQ(feed_packet(p1, builder), 3u);

    auto p2 = build_mold_packet("SESS000002", 103, {
        build_order_executed(2, 200),     // 99.50 bid: 500 → 300
        build_order_cancel(1, 400),       // 99.00 bid: 1000 → 600
        build_order_delete(3),            // 100.00 ask gone
    });
    EXPECT_EQ(feed_packet(p2, builder), 3u);

    // Best bid still 99.50, qty 300.
    auto bb = builder.book().best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_NEAR(bb->price, 99.50, kEps);
    EXPECT_NEAR(bb->qty,   300.0, kEps);

    // 99.00 still present at remaining 600 qty.
    EXPECT_EQ(builder.book().bid_depth(), 2u);

    // Ask side empty.
    EXPECT_FALSE(builder.book().best_ask().has_value());
    EXPECT_EQ(builder.order_count(), 2u);
}

/// Truncated trailing message inside a MoldUDP64 packet: parse_moldudp64
/// must deliver only the messages whose body fully fits in the buffer and
/// stop cleanly. The book must reflect a strict prefix of the intended
/// updates — no partial / phantom mutation from the chopped tail.
TEST(ItchMoldBookE2E, TruncatedTailDeliversPrefixOnly) {
    ItchBookBuilder<10> builder;

    auto pkt = build_mold_packet("SESS000003", /*seq=*/1, {
        build_add_order(/*ref=*/10, 'B', 100, 50.00),
        build_add_order(/*ref=*/11, 'S', 200, 51.00),
        build_add_order(/*ref=*/12, 'B', 300, 49.50),  // will be lopped off
    });
    // Drop the final ITCH body (36 bytes) AND its 2-byte length prefix
    // — leaves header + 2 messages + 2 messages' lp prefixes intact.
    ASSERT_GE(pkt.size(), kAddOrderSize + 2u);
    pkt.resize(pkt.size() - kAddOrderSize - 2u);

    std::vector<uint64_t> seqs;
    EXPECT_EQ(feed_packet(pkt, builder, &seqs), 2u)
        << "only the two complete messages should survive";
    ASSERT_EQ(seqs.size(), 2u);
    EXPECT_EQ(seqs[0], 1u);
    EXPECT_EQ(seqs[1], 2u);

    auto bb = builder.book().best_bid();
    auto ba = builder.book().best_ask();
    ASSERT_TRUE(bb.has_value());
    ASSERT_TRUE(ba.has_value());
    EXPECT_NEAR(bb->price, 50.00, kEps);
    EXPECT_NEAR(bb->qty,  100.0, kEps);
    EXPECT_NEAR(ba->price, 51.00, kEps);
    EXPECT_NEAR(ba->qty,  200.0, kEps);
    // The dropped third order's price (49.50) must NOT have leaked into
    // either side — same regression class as the duplicate-AddOrder
    // phantom qty bug in test_itch_adapter.
    EXPECT_EQ(builder.book().bid_depth(), 1u);
    EXPECT_EQ(builder.order_count(), 2u);
}

/// Truncated message length-prefix (only 1 of 2 bytes): parse_moldudp64
/// must stop before invoking the callback for that message. No book change.
TEST(ItchMoldBookE2E, TruncatedLengthPrefixStopsBeforeCallback) {
    ItchBookBuilder<10> builder;

    auto pkt = build_mold_packet("SESS000004", /*seq=*/1, {
        build_add_order(/*ref=*/20, 'B', 100, 75.00),
        build_add_order(/*ref=*/21, 'S', 100, 76.00),
    });
    // Lop off the second message's body AND one byte of its 2-byte length
    // prefix — leaves only one byte of LP at the tail.
    pkt.resize(pkt.size() - kAddOrderSize - 1u);

    std::vector<uint64_t> seqs;
    EXPECT_EQ(feed_packet(pkt, builder, &seqs), 1u);
    ASSERT_EQ(seqs.size(), 1u);

    auto bb = builder.book().best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_NEAR(bb->price, 75.00, kEps);
    EXPECT_FALSE(builder.book().best_ask().has_value());
    EXPECT_EQ(builder.order_count(), 1u);
}

/// End-of-session marker (count == 0xFFFF): callback is NEVER invoked,
/// book is not mutated. Nasdaq sends this to mark the daily channel
/// shutdown — a buggy parser that mis-counted the sentinel could
/// arbitrarily advance state and corrupt the book.
TEST(ItchMoldBookE2E, EndOfSessionMarkerIsNoOp) {
    ItchBookBuilder<10> builder;

    // Pre-populate so we can prove the EOS packet didn't clear / mutate it.
    auto warmup = build_mold_packet("SESS000005", 1, {
        build_add_order(/*ref=*/30, 'B', 100, 80.00),
    });
    feed_packet(warmup, builder);
    auto bb_before = builder.book().best_bid();
    ASSERT_TRUE(bb_before.has_value());

    auto eos = build_mold_eos("SESS000005", /*seq=*/2);
    std::vector<uint64_t> seqs;
    EXPECT_EQ(feed_packet(eos, builder, &seqs), 0u);
    EXPECT_TRUE(seqs.empty()) << "EOS must never invoke the message callback";

    auto bb_after = builder.book().best_bid();
    ASSERT_TRUE(bb_after.has_value());
    EXPECT_NEAR(bb_after->price, bb_before->price, kEps);
    EXPECT_NEAR(bb_after->qty,   bb_before->qty,   kEps);
    EXPECT_EQ(builder.order_count(), 1u);
}

/// Multi-packet stream: sequence numbers continue across the boundary,
/// book mutations accumulate. Mirrors the canonical Nasdaq ITCH-FOP
/// gap-detection use case where a downstream consumer counts seq deltas.
TEST(ItchMoldBookE2E, ConsecutivePacketsAccumulateAndSeqContinues) {
    ItchBookBuilder<10> builder;
    std::vector<uint64_t> all_seqs;

    auto p1 = build_mold_packet("SESS000006", /*seq=*/500, {
        build_add_order(40, 'B', 100, 25.00),
        build_add_order(41, 'B', 200, 25.50),
        build_add_order(42, 'S', 150, 26.00),
    });
    EXPECT_EQ(feed_packet(p1, builder, &all_seqs), 3u);

    auto p2 = build_mold_packet("SESS000006", /*seq=*/503, {
        build_order_executed(42, 100),  // 26.00 ask: 150 → 50
        build_add_order(43, 'S', 80, 25.75),  // new best ask
    });
    EXPECT_EQ(feed_packet(p2, builder, &all_seqs), 2u);

    ASSERT_EQ(all_seqs.size(), 5u);
    EXPECT_EQ(all_seqs[0], 500u);
    EXPECT_EQ(all_seqs[1], 501u);
    EXPECT_EQ(all_seqs[2], 502u);
    EXPECT_EQ(all_seqs[3], 503u);
    EXPECT_EQ(all_seqs[4], 504u);

    auto bb = builder.book().best_bid();
    auto ba = builder.book().best_ask();
    ASSERT_TRUE(bb.has_value());
    ASSERT_TRUE(ba.has_value());
    EXPECT_NEAR(bb->price, 25.50, kEps);
    EXPECT_NEAR(bb->qty,  200.0,  kEps);
    EXPECT_NEAR(ba->price, 25.75, kEps);  // new ref 43 took over BBO ask
    EXPECT_NEAR(ba->qty,   80.0,  kEps);
    EXPECT_EQ(builder.order_count(), 4u);  // 40, 41, 42 (50 left), 43
}

/// Header-only truncation (< 20 bytes): parse_moldudp64 must reject the
/// entire packet — no callback, no book mutation. Tests the header-vs-body
/// boundary path, distinct from the body-truncation path above.
TEST(ItchMoldBookE2E, TruncatedHeaderRejectsEntirePacket) {
    ItchBookBuilder<10> builder;
    std::array<uint8_t, moldudp64::kHeaderLen - 1> partial{};
    std::vector<uint64_t> seqs;
    auto delivered = parse_moldudp64(partial.data(), partial.size(),
        [&](const uint8_t*, uint16_t, uint64_t s) { seqs.push_back(s); });
    EXPECT_EQ(delivered, 0u);
    EXPECT_TRUE(seqs.empty());
    EXPECT_EQ(builder.order_count(), 0u);
}
