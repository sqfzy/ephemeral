#pragma once

/// @file tcp_state.hpp
/// TCP connection state enum (RFC 793) for the `eph::net` concept layer.
///
/// The actual `TcpState` enum definition lives in `eph/core/tcp_state.hpp`
/// (shared with `eph/core/tcp_concept.hpp` to avoid an ODR conflict when
/// both headers end up in the same translation unit). This file is a thin
/// re-export so that `#include "eph/net/tcp_state.hpp"` users continue to
/// compile unchanged.

#include "eph/core/tcp_state.hpp"
