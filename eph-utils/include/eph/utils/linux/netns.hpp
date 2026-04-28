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

#include <spdlog/spdlog.h>

namespace eph::utils::linux_ {

/// Move the calling thread into the network namespace
/// `/var/run/netns/<name>`.  `name` must not contain '/'.  Returns `{}`
/// on success, error string on failure.
///
/// Both error branches log at WARN with errno context — `enter_netns`
/// is the kind of helper that is invoked once at fixture / tool init
/// time and a silent failure manifests later as "the wrong NIC" /
/// "no packets seen" rather than "namespace switch failed". The
/// matching errno-bearing string is also returned to the caller so
/// the unexpected value can be surfaced through whatever upper
/// diagnostic path exists.
[[nodiscard]] inline std::expected<void, std::string>
enter_netns(std::string_view name) {
    std::string path = "/var/run/netns/" + std::string(name);
    SPDLOG_DEBUG("netns::enter_netns: enter name=\"{}\" path=\"{}\"", name, path);
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        SPDLOG_WARN("netns::enter_netns: open(\"{}\") failed errno={} ({}) "
                    "(netns must exist; create with `ip netns add {}`)",
                    path, errno, std::strerror(errno), name);
        return std::unexpected(
            std::string("open(") + path + ") failed: " +
            std::strerror(errno) +
            " (netns must exist before this call)");
    }
    if (::setns(fd, CLONE_NEWNET) < 0) {
        int err = errno;
        ::close(fd);
        SPDLOG_WARN("netns::enter_netns: setns(\"{}\", CLONE_NEWNET) failed "
                    "errno={} ({}) (CAP_SYS_ADMIN required — run as root or "
                    "setcap cap_sys_admin+ep)",
                    path, err, std::strerror(err));
        return std::unexpected(
            std::string("setns(") + path + ", CLONE_NEWNET) failed: " +
            std::strerror(err) +
            " (CAP_SYS_ADMIN required)");
    }
    ::close(fd);
    SPDLOG_DEBUG("netns::enter_netns: ok name=\"{}\"", name);
    return {};
}

} // namespace eph::utils::linux_
