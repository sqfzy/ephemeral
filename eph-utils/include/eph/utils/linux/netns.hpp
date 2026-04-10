#pragma once

/// @file linux/netns.hpp
/// Enter a named Linux network namespace via `setns(2)`.
///
/// The named namespace must already exist (e.g. created by `ip netns add`
/// or by a wrapper script).  Requires CAP_SYS_ADMIN.
///
/// Originally lived under benchmarks/latency/core/netns.hpp; promoted
/// to eph-utils so that any test fixture or tool that needs network
/// namespace isolation can use it without reverse-including the bench
/// tree.

#include <cerrno>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sched.h>
#include <unistd.h>

namespace eph::utils::linux_ {

/// Move the calling thread into the network namespace
/// `/var/run/netns/<name>`.  `name` must not contain '/'.  Returns `{}`
/// on success, error string on failure.
[[nodiscard]] inline std::expected<void, std::string>
enter_netns(std::string_view name) {
    std::string path = "/var/run/netns/" + std::string(name);
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return std::unexpected(
            std::string("open(") + path + ") failed: " +
            std::strerror(errno) +
            " (netns must exist before this call)");
    }
    if (::setns(fd, CLONE_NEWNET) < 0) {
        int err = errno;
        ::close(fd);
        return std::unexpected(
            std::string("setns(") + path + ", CLONE_NEWNET) failed: " +
            std::strerror(err) +
            " (CAP_SYS_ADMIN required)");
    }
    ::close(fd);
    return {};
}

} // namespace eph::utils::linux_
