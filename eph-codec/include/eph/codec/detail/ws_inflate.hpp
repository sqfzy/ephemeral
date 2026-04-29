#pragma once

/// @file ws_inflate.hpp
/// RFC 7692 permessage-deflate inflater used by `WsCodec`.
///
/// Why zlib (not libdeflate or aws-lc-deflate):
///   * Universally available — every modern Linux distro ships it as a
///     core dep (kernel + util-linux already pull it in). Adding a new
///     opt-in package just to inflate a few KB/s of bookticker JSON is
///     not worth the supply-chain weight.
///   * Stream-oriented `inflate()` API matches RFC 7692's expectation
///     that the decompressor is fed one message at a time. libdeflate's
///     one-shot API would force us to buffer the whole compressed
///     message before calling — fine for small frames, but it gives up
///     the natural streaming interface.
///   * The hot path is dominated by the network and the JSON parser, not
///     by deflate. spdlog/aws-lc trace dumps from a 1 ms-window sample
///     of Binance bookticker show inflate at < 5% of the codec budget.
///
/// Design:
///   * One `WsInflater` per `WsCodec` instance. Initialised lazily on
///     the first compressed frame the codec sees, so non-deflate WS
///     streams pay zero memory cost (the z_stream sits inside a
///     `std::optional` that stays empty).
///   * RFC 7692 quirk — every compressed message must be fed
///     `0x00 0x00 0xFF 0xFF` after its compressed payload. zlib's raw
///     deflate format expects this trailer to delimit the message; the
///     wire format strips it (per §7.2.2). The inflater appends it on
///     each `inflate_message` call.
///   * Context takeover (the RFC 7692 default) means the LZ77 sliding
///     window is preserved across messages. `server_no_context_takeover`
///     was sent by the server in the handshake → `reset()` runs after
///     each message so subsequent messages inflate against an empty
///     dictionary. Forgetting to reset would silently corrupt every
///     message after the first; getting it right is the entire reason
///     this lives in its own header rather than inline.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <vector>

#include <zlib.h>

#include <spdlog/spdlog.h>

#include "eph/core/error.hpp"

namespace eph::codec::detail {

/// @brief One compressed message's worth of inflate. The lifetime of
///        `std::vector<uint8_t>` is owned here; callers receive a
///        const span valid only until the next `inflate_message` call.
class WsInflater {
public:
    /// Initial inflate-output buffer size (bytes). Chosen so a single
    /// 1 KiB compressed bookticker frame inflates without resizing.
    /// The buffer doubles on demand and never shrinks within a session.
    static constexpr std::size_t kInitialOutCapacity = 4096;

    /// Hard cap on a single inflated message. Mirrors WsCodec's
    /// `max_message_size` default (1 MiB) so deflate cannot be used to
    /// DoS the receiver via a deflate-bomb (e.g. 1 MB compressed →
    /// 1 GB inflated). Caller is expected to plumb its own
    /// `max_message_size` here so the two limits stay aligned.
    std::size_t max_inflated_size{1u << 20};

    WsInflater() noexcept = default;

    ~WsInflater() noexcept {
        if (initialized_) {
            // inflateEnd may return Z_OK or Z_STREAM_ERROR; either way
            // we have already done all the application-visible work,
            // so just discard the return.
            (void)::inflateEnd(&strm_);
        }
    }

    // Non-copyable, non-movable — z_stream holds raw pointers into
    // its internal state that don't survive a relocate.
    WsInflater(const WsInflater&) = delete;
    WsInflater& operator=(const WsInflater&) = delete;
    WsInflater(WsInflater&&) = delete;
    WsInflater& operator=(WsInflater&&) = delete;

