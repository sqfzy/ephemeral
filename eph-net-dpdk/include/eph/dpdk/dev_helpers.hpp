#pragma once

/// @file dev_helpers.hpp
/// Dev / small-project ergonomics for the daemon-led Platform model.
///
/// The "production" deployment story for `eph-nicd` is systemd:
///
///     systemctl start eph-nicd@<bdf>.service
///
/// — toml lives in `/etc/eph/<bdf>.toml`, the unit handles bring-up,
/// `Restart=on-failure` provides crash revival, and the operator
/// has explicit control over the daemon's lifecycle. Application
/// processes attach as DPDK secondaries via `Platform::create`.
///
/// That's a heavyweight deployment shape for one-off dev work,
/// integration tests on a developer laptop, or small-project
/// installs without systemd. `eph::dpdk::dev::ensure_local_daemon`
/// provides an "auto-spawn the daemon if absent" helper for those
/// settings:
///
///     #include "eph/dpdk/dev_helpers.hpp"
///     // Best-effort daemon revive — no-op if already running, fork
///     // + exec if not.
///     auto e = eph::dpdk::dev::ensure_local_daemon("0000:01:00.1");
///     if (!e) { fprintf(stderr, "%s\n", e.error().detail); return 1; }
///     auto plat = eph::dpdk::Platform::create({.pci="0000:01:00.1"});
///
/// **This header is intentionally not included by `platform.hpp`** —
/// production code shouldn't auto-spawn anything. The dev helper has
/// to be opted into explicitly so the choice is visible at the call
/// site (and `git grep ensure_local_daemon` finds every dev-mode
/// caller).
///
/// Per plan decision #6: this lives outside `Platform::create` because:
///   1. `Platform::create` is called in latency-sensitive reconnect
///      loops; an auto-spawn branch would be wasted cycles in the
///      99% common case where the daemon is already up.
///   2. Production deploys must NEVER auto-spawn — systemd owns
///      lifecycle. Putting auto-spawn in the always-on path would
///      break that invariant.

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "eph/core/error.hpp"
#include "eph/dpdk/detail/bdf_sanitize.hpp"

