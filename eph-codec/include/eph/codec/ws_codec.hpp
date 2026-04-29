#pragma once

/// @file ws_codec.hpp
/// Stateful WebSocket codec — frame parsing + control-frame state machine.
///
/// This replaces the legacy:
///   - `eph::net::WsFramer` (in eph-transport/include/eph/transport/ws_framer.hpp)
///   - The WS control-frame handling logic in
///     `eph-transport/include/eph/transport/detail/frame_processor.hpp`
///
/// Responsibilities folded into the codec:
///   1. Incremental decode of RFC 6455 frames on top of a PacketView.
///   2. Fragmentation reassembly across multiple frames of the same
///      message (RFC 6455 §5.4). The reassembled payload lives in an
///      internal buffer and is returned as a span to the caller; the
///      span is valid until the next call to `decode()`.
///   3. Auto-respond to control frames via the `OutputBuffer` sink:
///        - ping → encode pong with identical payload, return Ok(None)
///        - close → encode a close-ack (possibly overriding an invalid
///          peer status code per RFC 6455 §7.4.1), return
///          Err(WsCloseReceived) so the caller can drain and shut down.
///   4. Reject protocol violations (reserved bits, invalid opcodes,
///      oversized control frames, non-minimal length encoding, etc.)
///      with a typed `CodecBad`/`WsFrameBad` ErrorInfo.
///
/// Design note — we deliberately REUSE the battle-tested
/// `eph::net::ws::decode_frame` / `encode_frame` from
/// `eph-transport/include/eph/transport/detail/websocket.hpp`. That file is
/// The codec is a thin stateful shell on top of the underlying
/// wire-format helpers.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "eph/codec/detail/span_packet_view.hpp"
#include "eph/codec/detail/ws_inflate.hpp"
#include "eph/core/codec.hpp"
#include "eph/core/error.hpp"
#include "eph/net/detail/websocket.hpp"  // ws::decode_frame, encode_frame, opcode

namespace eph::codec {

namespace detail {

/// @brief Lazily-initialised logger for the WebSocket codec.
inline spdlog::logger* ws_codec_logger() {
    static auto* l = [] {
        auto lg = spdlog::get("codec.ws");
        if (!lg) {
            try {
                lg = spdlog::stdout_color_mt("codec.ws");
            } catch (const spdlog::spdlog_ex&) {
                lg = spdlog::get("codec.ws");
            }
        }
        return lg.get();
    }();
    return l;
}

} // namespace detail

/// @brief Configuration for `WsCodec`.
struct WsCodecConfig {
    /// Maximum per-message payload after reassembly. Oversized fragmented
    /// messages are dropped with `CodecOverflow`. 1 MiB is the historical
    /// production default — it covers every frame size HFT venues actually
    /// emit (Binance bookTicker / depth / agg-trades all max out at well
    /// under 64 KiB) with comfortable headroom.
    std::size_t max_message_size = 1u << 20;  // 1 MiB

    /// If true, automatically append a pong frame to the OutputBuffer on
    /// receiving a ping. Set to false if the application wants to handle
    /// ping manually (then the codec returns Ok(None) without writing a
    /// response).
    bool auto_pong = true;

    /// If true, automatically append a close-ack frame to the OutputBuffer
    /// on receiving a close frame. The caller still gets
    /// Err(WsCloseReceived) so it can drain the TX and shut down.
    bool auto_close_ack = true;

    /// RFC 7692 permessage-deflate inflate enable. Set to true ONLY by
    /// the `TcpStream` factory after the WS handshake confirmed the
    /// server accepted the extension — manual users typically leave this
    /// at false and call `enable_permessage_deflate()` instead so the
    /// flag and the `server_no_context_takeover` plumbing stay in sync.
    /// Outbound deflate (compressing client→server) is intentionally
    /// not implemented: crypto venues never require client-side
    /// compression and adding it would double the codec's surface.
    bool permessage_deflate = false;