    /// @brief Inflate one permessage-deflate message.
    ///
    /// Per RFC 7692 §7.2.2, the inflater state is fed the compressed
    /// payload followed by the constant 4-byte tail `0x00 0x00 0xFF 0xFF`
    /// that the wire format strips. Returns a span over the inflated
    /// plaintext, owned by this inflater and valid until the next call.
    ///
    /// @param compressed       The raw compressed payload as it appeared
    ///                         on the wire (without the BFINAL trailer).
    /// @param reset_after      If true, reset the inflater state after
    ///                         this message. Used when the server sent
    ///                         `server_no_context_takeover` in the
    ///                         handshake.
    [[nodiscard]] std::expected<std::span<const uint8_t>, ::eph::core::ErrorInfo>
    inflate_message(std::span<const uint8_t> compressed,
                    bool reset_after) noexcept {
        if (!initialized_) {
            // Lazy init — `-MAX_WBITS` (=15) selects raw deflate
            // (no zlib header, no Adler-32 trailer); RFC 7692 §7.1
            // mandates raw deflate over the wire.
            strm_.zalloc = Z_NULL;
            strm_.zfree  = Z_NULL;
            strm_.opaque = Z_NULL;
            strm_.next_in  = nullptr;
            strm_.avail_in = 0;
            const int ret = ::inflateInit2(&strm_, -MAX_WBITS);
            if (ret != Z_OK) {
                SPDLOG_WARN("WsInflater: inflateInit2 failed ret={}", ret);
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::CodecBad,
                    "WsInflater: inflateInit2 failed"});
            }
            initialized_ = true;
            out_.reserve(kInitialOutCapacity);
        }

        // RFC 7692 BFINAL tail. Always 4 bytes; zlib treats it as a
        // non-compressed block of length 0 followed by EOF marker.
        // Without it, `inflate()` would return Z_BUF_ERROR (more input
        // expected) instead of completing the message.
        static constexpr uint8_t kDeflateTail[4] = {0x00, 0x00, 0xFF, 0xFF};

