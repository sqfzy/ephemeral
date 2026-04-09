/// @file tcp/client.hpp
/// Tiny POSIX TCP client used by `bench.cpp` (both kernel and DPDK builds).
///
/// The DPDK build path (`EPH_USE_DPDK` defined) currently still uses POSIX
/// sockets and emits a startup warning to stderr. A real DPDK transport
/// adapter is a non-trivial follow-up — see the file-header comment in
/// bench.cpp. The bench_*_dpdk targets exist now so the build matrix is
/// complete and so the script wiring can be developed in stage 5.
///
/// API: connect_tcp(ip, port) → fd; send_all/recv_exact wrap blocking I/O
/// with EINTR handling. The bench scenario builds the binary 24-byte TSC
/// header inside the payload buffer just before send.
#pragma once

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace bench::tcp {

[[nodiscard]] inline std::expected<int, std::string>
connect_tcp(std::string_view ip, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return std::unexpected(std::string("socket: ") + std::strerror(errno));

    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    std::string ip_str(ip);
    if (::inet_pton(AF_INET, ip_str.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return std::unexpected("invalid server ip: " + ip_str);
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::string e = std::string("connect: ") + std::strerror(errno);
        ::close(fd);
        return std::unexpected(std::move(e));
    }
    return fd;
}

inline bool send_all(int fd, const void* data, size_t len) noexcept {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, static_cast<const uint8_t*>(data) + sent,
                           len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

inline bool recv_exact(int fd, void* buf, size_t len) noexcept {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, static_cast<uint8_t*>(buf) + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false; // peer closed
        got += static_cast<size_t>(n);
    }
    return true;
}

} // namespace bench::tcp
