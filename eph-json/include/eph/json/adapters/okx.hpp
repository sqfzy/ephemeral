#pragma once

/// @file adapters/okx.hpp
/// Zero-copy OKX WebSocket message adapters for HFT market data.
///
/// OKX pushes data in a wrapper format: {"arg":{...},"data":[{...}]}
/// where "arg" contains channel metadata and "data" is an array of
/// one or more data objects. This adapter provides typed structs for
/// the most latency-sensitive feeds (bookTicker/bbo-tbt) with
/// zero-copy field extraction from JsonView.
///
/// Key differences from Binance:
///   - Wrapped in {"arg":{...},"data":[{...}]} (channel + data array)
///   - Different field names: bidPx/bidSz/askPx/askSz
///   - Timestamps are strings ("ts":"1711612345678"), not integers
///   - instId format: "BTC-USDT" (hyphenated)
///
/// Usage:
///   auto json = eph::json::parse(data, len);
///   if (auto push = OkxPushMessage::from(json.value())) {
///       // Extract first data element for bbo-tbt
///       if (push->channel == "bbo-tbt") {
///           if (auto ticker = OkxBookTicker::from(json.value())) {
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
#include <spdlog/sinks/stdout_color_sinks.h>

#include "eph/core/parse_number.hpp"
#include "eph/json/parser.hpp"

namespace eph::json::okx {

namespace detail {
/// @brief Get or create the shared spdlog logger for the OKX adapter.
/// @return Non-owning pointer to the "json.okx" logger instance.
///
/// Thread-safe: uses Meyers-singleton initialization.
inline spdlog::logger* okx_logger() {
    static auto l = [] {
        try {
            return spdlog::stdout_color_mt("json.okx");
        } catch (const spdlog::spdlog_ex&) {
            auto recovered = spdlog::get("json.okx");
            return recovered ? recovered : spdlog::default_logger();
        }
    }();
    return l.get();
}
} // namespace detail

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace detail {

/// @brief Extract the first JSON object from an array string "[{...},...]".
///
/// Scans for the opening '{', then counts brace depth (respecting quoted
/// strings and escape sequences) to find the matching '}'. Used to pull
/// the first data element from OKX's "data":[{...}] array without a
/// full array parser.
///
/// @param arr  Raw JSON array string, e.g., @c [{"bidPx":"1.0",...},...]
/// @return Substring for the first element (including braces), or an empty
///         string_view if the array is empty, does not start with '[', or
///         the first element is not an object.
inline std::string_view first_array_element(std::string_view arr) noexcept {
    // Skip leading whitespace and '['
    size_t pos = 0;
    while (pos < arr.size() && (arr[pos] == ' ' || arr[pos] == '\t' ||
                                 arr[pos] == '\n' || arr[pos] == '\r'))
        ++pos;
    if (pos >= arr.size() || arr[pos] != '[') return {};
    ++pos;

    // Skip whitespace after '['
    while (pos < arr.size() && (arr[pos] == ' ' || arr[pos] == '\t' ||
                                 arr[pos] == '\n' || arr[pos] == '\r'))
        ++pos;

    if (pos >= arr.size() || arr[pos] != '{') return {};

    // Find matching closing brace, respecting nesting and strings
    const char* p = arr.data() + pos;
    const char* end = arr.data() + arr.size();
    const char* start = p;
    int depth = 1;
    ++p;
    while (p < end && depth > 0) {
        if (*p == '"') {
            ++p;
            while (p < end && *p != '"') {
                if (*p == '\\' && p + 1 < end) ++p;
                if (p < end) ++p;
            }
            if (p < end) ++p; // skip closing quote
            continue;
        }
        if (*p == '{') ++depth;
        else if (*p == '}') --depth;
        ++p;
    }
    if (depth != 0) return {};
    return {start, static_cast<size_t>(p - start)};
}

/// @brief Parse a string_view containing a decimal number as int64_t.
///
/// Used for OKX string timestamps: "ts":"1711612345678".
///
/// @param sv  String representation of an integer.
/// @return Parsed int64_t, or nullopt on overflow or invalid input.
inline std::optional<int64_t> parse_string_int(std::string_view sv) noexcept {
    return eph::core::parse_int(sv);
}

/// @brief Parse a string_view as double (for price/qty fields).
/// @param sv  String representation of a decimal number.
/// @return Parsed double, or nullopt on invalid input.
inline std::optional<double> parse_number(std::string_view sv) noexcept {
    return eph::core::parse_number(sv);
}

} // namespace detail

// ---------------------------------------------------------------------------
// OKX push message envelope
// ---------------------------------------------------------------------------

/// @brief Zero-copy view of an OKX push message envelope.
///
/// OKX WebSocket pushes have the format:
///   {"arg":{"channel":"bbo-tbt","instId":"BTC-USDT"},"data":[{...}]}
///
/// This struct extracts the channel routing info without parsing the
/// data payload, enabling fast dispatch by channel type.
///
/// @warning The @c from() factory re-parses the nested "arg" JSON object,
///          so the original buffer backing the outer JsonView must still be
///          alive when from() is called.
struct OkxPushMessage {
    std::string_view channel;   ///< "bbo-tbt", "trades", "books5", etc.
    std::string_view inst_id;   ///< From arg.instId, e.g., "BTC-USDT"
    std::string_view data_raw;  ///< Raw data array "[{...}]"

