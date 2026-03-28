#pragma once

/// @file itch.hpp
/// Convenience header — includes the eph-itch public API.
///
/// Provides zero-copy ITCH 5.0 message definitions, parser, framers,
/// and Nasdaq order entry protocols (OUCH 5.0, SoupBinTCP, MoldUDP64).

#include "eph/itch/messages.hpp"
#include "eph/itch/parser.hpp"
#include "eph/itch/framer.hpp"
#include "eph/itch/soupbintcp.hpp"
#include "eph/itch/moldudp64.hpp"
#include "eph/itch/ouch.hpp"
