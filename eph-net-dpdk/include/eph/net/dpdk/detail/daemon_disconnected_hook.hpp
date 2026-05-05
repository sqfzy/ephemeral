#pragma once

/// @file detail/daemon_disconnected_hook.hpp
/// **Compatibility shim** — the InFlightStatus three-state semantics
/// were generalised to a backend-neutral header at
/// `eph/net/in_flight_status.hpp` (kernel backend uses the same
/// thread_local storage now too — see commit message for details).
///
/// This header re-exports the original DPDK-namespaced symbols so
/// existing call sites (DpdkTcpStream / DpdkUdpSocket / tests) keep
/// working without a recompilation cascade. New code should prefer
/// `eph::net::InFlightStatus` directly.
///
/// Original motivation (T1.1+T1.2 in the 2026-05-05 action list):
/// the DPDK daemon dying mid-send forces apps to know whether bytes
/// hit the wire. Generalised because the kernel backend has the
/// exact same in-flight-bytes question on `EPIPE` / `ECONNRESET` /
/// partial send.

#include "eph/net/in_flight_status.hpp"

namespace eph::net::dpdk::detail {

/// @brief Alias of `eph::net::InFlightStatus` (backend-neutral).
///
/// Kept under the original DPDK namespace so existing call sites
/// (DpdkTcpStream::check_post_burst_, etc.) compile unchanged. The
/// underlying enum + values are identical to the shared definition.
using InFlightStatus = ::eph::net::InFlightStatus;

/// @brief Alias of `eph::net::InFlightDetail` (backend-neutral).
using DaemonDisconnectedDetail = ::eph::net::InFlightDetail;

[[nodiscard]] inline const char*
to_string(InFlightStatus s) noexcept {
    return ::eph::net::to_string(s);
}

/// @brief Read the most-recent in-flight detail. Aliases the shared
/// thread_local accessor — DPDK call sites and kernel call sites
/// share the same storage on a given thread (a thread that issues
/// sends to multiple backends sees the most-recent detail
/// regardless of which backend produced it).
[[nodiscard]] inline ::eph::net::InFlightDetail&
last_daemon_disconnected_detail() noexcept {
    return ::eph::net::last_in_flight_detail();
}

inline void
set_daemon_disconnected_detail(InFlightStatus status,
                               size_t bytes_observed,
                               size_t bytes_confirmed,
                               const char* phase) noexcept {
    ::eph::net::set_in_flight_detail(status, bytes_observed,
                                     bytes_confirmed, phase);
}

inline void
clear_daemon_disconnected_detail() noexcept {
    ::eph::net::clear_in_flight_detail();
}

}  // namespace eph::net::dpdk::detail