    /// @brief Extract push message envelope from a parsed JsonView.
    ///
    /// Re-parses the nested "arg" object to extract "channel" and "instId".
    ///
    /// @param json  Parsed JSON view of an OKX WebSocket message.
    /// @return Populated OkxPushMessage, or nullopt if "arg" or "data" fields
    ///         are missing, or if the arg object lacks "channel"/"instId"
    ///         (indicating it is not a data push, e.g., subscription ack).
    [[nodiscard]] static std::optional<OkxPushMessage>
    from(const JsonView& json) noexcept {
        // Get the "arg" nested object (stored as opaque string_view)
        auto arg_raw = json.get("arg");
        auto data = json.get("data");

        if (arg_raw.empty() || data.empty()) {
            SPDLOG_LOGGER_DEBUG(detail::okx_logger(),"OkxPushMessage::from: missing arg or data field "
                         "(arg_empty={} data_empty={})",
                         arg_raw.empty(), data.empty());
            return std::nullopt;
        }

        // Re-parse the arg object to extract channel and instId
        auto arg = parse(
            reinterpret_cast<const uint8_t*>(arg_raw.data()),
            arg_raw.size());
        if (!arg) {
            SPDLOG_LOGGER_DEBUG(detail::okx_logger(),"OkxPushMessage::from: failed to parse arg object");
            return std::nullopt;
        }

        auto ch = arg->get_string("channel");
        auto inst = arg->get_string("instId");
        if (!ch || !inst) {
            SPDLOG_LOGGER_DEBUG(detail::okx_logger(),"OkxPushMessage::from: missing channel or instId in arg "
                         "(channel={} instId={})",
                         ch.has_value(), inst.has_value());
            return std::nullopt;
        }

        OkxPushMessage msg;
        msg.channel = *ch;
        msg.inst_id = *inst;
        msg.data_raw = data;
        return msg;
    }
};

// ---------------------------------------------------------------------------
// OKX bookTicker (bbo-tbt channel)
// ---------------------------------------------------------------------------

/// @brief Zero-copy view of an OKX bbo-tbt (best bid/offer tick-by-tick) message.
///
/// Extracted from the first element of the data array in an OKX push:
///   {"arg":{"channel":"bbo-tbt","instId":"BTC-USDT"},
///    "data":[{"bidPx":"87000.0","bidSz":"1.2","askPx":"87001.0",
///             "askSz":"0.5","ts":"1711612345678","instId":"BTC-USDT"}]}
///
/// @warning The @c from() factory extracts the first element from the "data"
///          array and re-parses it, so the original buffer must remain alive.
struct OkxBookTicker {
    std::string_view inst_id;    ///< "BTC-USDT"
    std::string_view bid_price;  ///< "87000.0"
    std::string_view bid_qty;    ///< "1.2"
    std::string_view ask_price;  ///< "87001.0"
    std::string_view ask_qty;    ///< "0.5"
    int64_t timestamp_ms = 0;   ///< Parsed from "ts" string (ms since epoch)

