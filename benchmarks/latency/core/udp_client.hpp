/// @file core/udp_client.hpp
/// Tiny POSIX UDP client used by the exchange/md_udp scenario.
///
/// `lat_udp.cpp` inlines its own UDP socket setup; this header exists so
/// the exchange/md_udp scenario can share the same low-level helpers
/// without depending on the lat_udp.cpp translation unit.
#pragma once

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace bench::udp_client {

/// A connected UDP client: one socket fd plus the pre-built peer
/// `sockaddr_in` that `sendto_one` feeds into every send. Kept as POD so
/// hot-path callers can stamp the struct on the stack without heap alloc.
struct Endpoint {
    int fd = -1;
    sockaddr_in peer{};
};

/// Open a UDP client socket connected (conceptually — we use `sendto`,
/// not `connect`) to `server_ip:port`. Installs a 100 ms recv timeout so a
/// single dropped datagram does not stall the measurement loop forever.
/// Returns the endpoint on success or an error string on socket / parse
/// failure. Caller must `close_endpoint()` the result.
[[nodiscard]] inline std::expected<Endpoint, std::string>
open_udp(std::string_view server_ip, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return std::unexpected(std::string("socket: ") + std::strerror(errno));

    timeval tv{};
    tv.tv_usec = 100'000;  // 100 ms recv timeout — single drops shouldn't stall
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    Endpoint ep{};
    ep.fd = fd;
    ep.peer.sin_family = AF_INET;
    ep.peer.sin_port = htons(port);
    std::string ip_str(server_ip);
    if (::inet_pton(AF_INET, ip_str.c_str(), &ep.peer.sin_addr) != 1) {
        ::close(fd);
        return std::unexpected("invalid server ip: " + ip_str);
    }
    return ep;
}

/// Send one UDP datagram to the endpoint's peer. Returns true on success.
/// A partial send is impossible for UDP — the kernel either takes the
/// whole datagram or rejects it.
inline bool sendto_one(const Endpoint& ep, const void* data, size_t len) noexcept {
    ssize_t n = ::sendto(ep.fd, data, len, 0,
                         reinterpret_cast<const sockaddr*>(&ep.peer),
                         sizeof(ep.peer));
    return n >= 0;
}

/// Receive one UDP datagram into `buf[0..cap)`. Returns the number of
/// bytes received, or -1 on error / timeout (`errno == EAGAIN` after the
/// 100 ms recv timeout installed in `open_udp`).
inline ssize_t recvfrom_one(const Endpoint& ep, void* buf, size_t cap) noexcept {
    return ::recvfrom(ep.fd, buf, cap, 0, nullptr, nullptr);
}

/// Close the endpoint's socket and clear the fd. Idempotent.
inline void close_endpoint(Endpoint& ep) noexcept {
    if (ep.fd >= 0) { ::close(ep.fd); ep.fd = -1; }
}

} // namespace bench::udp_client
