#pragma once

/// @file raw_datagram_codec.hpp
/// Identity datagram codec — one datagram in, one frame out.
///
/// For UDP use cases where each datagram is a single application message
/// (e.g. a pre-formatted market-data tick, a DNS reply). The codec emits
/// exactly one frame per `decode()` call via the `sink` callback and
/// consumes the entire datagram.
///
/// This is the datagram counterpart to `RawStreamCodec`. There is no
/// per-datagram framing overhead.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <span>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "eph/codec/detail/span_packet_view.hpp"
#include "eph/core/codec.hpp"
#include "eph/core/error.hpp"

namespace eph::codec {

namespace detail {

/// @brief Lazily-initialised logger for the raw datagram codec.
inline spdlog::logger* raw_datagram_codec_logger() {
    static auto* l = [] {
        auto lg = spdlog::get("codec.raw_datagram");
        if (!lg) {
            try {
                lg = spdlog::stdout_color_mt("codec.raw_datagram");
            } catch (const spdlog::spdlog_ex&) {
                lg = spdlog::get("codec.raw_datagram");
            }
        }
        if (!lg) lg = spdlog::default_logger();
        return lg.get();
    }();
    return l;
}

} // namespace detail

/// @brief Identity datagram codec: 1 datagram = 1 frame.
class RawDatagramCodec {
public:
    /// Zero-copy view into the datagram payload.
    using Frame         = std::span<const uint8_t>;
    using PacketViewRef = SpanPacketView&;

    static constexpr std::size_t max_overhead = 0;
    static constexpr bool        is_streaming = false;

    constexpr RawDatagramCodec() noexcept = default;

    /// @brief Emit exactly one frame spanning the whole datagram.
    ///
    /// Zero-length datagrams are rejected with `CodecBad` — there is no
    /// meaningful frame to emit. Empty `sink` (default-constructed
    /// `std::function`) is rejected with `InvalidConfig` so that an
    /// unwired caller surfaces a typed error instead of std::terminate
    /// via bad_function_call. Both rejection paths consume zero bytes
    /// from the datagram so the caller can either retry with a wired
    /// sink (in the InvalidConfig case) or fail-and-drop (in the
    /// CodecBad case). On success the datagram is fully consumed.
    ///
    /// @return the number of frames emitted (always 1 on success).
    template <class PacketView>
    [[nodiscard]] std::expected<std::size_t, core::ErrorInfo>
    decode(PacketView& dgram, core::OutputBuffer& /*out*/,
           const std::function<void(Frame)>& sink) noexcept {
        const std::size_t n = dgram.length();
        if (n == 0) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::raw_datagram_codec_logger(),
                "RawDatagramCodec::decode: empty datagram rejected");
            return std::unexpected(core::ErrorInfo{
                core::Error::CodecBad,
                "RawDatagramCodec: empty datagram"});
        }
        Frame frame{dgram.data(), n};
        // Datagram codecs must fully consume the input before returning.
        dgram.trim_front(n);
        // An empty std::function throws bad_function_call when invoked,
        // which would terminate the process via this function's noexcept.
        // Guard so a misconfigured caller (forgot to assign a sink, or
        // moved-from one) sees a typed error instead of std::terminate.
        // The DPDK Mold64 sibling already handles this; mirror the
        // discipline here for cross-backend symmetry.
        if (!sink) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::raw_datagram_codec_logger(),
                "RawDatagramCodec::decode: sink is empty (caller forgot "
                "to assign or moved-from); rejecting datagram of {} bytes", n);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "RawDatagramCodec::decode: sink is empty"});
        }
        sink(frame);
        SPDLOG_LOGGER_TRACE(detail::raw_datagram_codec_logger(),
            "RawDatagramCodec::decode: delivered {} bytes", n);
        return std::size_t{1};
    }

    /// @brief Encode a frame as a bare copy into `buf`.
    [[nodiscard]] std::expected<std::size_t, core::ErrorInfo>
    encode(uint8_t* buf, std::size_t cap, Frame payload) noexcept {
        if (payload.size() > cap) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::raw_datagram_codec_logger(),
                "RawDatagramCodec::encode: buffer too small (need {}, have {})",
                payload.size(), cap);
            return std::unexpected(core::ErrorInfo{
                core::Error::BufferFull,
                "RawDatagramCodec::encode: destination buffer too small"});
        }
        if (!payload.empty()) {
            std::memcpy(buf, payload.data(), payload.size());
        }
        return payload.size();
    }
};

static_assert(core::DatagramCodec<RawDatagramCodec>,
              "RawDatagramCodec must satisfy the DatagramCodec concept");

} // namespace eph::codec
