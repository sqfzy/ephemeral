#pragma once

/// @file mold64_codec.hpp
/// MoldUDP64 datagram codec — wraps `eph::itch::parse_moldudp64` into the
/// DatagramCodec interface.
///
/// MoldUDP64 is Nasdaq's multicast container for ITCH data distribution
/// (see `eph-itch/include/eph/itch/moldudp64.hpp` for the wire format).
/// A single UDP datagram carries a 20-byte header plus `message_count`
/// length-prefixed ITCH messages. The codec expands that into one Frame per
/// message via the `sink` callback.
///
/// Design notes:
///   - We intentionally do NOT duplicate the parsing logic from
///     `eph-itch/moldudp64.hpp`. Instead we delegate to `parse_moldudp64`
///     which is header-only and already has a message callback API. The
///     old header stays untouched.
///   - Gap detection is folded into the codec: we track `expected_seq_` and
///     increment `gaps_` when the incoming datagram's first sequence number
///     does not equal `expected_seq_`. Out-of-order packets are still
///     delivered — gap recovery is the application's job — but the codec
///     surfaces the count via `gap_count()` for metrics.
///   - The ErrorInfo path: `parse_moldudp64` silently returns 0 on a bad
///     header. We convert that to `CodecBad` by re-parsing the header
///     explicitly before the delivery loop, which gives the transport a
///     signal to drop the datagram.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <span>

#include "eph/codec/detail/span_packet_view.hpp"
#include "eph/core/codec.hpp"
#include "eph/core/error.hpp"
#include "eph/core/log.hpp"
#include "eph/itch/moldudp64.hpp"

namespace eph::codec {

namespace detail {

/// @brief Lazily-initialised logger for the Mold64 codec.
inline spdlog::logger* mold64_codec_logger() {
    static spdlog::logger* l = ::eph::log::get("codec.mold64");
    return l;
}

} // namespace detail

/// @brief Configuration for `Mold64Codec`.
struct Mold64Config {
    /// Initial expected sequence number. Defaults to 1 which matches the
    /// MoldUDP64 convention that the first delivered message has seq == 1.
    uint64_t initial_expected_seq = 1;
};

/// @brief MoldUDP64 datagram codec satisfying `core::DatagramCodec`.
class Mold64Codec {
public:
    /// Per-message frame: sequence number + zero-copy payload view.
    struct Frame {
        uint64_t                  seq_num;
        std::span<const uint8_t>  payload;  ///< single ITCH message
    };

    using PacketViewRef = SpanPacketView&;

    /// MoldUDP64 header is 20 bytes (10 session + 8 seq + 2 count). The
    /// per-message 2-byte length is accounted for inside the delivery loop
    /// — `max_overhead` on the codec refers to the *fixed* per-datagram
    /// framing only.
    static constexpr std::size_t max_overhead = eph::itch::moldudp64::kHeaderLen;  // 20
    static constexpr bool        is_streaming = false;

    constexpr Mold64Codec() noexcept = default;
    constexpr explicit Mold64Codec(Mold64Config cfg) noexcept
        : expected_seq_(cfg.initial_expected_seq) {}

