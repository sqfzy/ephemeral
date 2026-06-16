#pragma once

/// @file posix_listener.hpp
/// Kernel-side TCP/UDP bind helpers and a poll-based accept loop.
///
/// These are the small ergonomic wrappers around socket()/bind()/listen()/
/// accept() that mock servers and test fixtures need. Distinct from
/// `eph::net::kernel::KernelTcpStream` (which is the connect-side,
/// `std::expected`-returning client used by production code): the
/// helpers here target the *server* side of in-process / in-host
/// kernel socket pairs.
///
/// Originally lived under benchmarks/latency/core/socket_bind.hpp;
/// promoted to eph-net so that test fixtures can use them without
/// reverse-including the bench tree.

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

#include "eph/core/log.hpp"

namespace eph::net::posix {

namespace detail {
inline spdlog::logger* posix_listener_logger() {
    static spdlog::logger* l = ::eph::log::get("net.posix_listener");
    return l;
}
} // namespace detail

/// Open a TCP listening socket bound to `ip:port` with SO_REUSEADDR and
/// TCP_NODELAY.  `backlog` defaults to 1 — most mocks only ever serve
/// one client at a time.
///
/// Observability: every error branch logs at WARN with errno-derived
/// context (the returned `unexpected` carries the same string for the
/// caller's own diagnostic chain). DEBUG entry/exit lines so a strace-
/// equivalent of the bind sequence is recoverable from logs alone.
[[nodiscard]] inline std::expected<int, std::string>
tcp_bind_listen(std::string_view ip, uint16_t port, int backlog = 1) {
    EPH_LOG_DEBUG(detail::posix_listener_logger(), "posix::tcp_bind_listen: enter ip={} port={} backlog={}",
                 ip, port, backlog);
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        EPH_LOG_WARN(detail::posix_listener_logger(), "posix::tcp_bind_listen: socket() failed errno={} ({})",
                    errno, std::strerror(errno));
        return std::unexpected(std::string("socket() failed: ") +
                               std::strerror(errno));
    }

    int one = 1;
    // SO_REUSEADDR / TCP_NODELAY both legitimately fail on some kernels
    // (e.g. seccomp-restricted sandboxes) without breaking bind+listen.
    // Demote to DEBUG instead of silently swallowing — the test fixture
    // still works, but a strange port-already-in-use error or tail-
    // latency surprise downstream now has a breadcrumb in the journal.
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
        EPH_LOG_DEBUG(detail::posix_listener_logger(), "posix::tcp_bind_listen: setsockopt(SO_REUSEADDR) failed "
                     "fd={} errno={} ({})", fd, errno, std::strerror(errno));
    }
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
        EPH_LOG_DEBUG(detail::posix_listener_logger(), "posix::tcp_bind_listen: setsockopt(TCP_NODELAY) failed "
                     "fd={} errno={} ({})", fd, errno, std::strerror(errno));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    std::string ip_str(ip);
    if (::inet_pton(AF_INET, ip_str.c_str(), &addr.sin_addr) != 1) {
        EPH_LOG_WARN(detail::posix_listener_logger(), "posix::tcp_bind_listen: invalid bind IP \"{}\"", ip_str);
        ::close(fd);
        return std::unexpected("invalid bind IP \"" + ip_str + "\"");
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::string err = std::string("bind(") + ip_str + ":" +
                          std::to_string(port) + ") failed: " +
                          std::strerror(errno);
        EPH_LOG_WARN(detail::posix_listener_logger(), "posix::tcp_bind_listen: bind({}:{}) failed errno={} ({})",
                    ip_str, port, errno, std::strerror(errno));
        ::close(fd);
        return std::unexpected(std::move(err));
    }
    if (::listen(fd, backlog) < 0) {
        std::string err = std::string("listen() failed: ") + std::strerror(errno);
        EPH_LOG_WARN(detail::posix_listener_logger(), "posix::tcp_bind_listen: listen(backlog={}) failed errno={} ({})",
                    backlog, errno, std::strerror(errno));
        ::close(fd);
        return std::unexpected(std::move(err));
    }
    EPH_LOG_DEBUG(detail::posix_listener_logger(), "posix::tcp_bind_listen: ok fd={} {}:{}", fd, ip_str, port);
    return fd;
}

