#pragma once

/// @file json.hpp
/// @brief Aggregate header for the eph-json module.
///
/// Includes the core zero-copy JSON parser, the WebSocket pass-through
/// framer, and exchange-specific adapters. Consumers that only need a
/// subset should include the individual headers directly to minimize
/// compile-time dependencies.

#include "eph/json/framer.hpp"
#include "eph/json/parser.hpp"
#include "eph/json/adapters/binance.hpp"