    /// @brief Decode a full MoldUDP64 datagram and emit each contained
    ///        ITCH message via `sink`.
    ///
    /// Returns the number of frames emitted, or an `ErrorInfo` if the
    /// header is malformed / truncated. Empty `sink` (default-constructed
    /// `std::function`) is rejected with `InvalidConfig` before any
    /// datagram bytes are consumed (so the caller can retry with a
    /// wired sink); a malformed-header datagram is consumed and the
    /// codec returns `CodecBad`. See R56/R57 for the sibling
    /// RawDatagramCodec contract.
    ///
    /// Gap handling:
    ///   - If `header.sequence_number > expected_seq_`, we observe a gap
    ///     of size `header.sequence_number - expected_seq_`, increment
    ///     `gaps_`, and advance `expected_seq_` past the missing messages
    ///     after delivery.
    ///   - If `header.sequence_number < expected_seq_`, the datagram is
    ///     a duplicate / replay; we still deliver it (the application may
    ///     want to process it) but do not rewind `expected_seq_`.
    ///   - End-of-session marker (`message_count == 0xFFFF`) delivers no
    ///     frames and is logged.
    template <class PacketView>
    [[nodiscard]] std::expected<std::size_t, core::ErrorInfo>
    decode(PacketView& dgram, core::OutputBuffer& /*out*/,
           const std::function<void(Frame)>& sink) noexcept {
        const std::size_t len = dgram.length();
        const uint8_t*    data = dgram.data();

        // An empty std::function throws bad_function_call when invoked
        // inside the parse_moldudp64 callback below, terminating the
        // process via this function's noexcept. Surface the misuse as a
        // typed InvalidConfig before we touch any datagram bytes.
        // Mirrors the same defense applied to RawDatagramCodec (R56).
        if (!sink) [[unlikely]] {
            EPH_LOG_WARN(detail::mold64_codec_logger(),
                "Mold64Codec::decode: sink is empty (caller forgot to "
                "assign or moved-from); rejecting datagram of {} bytes", len);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "Mold64Codec::decode: sink is empty"});
        }

        // Parse header first so we can surface a typed error on malformed
        // datagrams (parse_moldudp64 silently returns 0 on bad headers).
        auto hdr = eph::itch::parse_moldudp64_header(data, len);
        if (!hdr) [[unlikely]] {
            EPH_LOG_WARN(detail::mold64_codec_logger(),
                "Mold64Codec::decode: malformed header, len={}", len);
            // Consume the datagram regardless — contract says we must.
            dgram.trim_front(len);
            return std::unexpected(core::ErrorInfo{
                core::Error::CodecBad,
                "Mold64Codec: malformed MoldUDP64 header"});
        }

        // End-of-session: no frames, no gap bookkeeping.
        if (hdr->message_count == eph::itch::moldudp64::kEndOfSession) {
            EPH_LOG_INFO(detail::mold64_codec_logger(),
                "Mold64Codec::decode: end-of-session for session='{}'",
                hdr->session);
            dgram.trim_front(len);
            return std::size_t{0};
        }

        // Gap detection — only when the incoming seq is ahead of our
        // expected. A datagram arriving at exactly expected_seq_ is the
        // steady-state case and silently advances the counter.
        if (hdr->sequence_number > expected_seq_) {
            const uint64_t gap = hdr->sequence_number - expected_seq_;
            gaps_ += gap;
            EPH_LOG_WARN(detail::mold64_codec_logger(),
                "Mold64Codec::decode: gap detected, expected={} got={} "
                "(missing {} msgs, total gaps={})",
                expected_seq_, hdr->sequence_number, gap, gaps_);
        } else if (hdr->sequence_number < expected_seq_) {
            EPH_LOG_DEBUG(detail::mold64_codec_logger(),
                "Mold64Codec::decode: duplicate/replay, expected={} got={} "
                "(delivering but not rewinding)",
                expected_seq_, hdr->sequence_number);
        }

        // Delegate the per-message iteration to the eph-itch helper. The
        // lambda captures `sink` and forwards each message as a Frame.
        std::size_t delivered = 0;
        eph::itch::parse_moldudp64(data, len,
            [&](const uint8_t* msg_data, uint16_t msg_len, uint64_t seq) noexcept {
                sink(Frame{
                    .seq_num = seq,
                    .payload = std::span<const uint8_t>{msg_data, msg_len},
                });
                ++delivered;
            });

        // Advance expected_seq_ past the just-delivered batch, but only
        // when the batch did not rewind the stream.
        if (hdr->sequence_number + delivered > expected_seq_) {
            expected_seq_ = hdr->sequence_number + delivered;
        }

