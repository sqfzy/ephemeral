#pragma once

/// @file core/socket_bind.hpp
/// Thin compat shim — the real implementation lives in
/// eph-net/include/eph/net/posix_listener.hpp.
///
/// Bench code uses unqualified `tcp_bind_listen` / `udp_bind` / `accept_one`
/// (via `using namespace bench`); this shim aliases the canonical
/// eph::net::posix symbols into the bench namespace to keep callers
/// unchanged.  New consumers should
/// #include "eph/net/posix_listener.hpp" directly.

#include "eph/net/posix_listener.hpp"

namespace bench {

using ::eph::net::posix::tcp_bind_listen;
using ::eph::net::posix::udp_bind;
using ::eph::net::posix::accept_one;

} // namespace bench
