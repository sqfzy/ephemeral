#pragma once

/// @file book.hpp
/// Convenience header — includes the eph-book public API.
///
/// Provides order book implementations for HFT:
///   - ArrayBook: fixed-size, zero-allocation L2 book (5-20 levels)
///   - MapBook:   dynamic-depth L3 book (1000+ levels, std::map-backed)
///
/// Both support explicit price-level feeds (crypto exchanges) and implicit
/// order-level feeds (ITCH) after external aggregation.

/// @note This header pulls in both ArrayBook and MapBook.  If you only need
///       one implementation, prefer including the specific header to reduce
///       compile times.

#include "eph/book/array_book.hpp"
#include "eph/book/map_book.hpp"