    /// When true, the inflater is reset after each message — the
    /// server told us in the handshake that it would not preserve its
    /// LZ77 sliding window across messages
    /// (`server_no_context_takeover`). When false (RFC 7692 default),
    /// the inflater keeps state and message N+1 can reference back-
    /// references to message N. Ignored when `permessage_deflate=false`.
    bool server_no_context_takeover = false;
};

/// @brief Stateful WebSocket codec.
class WsCodec {
public:
    /// Zero-copy view of the application payload. Valid until the next
    /// decode() call.
    using Frame         = std::span<const uint8_t>;
    using PacketViewRef = SpanPacketView&;

    /// RFC 6455 max header: 2 base + 8 extended length + 4 mask.
    static constexpr std::size_t max_overhead = eph::net::ws::kMaxFrameHeaderLen;  // 14
    static constexpr bool        is_streaming = true;

    /// @brief Default-construct with default config.
    WsCodec() noexcept : cfg_{} {}

    /// @brief Construct with explicit config.
    explicit WsCodec(WsCodecConfig cfg) noexcept : cfg_(cfg) {}

    /// @brief Enable RFC 7692 permessage-deflate inflate for subsequent
    ///        decode calls.
    ///
    /// Invoked by the WS-aware `TcpStream` factory after the HTTP
    /// upgrade response confirmed the server accepted the extension.
    /// Application code that drives the codec directly (e.g. tests
    /// against a recorded byte stream) calls this to mirror the
    /// negotiation outcome.
    ///
    /// @param server_no_ctx_takeover  True if the server included
    ///        `server_no_context_takeover` in its
    ///        `Sec-WebSocket-Extensions` response. The inflater state
    ///        will be reset after every inbound message in that mode.
    ///        Default false (RFC 7692 default — context preserved).
    void enable_permessage_deflate(
        bool server_no_ctx_takeover = false) noexcept {
        cfg_.permessage_deflate = true;
        cfg_.server_no_context_takeover = server_no_ctx_takeover;
        SPDLOG_LOGGER_INFO(detail::ws_codec_logger(),
            "WsCodec: permessage-deflate enabled "
            "(server_no_context_takeover={})", server_no_ctx_takeover);
    }

    /// @brief Read-only accessor for the deflate-bytes-in counter
    ///        (compressed bytes seen on the wire). Unused when
    ///        permessage-deflate is not negotiated.
    [[nodiscard]] std::uint64_t ws_deflate_bytes_in() const noexcept {
        return deflate_bytes_in_.load(std::memory_order_relaxed);
    }

    /// @brief Read-only accessor for the deflate-bytes-out counter
    ///        (plaintext bytes produced by inflate).
    [[nodiscard]] std::uint64_t ws_deflate_bytes_out() const noexcept {
        return deflate_bytes_out_.load(std::memory_order_relaxed);
    }

    /// @brief True if permessage-deflate is currently enabled on this
    ///        codec instance. Exposed for testability.
    [[nodiscard]] bool permessage_deflate_enabled() const noexcept {
        return cfg_.permessage_deflate;
    }

