#pragma once

/// @file tcp_state.hpp
/// TCP connection state enum (RFC 793) for the v3.3 `eph::net` concept layer.
///
/// Part of Phase 2 of the v3.3 refactor. The actual `TcpState` enum
/// definition lives in `eph/core/tcp_state.hpp` (since Phase 4 needed
/// it shared with `eph/core/tcp_concept.hpp` to avoid an ODR conflict
/// when both headers end up in the same translation unit). This file
/// is now a thin re-export so that legacy `#include "eph/net/tcp_state.hpp"`
/// users continue to compile unchanged.

#include "eph/core/tcp_state.hpp"