    /// @brief Extract OkxBookTicker from a parsed JsonView of the outer push message.
    ///
    /// Navigates: outer.data[0] -> parse inner -> extract fields.
    /// Reads "instId", "bidPx", "bidSz", "askPx", "askSz" (required) and
    /// "ts" (optional) from the first element of the data array.
    ///
    /// @param json  Parsed JSON view of the outer OKX push message.
    /// @return Populated OkxBookTicker, or nullopt if the data array is
    ///         missing/empty, unparseable, or lacks required fields.
    [[nodiscard]] static std::optional<OkxBookTicker>
    from(const JsonView& json) noexcept {
        auto data_raw = json.get("data");
        if (data_raw.empty()) {
            SPDLOG_LOGGER_DEBUG(detail::okx_logger(),"OkxBookTicker::from: no data field in outer message");
            return std::nullopt;
        }

        // Extract first element from the data array
        auto elem = detail::first_array_element(data_raw);
        if (elem.empty()) {
            SPDLOG_LOGGER_DEBUG(detail::okx_logger(),"OkxBookTicker::from: data array empty or malformed");
            return std::nullopt;
        }

        // Parse the inner data object
        auto inner = parse(
            reinterpret_cast<const uint8_t*>(elem.data()),
            elem.size());
        if (!inner) {
            SPDLOG_LOGGER_DEBUG(detail::okx_logger(),"OkxBookTicker::from: failed to parse data element");
            return std::nullopt;
        }

        auto inst = inner->get_string("instId");
        auto bp   = inner->get_string("bidPx");
        auto bs   = inner->get_string("bidSz");
        auto ap   = inner->get_string("askPx");
        auto as   = inner->get_string("askSz");

        if (!inst || !bp || !bs || !ap || !as) {
            SPDLOG_LOGGER_DEBUG(detail::okx_logger(),"OkxBookTicker::from: missing required field "
                         "(instId={} bidPx={} bidSz={} askPx={} askSz={})",
                         inst.has_value(), bp.has_value(), bs.has_value(),
                         ap.has_value(), as.has_value());
            return std::nullopt;
        }

        OkxBookTicker t;
        t.inst_id = *inst;
        t.bid_price = *bp;
        t.bid_qty = *bs;
        t.ask_price = *ap;
        t.ask_qty = *as;

        // Parse string timestamp
        if (auto ts_str = inner->get_string("ts")) {
            if (auto ts_val = detail::parse_string_int(*ts_str)) {
                t.timestamp_ms = *ts_val;
            }
        }

        return t;
    }

    /// @brief Compute mid price as (bid + ask) / 2.
    /// @return Mid price as double, or nullopt if either price string
    ///         cannot be parsed as a valid number.
    [[nodiscard]] std::optional<double> mid_price() const noexcept {
        auto bid = detail::parse_number(bid_price);
        auto ask = detail::parse_number(ask_price);
        if (!bid || !ask) return std::nullopt;
        return (*bid + *ask) / 2.0;
    }