    /// @brief Incrementally decode one WebSocket frame from `view`.
    ///
    /// Semantics (StreamCodec concept):
    ///
    ///   - Not enough bytes for a complete frame → Ok(None), view is
    ///     unchanged so the caller can retry after more RX.
    ///   - Ping received → pong appended to `out` (if auto_pong),
    ///     view trimmed past the ping, returns Ok(None). Caller should
    ///     loop and call decode() again.
    ///   - Pong received → view trimmed past it, returns Ok(None).
    ///   - Close received → close-ack appended to `out` (if
    ///     auto_close_ack), view trimmed past the close,
    ///     returns Err(WsCloseReceived) so the caller can drain and
    ///     shut down.
    ///   - Data frame, single-frame message (FIN=1) → returns Ok(Some(payload)).
    ///   - Data frame, fragment → accumulate into internal buffer,
    ///     returns Ok(None). Caller loops until the final fragment
    ///     delivers the reassembled message.
    ///   - Protocol violation → returns Err with code WsFrameBad / CodecBad /
    ///     CodecOverflow as appropriate. The codec state is NOT reset;
    ///     the caller is expected to drop the connection.
    template <class PacketView>
    [[nodiscard]] std::expected<std::optional<Frame>, core::ErrorInfo>
    decode(PacketView& view, core::OutputBuffer& out) noexcept {
        namespace ws = eph::net::ws;

        const std::size_t avail = view.length();
        if (avail < 2) {
            // Even the shortest frame header (unmasked, payload < 126) is
            // 2 bytes. Anything less = caller must re-poll.
            return std::optional<Frame>{};
        }

        auto decoded = ws::decode_frame(view.data(), avail,
                                        /*allow_rsv1=*/cfg_.permessage_deflate);
        if (!decoded) {
            switch (decoded.error()) {
            case ws::DecodeError::kIncomplete:
                // Header or payload not fully present yet — normal back-pressure
                // case; return None so the caller waits for more bytes.
                SPDLOG_LOGGER_TRACE(detail::ws_codec_logger(),
                    "WsCodec::decode: incomplete frame, avail={}", avail);
                return std::optional<Frame>{};

            case ws::DecodeError::kReservedBits:
            case ws::DecodeError::kFragmentedControl:
            case ws::DecodeError::kControlPayloadTooLarge:
            case ws::DecodeError::kInvalidOpcode:
            case ws::DecodeError::kInvalidLengthEncoding:
            case ws::DecodeError::kInvalidCloseFrame:
                SPDLOG_LOGGER_WARN(detail::ws_codec_logger(),
                    "WsCodec::decode: protocol error: {}",
                    ws::decode_error_name(decoded.error()));
                return std::unexpected(core::ErrorInfo{
                    core::Error::WsFrameBad,
                    ws::decode_error_name(decoded.error()).data()});
            }
            // Fallthrough should be unreachable; be defensive.
            return std::unexpected(core::ErrorInfo{
                core::Error::WsFrameBad,
                "WsCodec::decode: unknown decode error"});
        }

        const auto& frame = *decoded;
        const std::size_t consumed = frame.total_len;

        // ── Control frames ────────────────────────────────────────────
        // RFC 7692 §6.1: control frames MUST NOT have RSV1 set even when
        // permessage-deflate is negotiated. If we see one, treat it as a
        // protocol violation rather than try to inflate a 125-byte
        // control payload.
        if (frame.rsv1 && frame.is_control()) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::ws_codec_logger(),
                "WsCodec::decode: RSV1 set on control opcode 0x{:02X} "
                "(RFC 7692 §6.1)", frame.opcode);
            return std::unexpected(core::ErrorInfo{
                core::Error::WsFrameBad,
                "WsCodec::decode: RSV1 set on control frame"});
        }

        if (frame.is_ping()) {
            SPDLOG_LOGGER_DEBUG(detail::ws_codec_logger(),
                "WsCodec::decode: ping, payload_len={}", frame.payload_len);
            if (cfg_.auto_pong) {
                auto err = emit_pong(out, frame);
                if (!err) return std::unexpected(err.error());
            }
            view.trim_front(consumed);
            return std::optional<Frame>{};
        }

        if (frame.is_pong()) {
            SPDLOG_LOGGER_DEBUG(detail::ws_codec_logger(),
                "WsCodec::decode: pong, payload_len={}", frame.payload_len);
            view.trim_front(consumed);
            return std::optional<Frame>{};
        }

