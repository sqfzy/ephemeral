#pragma once

/// @file book.hpp
/// Convenience header — includes the eph-book public API.
///
/// Provides a fixed-size, zero-allocation L2 order book (ArrayBook) suitable
/// for both explicit price-level feeds (crypto exchanges) and implicit
/// order-level feeds (ITCH) after external aggregation.

#include "eph/book/array_book.hpp"
