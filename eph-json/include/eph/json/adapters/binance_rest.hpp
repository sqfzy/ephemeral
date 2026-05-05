#pragma once

/// @file adapters/binance_rest.hpp
/// Typed response parsers for Binance public REST endpoints.
///
/// Provides `parse_depth_response`, `parse_server_time_response`, and
/// `parse_depth_levels` — pure functions that turn raw JSON response bodies
/// into typed values. Transport (HTTPS GET) is the caller's responsibility;
/// The legacy `BinanceRestClient` wrapper was removed along with
/// `eph-transport`. Callers should perform the GET via their own
/// `eph-net-kernel` + `eph-codec` stack and pass the response body
/// into the parsers here.

#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "eph/core/parse_number.hpp"
#include "eph/json/adapters/binance_depth_types.hpp"
#include "eph/json/parser.hpp"

namespace eph::json::binance {

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------

namespace detail {

/// @brief Get or create the shared spdlog logger for the Binance REST adapter.
/// @return Non-owning pointer to the "json.binance_rest" logger instance.
///
/// Thread-safe: uses Meyers-singleton initialization.
inline spdlog::logger* binance_rest_logger() {
    static auto l = [] {
        auto lg = spdlog::get("json.binance_rest");
        if (!lg) {
            try { lg = spdlog::stdout_color_mt("json.binance_rest"); }
            catch (const spdlog::spdlog_ex&) { lg = spdlog::get("json.binance_rest"); }
        }
        if (!lg) lg = spdlog::default_logger();
        return lg;
    }();
    return l.get();
}

// ---------------------------------------------------------------------------
// Depth array parser
// ---------------------------------------------------------------------------

/// @brief Parse a double from a string_view.
/// @param sv  String representation of a decimal number.
/// @return Parsed double, or nullopt on failure.
inline std::optional<double> parse_double(std::string_view sv) noexcept {
    return eph::core::parse_number(sv);
}

/// Parse Binance depth level arrays: [["price","qty"],["price","qty"],...].
///
/// The JSON parser stores arrays as opaque string_views. This helper parses
/// the specific structure Binance uses for bid/ask arrays. Each inner element
/// is a 2-element array of quoted strings (price, quantity).
///
/// @param raw  The raw array string from JsonView::get(), e.g. [["0.1","1.0"],...]
/// @return Vector of DepthLevel on success, error string on parse failure
[[nodiscard]] inline std::expected<std::vector<DepthLevel>, std::string>
parse_depth_levels(std::string_view raw) noexcept {
    auto* log = binance_rest_logger();

    std::vector<DepthLevel> levels;

    if (raw.empty()) {
        return levels;  // empty is fine — no levels
    }

    const char* p = raw.data();
    const char* end = p + raw.size();

    // Expect outer '['
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
    if (p >= end || *p != '[') {
        SPDLOG_LOGGER_WARN(log, "parse_depth_levels: expected '[', got '{}'",
                           p < end ? std::string(1, *p) : "EOF");
        return std::unexpected(std::string("Expected '[' at start of depth levels"));
    }
    ++p;

    while (p < end) {
        // Skip whitespace
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',')) ++p;
        if (p >= end) break;

        // End of outer array
        if (*p == ']') break;

        // Expect inner '['
        if (*p != '[') {
            SPDLOG_LOGGER_WARN(log,
                "parse_depth_levels: expected inner '[', got '{}' at offset {}",
                *p, static_cast<size_t>(p - raw.data()));
            return std::unexpected(std::format(
                "Expected inner '[' at offset {}", p - raw.data()));
        }
        ++p;

        // Parse first quoted string (price)
        while (p < end && *p != '"') ++p;
        if (p >= end) {
            return std::unexpected(std::string("Unterminated price string in depth level"));
        }
        ++p; // skip opening quote
        const char* price_start = p;
        while (p < end && *p != '"') ++p;
        if (p >= end) {
            return std::unexpected(std::string("Unterminated price string in depth level"));
        }
        std::string_view price_sv(price_start, static_cast<size_t>(p - price_start));
        ++p; // skip closing quote

        // Skip comma between price and qty
        while (p < end && (*p == ' ' || *p == ',')) ++p;

        // Parse second quoted string (qty)
        while (p < end && *p != '"') ++p;
        if (p >= end) {
            return std::unexpected(std::string("Unterminated qty string in depth level"));
        }
        ++p; // skip opening quote
        const char* qty_start = p;
        while (p < end && *p != '"') ++p;
        if (p >= end) {
            return std::unexpected(std::string("Unterminated qty string in depth level"));
        }
        std::string_view qty_sv(qty_start, static_cast<size_t>(p - qty_start));
        ++p; // skip closing quote

        // Skip to closing ']' of inner array. Must observe ']' before
        // EOF — otherwise the response was truncated mid-element and
        // we'd silently emit a level that was never fully on the wire
        // (network clip / half-close during recv). Surface the truncation
        // as an error so the caller refuses the entire response rather
        // than mixing phantom levels into the book.
        while (p < end && *p != ']') ++p;
        if (p >= end) {
            SPDLOG_LOGGER_WARN(log,
                "parse_depth_levels: truncated input — inner array missing "
                "closing ']' (raw_size={}, parsed_levels_so_far={})",
                raw.size(), levels.size());
            return std::unexpected(std::string(
                "Truncated depth response: inner array missing closing ']'"));
        }
        ++p; // skip ']'