namespace eph::dpdk::dev {

namespace detail {

/// @brief DPDK's runtime config dir for this prefix; presence of the
/// `config` file indicates a primary is (or was) running. Mirrors
/// what `rte_eal_init --file-prefix=<p>` writes.
[[nodiscard]] inline std::string
runtime_config_path_(std::string_view file_prefix) {
    std::string p = "/var/run/dpdk/";
    p.append(file_prefix);
    p.append("/config");
    return p;
}

/// @brief Probe whether the daemon's runtime config file exists. Does
/// NOT verify pid liveness — `rte_eal_init` against the primary will
/// itself surface a stale-config error if the previous daemon died
/// without cleanup. Best-effort: a `stat` race against a daemon
/// startup/shutdown is benign (the worst outcome is one extra spawn
/// attempt that races and the second daemon fails to take primary).
[[nodiscard]] inline bool
daemon_config_exists_(std::string_view file_prefix) noexcept {
    const std::string p = runtime_config_path_(file_prefix);
    struct ::stat st;
    return ::stat(p.c_str(), &st) == 0;
}

/// @brief Locate the eph-nicd binary. Tries `$EPH_NICD_BIN`, then
/// `/usr/local/bin/eph-nicd`, then `/usr/bin/eph-nicd`, then a
/// last-ditch search of `$PATH`.
[[nodiscard]] inline std::string
locate_eph_nicd_() noexcept {
    if (const char* env = ::getenv("EPH_NICD_BIN"); env != nullptr && env[0]) {
        struct ::stat st;
        if (::stat(env, &st) == 0 && (st.st_mode & S_IXUSR)) {
            return std::string{env};
        }
    }
    static constexpr const char* kCandidates[] = {
        "/usr/local/bin/eph-nicd",
        "/usr/bin/eph-nicd",
        "/opt/eph/bin/eph-nicd",
    };
    for (const char* c : kCandidates) {
        struct ::stat st;
        if (::stat(c, &st) == 0 && (st.st_mode & S_IXUSR)) {
            return std::string{c};
        }
    }
    // Fallback: rely on PATH lookup at execve time. Returning the bare
    // name lets execvp do the search; if the binary isn't anywhere on
    // PATH the spawned child will fail loud and the parent's post-spawn
    // probe will surface the failure as "config still not present".
    return std::string{"eph-nicd"};
}

/// @brief Double-fork the daemon so the grandchild gets reparented to
/// init (PID 1) and outlives the calling process. The first child
/// `setsid()`s to detach from the controlling tty, then forks the
/// grandchild that execs eph-nicd. We `waitpid` the first child so
/// it doesn't zombify; the grandchild is detached by then.
[[nodiscard]] inline std::expected<void, ::eph::core::ErrorInfo>
double_fork_exec_(const std::string& bin_path,
                  const std::string& pci) noexcept {
    pid_t pid1 = ::fork();
    if (pid1 < 0) {
        SPDLOG_ERROR("ensure_local_daemon: first fork failed: {}",
                     ::strerror(errno));
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::OutOfMemory,
            "ensure_local_daemon: first fork failed"});
    }
    if (pid1 == 0) {
        // First child. Detach from controlling terminal so the
        // grandchild isn't killed if our parent's tty closes.
        if (::setsid() < 0) {
            // setsid() fails with EPERM when the caller is already
            // a session leader (rare here — fork() guarantees we
            // aren't) or with other errors that imply a hostile
            // process environment. We bail rather than spawn a
            // grandchild that could be killed by SIGHUP on tty
            // close. The parent's post-spawn probe times out and
            // the caller sees the documented `Error::Timeout`.
            ::_exit(127);
        }
        pid_t pid2 = ::fork();
        if (pid2 < 0) {
            ::_exit(126);
        }
        if (pid2 != 0) {
            // First child exits immediately; grandchild is orphaned
            // and inherited by init.
            ::_exit(0);
        }
        // Grandchild: redirect stdio to /dev/null, then exec.
        int dn = ::open("/dev/null", O_RDWR);
        if (dn >= 0) {
            ::dup2(dn, 0);
            ::dup2(dn, 1);
            ::dup2(dn, 2);
            if (dn > 2) ::close(dn);
        }
        // argv: [eph-nicd, --no-config-file, --pci=<bdf>, NULL]
        const std::string flag_pci = std::string{"--pci="} + pci;
        char* argv[] = {
            const_cast<char*>(bin_path.c_str()),
            const_cast<char*>("--no-config-file"),
            const_cast<char*>(flag_pci.c_str()),
            nullptr,
        };
        // Use execvp so PATH lookup happens if bin_path is bare.
        ::execvp(argv[0], argv);
        // If we get here, exec failed. _exit so we don't run any
        // atexit handlers / static destructors from a half-forked
        // process.
        ::_exit(125);
    }
    // Parent: reap the first child (which exits ~immediately after
    // fork()ing the grandchild).
    int status = 0;
    pid_t r = ::waitpid(pid1, &status, 0);
    if (r < 0) {
        SPDLOG_WARN("ensure_local_daemon: waitpid({}) failed: {}",
                    pid1, ::strerror(errno));
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        const int code = WEXITSTATUS(status);
        SPDLOG_WARN("ensure_local_daemon: first child exited with code {} "
                    "(125 = execvp failed; 126 = second fork failed; "
                    "127 = setsid failed)", code);
        // Don't return error here — the grandchild may have started
        // anyway (e.g. if setsid failed at code 127, no fork happened
        // and the daemon was never spawned, but the post-spawn probe
        // catches that). Let the post-probe decide.
    }
    return {};
}

}  // namespace detail

