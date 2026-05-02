#pragma once

/// @file error.hpp
/// Unified error enum and ErrorInfo struct for eph-core. Replaces the
/// scattered per-module error enums (`SendError`, `ConnectionError`,
/// `TcpError`, `WsError`, ...) with a single flat `Error` enum that every
/// fallible API in the eph stack returns via `std::expected<T, ErrorInfo>`.
///
/// Design rationale:
///   - A single enum keeps `std::expected` instantiations monomorphic across
///     module boundaries, which matters both for binary size and for the
///     ability to forward errors without lossy conversions.
///   - `ErrorInfo::detail` is a `const char*` rather than `std::string` so
///     that constructing an error is allocation-free on the hot path. The
///     contract is that `detail` must point to a string literal or other
///     storage with static lifetime — callers never free it.
///   - Strictly noexcept + header-only + no virtual dispatch, per project
///     conventions (see CLAUDE.md).

#include <cstdint>
#include <format>
#include <ostream>
#include <string>

namespace eph::core {

// ---------------------------------------------------------------------------
// Error enum
// ---------------------------------------------------------------------------

/// @brief Unified error code for every fallible API in the eph stack.
///
/// Values are grouped by logical subsystem; the grouping is informational
/// only (the underlying integer values are not guaranteed to be stable and
/// must not be relied on by callers).
enum class Error : uint8_t {
    Ok = 0,

    // ── Connection lifecycle ───────────────────────────────────────────────
    ConnectFailed,       ///< connect(2) / DPDK TCP SYN failed
    Disconnected,        ///< peer closed the connection
    Timeout,             ///< operation exceeded its deadline
    NotAttached,         ///< Stream/Socket used before being attached to a Poller

    // ── TLS ────────────────────────────────────────────────────────────────
    TlsHandshakeFailed,  ///< aws-lc handshake returned an error
    TlsRecordBad,        ///< malformed TLS record on the wire
    TlsCipherFailed,     ///< AEAD encrypt/decrypt failed

    // ── WebSocket ─────────────────────────────────────────────────────────
    WsHandshakeFailed,   ///< HTTP upgrade handshake rejected / malformed
    WsFrameBad,          ///< WebSocket frame violates RFC 6455
    WsCloseReceived,     ///< peer sent a Close frame (clean shutdown signal)

    // ── Codec / application protocol ──────────────────────────────────────
    CodecBad,            ///< application protocol violation
    CodecOverflow,       ///< decoded frame exceeds buffer capacity

    // ── I/O ───────────────────────────────────────────────────────────────
    WouldBlock,          ///< non-blocking op would block (EAGAIN equivalent)
    BufferFull,          ///< TX/RX buffer has no room

    // ── Internal ──────────────────────────────────────────────────────────
    InvalidConfig,       ///< config struct failed validation
    OutOfMemory,         ///< allocation failed (or pool exhausted)

    // ── HTTP CONNECT proxy ─────────────────────────────────────────────────
    //
    // Appended at the end of the enum on purpose: older switch statements
    // keep compiling without re-order churn, and the underlying integer
    // values of earlier errors stay stable.
    ProxyConnectFailed,   ///< TCP connect to the proxy server itself failed
    ProxyHandshakeFailed, ///< proxy returned a non-200 / malformed response
    ProxyAuthRequired,    ///< proxy returned 407 — missing/wrong Basic auth

    // ── Registry / lookup lifecycle ───────────────────────────────────────
    //
    // Distinct from `InvalidConfig` (caller programming error, e.g. nullptr
    // / out-of-range) — `NotFound` signals a recoverable state mismatch
    // between caller and callee (e.g. unregistering something that was
    // never registered, looking up a key that expired). Callers may ignore
    // NotFound where InvalidConfig should halt.
    NotFound,             ///< registered item / lookup key does not exist
};

// ---------------------------------------------------------------------------
// ErrorInfo
// ---------------------------------------------------------------------------

/// @brief Structured error returned from fallible APIs via std::expected.
///
/// Pairs a typed `Error` code (for programmatic matching) with a static-lifetime
/// `detail` string (for logging/diagnostics). Deliberately trivially copyable
/// and allocation-free — constructing an ErrorInfo on the hot path is free.
struct ErrorInfo {
    Error       code;    ///< typed category, for programmatic matching
    const char* detail;  ///< static-lifetime detail string (may be empty, never nullptr)

    /// @brief Construct an ErrorInfo with an optional detail string.
    /// @param c  the Error category
    /// @param d  a string literal or other static-storage C string
    ///
    /// The `detail` pointer must outlive every copy of this ErrorInfo. In
    /// practice every call site passes a string literal, so lifetime is a
    /// non-issue. We explicitly never own heap storage here so that
    /// constructing an error path stays allocation-free.
    constexpr ErrorInfo(Error c, const char* d = "") noexcept
        : code(c), detail(d == nullptr ? "" : d) {}

