/// @file core/socket_bind.hpp
/// POSIX TCP/UDP socket bootstrap helpers for mock servers.
///
/// Three functions cover every mock's "open socket + start serving" path:
///   - `tcp_bind_listen(ip, port, backlog)` — TCP listening socket
///     with SO_REUSEADDR + TCP_NODELAY already set
///   - `udp_bind(ip, port)` — UDP socket with SO_REUSEADDR
///   - `accept_one(listen_fd, running)` — blocking accept with a 100 ms
///     poll for `running` so SIGTERM can unblock the loop
#pragma once

#include <atomic>
#include <cerrno>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

namespace bench {

/// Open a TCP listening socket bound to `ip:port` with SO_REUSEADDR and
/// TCP_NODELAY. `backlog` defaults to 1 — mocks only ever serve one
/// client at a time.
[[nodiscard]] inline std::expected<int, std::string>
tcp_bind_listen(std::string_view ip, uint16_t port, int backlog = 1) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return std::unexpected(std::string("socket() failed: ") +
                               std::strerror(errno));
    }

    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    std::string ip_str(ip);
    if (::inet_pton(AF_INET, ip_str.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return std::unexpected("invalid bind IP \"" + ip_str + "\"");
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::string err = std::string("bind(") + ip_str + ":" +
                          std::to_string(port) + ") failed: " +
                          std::strerror(errno);
        ::close(fd);
        return std::unexpected(std::move(err));
    }
    if (::listen(fd, backlog) < 0) {
        std::string err = std::string("listen() failed: ") + std::strerror(errno);
        ::close(fd);
        return std::unexpected(std::move(err));
    }
    return fd;
}

/// Open a UDP socket bound to `ip:port` with SO_REUSEADDR.
[[nodiscard]] inline std::expected<int, std::string>
udp_bind(std::string_view ip, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return std::unexpected(std::string("socket() failed: ") +
                               std::strerror(errno));
    }

    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    std::string ip_str(ip);
    if (::inet_pton(AF_INET, ip_str.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return std::unexpected("invalid bind IP \"" + ip_str + "\"");
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::string err = std::string("bind(") + ip_str + ":" +
                          std::to_string(port) + ") failed: " +
                          std::strerror(errno);
        ::close(fd);
        return std::unexpected(std::move(err));
    }
    return fd;
}

/// Block on `accept()` until a client connects, polling `running` every
/// 100 ms so the caller can be woken by SIGTERM. Returns -1 if shutdown
/// was requested before a client arrived.
///
/// TCP_NODELAY is applied to the new client socket too, since low-latency
/// echo is the whole point.
[[nodiscard]] inline std::expected<int, std::string>
accept_one(int listen_fd, std::atomic<bool>& running) {
    while (running.load(std::memory_order_acquire)) {
        pollfd p{}; p.fd = listen_fd; p.events = POLLIN;
        int rv = ::poll(&p, 1, 100);
        if (rv < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(
                std::string("poll(accept) failed: ") + std::strerror(errno));
        }
        if (rv == 0) continue;
        sockaddr_in caddr{};
        socklen_t clen = sizeof(caddr);
        int cfd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&caddr), &clen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(
                std::string("accept() failed: ") + std::strerror(errno));
        }
        int one = 1;
        ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        char ip[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &caddr.sin_addr, ip, sizeof(ip));
        spdlog::info("accept: client {}:{}", ip, ntohs(caddr.sin_port));
        return cfd;
    }
    return -1;
}

} // namespace bench