/// @brief Ensure an `eph-nicd` daemon is running for the given pci.
///
/// Probes `/var/run/dpdk/eph_<sanitize_bdf(pci)>/config` for an active
/// primary's runtime config; if absent, fork+exec
/// `eph-nicd --no-config-file --pci=<pci>` (with hard-coded defaults
/// — `total_queues=16`, `daemon_lcore=0`, etc — chosen by the daemon
/// binary itself; this helper only chooses the pci).
///
/// REQUIRES current process is root (`geteuid() == 0`). Returns
/// `Error::InvalidConfig` (with a "needs sudo" detail) otherwise.
/// `eph-nicd` itself needs `CAP_SYS_ADMIN` to bind vfio-pci, so a
/// non-root caller can't spawn it usefully.
///
/// Usage: dev / small-project deploys without systemd. **Production
/// deploys should use** `systemctl start eph-nicd@<bdf>` and treat
/// daemon lifecycle as an ops concern.
///
/// Side effect: when the daemon isn't running, spawns a detached
/// daemon process via double-fork. The daemon outlives the current
/// process. Subsequent calls with the same pci are no-ops if the
/// daemon is still alive.
///
/// Cold path / blocking: this function may sleep up to ~1s waiting
/// for the post-spawn config file to appear. Never call from a hot
/// loop; call once at startup, then drive `Platform::create` normally.
[[nodiscard]] inline std::expected<void, ::eph::core::ErrorInfo>
ensure_local_daemon(std::string_view pci) noexcept {
    if (pci.empty()) {
        SPDLOG_ERROR("ensure_local_daemon: pci must be non-empty");
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::InvalidConfig,
            "ensure_local_daemon: pci must be non-empty"});
    }
    auto san = ::eph::dpdk::detail::sanitize_bdf_for_file_prefix(pci);
    if (!san) {
        SPDLOG_ERROR("ensure_local_daemon: bad pci '{}': {}",
                     pci, san.error().detail);
        return std::unexpected(san.error());
    }
    const std::string file_prefix = std::string{"eph_"} + *san;

    // Already up? No-op.
    if (detail::daemon_config_exists_(file_prefix)) {
        SPDLOG_DEBUG(
            "ensure_local_daemon: daemon for pci='{}' (file_prefix='{}') "
            "already running — no-op", pci, file_prefix);
        return {};
    }

    if (::geteuid() != 0) {
        SPDLOG_ERROR(
            "ensure_local_daemon: daemon for pci='{}' is not running and "
            "current process (euid={}) is not root — cannot spawn eph-nicd. "
            "Either run this binary under sudo OR pre-launch eph-nicd "
            "manually (sudo systemctl start eph-nicd@{})",
            pci, ::geteuid(), pci);
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::InvalidConfig,
            "ensure_local_daemon: daemon not running and current process "
            "is not root — cannot fork+exec eph-nicd. Run under sudo or "
            "start the daemon manually."});
    }

    const std::string bin = detail::locate_eph_nicd_();
    SPDLOG_INFO(
        "ensure_local_daemon: daemon for pci='{}' not running; "
        "double-fork-exec'ing '{}' --no-config-file --pci={}",
        pci, bin, pci);

    auto e = detail::double_fork_exec_(bin, std::string{pci});
    if (!e) return std::unexpected(e.error());

    // Poll for the config file to appear. eph-nicd's primary
    // bring-up is dominated by rte_eal_init + rte_eth_dev_configure
    // — typically 200-800ms on AWS ENA. Cap at 3s; that's a
    // generous upper bound that still surfaces "binary missing /
    // immediate startup failure" reasonably fast.
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (detail::daemon_config_exists_(file_prefix)) {
            SPDLOG_INFO(
                "ensure_local_daemon: daemon for pci='{}' is up "
                "(file_prefix='{}')", pci, file_prefix);
            return {};
        }
        std::this_thread::sleep_for(50ms);
    }

    SPDLOG_ERROR(
        "ensure_local_daemon: timed out waiting for daemon to come up "
        "(pci='{}', file_prefix='{}', expected '{}'). Common causes: "
        "eph-nicd binary not installed (check /usr/local/bin/eph-nicd "
        "or set $EPH_NICD_BIN); NIC not bound to vfio-pci; hugepages "
        "exhausted; toml syntax error.",
        pci, file_prefix, detail::runtime_config_path_(file_prefix));
    return std::unexpected(::eph::core::ErrorInfo{
        ::eph::core::Error::Timeout,
        "ensure_local_daemon: timed out waiting for daemon — check "
        "eph-nicd installation, vfio-pci binding, hugepages"});
}

}  // namespace eph::dpdk::dev
