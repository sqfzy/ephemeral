/// @file itch_feed_demo.cpp
/// ITCH 5.0 message parsing demo — decode a simulated order book feed.
///
/// Demonstrates the ITCH parser, per-message-type field accessors, and
/// batch parsing. Builds a synthetic ITCH byte stream (simulating what
/// arrives from a Nasdaq TotalView feed) and parses it with zero-copy
/// accessors.
///
/// This is a standalone demo — no network connection required. For
/// receiving ITCH over a real transport, combine with Transport +
/// LengthPrefixFramer (see framer_showcase.cpp).
///
/// Usage:
///   xmake build itch_feed_demo && xmake run itch_feed_demo

#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/itch.hpp"

using namespace eph::itch;

// ---------------------------------------------------------------------------
// Helpers: build synthetic ITCH messages (big-endian wire format)
// ---------------------------------------------------------------------------

static void write_be16(uint8_t* p, uint16_t v) {
    v = std::byteswap(v);
    std::memcpy(p, &v, 2);
}

static void write_be32(uint8_t* p, uint32_t v) {
    v = std::byteswap(v);
    std::memcpy(p, &v, 4);
}

static void write_be64(uint8_t* p, uint64_t v) {
    v = std::byteswap(v);
    std::memcpy(p, &v, 8);
}

/// Write a 6-byte big-endian timestamp (nanoseconds since midnight).
static void write_be48(uint8_t* p, uint64_t ns) {
    uint64_t be = std::byteswap(ns);
    std::memcpy(p, reinterpret_cast<const uint8_t*>(&be) + 2, 6);
}

/// Write a right-padded 8-byte stock symbol.
static void write_stock(uint8_t* p, std::string_view sym) {
    std::memset(p, ' ', 8);
    std::memcpy(p, sym.data(), std::min(sym.size(), size_t{8}));
}

/// Build a SystemEvent message ('S', 12 bytes).
static std::vector<uint8_t> make_system_event(uint16_t locate, uint64_t ts_ns,
                                                char event_code) {
    std::vector<uint8_t> buf(kSystemEventSize, 0);
    buf[0] = kSystemEvent; // 'S'
    write_be16(buf.data() + 1, locate);
    write_be16(buf.data() + 3, 0); // tracking
    write_be48(buf.data() + 5, ts_ns);
    buf[11] = static_cast<uint8_t>(event_code);
    return buf;
}

/// Build an AddOrder message ('A', 36 bytes).
static std::vector<uint8_t> make_add_order(uint16_t locate, uint64_t ts_ns,
                                            uint64_t order_ref, char side,
                                            uint32_t shares, std::string_view stock,
                                            uint32_t price_raw) {
    std::vector<uint8_t> buf(kAddOrderSize, 0);
    buf[0] = kAddOrder; // 'A'
    write_be16(buf.data() + 1, locate);
    write_be16(buf.data() + 3, 0);
    write_be48(buf.data() + 5, ts_ns);
    write_be64(buf.data() + 11, order_ref);
    buf[19] = static_cast<uint8_t>(side);
    write_be32(buf.data() + 20, shares);
    write_stock(buf.data() + 24, stock);
    write_be32(buf.data() + 32, price_raw);
    return buf;
}

/// Build an OrderDelete message ('D', 19 bytes).
static std::vector<uint8_t> make_order_delete(uint16_t locate, uint64_t ts_ns,
                                               uint64_t order_ref) {
    std::vector<uint8_t> buf(kOrderDeleteSize, 0);
    buf[0] = kOrderDelete; // 'D'
    write_be16(buf.data() + 1, locate);
    write_be16(buf.data() + 3, 0);
    write_be48(buf.data() + 5, ts_ns);
    write_be64(buf.data() + 11, order_ref);
    return buf;
}

/// Build a NonCrossTrade message ('P', 44 bytes).
static std::vector<uint8_t> make_trade(uint16_t locate, uint64_t ts_ns,
                                        uint64_t order_ref, char side,
                                        uint32_t shares, std::string_view stock,
                                        uint32_t price_raw, uint64_t match_num) {
    std::vector<uint8_t> buf(kNonCrossTradeSize, 0);
    buf[0] = kNonCrossTrade; // 'P'
    write_be16(buf.data() + 1, locate);
    write_be16(buf.data() + 3, 0);
    write_be48(buf.data() + 5, ts_ns);
    write_be64(buf.data() + 11, order_ref);
    buf[19] = static_cast<uint8_t>(side);
    write_be32(buf.data() + 20, shares);
    write_stock(buf.data() + 24, stock);
    write_be32(buf.data() + 32, price_raw);
    write_be64(buf.data() + 36, match_num);
    return buf;
}

// ---------------------------------------------------------------------------
// Demo
// ---------------------------------------------------------------------------

