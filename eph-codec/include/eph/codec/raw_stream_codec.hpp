#pragma once

/// @file raw_stream_codec.hpp
/// Identity stream codec — no framing, every byte is data.
///
/// This replaces the legacy `eph::net::RawFramer` (in
/// `eph-transport/include/eph/transport/raw_framer.hpp`). Unlike the old
/// framer which took `(const uint8_t* data, size_t len)` and returned a
/// `DecodedFrame`, the new codec takes a duck-typed PacketView by reference
/// and satisfies the `eph::core::StreamCodec` concept.
///
/// Behavior:
///   - `decode(view, out)`: if `view.length() == 0` returns `Ok(None)`
///     (need more data). Otherwise returns `Ok(Some(frame))` where `frame`
///     spans the whole view, then trims the view to zero-length to mark
///     the bytes consumed — matching the stream contract that the codec
///     is responsible for advancing the read head.
///   - `encode(buf, cap, frame)`: straight memcpy, returns bytes written.
///
/// The codec is stateless (an empty struct) but still takes `T&` in the
/// decode signature because the StreamCodec concept mandates it. The
/// compiler elides the unused `this` at -O2.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "eph/codec/detail/span_packet_view.hpp"
#include "eph/core/codec.hpp"
#include "eph/core/error.hpp"

namespace eph::codec {

namespace detail {

/// @brief Lazily-initialized logger for the raw stream codec.
inline spdlog::logger* raw_stream_codec_logger() {
    static auto* l = [] {
        auto lg = spdlog::get("codec.raw_stream");
        if (!lg) {
            try {
                lg = spdlog::stdout_color_mt("codec.raw_stream");
            } catch (const spdlog::spdlog_ex&) {
                lg = spdlog::get("codec.raw_stream");
            }
        }
        // Final defense-in-depth: if both `get` paths failed (concurrent
        // drop racing the create), fall through to the always-non-null
        // default_logger so the SPDLOG_LOGGER_* macros at every call
        // site never deref a nullptr.
        if (!lg) lg = spdlog::default_logger();
        return lg.get();
    }();
    return l;
}

} // namespace detail

/// @brief Identity stream codec. Each decode delivers the entire view as one
///        frame; each encode is a pure memcpy.
///
/// Use this with TCP transports for protocols that handle their own message
/// boundaries (e.g. FIX with SOH-delimited fields, or when the application
/// does the framing at a higher layer).
class RawStreamCodec {
public:
    /// Zero-copy view into the PacketView's current window. Caller must
    /// consume the frame before touching the PacketView again.
    using Frame         = std::span<const uint8_t>;

    /// The codec concept requires a `PacketViewRef` associated type. We pin
    /// it to the canonical `SpanPacketView&` so the compile-time
    /// `StreamCodec` requirement can be checked against a concrete type.
    /// The `decode` method itself is a template, so later backends can pass
    /// their own PacketView types (e.g. DPDK's `MbufView`) without touching
    /// this header.
    using PacketViewRef = SpanPacketView&;

    /// Maximum per-frame framing overhead — zero for a pass-through codec.
    static constexpr std::size_t max_overhead = 0;
    /// Stream codec (TCP-style).
    static constexpr bool        is_streaming = true;

    /// @brief Default-constructible and trivially copyable — no state.
    constexpr RawStreamCodec() noexcept = default;

    /// @brief Decode a view as a single raw frame.
    ///
    /// Returns `Ok(None)` when the view is empty (need more data), otherwise
    /// `Ok(Some(frame))` spanning the view and trims the view to zero.
    ///
    /// `out` is unused — a raw codec never sends an auto-response.
    template <class PacketView>
    [[nodiscard]] std::expected<std::optional<Frame>, core::ErrorInfo>
    decode(PacketView& view, core::OutputBuffer& /*out*/) noexcept {
        const std::size_t n = view.length();
        if (n == 0) {
            // Nothing to consume; the contract is "Ok(None) = need more".
            SPDLOG_LOGGER_TRACE(detail::raw_stream_codec_logger(),
                "RawStreamCodec::decode: empty view, returning None");
            return std::optional<Frame>{};
        }
        // Snapshot the current window before we advance the read head.
        Frame frame{view.data(), n};
        // Mark all bytes consumed — the caller has the zero-copy frame
        // in hand and is responsible for handing it to the application
        // before the next decode() call clobbers the backing storage.
        view.trim_front(n);
        SPDLOG_LOGGER_TRACE(detail::raw_stream_codec_logger(),
            "RawStreamCodec::decode: delivered {} bytes as single frame", n);
        return std::optional<Frame>{frame};
    }

    /// @brief Encode a frame by copying its bytes into `buf`.
    ///
    /// Returns `BufferFull` if `cap` is smaller than the frame payload; no
    /// partial writes.
    [[nodiscard]] std::expected<std::size_t, core::ErrorInfo>
    encode(uint8_t* buf, std::size_t cap, Frame payload) noexcept {
        if (payload.size() > cap) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::raw_stream_codec_logger(),
                "RawStreamCodec::encode: buffer too small (need {}, have {})",
                payload.size(), cap);
            return std::unexpected(core::ErrorInfo{
                core::Error::BufferFull,
                "RawStreamCodec::encode: destination buffer too small"});
        }
        if (!payload.empty()) {
            std::memcpy(buf, payload.data(), payload.size());
        }
        return payload.size();
    }
};

static_assert(core::StreamCodec<RawStreamCodec>,
              "RawStreamCodec must satisfy the StreamCodec concept");

} // namespace eph::codec