        if (frame.is_close()) {
            uint16_t code = frame.close_status_code();
            SPDLOG_LOGGER_INFO(detail::ws_codec_logger(),
                "WsCodec::decode: close received, code={}", code);
            if (cfg_.auto_close_ack) {
                // RFC 6455 §7.4.1: if the peer's code is reserved/invalid
                // (0-999, 1004-1006, 1015, 1016-2999 etc.) we MUST NOT echo
                // it back — respond with kProtocolError instead.
                uint16_t ack_code = code;
                if (frame.payload_len == 0) {
                    ack_code = ws::close_code::kNormal;
                } else if (!ws::is_valid_close_code(code)) {
                    SPDLOG_LOGGER_WARN(detail::ws_codec_logger(),
                        "WsCodec::decode: invalid close code {}, "
                        "responding with kProtocolError", code);
                    ack_code = ws::close_code::kProtocolError;
                }
                auto err = emit_close(out, ack_code);
                if (!err) return std::unexpected(err.error());
            }
            view.trim_front(consumed);
            // Signal to the caller that the peer has initiated close.
            return std::unexpected(core::ErrorInfo{
                core::Error::WsCloseReceived,
                "WsCodec::decode: peer sent close frame"});
        }

        // ── Data frames ───────────────────────────────────────────────
        // Guard: unexpected opcode (should have been caught by decode_frame
        // via kInvalidOpcode, but defensive).
        if (!frame.is_data()) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::ws_codec_logger(),
                "WsCodec::decode: non-data, non-control opcode 0x{:02X}",
                frame.opcode);
            return std::unexpected(core::ErrorInfo{
                core::Error::WsFrameBad,
                "WsCodec::decode: unexpected non-data frame"});
        }

        const bool is_continuation = (frame.opcode == ws::opcode::kContinuation);
        const bool is_final        = frame.fin;

        // Start of a new message — if we had a partial reassembly from a
        // previous message, drop it (stale state, not recoverable).
        // Note: frag_opcode_ is the authoritative "in-flight fragment"
        // signal. A non-empty frag_buf_ with frag_opcode_==0 is just
        // retained scratch from the previous (cleanly delivered) message
        // — clear silently without the WARN.
        if (!is_continuation) {
            if (frag_opcode_ != 0) {
                SPDLOG_LOGGER_WARN(detail::ws_codec_logger(),
                    "WsCodec::decode: new message started while {} bytes "
                    "of previous fragment pending, discarding",
                    frag_buf_.size());
            }
            frag_buf_.clear();
            frag_opcode_ = frame.opcode;
            // RFC 7692 §6: only the first frame of a message carries
            // the per-message-compressed bit (RSV1). Continuation
            // frames inherit the bit. Capture once at message start.
            compressed_msg_ = frame.rsv1;
            if (compressed_msg_ && !cfg_.permessage_deflate) [[unlikely]] {
                // decode_frame should have rejected with kReservedBits
                // already (we passed allow_rsv1=false), but defend
                // anyway — silent acceptance would feed garbage to the
                // application.
                SPDLOG_LOGGER_WARN(detail::ws_codec_logger(),
                    "WsCodec::decode: compressed frame seen but "
                    "permessage-deflate not negotiated");
                return std::unexpected(core::ErrorInfo{
                    core::Error::CodecBad,
                    "WsCodec::decode: compressed frame without "
                    "permessage-deflate negotiation"});
            }
        } else if (frag_opcode_ == 0) {
            // Continuation without a prior in-flight fragmented message =
            // protocol violation. NOTE: we check frag_opcode_ alone — not
            // frag_buf_.empty() — because the FIN of the previous fragmented
            // message resets frag_opcode_ but deliberately leaves frag_buf_
            // populated so the returned span stays valid until the caller
            // makes the next decode() call. A stale-but-non-empty frag_buf_
            // is therefore not a signal that a message is in flight; an
            // orphan continuation arriving in that window must still be
            // rejected, otherwise its payload would be appended to the
            // previous message's bytes and delivered to the application
            // as a corrupted "reassembly".
            SPDLOG_LOGGER_WARN(detail::ws_codec_logger(),
                "WsCodec::decode: continuation frame without initial "
                "data frame");
            return std::unexpected(core::ErrorInfo{
                core::Error::WsFrameBad,
                "WsCodec::decode: orphan continuation frame"});
        } else if (frame.rsv1) [[unlikely]] {
            // RFC 7692 §6.1: continuation frames MUST NOT set RSV1.
            // The compressed-message bit is per-message, not per-frame.
            SPDLOG_LOGGER_WARN(detail::ws_codec_logger(),
                "WsCodec::decode: RSV1 set on continuation frame");
            return std::unexpected(core::ErrorInfo{
                core::Error::WsFrameBad,
                "WsCodec::decode: RSV1 on continuation frame"});
        }

        // ── Compressed message path (RFC 7692) ───────────────────────
        // Always accumulate even single-frame compressed messages into
        // frag_buf_ — we need a contiguous compressed buffer to feed
        // the inflater. The zero-copy fast path is reserved for the
        // uncompressed case (it's the > 95% case for non-deflate
        // streams; for deflate streams the inflate copy is unavoidable
        // by definition).
        if (compressed_msg_) {
            const std::size_t new_size = frag_buf_.size() + frame.payload_len;
            if (new_size > cfg_.max_message_size) [[unlikely]] {
                SPDLOG_LOGGER_WARN(detail::ws_codec_logger(),
                    "WsCodec::decode: compressed accum exceeds max "
                    "(accumulated={} max={})",
                    new_size, cfg_.max_message_size);
                frag_buf_.clear();
                view.trim_front(consumed);
                return std::unexpected(core::ErrorInfo{
                    core::Error::CodecOverflow,
                    "WsCodec::decode: compressed payload exceeds max"});
            }
            if (frame.payload && frame.payload_len > 0) {
                const std::size_t old_size = frag_buf_.size();
                frag_buf_.resize(new_size);
                std::memcpy(frag_buf_.data() + old_size,
                            frame.payload, frame.payload_len);
                if (frame.masked) {
                    ws::apply_mask(frag_buf_.data() + old_size,
                                   frame.payload_len, frame.mask_key);
                }
            }
            view.trim_front(consumed);

            if (!is_final) {
                // More compressed fragments coming.
                return std::optional<Frame>{};
            }

            // Final fragment — feed the accumulated compressed payload
            // into the inflater and deliver the inflated plaintext.
            // Bookkeeping: track wire bytes (in) and inflated bytes (out)
            // for the deflate-ratio metric. Lazy-construct the inflater
            // here so non-deflate codec instances pay zero cost.
            if (!inflater_) {
                inflater_ = std::make_unique<detail::WsInflater>();
                inflater_->max_inflated_size = cfg_.max_message_size;
            }
            const std::size_t in_bytes = frag_buf_.size();
            auto inflated = inflater_->inflate_message(
                std::span<const uint8_t>{frag_buf_.data(), frag_buf_.size()},
                /*reset_after=*/cfg_.server_no_context_takeover);
            // The compressed accumulator is per-message; whether or not
            // inflate succeeded we drop it before returning — leaving
            // stale compressed bytes around could be confused with a
            // pending fragment by the next decode() call.
            frag_buf_.clear();
            frag_opcode_ = 0;
            compressed_msg_ = false;
            if (!inflated) {
                return std::unexpected(inflated.error());
            }
            // Update the per-codec deflate counters. Relaxed memory
            // ordering is fine — these are reader-friendly stats, not
            // synchronisation points (matches the StreamMetric pattern
            // used by the four backends).
            deflate_bytes_in_.fetch_add(in_bytes,
                                        std::memory_order_relaxed);
            deflate_bytes_out_.fetch_add(inflated->size(),
                                         std::memory_order_relaxed);
            SPDLOG_LOGGER_TRACE(detail::ws_codec_logger(),
                "WsCodec::decode: inflated {} -> {} bytes (ratio {:.2f}x)",
                in_bytes, inflated->size(),
                in_bytes ? double(inflated->size()) / double(in_bytes)
                         : 0.0);
            return std::optional<Frame>{*inflated};
        }

        // Fast path: single-frame uncompressed message (first=FIN=1, not continuation).
        const bool single_frame = !is_continuation && is_final;
        if (single_frame && frag_buf_.empty()) {
            if (frame.payload_len > cfg_.max_message_size) [[unlikely]] {
                SPDLOG_LOGGER_WARN(detail::ws_codec_logger(),
                    "WsCodec::decode: oversized single-frame message, "
                    "payload_len={} max={}",
                    frame.payload_len, cfg_.max_message_size);
                view.trim_front(consumed);
                return std::unexpected(core::ErrorInfo{
                    core::Error::CodecOverflow,
                    "WsCodec::decode: single-frame payload exceeds max"});
            }
            Frame payload{frame.payload, static_cast<std::size_t>(frame.payload_len)};
            view.trim_front(consumed);
            // Note: server-to-client frames are unmasked in practice
            // (masked is almost always false here). We still honour the
            // mask bit defensively — if the remote sent a masked frame we
            // must unmask before delivering. The ws::decode_frame output
            // is zero-copy, so we'd need to unmask in-place, which would
            // mutate the PacketView storage. That's allowed per the
            // PacketView contract (writable_data()), but we keep it simple
            // by unmasking into the reassembly buffer instead.
            if (frame.masked && frame.payload_len > 0) {
                frag_buf_.assign(frame.payload,
                                 frame.payload + frame.payload_len);
                ws::apply_mask(frag_buf_.data(), frag_buf_.size(),
                               frame.mask_key);
                Frame out_frame{frag_buf_.data(), frag_buf_.size()};
                // NOTE: this frame view is valid only until the next
                // decode() call, matching the contract.
                return std::optional<Frame>{out_frame};
            }
            return std::optional<Frame>{payload};
        }

        // Slow path: fragmented uncompressed message — accumulate into frag_buf_.
        const std::size_t new_size = frag_buf_.size() + frame.payload_len;
        if (new_size > cfg_.max_message_size) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::ws_codec_logger(),
                "WsCodec::decode: oversized reassembled message, "
                "accumulated={} max={}", new_size, cfg_.max_message_size);
            frag_buf_.clear();
            view.trim_front(consumed);
            return std::unexpected(core::ErrorInfo{
                core::Error::CodecOverflow,
                "WsCodec::decode: reassembled payload exceeds max"});
        }

        if (frame.payload && frame.payload_len > 0) {
            const std::size_t old_size = frag_buf_.size();
            frag_buf_.resize(new_size);
            std::memcpy(frag_buf_.data() + old_size,
                        frame.payload, frame.payload_len);
            if (frame.masked) {
                ws::apply_mask(frag_buf_.data() + old_size,
                               frame.payload_len, frame.mask_key);
            }
        }
        view.trim_front(consumed);

        if (!is_final) {
            // More fragments coming.
            return std::optional<Frame>{};
        }

        // Reassembly complete — deliver as a single span. The span is
        // valid until the next decode() call (when frag_buf_ may be
        // cleared or resized).
        Frame reassembled{frag_buf_.data(), frag_buf_.size()};
        // Reset fragmentation state so we can start the next message
        // fresh on the following decode() call. The caller gets the
        // span NOW — it must be consumed before the next decode().
        // To preserve the buffer long enough we defer the clear by
        // just leaving it; clearing happens implicitly when the next
        // non-continuation frame arrives. We DO reset frag_opcode_ so
        // orphan-continuation detection works.
        frag_opcode_ = 0;
        return std::optional<Frame>{reassembled};
    }

    /// @brief Encode a binary application frame.
    ///
    /// Wraps `ws::encode_frame` with opcode=kBinary and FIN=1. Returns the
    /// number of bytes written, or `BufferFull` if `cap` is insufficient.
    [[nodiscard]] std::expected<std::size_t, core::ErrorInfo>
    encode(uint8_t* buf, std::size_t cap, Frame payload) noexcept {
        namespace ws = eph::net::ws;
        const std::size_t needed = ws::total_frame_size(payload.size());
        if (needed == SIZE_MAX || needed > cap) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::ws_codec_logger(),
                "WsCodec::encode: buffer too small (need {}, have {})",
                needed, cap);
            return std::unexpected(core::ErrorInfo{
                core::Error::BufferFull,
                "WsCodec::encode: destination buffer too small"});
        }
        const std::size_t n = ws::encode_frame(
            buf, ws::opcode::kBinary,
            payload.data(), payload.size(), /*fin=*/true);
        if (n == 0) [[unlikely]] {
            return std::unexpected(core::ErrorInfo{
                core::Error::CodecBad,
                "WsCodec::encode: ws::encode_frame rejected payload"});
        }
        return n;
    }