/// Open a UDP socket bound to `ip:port` with SO_REUSEADDR.
[[nodiscard]] inline std::expected<int, std::string>
udp_bind(std::string_view ip, uint16_t port) {
    EPH_LOG_DEBUG(detail::posix_listener_logger(), "posix::udp_bind: enter ip={} port={}", ip, port);
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        EPH_LOG_WARN(detail::posix_listener_logger(), "posix::udp_bind: socket() failed errno={} ({})",
                    errno, std::strerror(errno));
        return std::unexpected(std::string("socket() failed: ") +
                               std::strerror(errno));
    }

    int one = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
        EPH_LOG_DEBUG(detail::posix_listener_logger(), "posix::udp_bind: setsockopt(SO_REUSEADDR) failed "
                     "fd={} errno={} ({})", fd, errno, std::strerror(errno));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    std::string ip_str(ip);
    if (::inet_pton(AF_INET, ip_str.c_str(), &addr.sin_addr) != 1) {
        EPH_LOG_WARN(detail::posix_listener_logger(), "posix::udp_bind: invalid bind IP \"{}\"", ip_str);
        ::close(fd);
        return std::unexpected("invalid bind IP \"" + ip_str + "\"");
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::string err = std::string("bind(") + ip_str + ":" +
                          std::to_string(port) + ") failed: " +
                          std::strerror(errno);
        EPH_LOG_WARN(detail::posix_listener_logger(), "posix::udp_bind: bind({}:{}) failed errno={} ({})",
                    ip_str, port, errno, std::strerror(errno));
        ::close(fd);
        return std::unexpected(std::move(err));
    }
    EPH_LOG_DEBUG(detail::posix_listener_logger(), "posix::udp_bind: ok fd={} {}:{}", fd, ip_str, port);
    return fd;
}

/// Block on `accept()` until a client connects, polling `running` every
/// 100 ms so the caller can be woken by SIGTERM.
///
/// Return contract:
///   - `expected{cfd}` with `cfd >= 0`  — valid client fd (caller owns it).
///   - `expected{-1}`                   — shutdown requested before any
///                                        client arrived (`running` flipped
///                                        to false). Caller MUST check for
///                                        this sentinel; treating `-1` as a
///                                        valid fd in subsequent recv()
///                                        calls would yield EBADF. The six
///                                        in-tree call sites under
///                                        `benchmarks/mockex/scenarios/*`
///                                        already check `*cfd_e < 0` and
///                                        break.
///   - `unexpected(msg)`                — system error (poll/accept failed
///                                        with a non-EINTR errno).
///
/// TCP_NODELAY is applied to the new client socket too, since low-latency
/// echo is the whole point.
///
/// Uses the SPDLOG_INFO macro (not the runtime `spdlog::info`) so the
/// "client connected" line compiles out cleanly when the active level
/// is WARN+ — consistent with the rest of the codebase per CLAUDE.md.
[[nodiscard]] inline std::expected<int, std::string>
accept_one(int listen_fd, std::atomic<bool>& running) {
    EPH_LOG_DEBUG(detail::posix_listener_logger(), "posix::accept_one: enter listen_fd={}", listen_fd);
    while (running.load(std::memory_order_acquire)) {
        pollfd p{}; p.fd = listen_fd; p.events = POLLIN;
        int rv = ::poll(&p, 1, 100);
        if (rv < 0) {
            if (errno == EINTR) continue;
            EPH_LOG_WARN(detail::posix_listener_logger(), "posix::accept_one: poll() failed listen_fd={} errno={} ({})",
                        listen_fd, errno, std::strerror(errno));
            return std::unexpected(
                std::string("poll(accept) failed: ") + std::strerror(errno));
        }
        if (rv == 0) continue;
        sockaddr_in caddr{};
        socklen_t clen = sizeof(caddr);
        int cfd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&caddr), &clen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            EPH_LOG_WARN(detail::posix_listener_logger(), "posix::accept_one: accept() failed listen_fd={} errno={} ({})",
                        listen_fd, errno, std::strerror(errno));
            return std::unexpected(
                std::string("accept() failed: ") + std::strerror(errno));
        }
        int one = 1;
        // TCP_NODELAY may legitimately fail on some kernels (rare) — log
        // at DEBUG instead of silently swallowing so a strange tail
        // latency in a downstream test has a breadcrumb.
        if (::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
            EPH_LOG_DEBUG(detail::posix_listener_logger(), "posix::accept_one: setsockopt(TCP_NODELAY) failed "
                         "cfd={} errno={} ({}) — connection still usable",
                         cfd, errno, std::strerror(errno));
        }

        // Zero-init the scratch buffer so a (theoretical) inet_ntop
        // failure can't have us SPDLOG_INFO an uninitialized C-string
        // (UB: spdlog/fmt would read past unmapped memory until a stray
        // NUL or a SIGSEGV). inet_ntop only returns nullptr for
        // EAFNOSUPPORT / ENOSPC — both impossible given AF_INET +
        // INET_ADDRSTRLEN below — but the cost is negligible and the
        // failure mode is invisible from a journal entry alone.
        char ip[INET_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET, &caddr.sin_addr, ip, sizeof(ip)) == nullptr) {
            EPH_LOG_WARN(detail::posix_listener_logger(), "posix::accept_one: inet_ntop failed cfd={} errno={} ({}) "
                        "— peer IP rendered as '?'", cfd, errno, std::strerror(errno));
            ip[0] = '?';
            ip[1] = '\0';
        }
        EPH_LOG_INFO(detail::posix_listener_logger(), "posix::accept_one: client {}:{} cfd={}",
                    ip, ntohs(caddr.sin_port), cfd);
        return cfd;
    }
    EPH_LOG_DEBUG(detail::posix_listener_logger(), "posix::accept_one: exit before connect (running=false)");
    return -1;
}

} // namespace eph::net::posix