int main() {
    spdlog::set_level(spdlog::level::info);
    spdlog::info("=== ITCH 5.0 Feed Demo ===\n");

    // --- Step 1: Build a synthetic ITCH feed ---
    // Simulates a market open sequence: system event → add orders → trade → delete

    constexpr uint64_t t0 = 34200000000000ULL; // 09:30:00.000000000 (ns since midnight)
    constexpr uint16_t aapl_locate = 42;

    std::vector<uint8_t> feed;
    auto append = [&](const std::vector<uint8_t>& msg) {
        feed.insert(feed.end(), msg.begin(), msg.end());
    };

    // Market open event
    append(make_system_event(0, t0, 'Q')); // Q = Market Hours start

    // Add bid order: AAPL 100 shares @ $185.50 (price * 10000 = 1855000)
    append(make_add_order(aapl_locate, t0 + 1000, 1001, 'B', 100, "AAPL", 1855000));

    // Add ask order: AAPL 200 shares @ $185.75
    append(make_add_order(aapl_locate, t0 + 2000, 1002, 'S', 200, "AAPL", 1857500));

    // Add bid order: MSFT 500 shares @ $420.25
    append(make_add_order(7, t0 + 3000, 2001, 'B', 500, "MSFT", 4202500));

    // Trade: AAPL 50 shares @ $185.50
    append(make_trade(aapl_locate, t0 + 5000, 1001, 'B', 50, "AAPL", 1855000, 9900001));

    // Delete remaining bid order
    append(make_order_delete(aapl_locate, t0 + 8000, 1001));

    spdlog::info("Built synthetic feed: {} bytes, {} messages\n", feed.size(), 6);

    // --- Step 2: Parse individual messages ---
    spdlog::info("--- Single-Message Parsing ---\n");

    auto result = parse(feed.data(), feed.size());
    if (result) {
        auto& mv = *result;
        spdlog::info("First message: {} (type '{:c}'), {} bytes",
                     message_type_name(mv.msg_type),
                     static_cast<char>(mv.msg_type), mv.length);
    }

    // --- Step 3: Batch parse with per-type dispatch ---
    spdlog::info("\n--- Batch Parsing (all messages) ---\n");

    size_t msg_count = 0;
    size_t consumed = parse_all(feed.data(), feed.size(),
        [&](const MessageView& mv) {
            msg_count++;
            const uint8_t* msg = mv.data; // full message pointer (byte 0 = type)

            switch (mv.msg_type) {
            case kSystemEvent: {
                char event = static_cast<char>(msg[11]);
                spdlog::info("[{:>12}] SystemEvent: code='{}'",
                             mv.timestamp_ns(), event);
                break;
            }
            case kAddOrder: {
                auto ref    = add_order::order_ref(msg);
                auto side   = add_order::side(msg);
                auto qty    = add_order::shares(msg);
                auto sym    = add_order::stock_trimmed(msg);
                auto px     = add_order::price(msg);

                spdlog::info("[{:>12}] AddOrder: ref={} {} {} {}@{:.4f}",
                             mv.timestamp_ns(), ref,
                             side == 'B' ? "BUY " : "SELL", sym, qty, px);
                break;
            }
            case kNonCrossTrade: {
                auto ref    = non_cross_trade::order_ref(msg);
                auto qty    = non_cross_trade::shares(msg);
                auto sym    = non_cross_trade::stock_trimmed(msg);
                auto px     = non_cross_trade::price(msg);
                auto match  = non_cross_trade::match_number(msg);

                spdlog::info("[{:>12}] Trade: ref={} {} {}@{:.4f} match={}",
                             mv.timestamp_ns(), ref, sym, qty, px, match);
                break;
            }
            case kOrderDelete: {
                auto ref = order_delete::order_ref(msg);
                spdlog::info("[{:>12}] OrderDelete: ref={}",
                             mv.timestamp_ns(), ref);
                break;
            }
            default:
                spdlog::info("[{:>12}] {}: {} bytes",
                             mv.timestamp_ns(),
                             message_type_name(mv.msg_type), mv.length);
                break;
            }
        });

    spdlog::info("\nParsed {} messages, consumed {} / {} bytes",
                 msg_count, consumed, feed.size());

    // --- Step 4: Demonstrate message classification ---
    spdlog::info("\n--- Message Classification ---\n");

    parse_all(feed.data(), feed.size(),
        [](const MessageView& mv) {
            const char* cat = "other";
            if (is_order_message(mv.msg_type))  cat = "order";
            if (is_trade_message(mv.msg_type))  cat = "trade";
            if (is_system_message(mv.msg_type)) cat = "system";
            spdlog::info("  {:c} ({}) → {}", static_cast<char>(mv.msg_type),
                         message_type_name(mv.msg_type), cat);
        });

    // --- Step 5: JSON export ---
    spdlog::info("\n--- JSON Export (first AddOrder) ---\n");

    // Skip to second message (first AddOrder)
    size_t offset = kSystemEventSize; // skip SystemEvent
    auto add_result = parse(feed.data() + offset, feed.size() - offset);
    if (add_result) {
        spdlog::info("{}", add_result->to_json());
    }

    return 0;
}
