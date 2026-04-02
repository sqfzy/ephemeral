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
#include <spdlog/sinks/stdout_color_sinks.h>

#include "eph/core/parse_number.hpp"
#include "eph/json/parser.hpp"

namespace eph::json::binance {

namespace detail {
/// @brief Get or create the shared spdlog logger for the Binance adapter.
/// @return Non-owning pointer to the "json.binance" logger instance.
///
/// Thread-safe: uses Meyers-singleton initialization. The logger is
/// created on first call and reused thereafter. If the name is already
/// registered (e.g., by a prior TU), the existing logger is returned.
inline spdlog::logger* binance_logger() {
    static auto l = [] {
        try {
            return spdlog::stdout_color_mt("json.binance");
        } catch (const spdlog::spdlog_ex&) {
            return spdlog::get("json.binance");
        }
    }();
    return l.get();
}
} // namespace detail

// ---------------------------------------------------------------------------
// Symbol extraction helpers
// ---------------------------------------------------------------------------

/// @brief Extract the symbol name from a Binance stream suffix.
///
/// Splits on the first '@' character and returns the prefix.
/// Examples: "btcusdt@bookTicker" -> "btcusdt", "ethusdt@trade" -> "ethusdt".
///
/// @param stream  Stream name (e.g., "btcusdt@bookTicker")
/// @return The symbol portion before '@', or the full input if no '@' is found.
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
    if (!data || len == 0) return 0;

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

/// @brief Zero-copy view of a Binance bookTicker message.
///
/// Maps the single-letter JSON fields from Binance's bookTicker stream
/// into a typed struct with descriptive member names. All string_view
/// members point into the original parse buffer -- the caller must
/// ensure the buffer outlives this struct.
///
/// Binance JSON field mapping:
///   - @c e : "bookTicker" (event type)
///   - @c u : updateId (order book sequence number)
///   - @c s : symbol (e.g., "BTCUSDT")
///   - @c b : bestBidPrice
///   - @c B : bestBidQty
///   - @c a : bestAskPrice
///   - @c A : bestAskQty
///   - @c T : transaction time (ms since epoch)
///   - @c E : event time (ms since epoch)
///
/// @note Prices are stored as string_views for zero-copy. Use mid_price()
///       or spread() for pre-parsed double access, or parse manually with
///       eph::core::parse_number().
struct BookTicker {
    std::string_view symbol;     ///< "BTCUSDT"
    std::string_view bid_price;  ///< Best bid price (string, e.g., "87245.30")
    std::string_view bid_qty;    ///< Best bid quantity
    std::string_view ask_price;  ///< Best ask price
    std::string_view ask_qty;    ///< Best ask quantity
    int64_t update_id = 0;       ///< Order book updateId
    int64_t event_time = 0;      ///< Event time (milliseconds since epoch)
    int64_t txn_time = 0;        ///< Transaction time (milliseconds since epoch)

    // Cached parsed prices — populated once during from() to avoid re-parsing
    // in mid_price()/spread() on every call.
    std::optional<double> cached_bid{};  ///< Cached parsed bid price
    std::optional<double> cached_ask{};  ///< Cached parsed ask price

