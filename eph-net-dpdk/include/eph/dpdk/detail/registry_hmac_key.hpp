#pragma once

/// @file detail/registry_hmac_key.hpp
/// Daemon-distributed HMAC key for cross-process registry tamper
/// protection (T2.3 wiring, 2026-05-05).
///
/// Threat model: see `NicServiceConfig::enable_registry_hmac` doc in
/// `detail/platform_config.hpp`. Short version: a compromised
/// secondary (or a wild-pointer write into the shared hugepage
/// segment) can tamper with another secondary's `MpRegistry::ProcSlot`
/// without detection in the original POD-only design. With HMAC enabled
/// the daemon-side primary signs every slot write, and every secondary
/// verifies on read; a tamper surfaces as `Error::InvalidConfig` with
/// a `registry-hmac-mismatch` diagnostic.
///
/// Key material:
///   - 32 bytes from aws-lc's CSPRNG (`RAND_bytes`) generated once at
///     `Platform::serve_nic` time.
///   - Written to `/run/eph/<sanitize_bdf(pci)>.key` with mode `0440`
///     and `root:root` ownership. Operators can override by chown'ing
///     after the daemon writes — the daemon does not enforce a
///     specific group.
///   - Tenants read the file at `Platform::create` time; failure to
///     read surfaces as `Error::InvalidConfig` (the registry header's
///     `hmac_enabled` bit tells the tenant whether to expect the file
///     in the first place — a daemon that didn't enable HMAC will not
///     have written it, and tenants short-circuit before trying).
///
/// Why a file (and not, say, a shared-memory key)?
///   - Filesystem permissions give operator-controlled access ACL
///     (group `eph`, uid-based isolation, MAC labels via SELinux/
///     AppArmor — all standard tooling).
///   - A shared-memory key would require every secondary to be in the
///     same uid/gid as the daemon, which collapses the multi-tenant
///     isolation model.
///   - Cost: one file `read(2)` per `Platform::create`. Cold path.
///
/// Why not derive the key from the BDF or a config field?
///   - A deterministic key would let a compromised tenant forge HMAC
///     tags by recomputing the derivation. Random + filesystem-ACL is
///     the only approach that resists a compromised peer in the same
///     hugepage namespace.

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/rand.h>  // aws-lc RAND_bytes

#include <spdlog/spdlog.h>

#include "eph/core/error.hpp"
#include "eph/dpdk/detail/bdf_sanitize.hpp"

namespace eph::dpdk::detail {

inline constexpr size_t kRegistryHmacKeyBytes = 32;

/// @brief Compose the per-NIC key file path. Returns `InvalidConfig` if
/// `pci` doesn't pass `sanitize_bdf_for_file_prefix`.
[[nodiscard]] inline std::expected<std::filesystem::path, core::ErrorInfo>
registry_hmac_key_path(std::string_view pci) {
    auto sanitized = sanitize_bdf_for_file_prefix(pci);
    if (!sanitized) return std::unexpected(sanitized.error());
    return std::filesystem::path("/run/eph") /
           (std::string{*sanitized} + ".key");
}

/// @brief Generate a fresh 32-byte HMAC key from aws-lc's CSPRNG and
/// write it to `path` with `0440 root:root`. Caller (the daemon) is
/// expected to be running as root.
///
/// On success returns the key bytes (so the daemon can also keep them
/// in process memory without reading the file back). On failure leaves
/// no partial file behind.
///
/// Idempotent across daemon restarts in the sense that an existing key
/// file is **overwritten** (the daemon's threat model assumes its own
/// process is the trust root; key rotation on restart is fine — every
/// running tenant will see the registry's `hmac_enabled` flip-flop and
/// re-read).
[[nodiscard]] inline std::expected<
    std::array<uint8_t, kRegistryHmacKeyBytes>, core::ErrorInfo>
write_registry_hmac_key(const std::filesystem::path& path) noexcept {
    namespace fs = std::filesystem;

    // Ensure parent dir exists (typically /run/eph). Mode 0755 root:root.
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        SPDLOG_ERROR(
            "registry_hmac_key: mkdir {} failed: {}",
            path.parent_path().string(), ec.message());
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "registry_hmac_key: failed to create parent directory"});
    }

    std::array<uint8_t, kRegistryHmacKeyBytes> key{};
    if (RAND_bytes(key.data(),
                   static_cast<int>(key.size())) != 1) {
        SPDLOG_ERROR(
            "registry_hmac_key: RAND_bytes failed (aws-lc CSPRNG)");
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "registry_hmac_key: RAND_bytes failed"});
    }

    // Write atomically: <path>.tmp, fchmod 0440, write, fsync, rename.
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    const std::string tmp_str = tmp.string();
    const int fd = ::open(tmp_str.c_str(),
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                          /*mode=*/0440);
    if (fd < 0) {
        const int err = errno;
        SPDLOG_ERROR(
            "registry_hmac_key: open({}) failed: {}",
            tmp_str, std::strerror(err));
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "registry_hmac_key: open failed (need root + /run writable)"});
    }
    // open() with mode honours umask; force the exact 0440 we want.
    if (::fchmod(fd, 0440) != 0) {
        const int err = errno;
        ::close(fd);
        ::unlink(tmp_str.c_str());
        SPDLOG_ERROR(
            "registry_hmac_key: fchmod({}, 0440) failed: {}",
            tmp_str, std::strerror(err));
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "registry_hmac_key: fchmod failed"});
    }
    const uint8_t* p = key.data();
    size_t left = key.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            const int err = errno;
            ::close(fd);
            ::unlink(tmp_str.c_str());
            SPDLOG_ERROR(
                "registry_hmac_key: write failed: {}", std::strerror(err));
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "registry_hmac_key: write failed"});
        }
        p += n;
        left -= static_cast<size_t>(n);
    }
    // Durability: fsync the file, then rename, then fsync the parent
    // dir. Without the parent-dir fsync the rename can revert under a
    // crash; without the file fsync the bytes can be lost. A daemon
    // that believes it wrote a key but actually didn't (and then
    // crashes + restarts) would mint a fresh key on second boot,
    // diverging from any tenant that successfully read the original
    // bytes between the failed write and the restart.
    if (::fsync(fd) != 0) {
        const int err = errno;
        ::close(fd);
        ::unlink(tmp_str.c_str());
        SPDLOG_ERROR(
            "registry_hmac_key: fsync({}) failed: {} — bytes may not "
            "be durably persisted; refusing to rename to avoid "
            "publishing a key the daemon cannot reload after crash",
            tmp_str, std::strerror(err));
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "registry_hmac_key: fsync of tmp file failed"});
    }
    ::close(fd);

    if (::rename(tmp_str.c_str(), path.string().c_str()) != 0) {
        const int err = errno;
        ::unlink(tmp_str.c_str());
        SPDLOG_ERROR(
            "registry_hmac_key: rename({} -> {}) failed: {}",
            tmp_str, path.string(), std::strerror(err));
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "registry_hmac_key: rename failed"});
    }
    // Parent-dir fsync to make the rename durable. Best-effort: if it
    // fails we log but don't roll back — rename already moved the
    // bytes into place, so a tenant that races a read at this moment
    // observes a valid key. The parent-dir fsync just protects against
    // a concurrent crash-then-recover scenario.
    if (const int dfd = ::open(path.parent_path().string().c_str(),
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        dfd >= 0) {
        if (::fsync(dfd) != 0) {
            SPDLOG_WARN(
                "registry_hmac_key: parent-dir fsync({}) failed: {} — "
                "rename may not survive an immediate crash, but the "
                "key is already visible to readers; daemon will "
                "rewrite on next boot if reload fails",
                path.parent_path().string(), std::strerror(errno));
        }
        ::close(dfd);
    } else {
        SPDLOG_WARN(
            "registry_hmac_key: open(parent_dir={}) for fsync failed: "
            "{} — see fsync warning rationale above",
            path.parent_path().string(), std::strerror(errno));
    }
    SPDLOG_INFO(
        "registry_hmac_key: wrote 32-byte key to {} mode 0440",
        path.string());
    return key;
}

