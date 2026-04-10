#pragma once

/// @file net_header.hpp
/// Convenience umbrella header — includes all packet sub-modules.
///
/// For faster compilation, prefer including specific headers:
///   - packet_core.hpp     — constants, byte order, checksum, ConnectionTuple
///   - packet_parse.hpp    — ParsedIpHeader, ParsedPacket, ParsedUdpPacket, parse functions
///   - packet_template.hpp — PacketTemplate, UdpPacketTemplate

#include "eph/dpdk/packet_core.hpp"
#include "eph/dpdk/packet_parse.hpp"
#include "eph/dpdk/packet_template.hpp"
