#pragma once

/// @file schema.hpp
/// Binance spot **market-data stream** SBE schema constants (schema id=1,
/// version=0, package `spot_stream`).
///
/// Distinct from the WS API spot schema (id=3, in ../schema.hpp): the real-time
/// market-data streams at `stream-sbe.binance.com` carry their own schema and
/// message set (TradesStreamEvent, BestBidAskStreamEvent, DepthSnapshot/Diff).
/// Authoritative source: eph-sbe/schemas/stream_1_0.xml (vendored from
/// github.com/binance/binance-spot-api-docs).

#include <cstdint>

#include "eph/sbe/parser.hpp"

namespace eph::sbe::binance::stream {

/// @brief Binance spot_stream SBE schema id.
inline constexpr uint16_t kSchemaId = 1;

/// @brief Binance spot_stream SBE schema version this module decodes against.
inline constexpr uint16_t kSchemaVersion = 0;

/// @brief Template ids for the spot_stream messages decoded here.
namespace tid {
/// @brief BestBidAskStreamEvent — real-time BBO (`<sbe:message id="10001">`).
inline constexpr uint16_t kBestBidAsk = 10001;
} // namespace tid

/// @brief Whether a parsed message matches the spot_stream schema this module
///        was built against (id=1, version=0). Accessor offsets are only
///        guaranteed for this exact schema/version.
[[nodiscard]] inline bool is_supported(const MessageView& view) noexcept {
    return view.schema_id == kSchemaId && view.version == kSchemaVersion;
}

} // namespace eph::sbe::binance::stream
