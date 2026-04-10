#pragma once

/// @file error.hpp
/// Unified error enum and ErrorInfo struct for the v3.3 refactor.
///
/// This header is part of the Phase 0 slim-down of eph-core (see
/// .artifacts/design-eph-v3.3-architecture-20260410.md). It replaces the
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
    CodecNeedMoreData,   ///< stream codec needs more bytes (internal signal)
    CodecBad,            ///< application protocol violation
    CodecOverflow,       ///< decoded frame exceeds buffer capacity

    // ── I/O ───────────────────────────────────────────────────────────────
    WouldBlock,          ///< non-blocking op would block (EAGAIN equivalent)
    NoData,              ///< receive poll returned zero packets
    BufferFull,          ///< TX/RX buffer has no room

    // ── Internal ──────────────────────────────────────────────────────────
    InvalidConfig,       ///< config struct failed validation
    OutOfMemory,         ///< allocation failed (or pool exhausted)

    // ── HTTP CONNECT proxy (Sub-phase 9.6) ─────────────────────────────────
    //
    // Appended at the end of the enum on purpose: older switch statements
    // that predate 9.6 keep compiling without re-order churn, and the
    // underlying integer values of pre-9.6 errors stay stable.
    ProxyConnectFailed,   ///< TCP connect to the proxy server itself failed
    ProxyHandshakeFailed, ///< proxy returned a non-200 / malformed response
    ProxyAuthRequired,    ///< proxy returned 407 — missing/wrong Basic auth
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
        case Error::CodecNeedMoreData:   return "CODEC_NEED_MORE_DATA";
        case Error::CodecBad:            return "CODEC_BAD";
        case Error::CodecOverflow:       return "CODEC_OVERFLOW";
        case Error::WouldBlock:          return "WOULD_BLOCK";
        case Error::NoData:              return "NO_DATA";
        case Error::BufferFull:          return "BUFFER_FULL";
        case Error::InvalidConfig:       return "INVALID_CONFIG";
        case Error::OutOfMemory:         return "OUT_OF_MEMORY";
        case Error::ProxyConnectFailed:  return "PROXY_CONNECT_FAILED";
        case Error::ProxyHandshakeFailed:return "PROXY_HANDSHAKE_FAILED";
        case Error::ProxyAuthRequired:   return "PROXY_AUTH_REQUIRED";
    }
    return "UNKNOWN";
}

} // namespace eph::core