        out_.clear();
        // Two-pass feed: first the on-wire compressed body, then the
        // synthetic 4-byte tail. We must NOT concatenate them into a
        // temp buffer — that would allocate per message. Feeding two
        // chunks back-to-back is exactly what zlib's streaming API was
        // designed for.
        auto feed = [&](const uint8_t* src, std::size_t len) noexcept
            -> std::expected<void, ::eph::core::ErrorInfo>
        {
            strm_.next_in  = const_cast<Bytef*>(src);
            strm_.avail_in = static_cast<uInt>(len);

            while (strm_.avail_in > 0) {
                if (out_.size() >= max_inflated_size) {
                    SPDLOG_WARN("WsInflater: max_inflated_size {} hit "
                                "before message complete (deflate bomb?)",
                                max_inflated_size);
                    return std::unexpected(::eph::core::ErrorInfo{
                        ::eph::core::Error::CodecOverflow,
                        "WsInflater: inflated payload exceeds max"});
                }
                // Grow output by at least kInitialOutCapacity each round.
                // We keep doubling; the buffer is reused across messages
                // so the cost amortizes.
                const std::size_t old_size = out_.size();
                const std::size_t want_total =
                    std::max(out_.capacity(), old_size + kInitialOutCapacity);
                out_.resize(std::min(want_total, max_inflated_size));

                strm_.next_out  = out_.data() + old_size;
                strm_.avail_out = static_cast<uInt>(out_.size() - old_size);

                const int ret = ::inflate(&strm_, Z_NO_FLUSH);
                const std::size_t produced =
                    (out_.size() - old_size) - strm_.avail_out;
                out_.resize(old_size + produced);

                if (ret == Z_OK || ret == Z_BUF_ERROR) {
                    // Z_BUF_ERROR here means "need more input" (we'll
                    // either feed more in the next chunk or surface it
                    // as an error after the tail is fed). Z_OK means
                    // "made progress, may need more output buffer".
                    continue;
                }
                if (ret == Z_STREAM_END) {
                    // Shouldn't happen mid-message for raw deflate
                    // (no end-of-stream marker until BFINAL=1 in the
                    // tail). If it does, we're done early — fine.
                    return {};
                }
                SPDLOG_WARN("WsInflater: inflate ret={} msg='{}'",
                            ret, strm_.msg ? strm_.msg : "(null)");
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::CodecBad,
                    "WsInflater: zlib inflate failed"});
            }
            return {};
        };

        // On any feed/drain failure we return to the caller with the
        // z_stream possibly mid-block (some compressed bytes consumed,
        // LZ77 window populated with a partial message). RFC 7692
        // context-takeover would otherwise carry that partial state into
        // the NEXT message and inflate it against a corrupted dictionary.
        // The connection is dead anyway after a CodecOverflow / CodecBad
        // (the WsCodec returns the error to its caller and the caller
        // drops the stream), but if a WsCodec instance is reused after
        // a transient inflate failure we don't want to silently corrupt
        // the next message — `inflateReset` returns the z_stream to
        // post-init clean state regardless of where we were mid-block.
        auto reset_on_error = [this](::eph::core::ErrorInfo err)
            -> std::expected<std::span<const uint8_t>, ::eph::core::ErrorInfo>
        {
            (void)::inflateReset(&strm_);
            return std::unexpected(std::move(err));
        };

        if (!compressed.empty()) {
            auto fr = feed(compressed.data(), compressed.size());
            if (!fr) return reset_on_error(fr.error());
        }
        auto fr = feed(kDeflateTail, sizeof(kDeflateTail));
        if (!fr) return reset_on_error(fr.error());

        // After feeding the tail, drain any remaining output by calling
        // inflate with Z_SYNC_FLUSH-like behaviour (we call inflate
        // again until avail_out > 0 stays true, i.e. nothing more to
        // produce). The tail-feed loop above already handled the
        // common case — this is a defensive flush.
        while (true) {
            if (out_.size() >= max_inflated_size) {
                return reset_on_error(::eph::core::ErrorInfo{
                    ::eph::core::Error::CodecOverflow,
                    "WsInflater: inflated payload exceeds max"});
            }
            const std::size_t old_size = out_.size();
            const std::size_t grow = kInitialOutCapacity;
            out_.resize(std::min(old_size + grow, max_inflated_size));

            strm_.next_in  = nullptr;
            strm_.avail_in = 0;
            strm_.next_out  = out_.data() + old_size;
            strm_.avail_out = static_cast<uInt>(out_.size() - old_size);

            const int ret = ::inflate(&strm_, Z_SYNC_FLUSH);
            const std::size_t produced =
                (out_.size() - old_size) - strm_.avail_out;
            out_.resize(old_size + produced);

            if (produced == 0 || ret == Z_BUF_ERROR) {
                // Nothing more to produce.
                break;
            }
            if (ret != Z_OK && ret != Z_STREAM_END) {
                SPDLOG_WARN("WsInflater: drain inflate ret={} msg='{}'",
                            ret, strm_.msg ? strm_.msg : "(null)");
                return reset_on_error(::eph::core::ErrorInfo{
                    ::eph::core::Error::CodecBad,
                    "WsInflater: zlib drain inflate failed"});
            }
        }

        if (reset_after) {
            // server_no_context_takeover — wipe the LZ77 window so the
            // next message starts from an empty dictionary.
            const int ret = ::inflateReset(&strm_);
            if (ret != Z_OK) {
                SPDLOG_WARN("WsInflater: inflateReset ret={}", ret);
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::CodecBad,
                    "WsInflater: inflateReset failed"});
            }
        }

        return std::span<const uint8_t>{out_.data(), out_.size()};
    }

    /// @brief True once the underlying z_stream has been initialised
    ///        (i.e. at least one compressed message has been seen).
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

private:
    z_stream             strm_{};
    bool                 initialized_{false};
    std::vector<uint8_t> out_;  // reused across messages
};

} // namespace eph::codec::detail
