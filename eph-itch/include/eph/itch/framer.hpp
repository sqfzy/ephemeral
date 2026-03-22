#pragma once

/// @file framer.hpp
/// ITCH message framer — thin alias over LengthPrefixFramer.
///
/// ITCH uses 2-byte big-endian length-prefix framing (MoldUDP64 / SoupBinTCP).
/// LengthPrefixFramer already implements exactly that wire format, so ItchFramer
/// is a direct type alias.

#include "eph/net/length_prefix_framer.hpp"

namespace eph::itch {

/// ITCH framer: 2-byte big-endian length prefix, same as MoldUDP64.
using ItchFramer = eph::net::LengthPrefixFramer;

} // namespace eph::itch
