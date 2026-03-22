#pragma once

/// @file fix.hpp
/// FIX protocol module -- aggregation header.
///
/// Provides tag-value parsing and building for the FIX protocol (no session
/// management). Zero-copy, header-only, std::expected error handling.

#include "eph/fix/tags.hpp"
#include "eph/fix/parser.hpp"
#include "eph/fix/builder.hpp"
#include "eph/fix/framer.hpp"
