#pragma once

/// @file adapters/binance.hpp
/// Zero-copy Binance WebSocket message adapters for HFT market data.
///
/// Provides typed structs for Binance's most latency-sensitive feeds
/// (bookTicker, trade, depth) with zero-copy field extraction from
/// JsonView. Also provides the symbol_hash extractor for the Transport
/// two-phase frame filter (latest-per-symbol deduplication).
///
/// Usage:
///   auto json = eph::json::parse(data, len);
///   if (auto ticker = BinanceBookTicker::from(json.value())) {
///       auto bid = ticker->bid_price;  // string_view into original buffer
///       auto sym = ticker->symbol;     // "btcusdt"
///   }

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

#include "eph/json/parser.hpp"

namespace eph::json::binance {

// ---------------------------------------------------------------------------
// Symbol extraction helpers
// ---------------------------------------------------------------------------

/// Extract symbol name from a Binance stream suffix.
/// "btcusdt@bookTicker" → "btcusdt"
/// "ethusdt@trade" → "ethusdt"
/// Returns the full input if no '@' found.
[[nodiscard]] inline constexpr std::string_view
extract_symbol(std::string_view stream) noexcept {
    auto pos = stream.find('@');
    return pos != std::string_view::npos ? stream.substr(0, pos) : stream;
}

/// FNV-1a hash of the symbol field in a Binance JSON payload.
///
/// Designed for use with Transport's two-phase frame filter
/// (make_twophase_filter). Extracts the "s" field from the JSON
/// payload and hashes it. Returns 0 for unrecognized payloads
/// (which are delivered unconditionally by the filter).
///
/// @param data  Raw JSON payload bytes
/// @param len   Payload length
/// @return FNV-1a hash of the symbol, or 0 if "s" field not found
[[nodiscard]] inline uint32_t
symbol_hash(const uint8_t* data, size_t len) noexcept {
    // Fast scan for "s":" pattern (avoids full JSON parse on hot path)
    // Binance bookTicker: {"e":"bookTicker","u":...,"s":"BTCUSDT",...}
    // The "s" field is typically within the first 80 bytes.
    const char* p = reinterpret_cast<const char*>(data);
    const char* end = p + len;

    while (p + 5 < end) {
        // Look for "s":" pattern
        if (p[0] == '"' && p[1] == 's' && p[2] == '"' && p[3] == ':' && p[4] == '"') {
            p += 5; // skip to value start
            const char* sym_start = p;
            while (p < end && *p != '"') ++p;
            if (p >= end) return 0;

            // FNV-1a hash
            uint32_t h = 2166136261u;
            for (const char* s = sym_start; s < p; ++s) {
                h ^= static_cast<uint32_t>(static_cast<uint8_t>(*s));
                h *= 16777619u;
            }
            return h;
        }
        ++p;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Binance bookTicker
// ---------------------------------------------------------------------------

/// Zero-copy view of a Binance bookTicker message.
///
/// Fields:
///   e: "bookTicker"
///   u: updateId
///   s: symbol (e.g., "BTCUSDT")
///   b: bestBidPrice
///   B: bestBidQty
///   a: bestAskPrice
///   A: bestAskQty
///   T: transaction time (ms)
///   E: event time (ms)
struct BookTicker {
    std::string_view symbol;     ///< "BTCUSDT"
    std::string_view bid_price;  ///< Best bid price (string, e.g., "87245.30")
    std::string_view bid_qty;    ///< Best bid quantity
    std::string_view ask_price;  ///< Best ask price
    std::string_view ask_qty;    ///< Best ask quantity
    int64_t update_id = 0;       ///< Order book updateId
    int64_t event_time = 0;      ///< Event time (milliseconds since epoch)
    int64_t txn_time = 0;        ///< Transaction time (milliseconds since epoch)

    /// Extract BookTicker from a parsed JsonView.
    /// Returns nullopt if the required fields are missing or invalid.
    [[nodiscard]] static std::optional<BookTicker>
    from(const JsonView& json) noexcept {
        auto s = json.get_string("s");
        auto b = json.get_string("b");
        auto B = json.get_string("B");
        auto a = json.get_string("a");
        auto A = json.get_string("A");

        if (!s || !b || !B || !a || !A) {
            SPDLOG_DEBUG("BookTicker::from: missing required field "
                         "(s={} b={} B={} a={} A={})",
                         s.has_value(), b.has_value(), B.has_value(),
                         a.has_value(), A.has_value());
            return std::nullopt;
        }

        BookTicker t;
        t.symbol = *s;
        t.bid_price = *b;
        t.bid_qty = *B;
        t.ask_price = *a;
        t.ask_qty = *A;

        if (auto u = json.get_int("u")) t.update_id = *u;
        if (auto E = json.get_int("E")) t.event_time = *E;
        if (auto T = json.get_int("T")) t.txn_time = *T;

        return t;
    }

    /// Compute mid price as double. Returns nullopt if prices are not valid numbers.
    [[nodiscard]] std::optional<double> mid_price() const noexcept {
        // Reuse JsonView's parse for string→double. Create a temporary
        // single-field view to access the private parse_double helper.
        auto bid = parse_number(bid_price);
        auto ask = parse_number(ask_price);
        if (!bid || !ask) return std::nullopt;
        return (*bid + *ask) / 2.0;
    }

    /// Compute spread as double. Returns nullopt if prices are not valid numbers.
    [[nodiscard]] std::optional<double> spread() const noexcept {
        auto bid = parse_number(bid_price);
        auto ask = parse_number(ask_price);
        if (!bid || !ask) return std::nullopt;
        return *ask - *bid;
    }

private:
    /// Parse a string_view as double (for price fields).
    static std::optional<double> parse_number(std::string_view sv) noexcept {
        if (sv.empty()) return std::nullopt;
        bool negative = false;
        size_t pos = 0;
        if (sv[0] == '-') { negative = true; pos = 1; }
        if (pos >= sv.size()) return std::nullopt;
        double result = 0.0;
        for (; pos < sv.size() && sv[pos] != '.'; ++pos) {
            char c = sv[pos];
            if (c < '0' || c > '9') return std::nullopt;
            result = result * 10.0 + (c - '0');
        }
        if (pos < sv.size() && sv[pos] == '.') {
            ++pos;
            double divisor = 10.0;
            for (; pos < sv.size(); ++pos) {
                char c = sv[pos];
                if (c < '0' || c > '9') return std::nullopt;
                result += (c - '0') / divisor;
                divisor *= 10.0;
            }
        }
        return negative ? -result : result;
    }

public:
};

// ---------------------------------------------------------------------------
// Binance combined stream wrapper
// ---------------------------------------------------------------------------

/// Parse a Binance combined stream wrapper: {"stream":"...","data":{...}}
/// Returns the stream name and the inner data object as a JsonView.
struct CombinedStream {
    std::string_view stream;   ///< e.g., "btcusdt@bookTicker"
    std::string_view symbol;   ///< Extracted from stream: "btcusdt"
    std::string_view data_raw; ///< Raw JSON of the inner "data" object

    /// Extract from a parsed JsonView of the combined stream wrapper.
    [[nodiscard]] static std::optional<CombinedStream>
    from(const JsonView& json) noexcept {
        auto stream = json.get_string("stream");
        auto data = json.get("data");  // Opaque nested object

        if (!stream || data.empty()) return std::nullopt;

        CombinedStream cs;
        cs.stream = *stream;
        cs.symbol = extract_symbol(*stream);
        cs.data_raw = data;
        return cs;
    }
};

// ---------------------------------------------------------------------------
// WebSocket subscription helpers
// ---------------------------------------------------------------------------

/// Build a WebSocket path for a single stream.
/// e.g., ws_path("btcusdt", "bookTicker") -> "/ws/btcusdt@bookTicker"
[[nodiscard]] inline std::string
ws_path(std::string_view symbol, std::string_view stream_type) noexcept {
    std::string result;
    result.reserve(4 + symbol.size() + 1 + stream_type.size());
    result.append("/ws/");
    result.append(symbol);
    result.push_back('@');
    result.append(stream_type);
    SPDLOG_DEBUG("ws_path: built path=\"{}\"", result);
    return result;
}

/// Build a combined stream WebSocket path for multiple symbols.
/// e.g., combined_ws_path({"btcusdt","ethusdt"}, "bookTicker")
///   -> "/stream?streams=btcusdt@bookTicker/ethusdt@bookTicker"
/// Returns "/stream?streams=" for an empty symbols list.
[[nodiscard]] inline std::string
combined_ws_path(std::span<const std::string_view> symbols,
                 std::string_view stream_type) noexcept {
    std::string result = "/stream?streams=";
    for (size_t i = 0; i < symbols.size(); ++i) {
        if (i > 0) result.push_back('/');
        result.append(symbols[i]);
        result.push_back('@');
        result.append(stream_type);
    }
    SPDLOG_DEBUG("combined_ws_path: built path=\"{}\" for {} symbols",
                 result, symbols.size());
    return result;
}

/// Build a SUBSCRIBE JSON message (sent after WebSocket connection).
/// Returns: {"method":"SUBSCRIBE","params":["sym@stream",...],"id":N}
/// Returns a message with empty params array for an empty symbols list.
[[nodiscard]] inline std::string
subscribe_message(std::span<const std::string_view> symbols,
                  std::string_view stream_type,
                  int id = 1) noexcept {
    std::string result = R"({"method":"SUBSCRIBE","params":[)";
    for (size_t i = 0; i < symbols.size(); ++i) {
        if (i > 0) result.push_back(',');
        result.push_back('"');
        result.append(symbols[i]);
        result.push_back('@');
        result.append(stream_type);
        result.push_back('"');
    }
    result.append(R"(],"id":)");
    result.append(std::to_string(id));
    result.push_back('}');
    SPDLOG_DEBUG("subscribe_message: built msg for {} symbols, id={}",
                 symbols.size(), id);
    return result;
}

/// Build an UNSUBSCRIBE JSON message.
/// Returns: {"method":"UNSUBSCRIBE","params":["sym@stream",...],"id":N}
/// Returns a message with empty params array for an empty symbols list.
[[nodiscard]] inline std::string
unsubscribe_message(std::span<const std::string_view> symbols,
                    std::string_view stream_type,
                    int id = 2) noexcept {
    std::string result = R"({"method":"UNSUBSCRIBE","params":[)";
    for (size_t i = 0; i < symbols.size(); ++i) {
        if (i > 0) result.push_back(',');
        result.push_back('"');
        result.append(symbols[i]);
        result.push_back('@');
        result.append(stream_type);
        result.push_back('"');
    }
    result.append(R"(],"id":)");
    result.append(std::to_string(id));
    result.push_back('}');
    SPDLOG_DEBUG("unsubscribe_message: built msg for {} symbols, id={}",
                 symbols.size(), id);
    return result;
}

} // namespace eph::json::binance