private:
    /// @brief Write a pong frame into `out` echoing the ping payload.
    std::expected<void, core::ErrorInfo>
    emit_pong(core::OutputBuffer& out,
              const eph::net::ws::DecodedFrame& ping) noexcept {
        namespace ws = eph::net::ws;
        // Pong payload must equal the ping payload (RFC 6455 §5.5.3).
        // Header is at most kMaxFrameHeaderLen + 125 (control payload cap).
        constexpr std::size_t kMaxPong = ws::kMaxFrameHeaderLen + ws::kMaxControlPayloadLen;
        uint8_t scratch[kMaxPong];

        // If the incoming ping was masked, we need the UNMASKED payload for
        // the pong echo. Copy and unmask into a small stack buffer first.
        uint8_t  unmask_buf[ws::kMaxControlPayloadLen];
        const uint8_t* pong_payload = ping.payload;
        auto pong_len = ping.payload_len;
        if (ping.masked && ping.payload && ping.payload_len > 0) {
            std::memcpy(unmask_buf, ping.payload, ping.payload_len);
            ws::apply_mask(unmask_buf, ping.payload_len, ping.mask_key);
            pong_payload = unmask_buf;
        }

        const std::size_t n =
            ws::build_pong_frame(scratch, pong_payload, pong_len);
        if (n == 0) [[unlikely]] {
            return std::unexpected(core::ErrorInfo{
                core::Error::CodecBad,
                "WsCodec: build_pong_frame failed"});
        }
        return out.append(std::span<const uint8_t>(scratch, n));
    }

    /// @brief Write a close frame into `out` with the given status code.
    std::expected<void, core::ErrorInfo>
    emit_close(core::OutputBuffer& out, uint16_t status_code) noexcept {
        namespace ws = eph::net::ws;
        // Close frame: header + 2-byte status code (no reason).
        uint8_t scratch[ws::kMaxFrameHeaderLen + 2];
        const std::size_t n = ws::build_close_frame(scratch, status_code);
        if (n == 0) [[unlikely]] {
            return std::unexpected(core::ErrorInfo{
                core::Error::CodecBad,
                "WsCodec: build_close_frame failed"});
        }
        return out.append(std::span<const uint8_t>(scratch, n));
    }

    WsCodecConfig        cfg_;
    std::vector<uint8_t> frag_buf_;    ///< reassembly scratch (persists across calls)
    uint8_t              frag_opcode_ = 0;  ///< opcode of the in-flight fragmented message
    /// True between the start of a compressed message (RSV1=1 on the
    /// first frame) and the FIN of that message. Lets continuation
    /// frames inherit the compression bit per RFC 7692 §6.
    bool                 compressed_msg_ = false;
    /// Lazily-created inflater, only allocated when the first
    /// compressed frame arrives. unique_ptr so the codec stays small
    /// for non-deflate consumers (sizeof(WsCodec) ≈ vector + bool).
    std::unique_ptr<detail::WsInflater> inflater_{};
    /// Per-codec deflate counters. atomic<uint64_t> so external
    /// observers can read concurrently without locking, mirroring the
    /// `StreamMetric` array pattern. Relaxed loads/stores are correct
    /// here because the values are advisory diagnostics, not
    /// synchronisation primitives.
    alignas(64) std::atomic<std::uint64_t> deflate_bytes_in_{0};
    std::atomic<std::uint64_t>             deflate_bytes_out_{0};
};

static_assert(core::StreamCodec<WsCodec>,
              "WsCodec must satisfy the StreamCodec concept");

} // namespace eph::codec
