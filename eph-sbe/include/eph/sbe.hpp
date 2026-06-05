#pragma once

/// @file sbe.hpp
/// Aggregation header for eph-sbe — zero-copy Simple Binary Encoding decode.
///
/// Include this to pull in the schema-independent SBE core (header parsing,
/// little-endian primitives, repeating-group dimensions, template-id dispatch)
/// plus the bundled Binance spot schema accessors. See the per-header docs and
/// eph-sbe/README.md for usage.

#include "eph/sbe/byte_order.hpp"
#include "eph/sbe/errors.hpp"
#include "eph/sbe/message_header.hpp"
#include "eph/sbe/parser.hpp"

// Binance spot SBE schema 3:2 accessors.
#include "eph/sbe/binance/book_ticker.hpp"
#include "eph/sbe/binance/schema.hpp"
