/// @file mock/lib/busy_poll.hpp
/// Generic accept + single-connection busy-poll loop skeleton.
///
/// `accept_one(listen_fd)` blocks (with poll-driven shutdown polling)
/// until a client connects. The bench mocks each handle exactly one
/// client at a time — when the client disconnects, we go back to accept.
///
/// `serve_busy_poll(client_fd, on_iteration)` runs the per-iteration
/// callback until shutdown. The callback is responsible for any
/// `recv(MSG_DONTWAIT)` / `send` work and returning false on disconnect.
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

/// Run `iter(fd)` repeatedly until shutdown or `iter` returns false.
/// `iter` is the busy-poll body — it should do any non-blocking RX +
/// any time-driven work + any TX. Returning false signals "client
/// disconnected, close fd and go back to accept".
template <typename Iter>
inline void serve_busy_poll(int fd, std::atomic<bool>& running, Iter&& iter) {
    while (running.load(std::memory_order_acquire)) {
        if (!iter(fd)) break;
    }
}

} // namespace bench::mock
