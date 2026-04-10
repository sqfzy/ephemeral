#pragma once

/// @file length_prefix_codec.hpp
/// 4-byte big-endian length-prefix stream codec.
///
/// Wire format: `[uint32_t payload_len (BE)] [payload bytes...]`
///
/// The length prefix counts the payload only, not the header itself. Zero-
/// length payloads are legal (they decode to a zero-length frame). A single
/// `decode()` call consumes at most one frame even if multiple frames are
/// available in the view; the caller loops until `Ok(None)` is returned.
///
/// This is the v3.3 replacement for `eph::net::LengthPrefixFramer` (in
/// `eph-core/include/eph/core/length_prefix_framer.hpp`). The differences:
///
///   1. Widened from 2-byte to 4-byte length prefix per the v3.3 design doc
///      (see "Codec 实现示例" / `max_overhead` table).
///   2. Decode operates on a `PacketViewRef` and returns
///      `expected<optional<Frame>, ErrorInfo>`, matching `StreamCodec`.
///   3. A maximum frame size (kMaxFrameLen) is enforced — oversized prefixes
///      return `CodecOverflow` instead of leading the decoder to wait forever
///      for bytes that may never arrive (slow-loris DoS resistance).
///
/// The codec is stateless; all the stream state lives in the PacketView's
/// own `head`/`tail` window.

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

/// @brief Lazily-initialised logger for the length-prefix codec.
inline spdlog::logger* length_prefix_codec_logger() {
    static auto* l = [] {
        auto lg = spdlog::get("codec.length_prefix");
        if (!lg) {
            try {
                lg = spdlog::stdout_color_mt("codec.length_prefix");
            } catch (const spdlog::spdlog_ex&) {
                lg = spdlog::get("codec.length_prefix");
            }
        }
        return lg.get();
    }();
    return l;
}

} // namespace detail

/// @brief 4-byte big-endian length-prefix stream codec.
class LengthPrefixCodec {
public:
    /// Zero-copy frame view (pointer into the backing PacketView storage).
    using Frame         = std::span<const uint8_t>;
    using PacketViewRef = SpanPacketView&;

    /// Maximum framing overhead — 4-byte big-endian length prefix.
    static constexpr std::size_t max_overhead = 4;
    static constexpr bool        is_streaming = true;

    /// Reject frames larger than this to avoid unbounded buffering and
    /// slow-loris-style DoS. 16 MiB is comfortable for ITCH-style payloads
    /// while still catching clearly bogus prefixes.
    static constexpr std::size_t kMaxFrameLen = 16u * 1024u * 1024u;

    constexpr LengthPrefixCodec() noexcept = default;

    /// @brief Decode at most one length-prefixed frame from the view.
    ///
    /// Returns:
    ///   - `Ok(Some(frame))` when a full frame is available. `view` is
    ///     trimmed forward by `4 + payload_len` bytes.
    ///   - `Ok(None)` when fewer than 4 bytes (no header yet) or fewer than
    ///     `4 + payload_len` bytes (header parsed, payload incomplete).
    ///   - `Err(CodecOverflow)` when the parsed length exceeds `kMaxFrameLen`.
    template <class PacketView>
    [[nodiscard]] std::expected<std::optional<Frame>, core::ErrorInfo>
    decode(PacketView& view, core::OutputBuffer& /*out*/) noexcept {
        const std::size_t avail = view.length();
        if (avail < 4) {
            // Header not yet complete — caller should re-poll for more data.
            SPDLOG_LOGGER_TRACE(detail::length_prefix_codec_logger(),
                "LengthPrefixCodec::decode: incomplete header, avail={}", avail);
            return std::optional<Frame>{};
        }

        const uint8_t* p = view.data();
        // Parse the length prefix as a big-endian uint32. We do this one
        // byte at a time to avoid any endianness surprises on the target.
        const std::uint32_t payload_len =
            (static_cast<std::uint32_t>(p[0]) << 24) |
            (static_cast<std::uint32_t>(p[1]) << 16) |
            (static_cast<std::uint32_t>(p[2]) << 8)  |
            (static_cast<std::uint32_t>(p[3]));

        if (payload_len > kMaxFrameLen) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::length_prefix_codec_logger(),
                "LengthPrefixCodec::decode: oversized frame, len={} max={}",
                payload_len, kMaxFrameLen);
            return std::unexpected(core::ErrorInfo{
                core::Error::CodecOverflow,
                "LengthPrefixCodec: frame length exceeds kMaxFrameLen"});
        }

        const std::size_t needed = std::size_t{4} + payload_len;
        if (avail < needed) {
            // Have header but not enough payload yet.
            SPDLOG_LOGGER_TRACE(detail::length_prefix_codec_logger(),
                "LengthPrefixCodec::decode: incomplete payload, "
                "need={} avail={}", needed, avail);
            return std::optional<Frame>{};
        }

        // Snapshot the payload span before advancing the view.
        Frame frame{p + 4, payload_len};
        view.trim_front(needed);
        SPDLOG_LOGGER_TRACE(detail::length_prefix_codec_logger(),
            "LengthPrefixCodec::decode: delivered frame payload_len={}",
            payload_len);
        return std::optional<Frame>{frame};
    }

    /// @brief Encode a frame as `[BE uint32 len][payload]` into `buf`.
    ///
    /// Returns `BufferFull` if the destination is too small and
    /// `CodecOverflow` if the payload exceeds `kMaxFrameLen`.
    [[nodiscard]] std::expected<std::size_t, core::ErrorInfo>
    encode(uint8_t* buf, std::size_t cap, Frame payload) noexcept {
        if (payload.size() > kMaxFrameLen) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::length_prefix_codec_logger(),
                "LengthPrefixCodec::encode: payload {} exceeds kMaxFrameLen {}",
                payload.size(), kMaxFrameLen);
            return std::unexpected(core::ErrorInfo{
                core::Error::CodecOverflow,
                "LengthPrefixCodec: payload exceeds kMaxFrameLen"});
        }
        const std::size_t needed = 4 + payload.size();
        if (needed > cap) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::length_prefix_codec_logger(),
                "LengthPrefixCodec::encode: buffer too small (need {}, have {})",
                needed, cap);
            return std::unexpected(core::ErrorInfo{
                core::Error::BufferFull,
                "LengthPrefixCodec::encode: destination buffer too small"});
        }

        const auto n = static_cast<std::uint32_t>(payload.size());
        buf[0] = static_cast<uint8_t>((n >> 24) & 0xFF);
        buf[1] = static_cast<uint8_t>((n >> 16) & 0xFF);
        buf[2] = static_cast<uint8_t>((n >>  8) & 0xFF);
        buf[3] = static_cast<uint8_t>((n      ) & 0xFF);
        if (!payload.empty()) {
            std::memcpy(buf + 4, payload.data(), payload.size());
        }
        return needed;
    }
};

static_assert(core::StreamCodec<LengthPrefixCodec>,
              "LengthPrefixCodec must satisfy the StreamCodec concept");

} // namespace eph::codec
