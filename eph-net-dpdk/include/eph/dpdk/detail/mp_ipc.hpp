#pragma once

/// @file detail/mp_ipc.hpp
/// Typed RAII wrappers around DPDK's `rte_mp_*` cross-process IPC.
///
/// Background: DPDK's primary+secondary multi-process model exposes a
/// per-host-prefix Unix-domain socket through `<rte_eal.h>`'s
/// `rte_mp_action_register` / `rte_mp_sendmsg` / `rte_mp_request_sync`
/// API. Registering an *action name* installs a handler that receives
/// messages from peer processes; sendmsg / request_sync push payloads
/// the other way. The whole machinery is cold-path (one msg per ICMP
/// dispatch / FlowDir install), so a thin RAII layer with explicit
/// degrade-on-failure semantics is the right abstraction here.
///
/// Why a wrapper at all (vs. calling rte_mp_* directly):
///   1. **RAII**: ad-hoc `rte_mp_action_register` calls must pair with
///      `rte_mp_action_unregister` on every exit path including
///      exceptions and `Platform` factory early-returns. The handle is
///      move-only and unregisters on destruction.
///   2. **Degrade-on-failure**: if the EAL was bring-up'd with
///      `--no-shconf` (or a future EAL mode disables IPC), action
///      registration fails. Reshape contract: secondary should still
///      run with degraded semantics (ICMP miss → drop, FlowDir
///      secondary install fails as before). The handle returns
///      `bool(handle) == false` and call-sites skip IPC paths.
///   3. **Type safety on payloads**: `rte_mp_msg::param` is
///      `uint8_t[256]`; raw memcpy is error-prone. `parse_payload<T>`
///      and `pack_payload<T>` enforce sizeof match against
///      `len_param`, refusing version drift / size mismatch up-front.
///
/// What this header does NOT do:
///   - Route by version field — that's the application's job in the
///     handler body (this header treats payload as opaque bytes).
///   - Manage `fds[]` —— eph's IPC use cases are pure value passes;
///     fd passing isn't needed.
///   - Async requests (`rte_mp_request_async`) — sync covers FlowDir
///     install (cold path, ~10us roundtrip is fine); fire-and-forget
///     covers ICMP. Async is over-engineered for both.
///
/// Hot path: NONE. Every entry here is on cold paths (Platform
/// bring-up, stream attach/detach, ICMP miss-and-forward).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <spdlog/spdlog.h>

#include <rte_eal.h>
#include <rte_errno.h>

#include "eph/core/error.hpp"

namespace eph::dpdk::detail {

/// @brief Effective max name length we accept. DPDK's
/// `RTE_MP_MAX_NAME_LEN = 64` includes trailing NUL; we cap user
/// input at 60 to leave headroom and reject overflow up-front.
inline constexpr size_t kMpIpcMaxNameLen  = 60;

/// @brief Mirrors DPDK's `RTE_MP_MAX_PARAM_LEN = 256` —— exposed for
/// call sites that want compile-time `static_assert(sizeof(MsgT) <=
/// kMpIpcMaxParamLen)` on their own payload structs.
inline constexpr size_t kMpIpcMaxParamLen = RTE_MP_MAX_PARAM_LEN;

// ─────────────────────────────────────────────────────────────────────────────
// MpIpcAction — RAII wrapper around rte_mp_action_register / unregister
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Registers an action handler in the constructor and
/// unregisters it in the destructor. Move-only.
///
/// Construction failure modes:
///   - empty / oversized name → `bool(action) == false`, no
///     SPDLOG_ERROR (caller bug, hopefully caught in tests)
///   - `rte_mp_action_register` returns non-zero (e.g. EAL was init'd
///     with `--no-shconf`, or duplicate name) → `bool(action) ==
///     false` with SPDLOG_ERROR carrying `rte_errno`. **Caller is
///     expected to keep going on the degraded path** —— this is the
///     point of the "warn + continue" decision in the reshape plan.
///
/// Note that DPDK's `rte_mp_action_unregister` is idempotent and
/// returns void —— we don't need to check anything on the unregister
/// path.
class MpIpcAction {
public:
    MpIpcAction() noexcept = default;

    MpIpcAction(std::string_view name, rte_mp_t handler) noexcept {
        if (name.empty() || name.size() >= kMpIpcMaxNameLen) {
            SPDLOG_ERROR(
                "MpIpcAction: name '{}' empty or exceeds {} bytes — "
                "skipping registration (handle will report bool==false)",
                name, kMpIpcMaxNameLen);
            return;
        }
        if (handler == nullptr) {
            SPDLOG_ERROR("MpIpcAction: handler is null for action '{}'", name);
            return;
        }
        // rte_mp_action_register expects null-terminated; std::string
        // ensures that.
        name_.assign(name);
        const int rc = rte_mp_action_register(name_.c_str(), handler);
        if (rc != 0) {
            SPDLOG_ERROR(
                "MpIpcAction: rte_mp_action_register('{}') failed (rc={}, "
                "rte_errno={}) — IPC will degrade for this action",
                name_, rc, rte_errno);
            // Keep name_ populated so dtor is a clean no-op (we never
            // call unregister, so the dangling-name worry doesn't apply).
            owns_ = false;
            return;
        }
        owns_ = true;
        SPDLOG_DEBUG("MpIpcAction: registered '{}'", name_);
    }

