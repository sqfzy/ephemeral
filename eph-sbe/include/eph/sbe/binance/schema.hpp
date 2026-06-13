#pragma once

/// @file schema.hpp
/// Binance spot SBE schema constants (schema id=3, version=2).
///
/// Authoritative source: eph-sbe/schemas/spot_3_2.xml (vendored from
/// github.com/binance/binance-spot-api-docs). Template ids, the schema
/// id/version, and the optional-field null sentinel are pinned here so the
/// per-message accessors and the dispatch wiring share one definition.

#include <cstdint>
#include <limits>

#include "eph/sbe/parser.hpp"

namespace eph::sbe::binance {

/// @brief Binance spot SBE schema id (the `id` attribute of spot_3_2.xml).
inline constexpr uint16_t kSchemaId = 3;

/// @brief Binance spot SBE schema version this module decodes against.
inline constexpr uint16_t kSchemaVersion = 2;

/// @brief Template ids for the Binance spot SBE messages decoded here.
namespace tid {
/// @brief WebSocketResponse — WS API result envelope (`<sbe:message id="50">`).
inline constexpr uint16_t kWebSocketResponse = 50;
/// @brief WebSocketSessionLogonResponse (`<sbe:message id="51">`).
inline constexpr uint16_t kSessionLogon = 51;
/// @brief ErrorResponse (`<sbe:message id="100">`).
inline constexpr uint16_t kErrorResponse = 100;
/// @brief BookTickerResponse message id (`<sbe:message id="212">`).
inline constexpr uint16_t kBookTicker = 212;
/// @brief NewOrderAckResponse — order.place ACK (`<sbe:message id="300">`).
inline constexpr uint16_t kNewOrderAck = 300;
/// @brief CancelOrderResponse — order.cancel ACK (`<sbe:message id="305">`).
inline constexpr uint16_t kCancelOrder = 305;
/// @brief ExecutionReportEvent — user-data fill/status push (`<sbe:message id="603">`).
inline constexpr uint16_t kExecutionReport = 603;
} // namespace tid

/// @brief SBE null sentinel for an optional mantissa64 (int64) field.
///
/// Per the FIX SBE standard, the null value for a signed N-bit integer is
/// -2^(N-1); spot_3_2.xml does not override nullValue on the optional
/// bidPrice/askPrice fields, so the default INT64_MIN applies.
inline constexpr int64_t kNullMantissa = std::numeric_limits<int64_t>::min();

/// @brief Whether a parsed message matches the Binance spot schema this module
///        was built against (id=3, version=2).
///
/// Accessor byte offsets are only guaranteed for this exact schema/version; a
/// mismatch means Binance has revised the layout and the bundled
/// schemas/spot_3_2.xml (and offsets) must be refreshed.
[[nodiscard]] inline bool is_supported(const MessageView& view) noexcept {
    return view.schema_id == kSchemaId && view.version == kSchemaVersion;
}

/// @brief Binance spot `orderStatus` enum values (spot_3_2.xml `<enum
///        name="orderStatus" encodingType="uint8">`). Returned as the raw uint8
///        by the order / execution-report accessors; this names the values so
///        consumers can map venue status → their own domain enum.
enum class OrderStatus : uint8_t {
    New             = 0,
    PartiallyFilled = 1,
    Filled          = 2,
    Canceled        = 3,
    PendingCancel   = 4,
    Rejected        = 5,
    Expired         = 6,
    ExpiredInMatch  = 9,
    PendingNew      = 11,
    Unknown         = 253,
    NonRepresentable = 254,
};

} // namespace eph::sbe::binance