    /// @brief Extract BookTicker from a parsed JsonView.
    ///
    /// Reads fields "s", "b", "B", "a", "A" (required) and "u", "E", "T"
    /// (optional) from the JSON object. Pre-parses bid/ask prices into
    /// cached doubles for fast mid_price()/spread() access.
    ///
    /// @param json  Parsed JSON view of a Binance bookTicker message.
    /// @return Populated BookTicker, or nullopt if any required field is missing.
    [[nodiscard]] static std::optional<BookTicker>
    from(const JsonView& json) noexcept {
        auto s = json.get_string("s");
        auto b = json.get_string("b");
        auto B = json.get_string("B");
        auto a = json.get_string("a");
        auto A = json.get_string("A");

        if (!s || !b || !B || !a || !A) {
            SPDLOG_LOGGER_DEBUG(detail::binance_logger(),"BookTicker::from: missing required field "
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

        // Pre-parse prices once to avoid repeated string→double conversion
        // in mid_price()/spread().
        t.cached_bid = parse_number(t.bid_price);
        t.cached_ask = parse_number(t.ask_price);

        return t;
    }

    /// @brief Compute mid price as (bid + ask) / 2.
    ///
    /// Uses cached parsed values when available (populated by from()),
    /// falls back to on-demand string-to-double parsing for manually
    /// constructed instances.
    ///
    /// @return Mid price as double, or nullopt if either price string
    ///         cannot be parsed as a valid number.
    [[nodiscard]] std::optional<double> mid_price() const noexcept {
        auto bid = cached_bid ? cached_bid : parse_number(bid_price);
        auto ask = cached_ask ? cached_ask : parse_number(ask_price);
        if (!bid || !ask) return std::nullopt;
        return (*bid + *ask) / 2.0;
    }

    /// @brief Compute spread as (ask - bid).
    ///
    /// Uses cached parsed values when available (populated by from()),
    /// falls back to on-demand string-to-double parsing for manually
    /// constructed instances.
    ///
    /// @return Spread as double, or nullopt if either price string
    ///         cannot be parsed as a valid number.
    [[nodiscard]] std::optional<double> spread() const noexcept {
        auto bid = cached_bid ? cached_bid : parse_number(bid_price);
        auto ask = cached_ask ? cached_ask : parse_number(ask_price);
        if (!bid || !ask) return std::nullopt;
        return *ask - *bid;
    }

private:
    /// @brief Parse a string_view as double (for price fields).
    /// @param sv  String representation of a decimal number.
    /// @return Parsed double, or nullopt on invalid input.
    static std::optional<double> parse_number(std::string_view sv) noexcept {
        return eph::core::parse_number(sv);
    }

public:
};

// ---------------------------------------------------------------------------
// Binance combined stream wrapper
// ---------------------------------------------------------------------------

/// @brief Zero-copy view of a Binance combined stream wrapper.
///
/// Binance's combined stream endpoint (/stream?streams=...) wraps each
/// push in {"stream":"<name>","data":{...}}. This struct extracts the
/// routing metadata (stream name, symbol) and the raw inner payload
/// without parsing the data object.
///
/// @note The @c data_raw member is an opaque JSON object string that must
///       be re-parsed (via eph::json::parse) to extract individual fields.
struct CombinedStream {
    std::string_view stream;   ///< Full stream name, e.g., "btcusdt@bookTicker"
    std::string_view symbol;   ///< Symbol extracted from stream via extract_symbol(), e.g., "btcusdt"
    std::string_view data_raw; ///< Raw JSON of the inner "data" object (including braces)

    /// @brief Extract CombinedStream from a parsed JsonView of the wrapper.
    ///
    /// Reads the "stream" string field and the "data" opaque nested object.
    ///
    /// @param json  Parsed JSON view of the combined stream wrapper.
    /// @return Populated CombinedStream, or nullopt if "stream" or "data" is missing.
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

/// @brief Build a WebSocket path for a single Binance stream.
///
/// Constructs the URL path component for connecting to a single
/// Binance WebSocket stream.
///
/// @param symbol       Lowercase symbol (e.g., "btcusdt")
/// @param stream_type  Stream type suffix (e.g., "bookTicker", "trade", "depth@100ms")
/// @return Path string, e.g., "/ws/btcusdt@bookTicker"
[[nodiscard]] inline std::string
ws_path(std::string_view symbol, std::string_view stream_type) noexcept {
    std::string result;
    result.reserve(4 + symbol.size() + 1 + stream_type.size());
    result.append("/ws/");
    result.append(symbol);
    result.push_back('@');
    result.append(stream_type);
    SPDLOG_LOGGER_DEBUG(detail::binance_logger(),"ws_path: built path=\"{}\"", result);
    return result;
}

/// @brief Build a combined stream WebSocket path for multiple symbols.
///
/// Constructs the URL path for Binance's combined stream endpoint,
/// which multiplexes multiple streams onto a single WebSocket connection.
///
/// @param symbols      Span of lowercase symbols (e.g., {"btcusdt", "ethusdt"})
/// @param stream_type  Stream type suffix (e.g., "bookTicker")
/// @return Path string, e.g., "/stream?streams=btcusdt@bookTicker/ethusdt@bookTicker"
/// @note Returns "/stream?streams=" for an empty symbols list.
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
    SPDLOG_LOGGER_DEBUG(detail::binance_logger(),"combined_ws_path: built path=\"{}\" for {} symbols",
                 result, symbols.size());
    return result;
}

/// @brief Build a Binance SUBSCRIBE JSON message (sent after WebSocket connection).
///
/// Produces a JSON message conforming to Binance's WebSocket API:
///   {"method":"SUBSCRIBE","params":["sym@stream",...],"id":N}
///
/// @param symbols      Span of lowercase symbols to subscribe (e.g., {"btcusdt", "ethusdt"})
/// @param stream_type  Stream type suffix (e.g., "bookTicker", "trade")
/// @param id           Request ID for matching the server's acknowledgment response
/// @return JSON subscription message string.
/// @note Returns a message with an empty params array for an empty symbols list.
// NOTE: subscribe_message() pattern is similar across exchange adapters (binance/okx/bybit)
// but JSON payload format differs per exchange, making a shared abstraction impractical.
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
    SPDLOG_LOGGER_DEBUG(detail::binance_logger(),"subscribe_message: built msg for {} symbols, id={}",
                 symbols.size(), id);
    return result;
}

/// @brief Build a Binance UNSUBSCRIBE JSON message.
///
/// Produces a JSON message conforming to Binance's WebSocket API:
///   {"method":"UNSUBSCRIBE","params":["sym@stream",...],"id":N}
///
/// @param symbols      Span of lowercase symbols to unsubscribe
/// @param stream_type  Stream type suffix (e.g., "bookTicker", "trade")
/// @param id           Request ID for matching the server's acknowledgment response
/// @return JSON unsubscription message string.
/// @note Returns a message with an empty params array for an empty symbols list.
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
    SPDLOG_LOGGER_DEBUG(detail::binance_logger(),"unsubscribe_message: built msg for {} symbols, id={}",
                 symbols.size(), id);
    return result;
}

} // namespace eph::json::binance