        // Convert strings to doubles
        auto price = parse_double(price_sv);
        auto qty = parse_double(qty_sv);
        if (!price || !qty) {
            SPDLOG_LOGGER_WARN(log,
                "parse_depth_levels: invalid number in level: price='{}', qty='{}'",
                price_sv, qty_sv);
            return std::unexpected(std::format(
                "Invalid number in depth level: price='{}', qty='{}'",
                price_sv, qty_sv));
        }

        levels.push_back(DepthLevel{*price, *qty});
    }

    // After exiting the per-element loop, p must point at the outer
    // array's closing ']' (we leave it unconsumed so a future caller
    // could chain). If p reached `end` without finding ']', the outer
    // array was also truncated — same silent-corruption surface as
    // inner truncation, surface it explicitly. We tolerate "trailing
    // whitespace before ']' is missing" only because the caller passes
    // us a string_view that the JSON parser already trimmed.
    if (p >= end) {
        SPDLOG_LOGGER_WARN(log,
            "parse_depth_levels: truncated input — outer array missing "
            "closing ']' (raw_size={}, parsed_levels={})",
            raw.size(), levels.size());
        return std::unexpected(std::string(
            "Truncated depth response: outer array missing closing ']'"));
    }

    SPDLOG_LOGGER_TRACE(log, "parse_depth_levels: parsed {} levels", levels.size());
    return levels;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Response parsers (public for testability)
// ---------------------------------------------------------------------------

/// Parse a Binance /api/v3/depth JSON response body into a DepthSnapshot.
///
/// Expected format:
///   {"lastUpdateId":123,"bids":[["price","qty"],...],"asks":[["price","qty"],...]}
///
/// @param body  Raw JSON response body
/// @return DepthSnapshot on success, error string on failure
[[nodiscard]] inline std::expected<DepthSnapshot, std::string>
parse_depth_response(std::string_view body) noexcept {
    auto* log = detail::binance_rest_logger();

    SPDLOG_LOGGER_DEBUG(log, "parse_depth_response: parsing {} bytes", body.size());

    auto result = parse(
        reinterpret_cast<const uint8_t*>(body.data()), body.size());
    if (!result) {
        SPDLOG_LOGGER_WARN(log,
            "parse_depth_response: JSON parse failed: {}", parse_error_name(result.error()));
        return std::unexpected(std::format(
            "JSON parse failed: {}", parse_error_name(result.error())));
    }

    auto& json = *result;

    // Extract lastUpdateId
    auto update_id = json.get_int("lastUpdateId");
    if (!update_id) {
        SPDLOG_LOGGER_WARN(log,
            "parse_depth_response: missing or invalid 'lastUpdateId'");
        return std::unexpected(std::string("Missing or invalid 'lastUpdateId' field"));
    }

    // Extract bids array (opaque string from parser)
    auto bids_raw = json.get("bids");
    if (bids_raw.empty()) {
        SPDLOG_LOGGER_WARN(log, "parse_depth_response: missing 'bids' field");
        return std::unexpected(std::string("Missing 'bids' field"));
    }

    auto bids = detail::parse_depth_levels(bids_raw);
    if (!bids) {
        SPDLOG_LOGGER_WARN(log,
            "parse_depth_response: failed to parse bids: {}", bids.error());
        return std::unexpected(std::format("Failed to parse bids: {}", bids.error()));
    }

    // Extract asks array (opaque string from parser)
    auto asks_raw = json.get("asks");
    if (asks_raw.empty()) {
        SPDLOG_LOGGER_WARN(log, "parse_depth_response: missing 'asks' field");
        return std::unexpected(std::string("Missing 'asks' field"));
    }

    auto asks = detail::parse_depth_levels(asks_raw);
    if (!asks) {
        SPDLOG_LOGGER_WARN(log,
            "parse_depth_response: failed to parse asks: {}", asks.error());
        return std::unexpected(std::format("Failed to parse asks: {}", asks.error()));
    }

    DepthSnapshot snap;
    snap.last_update_id = *update_id;
    snap.bids = std::move(*bids);
    snap.asks = std::move(*asks);

    SPDLOG_LOGGER_DEBUG(log,
        "parse_depth_response: lastUpdateId={}, bids={}, asks={}",
        snap.last_update_id, snap.bids.size(), snap.asks.size());

    return snap;
}

/// Parse a Binance /api/v3/time JSON response body into a ServerTime.
///
/// Expected format: {"serverTime":1711612345678}
///
/// @param body  Raw JSON response body
/// @return ServerTime on success, error string on failure
[[nodiscard]] inline std::expected<ServerTime, std::string>
parse_server_time_response(std::string_view body) noexcept {
    auto* log = detail::binance_rest_logger();

    SPDLOG_LOGGER_DEBUG(log, "parse_server_time_response: parsing {} bytes", body.size());

    auto result = parse(
        reinterpret_cast<const uint8_t*>(body.data()), body.size());
    if (!result) {
        SPDLOG_LOGGER_WARN(log,
            "parse_server_time_response: JSON parse failed: {}",
            parse_error_name(result.error()));
        return std::unexpected(std::format(
            "JSON parse failed: {}", parse_error_name(result.error())));
    }

    auto& json = *result;
    auto server_time = json.get_int("serverTime");
    if (!server_time) {
        SPDLOG_LOGGER_WARN(log,
            "parse_server_time_response: missing or invalid 'serverTime'");
        return std::unexpected(std::string("Missing or invalid 'serverTime' field"));
    }

    SPDLOG_LOGGER_DEBUG(log, "parse_server_time_response: serverTime={}ms", *server_time);

    return ServerTime{*server_time};
}


} // namespace eph::json::binance