/// @brief Read the daemon-written key. Returns `InvalidConfig` if the
/// file is missing, has wrong size, or is unreadable (e.g. mode 0440
/// + caller not in root group).
///
/// Tenants call this only when the registry header advertises
/// `hmac_enabled=1`. A tenant that lacks read permission to the key
/// file *and* sees the registry expecting HMAC is misconfigured —
/// either the operator forgot to add the tenant to the file's group
/// or the tenant is running with the wrong identity.
[[nodiscard]] inline std::expected<
    std::array<uint8_t, kRegistryHmacKeyBytes>, core::ErrorInfo>
read_registry_hmac_key(const std::filesystem::path& path) noexcept {
    const int fd = ::open(path.string().c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        const int err = errno;
        SPDLOG_ERROR(
            "registry_hmac_key: open({}) for read failed: {} — "
            "registry header advertises hmac_enabled but the key file "
            "is unreadable; check that this tenant's uid/gid has read "
            "access (mode 0440 root:root by default)",
            path.string(), std::strerror(err));
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "registry_hmac_key: key file unreadable"});
    }
    std::array<uint8_t, kRegistryHmacKeyBytes> key{};
    uint8_t* p = key.data();
    size_t left = key.size();
    while (left > 0) {
        const ssize_t n = ::read(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            const int err = errno;
            ::close(fd);
            SPDLOG_ERROR(
                "registry_hmac_key: read failed: {}", std::strerror(err));
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "registry_hmac_key: read failed"});
        }
        if (n == 0) {  // EOF before full key read
            ::close(fd);
            SPDLOG_ERROR(
                "registry_hmac_key: file truncated ({} of {} bytes read)",
                key.size() - left, key.size());
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "registry_hmac_key: file truncated (expected 32 bytes)"});
        }
        p += n;
        left -= static_cast<size_t>(n);
    }
    ::close(fd);
    return key;
}

/// @brief Test-only: write a key under a custom path (typically a
/// tmpdir) so unit tests can exercise the read+verify path without
/// needing root + `/run/eph/`. Production code should call
/// `write_registry_hmac_key(registry_hmac_key_path(pci))` instead.
[[nodiscard]] inline std::expected<
    std::array<uint8_t, kRegistryHmacKeyBytes>, core::ErrorInfo>
write_registry_hmac_key_at(const std::filesystem::path& custom_path) noexcept {
    return write_registry_hmac_key(custom_path);
}

}  // namespace eph::dpdk::detail