    ~MpIpcAction() noexcept {
        if (owns_) {
            // rte_mp_action_unregister returns void; no error path.
            rte_mp_action_unregister(name_.c_str());
            SPDLOG_DEBUG("MpIpcAction: unregistered '{}'", name_);
        }
    }

    MpIpcAction(const MpIpcAction&) = delete;
    MpIpcAction& operator=(const MpIpcAction&) = delete;

    MpIpcAction(MpIpcAction&& o) noexcept
        : name_(std::move(o.name_)), owns_(o.owns_) {
        o.owns_ = false;
    }

    MpIpcAction& operator=(MpIpcAction&& o) noexcept {
        if (this != &o) {
            if (owns_) rte_mp_action_unregister(name_.c_str());
            name_ = std::move(o.name_);
            owns_ = o.owns_;
            o.owns_ = false;
        }
        return *this;
    }

    /// @brief True iff this handle currently owns a live registration
    /// (i.e. `rte_mp_action_register` returned 0). Call sites should
    /// treat `false` as "IPC degraded, skip this code path".
    [[nodiscard]] explicit operator bool() const noexcept { return owns_; }

    [[nodiscard]] std::string_view name() const noexcept { return name_; }

private:
    std::string name_;
    bool        owns_ = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Send / request helpers
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Pack `payload` (any trivially-copyable POD) into an
/// `rte_mp_msg` with `name`, ready to be passed to `rte_mp_sendmsg` /
/// `rte_mp_request_sync`. Caller is responsible for the rest (the msg
/// is on the caller's stack so no allocation needed). Returns false if
/// payload doesn't fit or name is invalid; in that case the caller
/// should bail without invoking the DPDK API.
template <typename MsgT>
[[nodiscard]] inline bool
pack_msg(rte_mp_msg& out, std::string_view name, const MsgT& payload) noexcept {
    static_assert(std::is_trivially_copyable_v<MsgT>,
                  "IPC payload must be trivially copyable for rte_mp wire format");
    static_assert(sizeof(MsgT) <= kMpIpcMaxParamLen,
                  "IPC payload exceeds RTE_MP_MAX_PARAM_LEN (256 bytes)");

    if (name.empty() || name.size() >= kMpIpcMaxNameLen) return false;

    std::memset(&out, 0, sizeof(out));
    std::memcpy(out.name, name.data(), name.size());
    // out.name is already NUL-terminated because we memset to 0 first.
    out.len_param = static_cast<int>(sizeof(MsgT));
    out.num_fds   = 0;
    std::memcpy(out.param, &payload, sizeof(MsgT));
    return true;
}

/// @brief Unpack `rte_mp_msg::param` into a typed `MsgT`. Returns
/// `InvalidConfig` if the wire `len_param` doesn't equal `sizeof(MsgT)`
/// (a clear signal of cross-version mismatch).
template <typename MsgT>
[[nodiscard]] inline std::expected<MsgT, core::ErrorInfo>
parse_payload(const rte_mp_msg* m) noexcept {
    static_assert(std::is_trivially_copyable_v<MsgT>);
    if (m == nullptr) {
        // Pure programmer error — DPDK never delivers a null msg to
        // an action handler. ERROR-log so the call site is identifiable.
        SPDLOG_ERROR("parse_payload: msg pointer is null (sizeof(MsgT)={})",
                     sizeof(MsgT));
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "parse_payload: msg pointer is null"});
    }
    if (m->len_param != static_cast<int>(sizeof(MsgT))) {
        // Cross-version IPC mismatch is a genuine protocol-layer bug —
        // log enough state so the operator can identify the offending
        // peer (action name comes from `m->name`).
        SPDLOG_ERROR("parse_payload: action='{}' wire len_param={} "
                     "expected sizeof(MsgT)={} — cross-version IPC mismatch?",
                     m->name, m->len_param, sizeof(MsgT));
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "parse_payload: wire len_param does not match sizeof(MsgT) "
            "— cross-version IPC mismatch?"});
    }
    MsgT out;
    std::memcpy(&out, m->param, sizeof(MsgT));
    return out;
}

/// @brief Send `payload` to the peer process(es) without waiting for
/// any reply. Best-effort: DPDK queues the msg into the IPC socket
/// and returns. If the socket is full or the peer isn't listening,
/// the msg is silently dropped (with `rte_errno` set, which we log).
///
/// Use case: ICMP cross-process forwarding — the originating
/// secondary doesn't block the RX poll loop on owner ack.
template <typename MsgT>
[[nodiscard]] inline std::expected<void, core::ErrorInfo>
mp_ipc_send_oneway(std::string_view name, const MsgT& payload) noexcept {
    rte_mp_msg msg{};
    if (!pack_msg(msg, name, payload))
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "mp_ipc_send_oneway: pack_msg failed (name oversize or empty)"});
    const int rc = rte_mp_sendmsg(&msg);
    if (rc != 0) {
        SPDLOG_DEBUG(
            "mp_ipc_send_oneway('{}'): rte_mp_sendmsg rc={} rte_errno={}",
            name, rc, rte_errno);
        return std::unexpected(core::ErrorInfo{
            core::Error::WouldBlock,
            "mp_ipc_send_oneway: rte_mp_sendmsg failed (peer not listening "
            "or socket full); message dropped"});
    }
    return {};
}

