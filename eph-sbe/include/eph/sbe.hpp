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

// Binance spot SBE schema 3:2 accessors (WS API + market data + user data).
#include "eph/sbe/binance/book_ticker.hpp"
#include "eph/sbe/binance/cancel_order.hpp"
#include "eph/sbe/binance/error_response.hpp"
#include "eph/sbe/binance/execution_report.hpp"
#include "eph/sbe/binance/new_order_ack.hpp"
#include "eph/sbe/binance/schema.hpp"
#include "eph/sbe/binance/session_logon.hpp"
#include "eph/sbe/binance/web_socket_response.hpp"

// Binance spot_stream SBE schema 1:0 accessors (real-time market data streams).
#include "eph/sbe/binance/stream/best_bid_ask.hpp"
#include "eph/sbe/binance/stream/schema.hpp"
