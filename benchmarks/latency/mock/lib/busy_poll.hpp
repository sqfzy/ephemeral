/// @file mock/lib/busy_poll.hpp
/// Single-connection accept helper shared by the TCP/WS mocks.
///
/// `accept_one(listen_fd)` blocks until a client connects, polling
/// `running` every 100 ms so SIGTERM can interrupt the wait. The caller
/// drives the per-connection receive loop itself — we do not abstract
/// that because the three mocks' hot loops look nothing alike.
#pragma once

#include <atomic>
#include <cerrno>
#include <cstring>
#include <expected>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

namespace bench::mock {

/// Accept one client on `listen_fd`, polling `running` between each
/// 100 ms wait so SIGTERM can interrupt the loop. Returns -1 if shutdown
/// was requested before a client arrived.
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
        // Apply NODELAY to the new client socket too.
        int one = 1;
        ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        char ip[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &caddr.sin_addr, ip, sizeof(ip));
        spdlog::info("accept: client {}:{}", ip, ntohs(caddr.sin_port));
        return cfd;
    }
    return -1;
}

} // namespace bench::mock
