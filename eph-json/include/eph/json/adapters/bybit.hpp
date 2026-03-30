#pragma once

/// @file adapters/bybit.hpp
/// Zero-copy Bybit WebSocket message adapters for HFT market data.
///
/// Bybit (third-largest crypto derivatives exchange) pushes data in a
/// topic-based wrapper: {"topic":"tickers.BTCUSDT","type":"snapshot",
/// "data":{...},"ts":1711612345679} where "topic" identifies the stream,
/// "type" is "snapshot" or "delta", and "data" contains the payload.
///
/// Key differences from Binance/OKX:
///   - `topic` field identifies the stream (e.g., "tickers.BTCUSDT")
///   - `type` field: "snapshot" or "delta"
///   - Different field names: bid1Price/bid1Size/ask1Price/ask1Size
///   - `symbol` format: "BTCUSDT" (no separator, same as Binance)
///   - `ts` at both data level (string) and outer level (integer)
///
/// Usage:
///   auto json = eph::json::parse(data, len);
///   if (auto push = BybitPushMessage::from(json.value())) {
///       if (push->topic.starts_with("tickers.")) {
///           if (auto ticker = BybitBookTicker::from(json.value())) {
///               auto bid = ticker->bid_price;  // string_view into original buffer
///           }
///       }
///   }

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

#include "eph/json/parser.hpp"

