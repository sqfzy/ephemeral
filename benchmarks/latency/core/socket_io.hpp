#pragma once

/// @file core/socket_io.hpp
/// Thin compat shim — the real implementation lives in
/// eph-net/include/eph/net/posix_io.hpp.
///
/// Bench code calls these as `bench::send_all_fd` / `bench::recv_exact_fd`
/// (the `_fd` suffix distinguishes them from any future TCP-session-aware
/// helpers); this shim aliases the canonical eph::net::posix names.
/// New consumers should #include "eph/net/posix_io.hpp" directly.

#include "eph/net/posix_io.hpp"

namespace bench {

inline bool send_all_fd(int fd, const void* data, size_t len) noexcept {
    return ::eph::net::posix::send_all(fd, data, len);
}

inline bool recv_exact_fd(int fd, void* buf, size_t len) noexcept {
    return ::eph::net::posix::recv_exact(fd, buf, len);
}

} // namespace bench
