/// @file core/netns.hpp
/// Enter a named Linux network namespace via `setns(2)`.
///
/// Used by the bench client parent process to move into `bench_ns` AFTER
/// forking off the mock child (the child inherits the original host
/// namespace — that's where NIC-A lives). The wrapper script
/// (`benchmarks/latency/lat`) has already created `bench_ns` and moved
/// NIC-B into it; we just need to join.
///
/// Requires CAP_SYS_ADMIN (run as root).
#pragma once

#include <expected>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sched.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace bench {

/// Move the calling thread into the network namespace `/var/run/netns/<name>`.
/// `name` must not contain '/'. Returns `{}` on success, error string on
/// failure.
[[nodiscard]] inline std::expected<void, std::string>
enter_netns(std::string_view name) {
    std::string path = "/var/run/netns/" + std::string(name);
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return std::unexpected(
            std::string("open(") + path + ") failed: " +
            std::strerror(errno) +
            " (netns must exist; the lat wrapper is responsible for creating it)");
    }
    if (::setns(fd, CLONE_NEWNET) < 0) {
        int err = errno;
        ::close(fd);
        return std::unexpected(
            std::string("setns(") + path + ", CLONE_NEWNET) failed: " +
            std::strerror(err) +
            " (are we running as root with CAP_SYS_ADMIN?)");
    }
    ::close(fd);
    return {};
}

} // namespace bench