    /// @brief Defaulted equality — compares code and detail pointer identity.
    ///
    /// Note: this compares `detail` by pointer, not by string content. That is
    /// intentional and sufficient because `detail` is always a string literal
    /// coming from the same translation-unit-deduplicated static pool. Tests
    /// that need content equality should compare `code` and `strcmp(detail, …)`
    /// explicitly.
    [[nodiscard]] friend constexpr bool operator==(const ErrorInfo&,
                                                   const ErrorInfo&) noexcept = default;
};

// ---------------------------------------------------------------------------
// error_name
// ---------------------------------------------------------------------------

/// @brief Return a stable, human-readable name for an Error value.
///
/// Used for logging and test assertions. The returned pointer always refers to
/// a string literal; callers may store it indefinitely without copying.
///
/// @param e  the Error value to name
/// @return a nul-terminated C string, never nullptr. Unknown values (e.g. from
///         future ABI drift or reinterpret_cast) return "UNKNOWN".
[[nodiscard]] constexpr const char* error_name(Error e) noexcept {
    switch (e) {
        case Error::Ok:                  return "OK";
        case Error::ConnectFailed:       return "CONNECT_FAILED";
        case Error::Disconnected:        return "DISCONNECTED";
        case Error::Timeout:              return "TIMEOUT";
        case Error::NotAttached:         return "NOT_ATTACHED";
        case Error::TlsHandshakeFailed:  return "TLS_HANDSHAKE_FAILED";
        case Error::TlsRecordBad:        return "TLS_RECORD_BAD";
        case Error::TlsCipherFailed:     return "TLS_CIPHER_FAILED";
        case Error::WsHandshakeFailed:   return "WS_HANDSHAKE_FAILED";
        case Error::WsFrameBad:          return "WS_FRAME_BAD";
        case Error::WsCloseReceived:     return "WS_CLOSE_RECEIVED";
        case Error::CodecBad:            return "CODEC_BAD";
        case Error::CodecOverflow:       return "CODEC_OVERFLOW";
        case Error::WouldBlock:          return "WOULD_BLOCK";
        case Error::BufferFull:          return "BUFFER_FULL";
        case Error::InvalidConfig:       return "INVALID_CONFIG";
        case Error::OutOfMemory:         return "OUT_OF_MEMORY";
        case Error::ProxyConnectFailed:  return "PROXY_CONNECT_FAILED";
        case Error::ProxyHandshakeFailed:return "PROXY_HANDSHAKE_FAILED";
        case Error::ProxyAuthRequired:   return "PROXY_AUTH_REQUIRED";
        case Error::NotFound:            return "NOT_FOUND";
    }
    return "UNKNOWN";
}

/// @brief Stream insertion: renders as `CODE: detail` (detail omitted when empty).
///
/// Lets existing diagnostic call sites that already stream `.error()` into
/// gtest / std::ostream continue to work unchanged after the TcpSession
/// error type migration. Keeps logging call sites short and readable.
inline std::ostream& operator<<(std::ostream& os, const ErrorInfo& e) {
    os << error_name(e.code);
    if (e.detail != nullptr && e.detail[0] != '\0') {
        os << ": " << e.detail;
    }
    return os;
}

/// @brief ADL-found formatting hook recognised by both std::format and
///        spdlog's bundled fmt. Lets `SPDLOG_LOGGER_*("{}", err)` work
///        without each TU having to include error.hpp before spdlog.
inline std::string format_as(const ErrorInfo& e) {
    if (e.detail != nullptr && e.detail[0] != '\0') {
        return std::string(error_name(e.code)) + ": " + e.detail;
    }
    return std::string(error_name(e.code));
}

inline std::string_view format_as(Error e) noexcept {
    return error_name(e);
}

} // namespace eph::core

// ---------------------------------------------------------------------------
// std::formatter specialization for ErrorInfo — enables `std::format("{}", err)`.
//
// This specialization covers the STANDARD library formatter path only.
// spdlog vendors its own fmt (spdlog/fmt/bundled/), which does NOT consult
// std::formatter specializations; it uses the `format_as` ADL hook defined
// above. Both paths must exist: std::format users get this specialization,
// spdlog/fmt users get format_as. Body is kept in lockstep with format_as
// and operator<< so all three render identically.
// ---------------------------------------------------------------------------

template <>
struct std::formatter<::eph::core::ErrorInfo> : std::formatter<std::string> {
    auto format(const ::eph::core::ErrorInfo& e, auto& ctx) const {
        if (e.detail != nullptr && e.detail[0] != '\0') {
            return std::formatter<std::string>::format(
                std::format("{}: {}", ::eph::core::error_name(e.code), e.detail),
                ctx);
        }
        return std::formatter<std::string>::format(
            std::string(::eph::core::error_name(e.code)), ctx);
    }
};

template <>
struct std::formatter<::eph::core::Error>
    : std::formatter<const char*> {
    auto format(::eph::core::Error e, auto& ctx) const {
        return std::formatter<const char*>::format(
            ::eph::core::error_name(e), ctx);
    }
};