    /// @brief Compute spread as (ask - bid).
    /// @return Spread as double, or nullopt if either price string
    ///         cannot be parsed as a valid number.
    [[nodiscard]] std::optional<double> spread() const noexcept {
        auto bid = detail::parse_number(bid_price);
        auto ask = detail::parse_number(ask_price);
        if (!bid || !ask) return std::nullopt;
        return *ask - *bid;
    }
};

// ---------------------------------------------------------------------------
// instId hash for two-phase frame filter
// ---------------------------------------------------------------------------

/// FNV-1a hash of the instId field in an OKX JSON payload.
///
/// Designed for use with Transport's two-phase frame filter
/// (make_twophase_filter). Fast-scans the raw JSON for "instId":"
/// pattern and hashes the value. Returns 0 for unrecognized payloads
/// (which are delivered unconditionally by the filter).
///
/// @param data  Raw JSON payload bytes
/// @param len   Payload length
/// @return FNV-1a hash of the instId, or 0 if not found
[[nodiscard]] inline uint32_t
inst_id_hash(const uint8_t* data, size_t len) noexcept {
    if (!data || len == 0) return 0;

    // Fast scan for "instId":" pattern in the data array section.
    // OKX data objects contain instId: {"instId":"BTC-USDT","bidPx":...}
    // We scan for the pattern inside "data":[...] to avoid matching the
    // arg.instId (which would also work, but data section is the convention).
    const char* p = reinterpret_cast<const char*>(data);
    const char* end = p + len;

    // Pattern: "instId":"
    static constexpr std::string_view kPattern = "\"instId\":\"";

    while (p + kPattern.size() < end) {
        if (p[0] == '"' && p[1] == 'i' &&
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

/// @brief Build an OKX WebSocket subscription message.
///
/// Produces a JSON message conforming to OKX's WebSocket API:
///   {"op":"subscribe","args":[{"channel":"bbo-tbt","instId":"BTC-USDT"},...],"id":"N"}
///
/// @param channel   Channel name (e.g., "bbo-tbt", "trades", "books5")
/// @param inst_ids  Instruments to subscribe (e.g., "BTC-USDT", "ETH-USDT")
/// @param id        Request ID for matching the server's acknowledgment response
/// @return JSON subscription message string.
/// @note Returns a message with an empty args array for an empty inst_ids span.
// NOTE: subscribe_message() pattern is similar across exchange adapters (binance/okx/bybit)
// but JSON payload format differs per exchange, making a shared abstraction impractical.
[[nodiscard]] inline std::string
subscribe_message(std::string_view channel,
                  std::span<const std::string_view> inst_ids,
                  int id = 1) noexcept {
    std::string result = R"({"op":"subscribe","args":[)";
    for (size_t i = 0; i < inst_ids.size(); ++i) {
        if (i > 0) result.push_back(',');
        result.append(R"({"channel":")");
        result.append(channel);
        result.append(R"(","instId":")");
        result.append(inst_ids[i]);
        result.append(R"("})");
    }
    result.append(R"(],"id":")");
    result.append(std::to_string(id));
    result.append(R"("})");
    SPDLOG_LOGGER_DEBUG(detail::okx_logger(),"okx::subscribe_message: channel=\"{}\" inst_ids={} id={}",
                 channel, inst_ids.size(), id);
    return result;
}

/// @brief Build an OKX WebSocket unsubscribe message.
///
/// Produces a JSON message conforming to OKX's WebSocket API:
///   {"op":"unsubscribe","args":[{"channel":"bbo-tbt","instId":"BTC-USDT"},...],"id":"N"}
///
/// @param channel   Channel name (e.g., "bbo-tbt", "trades", "books5")
/// @param inst_ids  Instruments to unsubscribe
/// @param id        Request ID for matching the server's acknowledgment response
/// @return JSON unsubscription message string.
/// @note Returns a message with an empty args array for an empty inst_ids span.
[[nodiscard]] inline std::string
unsubscribe_message(std::string_view channel,
                    std::span<const std::string_view> inst_ids,
                    int id = 2) noexcept {
    std::string result = R"({"op":"unsubscribe","args":[)";
    for (size_t i = 0; i < inst_ids.size(); ++i) {
        if (i > 0) result.push_back(',');
        result.append(R"({"channel":")");
        result.append(channel);
        result.append(R"(","instId":")");
        result.append(inst_ids[i]);
        result.append(R"("})");
    }
    result.append(R"(],"id":")");
    result.append(std::to_string(id));
    result.append(R"("})");
    SPDLOG_LOGGER_DEBUG(detail::okx_logger(),"okx::unsubscribe_message: channel=\"{}\" inst_ids={} id={}",
                 channel, inst_ids.size(), id);
    return result;
}

} // namespace eph::json::okx
