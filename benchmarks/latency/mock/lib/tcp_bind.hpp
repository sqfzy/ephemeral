/// @file mock/lib/tcp_bind.hpp
/// Listening TCP socket bootstrap with low-latency setsockopt presets.
///
/// Single function: open SOCK_STREAM, set SO_REUSEADDR + TCP_NODELAY,
/// bind to (ip, port), listen with backlog 1 (mocks only ever serve one
/// client at a time). Returns the fd or an error string.
#pragma once

#include <cerrno>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace bench::mock {

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

} // namespace bench::mock