        // Datagram codecs must consume the whole input on success.
        dgram.trim_front(len);
        EPH_LOG_TRACE(detail::mold64_codec_logger(),
            "Mold64Codec::decode: delivered {} msgs, expected_seq now {}",
            delivered, expected_seq_);
        return delivered;
    }

    /// @brief Encode a single ITCH message into a minimal MoldUDP64 datagram.
    ///
    /// Produces a 20-byte header (10 NUL session bytes + seq + count=1),
    /// followed by a 2-byte length and the payload. This is primarily a
    /// convenience for tests and is NOT intended to cover the full producer
    /// side of MoldUDP64 (batching, retransmit, session ids); applications
    /// wanting a proper producer should build the datagram themselves.
    [[nodiscard]] std::expected<std::size_t, core::ErrorInfo>
    encode(uint8_t* buf, std::size_t cap, Frame frame) noexcept {
        // Bound payload BEFORE computing `needed` — otherwise a caller-
        // supplied span with size() > SIZE_MAX - 22 would silently wrap
        // around in the addition below, slip past the `needed > cap` guard
        // (cap is small, wrapped needed is also small), and reach the
        // memcpy with mlen = static_cast<uint16_t>(SIZE_MAX) = 0xFFFF —
        // i.e. a 65535-byte heap overflow. The 16-bit length prefix is
        // the wire-format ceiling, so reject anything bigger up front.
        if (frame.payload.size() > 0xFFFFu) [[unlikely]] {
            EPH_LOG_WARN(detail::mold64_codec_logger(),
                "Mold64Codec::encode: payload {} exceeds 65535 bytes",
                frame.payload.size());
            return std::unexpected(core::ErrorInfo{
                core::Error::CodecOverflow,
                "Mold64Codec::encode: payload exceeds 65535 bytes"});
        }
        const std::size_t needed =
            eph::itch::moldudp64::kHeaderLen + 2 + frame.payload.size();
        if (needed > cap) [[unlikely]] {
            EPH_LOG_WARN(detail::mold64_codec_logger(),
                "Mold64Codec::encode: buffer too small (need {}, have {})",
                needed, cap);
            return std::unexpected(core::ErrorInfo{
                core::Error::BufferFull,
                "Mold64Codec::encode: destination buffer too small"});
        }

        // 10-byte session (zeroed), 8-byte BE seq, 2-byte BE count=1.
        std::memset(buf, 0, eph::itch::moldudp64::kSessionLen);
        const uint64_t seq = frame.seq_num;
        for (int i = 0; i < 8; ++i) {
            buf[eph::itch::moldudp64::kSessionLen + i] =
                static_cast<uint8_t>((seq >> ((7 - i) * 8)) & 0xFF);
        }
        buf[eph::itch::moldudp64::kSessionLen + 8] = 0x00;
        buf[eph::itch::moldudp64::kSessionLen + 9] = 0x01;

        // Per-message length prefix.
        const auto mlen = static_cast<uint16_t>(frame.payload.size());
        const std::size_t msg_off = eph::itch::moldudp64::kHeaderLen;
        buf[msg_off + 0] = static_cast<uint8_t>((mlen >> 8) & 0xFF);
        buf[msg_off + 1] = static_cast<uint8_t>(mlen & 0xFF);
        if (!frame.payload.empty()) {
            std::memcpy(buf + msg_off + 2, frame.payload.data(), mlen);
        }
        return needed;
    }

    /// @brief The next sequence number the codec expects to see.
    [[nodiscard]] uint64_t expected_seq() const noexcept { return expected_seq_; }

    /// @brief Running total of gap-detected messages (missing sequence numbers).
    [[nodiscard]] uint64_t gap_count() const noexcept { return gaps_; }

private:
    uint64_t expected_seq_ = 1;
    uint64_t gaps_         = 0;
};

static_assert(core::DatagramCodec<Mold64Codec>,
              "Mold64Codec must satisfy the DatagramCodec concept");

} // namespace eph::codec
