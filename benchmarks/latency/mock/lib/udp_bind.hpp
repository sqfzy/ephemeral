/// @file mock/lib/udp_bind.hpp
/// UDP socket bootstrap (bind only — UDP has no listen).
#pragma once

#include <cerrno>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace bench::mock {

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

} // namespace bench::mock
