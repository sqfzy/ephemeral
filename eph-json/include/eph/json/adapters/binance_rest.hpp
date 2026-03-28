#pragma once

/// @file adapters/binance_rest.hpp
/// Typed Binance REST API client for orderbook snapshots and clock sync.
///
/// Combines eph::net::HttpClient with eph::json::parse to provide typed
/// access to Binance's public REST endpoints needed for HFT recovery:
///   - GET /api/v3/depth   — orderbook snapshot (post-reconnect recovery)
///   - GET /api/v3/time    — server time (clock drift validation)
///
/// Order placement uses the FIX API (eph-fix), not REST, so only read-only
/// public endpoints are provided here. No authentication required.
///
/// Usage:
///   auto client = eph::json::binance::BinanceRestClient({});
///   auto depth = client.get_depth("BTCUSDT", 20);
///   if (depth) {
///       for (auto& bid : depth->bids) { ... }
///   }

#include <charconv>
#include <cstdint>
#include <chrono>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/json/parser.hpp"
#include "eph/net/http_client.hpp"

namespace eph::json::binance {

// ---------------------------------------------------------------------------
// Response types
// ---------------------------------------------------------------------------

/// A single price/quantity level in the orderbook.
struct DepthLevel {
    double price;
    double qty;
};

/// Orderbook depth snapshot from GET /api/v3/depth.
struct DepthSnapshot {
    int64_t last_update_id = 0;
    std::vector<DepthLevel> bids;  ///< Sorted descending by price
    std::vector<DepthLevel> asks;  ///< Sorted ascending by price
};

/// Server time from GET /api/v3/time.
struct ServerTime {
    int64_t server_time_ms = 0;  ///< Milliseconds since epoch
};

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------

namespace detail {

inline spdlog::logger* binance_rest_logger() {
    static auto l = [] {
        auto lg = spdlog::get("json.binance_rest");
        if (!lg) lg = spdlog::stdout_color_mt("json.binance_rest");
        return lg;
    }();
    return l.get();
}

// ---------------------------------------------------------------------------
// Depth array parser
// ---------------------------------------------------------------------------

/// Parse a double from a string_view using fast charconv.
/// Returns nullopt on failure.
inline std::optional<double> parse_double(std::string_view sv) noexcept {
    if (sv.empty()) return std::nullopt;
    double val = 0.0;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
    if (ec != std::errc{} || ptr != sv.data() + sv.size()) return std::nullopt;
    return val;
}

/// Parse Binance depth level arrays: [["price","qty"],["price","qty"],...].
///
/// The JSON parser stores arrays as opaque string_views. This helper parses
/// the specific structure Binance uses for bid/ask arrays. Each inner element
/// is a 2-element array of quoted strings (price, quantity).
///
/// @param raw  The raw array string from JsonView::get(), e.g. [["0.1","1.0"],...]
/// @return Vector of DepthLevel on success, error string on parse failure
inline std::expected<std::vector<DepthLevel>, std::string>
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

        // Skip to closing ']' of inner array
        while (p < end && *p != ']') ++p;
        if (p < end) ++p; // skip ']'

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

// ---------------------------------------------------------------------------
// REST client
// ---------------------------------------------------------------------------

/// Configuration for BinanceRestClient.
/// Defined outside the class to allow aggregate initialization as default arg.
struct BinanceRestConfig {
    std::string host = "api.binance.com";
    uint16_t port = 443;
    std::chrono::milliseconds timeout{5000};
};

/// Typed Binance REST client for public read-only endpoints.
///
/// Uses eph::net::HttpClient for HTTPS transport and eph::json::parse for
/// response parsing. Designed for off-hot-path use: orderbook snapshot
/// recovery after WebSocket reconnect, clock drift validation.
///
/// No authentication — these are all public endpoints.
class BinanceRestClient {
public:
    using Config = BinanceRestConfig;

    explicit BinanceRestClient(Config config = {})
        : http_(eph::net::HttpClient::Config{
              .host = config.host,
              .port = config.port,
              .use_tls = true,
              .timeout = config.timeout,
          }) {
        SPDLOG_LOGGER_DEBUG(detail::binance_rest_logger(),
            "BinanceRestClient created: host={}, port={}, timeout={}ms",
            config.host, config.port, config.timeout.count());
    }

    /// Get orderbook depth snapshot.
    ///
    /// Calls GET /api/v3/depth?symbol=<symbol>&limit=<limit>.
    /// Valid limit values: 5, 10, 20, 50, 100, 500, 1000, 5000.
    ///
    /// @param symbol  Trading pair (e.g., "BTCUSDT")
    /// @param limit   Number of levels per side (default 20)
    /// @return DepthSnapshot on success, error string on failure
    [[nodiscard]] std::expected<DepthSnapshot, std::string>
    get_depth(std::string_view symbol, int limit = 20) noexcept {
        auto* log = detail::binance_rest_logger();

        SPDLOG_LOGGER_DEBUG(log, "get_depth: symbol={}, limit={}", symbol, limit);

        auto path = std::format("/api/v3/depth?symbol={}&limit={}", symbol, limit);
        auto resp = http_.get(path);
        if (!resp) {
            SPDLOG_LOGGER_ERROR(log,
                "get_depth: HTTP request failed: {}", resp.error());
            return std::unexpected(std::format(
                "HTTP request failed: {}", resp.error()));
        }

        if (resp->status_code != 200) {
            SPDLOG_LOGGER_WARN(log,
                "get_depth: HTTP {}: {}", resp->status_code, resp->body);
            return std::unexpected(std::format(
                "HTTP {}: {}", resp->status_code, resp->body));
        }

        return parse_depth_response(resp->body);
    }

    /// Get server time for clock drift validation.
    ///
    /// Calls GET /api/v3/time.
    ///
    /// @return ServerTime on success, error string on failure
    [[nodiscard]] std::expected<ServerTime, std::string>
    get_server_time() noexcept {
        auto* log = detail::binance_rest_logger();

        SPDLOG_LOGGER_DEBUG(log, "get_server_time: requesting");

        auto resp = http_.get("/api/v3/time");
        if (!resp) {
            SPDLOG_LOGGER_ERROR(log,
                "get_server_time: HTTP request failed: {}", resp.error());
            return std::unexpected(std::format(
                "HTTP request failed: {}", resp.error()));
        }

        if (resp->status_code != 200) {
            SPDLOG_LOGGER_WARN(log,
                "get_server_time: HTTP {}: {}", resp->status_code, resp->body);
            return std::unexpected(std::format(
                "HTTP {}: {}", resp->status_code, resp->body));
        }

        return parse_server_time_response(resp->body);
    }

private:
    eph::net::HttpClient http_;
};

} // namespace eph::json::binance