namespace eph::json::bybit {

// ---------------------------------------------------------------------------
// Internal helpers (reused parsing utilities)
// ---------------------------------------------------------------------------
namespace detail {

/// Parse a string_view as double (for price/qty fields).
inline std::optional<double> parse_number(std::string_view sv) noexcept {
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

/// Parse a string_view containing a decimal number as int64_t.
/// Used for Bybit string timestamps in the data object: "ts":"1711612345678"
inline std::optional<int64_t> parse_string_int(std::string_view sv) noexcept {
    if (sv.empty()) return std::nullopt;
    int64_t result = 0;
    for (char c : sv) {
        if (c < '0' || c > '9') return std::nullopt;
        if (result > INT64_MAX / 10) return std::nullopt;
        result = result * 10;
        int digit = c - '0';
        if (result > INT64_MAX - digit) return std::nullopt;
        result += digit;
    }
    return result;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Bybit push message envelope
// ---------------------------------------------------------------------------

/// Zero-copy view of a Bybit push message envelope.
///
/// Bybit WebSocket pushes have the format:
///   {"topic":"tickers.BTCUSDT","type":"snapshot","data":{...},"ts":1711612345679}
///
/// This struct extracts the routing info (topic, type) without parsing
/// the data payload, enabling fast dispatch by topic.
struct BybitPushMessage {
    std::string_view topic;     ///< "tickers.BTCUSDT", "orderbook.1.BTCUSDT", etc.
    std::string_view type;      ///< "snapshot" or "delta"
    std::string_view data_raw;  ///< Raw data object "{...}"

    /// Extract push message envelope from a parsed JsonView.
    /// Returns nullopt if required fields are missing (not a push message).
    [[nodiscard]] static std::optional<BybitPushMessage>
    from(const JsonView& json) noexcept {
        auto topic_val = json.get_string("topic");
        auto type_val = json.get_string("type");
        auto data = json.get("data");

        if (!topic_val || !type_val || data.empty()) {
            SPDLOG_DEBUG("BybitPushMessage::from: missing required field "
                         "(topic={} type={} data_empty={})",
                         topic_val.has_value(), type_val.has_value(),
                         data.empty());
            return std::nullopt;
        }

        BybitPushMessage msg;
        msg.topic = *topic_val;
        msg.type = *type_val;
        msg.data_raw = data;
        return msg;
    }
};

// ---------------------------------------------------------------------------
// Bybit bookTicker (tickers channel)
// ---------------------------------------------------------------------------

/// Zero-copy view of a Bybit tickers message (best bid/offer + last price).
///
/// Extracted from the data object in a Bybit push:
///   {"topic":"tickers.BTCUSDT","type":"snapshot",
///    "data":{"symbol":"BTCUSDT","bid1Price":"87000.0","bid1Size":"1.2",
///            "ask1Price":"87001.0","ask1Size":"0.5","lastPrice":"87000.5",
///            "ts":"1711612345678"},"ts":1711612345679}
struct BybitBookTicker {
    std::string_view symbol;      ///< "BTCUSDT"
    std::string_view bid_price;   ///< "87000.0"
    std::string_view bid_qty;     ///< "1.2"
    std::string_view ask_price;   ///< "87001.0"
    std::string_view ask_qty;     ///< "0.5"
    std::string_view last_price;  ///< "87000.5"
    int64_t timestamp_ms = 0;    ///< Parsed from data "ts" string (ms since epoch)

    /// Extract BybitBookTicker from a parsed JsonView of the outer push message.
    /// Navigates: outer.data -> parse inner -> extract fields.
    /// Returns nullopt if required fields are missing or invalid.
    [[nodiscard]] static std::optional<BybitBookTicker>
    from(const JsonView& json) noexcept {
        auto data_raw = json.get("data");
        if (data_raw.empty()) {
            SPDLOG_DEBUG("BybitBookTicker::from: no data field in outer message");
            return std::nullopt;
        }

        // Parse the inner data object
        auto inner = parse(
            reinterpret_cast<const uint8_t*>(data_raw.data()),
            data_raw.size());
        if (!inner) {
            SPDLOG_DEBUG("BybitBookTicker::from: failed to parse data object");
            return std::nullopt;
        }

        auto sym = inner->get_string("symbol");
        auto bp  = inner->get_string("bid1Price");
        auto bs  = inner->get_string("bid1Size");
        auto ap  = inner->get_string("ask1Price");
        auto as  = inner->get_string("ask1Size");

        if (!sym || !bp || !bs || !ap || !as) {
            SPDLOG_DEBUG("BybitBookTicker::from: missing required field "
                         "(symbol={} bid1Price={} bid1Size={} ask1Price={} ask1Size={})",
                         sym.has_value(), bp.has_value(), bs.has_value(),
                         ap.has_value(), as.has_value());
            return std::nullopt;
        }

        BybitBookTicker t;
        t.symbol = *sym;
        t.bid_price = *bp;
        t.bid_qty = *bs;
        t.ask_price = *ap;
        t.ask_qty = *as;

        // lastPrice is optional (may not be present in delta updates)
        if (auto lp = inner->get_string("lastPrice")) {
            t.last_price = *lp;
        }

        // Parse string timestamp from data object
        if (auto ts_str = inner->get_string("ts")) {
            if (auto ts_val = detail::parse_string_int(*ts_str)) {
                t.timestamp_ms = *ts_val;
            }
        }

        return t;
    }

    /// Compute mid price as double. Returns nullopt if prices are not valid numbers.
    [[nodiscard]] std::optional<double> mid_price() const noexcept {
        auto bid = detail::parse_number(bid_price);
        auto ask = detail::parse_number(ask_price);
        if (!bid || !ask) return std::nullopt;
        return (*bid + *ask) / 2.0;
    }

    /// Compute spread as double. Returns nullopt if prices are not valid numbers.
    [[nodiscard]] std::optional<double> spread() const noexcept {
        auto bid = detail::parse_number(bid_price);
        auto ask = detail::parse_number(ask_price);
        if (!bid || !ask) return std::nullopt;
        return *ask - *bid;
    }
};

// ---------------------------------------------------------------------------
// symbol hash for two-phase frame filter
// ---------------------------------------------------------------------------

/// FNV-1a hash of the symbol field in a Bybit JSON payload.
///
/// Designed for use with Transport's two-phase frame filter
/// (make_twophase_filter). Fast-scans the raw JSON for "symbol":"
/// pattern and hashes the value. Returns 0 for unrecognized payloads
/// (which are delivered unconditionally by the filter).
///
/// @param data  Raw JSON payload bytes
/// @param len   Payload length
/// @return FNV-1a hash of the symbol, or 0 if not found
[[nodiscard]] inline uint32_t
symbol_hash(const uint8_t* data, size_t len) noexcept {
    if (!data || len == 0) return 0;

    const char* p = reinterpret_cast<const char*>(data);
    const char* end = p + len;

    // Pattern: "symbol":"
    static constexpr std::string_view kPattern = "\"symbol\":\"";

    while (p + kPattern.size() < end) {
        if (p[0] == '"' && p[1] == 's' &&
            std::string_view(p, kPattern.size()) == kPattern) {
            p += kPattern.size();
            const char* val_start = p;
            while (p < end && *p != '"') ++p;
            if (p >= end) return 0;

            // FNV-1a hash
            uint32_t h = 2166136261u;
            for (const char* s = val_start; s < p; ++s) {
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
// WebSocket subscription helpers
// ---------------------------------------------------------------------------

/// Build a Bybit WebSocket subscription message.
///
/// Bybit subscribe format:
///   {"op":"subscribe","args":["tickers.BTCUSDT","tickers.ETHUSDT"],"req_id":"1"}
///
/// @param channel  Channel prefix (e.g., "tickers", "orderbook.1", "publicTrade")
/// @param symbols  Symbols to subscribe (e.g., "BTCUSDT", "ETHUSDT")
/// @param req_id   Request ID for correlation
/// @return JSON subscription message string
[[nodiscard]] inline std::string
subscribe_message(std::string_view channel,
                  std::span<const std::string_view> symbols,
                  int req_id = 1) noexcept {
    std::string result = R"({"op":"subscribe","args":[)";
    for (size_t i = 0; i < symbols.size(); ++i) {
        if (i > 0) result.push_back(',');
        result.push_back('"');
        result.append(channel);
        result.push_back('.');
        result.append(symbols[i]);
        result.push_back('"');
    }
    result.append(R"(],"req_id":")");
    result.append(std::to_string(req_id));
    result.append(R"("})");
    SPDLOG_DEBUG("bybit::subscribe_message: channel=\"{}\" symbols={} req_id={}",
                 channel, symbols.size(), req_id);
    return result;
}

/// Build a Bybit WebSocket unsubscribe message.
///
/// @param channel  Channel prefix
/// @param symbols  Symbols to unsubscribe
/// @param req_id   Request ID for correlation
/// @return JSON unsubscription message string
[[nodiscard]] inline std::string
unsubscribe_message(std::string_view channel,
                    std::span<const std::string_view> symbols,
                    int req_id = 2) noexcept {
    std::string result = R"({"op":"unsubscribe","args":[)";
    for (size_t i = 0; i < symbols.size(); ++i) {
        if (i > 0) result.push_back(',');
        result.push_back('"');
        result.append(channel);
        result.push_back('.');
        result.append(symbols[i]);
        result.push_back('"');
    }
    result.append(R"(],"req_id":")");
    result.append(std::to_string(req_id));
    result.append(R"("})");
    SPDLOG_DEBUG("bybit::unsubscribe_message: channel=\"{}\" symbols={} req_id={}",
                 channel, symbols.size(), req_id);
    return result;
}

} // namespace eph::json::bybit