/// @brief Send `payload` to peer and block until reply or `timeout`.
/// Returns the reply payload (first reply only — eph IPC is 1:1
/// primary↔secondary, never multi-peer).
///
/// Use case: FlowDir install fallback — the secondary needs the
/// primary-side handle id back synchronously.
///
/// On timeout / no reply / multiple replies, returns Timeout /
/// InvalidConfig. Caller-allocated reply storage avoids the heap
/// double-free contract DPDK imposes via `reply.msgs`.
template <typename MsgT, typename ReplyT>
[[nodiscard]] inline std::expected<ReplyT, core::ErrorInfo>
mp_ipc_request_sync(std::string_view name,
                    const MsgT&       payload,
                    std::chrono::milliseconds timeout) noexcept {
    static_assert(std::is_trivially_copyable_v<ReplyT>);
    static_assert(sizeof(ReplyT) <= kMpIpcMaxParamLen);

    rte_mp_msg req{};
    if (!pack_msg(req, name, payload))
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "mp_ipc_request_sync: pack_msg failed"});

    // A negative timeout would propagate to a negative tv_sec / tv_nsec
    // (DPDK passes the timespec through to its own internal sem_timedwait
    // path, which is unspecified for negative inputs — historical builds
    // hung on this). A zero timeout is unspecified across DPDK versions
    // ("try once" on some, "no-op" on others); reject both up-front so the
    // caller gets a deterministic error rather than a flaky hang.
    const auto millis = timeout.count();
    if (millis <= 0) {
        SPDLOG_DEBUG("mp_ipc_request_sync('{}'): non-positive timeout={} ms "
                     "rejected up-front", name, millis);
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "mp_ipc_request_sync: timeout must be > 0 ms"});
    }
    timespec ts{
        .tv_sec  = static_cast<time_t>(millis / 1000),
        .tv_nsec = static_cast<long>((millis % 1000) * 1'000'000),
    };

    struct rte_mp_reply reply{};
    const int rc = rte_mp_request_sync(&req, &reply, &ts);
    // DPDK contract: reply.msgs is heap-allocated by DPDK on success;
    // caller must free even on partial-success / timeout. RAII guard
    // ensures it's freed on every return path including unexpected.
    struct ReplyGuard {
        struct rte_mp_reply* r;
        ~ReplyGuard() { ::free(r->msgs); r->msgs = nullptr; }
    } guard{&reply};

    if (rc != 0) {
        SPDLOG_DEBUG(
            "mp_ipc_request_sync('{}'): rc={} rte_errno={} (timeout={} ms)",
            name, rc, rte_errno, millis);
        return std::unexpected(core::ErrorInfo{
            core::Error::Timeout,
            "mp_ipc_request_sync: rte_mp_request_sync failed (timeout / "
            "peer down / IPC unavailable)"});
    }
    if (reply.nb_received != 1 || reply.nb_sent != 1) {
        // 0/1 mismatch under healthy IPC means the peer responded with an
        // unexpected number of messages — protocol violation worth logging.
        SPDLOG_ERROR(
            "mp_ipc_request_sync('{}'): nb_sent={} nb_received={} — "
            "expected exactly 1 reply (eph IPC is 1:1 primary↔secondary)",
            name, reply.nb_sent, reply.nb_received);
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "mp_ipc_request_sync: expected exactly 1 reply (eph IPC is 1:1 "
            "primary↔secondary)"});
    }
    auto parsed = parse_payload<ReplyT>(&reply.msgs[0]);
    if (!parsed) return std::unexpected(parsed.error());
    return *parsed;
}

/// @brief Send a reply from inside an action handler. Wrapper around
/// `rte_mp_reply` —— the global function name collides with the
/// struct, hence the `_send` suffix.
template <typename ReplyT>
[[nodiscard]] inline std::expected<void, core::ErrorInfo>
mp_ipc_reply_send(std::string_view  name,
                  const ReplyT&     payload,
                  const void*       peer) noexcept {
    rte_mp_msg msg{};
    if (!pack_msg(msg, name, payload))
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "mp_ipc_reply_send: pack_msg failed"});
    const int rc = rte_mp_reply(&msg, static_cast<const char*>(peer));
    if (rc != 0) {
        return std::unexpected(core::ErrorInfo{
            core::Error::WouldBlock,
            "mp_ipc_reply_send: rte_mp_reply failed"});
    }
    return {};
}

} // namespace eph::dpdk::detail
