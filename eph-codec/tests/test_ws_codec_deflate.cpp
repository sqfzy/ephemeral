/// @file test_ws_codec_deflate.cpp
/// Unit tests for `eph::codec::WsCodec` permessage-deflate (RFC 7692)
/// inflate path. Crafts compressed frames inline using zlib's `deflate()`
/// (raw-deflate, `-MAX_WBITS`) so we can exercise every interesting
/// transition without needing a recorded fixture from a real venue.
///
/// What's covered:
///   * Happy path: single compressed message decodes to the original
///     plaintext.
///   * Poison-pill: server sends a compressed frame but the codec was
///     never told to enable deflate → `Error::CodecBad`.
///   * Context-takeover: two messages compressed against a shared
///     dictionary inflate cleanly when the second message references
///     back-edges into the first.
///   * No-context-takeover: same setup but with the inflater reset
///     between messages — the second message must compress without
///     back-references for the inflater to recover.
///   * RSV1 on a control frame: protocol violation per RFC 7692 §6.1.
///   * RSV1 on a continuation frame: protocol violation per §6.1.
///   * Deflate metrics: `kWsDeflateBytesIn` / `kWsDeflateBytesOut` are
///     populated as expected.
///   * Decompression bomb: an inflated payload that exceeds
///     `max_message_size` returns `CodecOverflow` rather than runaway
///     allocating.

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <zlib.h>

#include "eph/codec/detail/span_packet_view.hpp"
#include "eph/codec/ws_codec.hpp"
#include "eph/core/codec.hpp"
#include "eph/core/error.hpp"

using eph::codec::SpanPacketView;
using eph::codec::WsCodec;
using eph::codec::WsCodecConfig;
using eph::core::Error;
using eph::core::OutputBuffer;

namespace {

constexpr uint8_t kFinBit  = 0x80;
constexpr uint8_t kRsv1Bit = 0x40;

constexpr uint8_t kOpText        = 0x1;
constexpr uint8_t kOpBinary      = 0x2;
constexpr uint8_t kOpClose       = 0x8;
constexpr uint8_t kOpPing        = 0x9;
constexpr uint8_t kOpContinuation = 0x0;

// ---------------------------------------------------------------------------
// raw_deflate — apply zlib raw-deflate (-MAX_WBITS) to a plaintext, then
// strip the trailing `0x00 0x00 0xFF 0xFF` SYNC marker that RFC 7692 §7.2.1
// requires the *encoder* to remove before sending. The inflater (run by
// WsCodec) will append it back internally before feeding zlib, matching
// the wire-format contract.
// ---------------------------------------------------------------------------
std::vector<uint8_t> raw_deflate(std::span<const uint8_t> input,
                                  z_stream* shared_strm = nullptr) {
    z_stream local{};
    z_stream& strm = shared_strm ? *shared_strm : local;
    if (!shared_strm) {
        // -MAX_WBITS selects raw deflate (no zlib header / Adler-32).
        // Z_DEFAULT_COMPRESSION level + DEFLATED method + DEFAULT_STRATEGY
        // matches what every real WS server uses.
        const int rv = ::deflateInit2(&strm, Z_DEFAULT_COMPRESSION,
                                       Z_DEFLATED, -MAX_WBITS,
                                       8 /*default memLevel*/,
                                       Z_DEFAULT_STRATEGY);
        if (rv != Z_OK) {
            ADD_FAILURE() << "deflateInit2 rv=" << rv;
            return {};
        }
    }

    std::vector<uint8_t> out;
    out.resize(::deflateBound(&strm, input.size()) + 16);

    strm.next_in   = const_cast<Bytef*>(input.data());
    strm.avail_in  = static_cast<uInt>(input.size());
    strm.next_out  = out.data();
    strm.avail_out = static_cast<uInt>(out.size());

    // Z_SYNC_FLUSH yields the BFINAL=1 + 0x00 0x00 0xFF 0xFF tail that
    // RFC 7692 says we must strip from the wire form.
    const int rv = ::deflate(&strm, Z_SYNC_FLUSH);
    if (rv != Z_OK && rv != Z_BUF_ERROR && rv != Z_STREAM_END) {
        ADD_FAILURE() << "deflate rv=" << rv;
        if (!shared_strm) ::deflateEnd(&strm);
        return {};
    }
    out.resize(out.size() - strm.avail_out);

    if (!shared_strm) {
        ::deflateEnd(&strm);
    }

    // Strip the four trailing tail bytes (0x00 0x00 0xFF 0xFF). We
    // assert their presence rather than blindly resize so a future zlib
    // version that emits a different sync marker would surface here
    // rather than as a silent test corruption.
    if (out.size() < 4 ||
        out[out.size() - 4] != 0x00 || out[out.size() - 3] != 0x00 ||
        out[out.size() - 2] != 0xFF || out[out.size() - 1] != 0xFF) {
        ADD_FAILURE() << "zlib produced unexpected sync marker";
        return out;
    }
    out.resize(out.size() - 4);
    return out;
}

// ---------------------------------------------------------------------------
// mk_compressed_frame — wrap a raw-deflate buffer in a single
// unmasked WS frame with FIN=1 and RSV1=1 (the on-wire form a server
// sends for a single-message permessage-deflate payload).
// ---------------------------------------------------------------------------
std::vector<uint8_t> mk_compressed_frame(uint8_t opcode,
                                          bool fin,
                                          bool rsv1,
                                          std::span<const uint8_t> payload) {
    std::vector<uint8_t> out;
    out.reserve(10 + payload.size());

    uint8_t b0 = static_cast<uint8_t>(opcode & 0x0F);
    if (fin)  b0 |= kFinBit;
    if (rsv1) b0 |= kRsv1Bit;
    out.push_back(b0);

    if (payload.size() < 126) {
        out.push_back(static_cast<uint8_t>(payload.size() & 0x7F));
    } else if (payload.size() <= 0xFFFF) {
        out.push_back(126);
        out.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
    } else {
        out.push_back(127);
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<uint8_t>(
                (payload.size() >> (i * 8)) & 0xFF));
        }
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// Build a plaintext WS frame (no compression). Used for unrelated tests
// that just need a raw frame on the wire.
std::vector<uint8_t> mk_plain_frame(uint8_t opcode, bool fin,
                                     std::span<const uint8_t> payload) {
    return mk_compressed_frame(opcode, fin, /*rsv1=*/false, payload);
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// Happy path
// ═══════════════════════════════════════════════════════════════════════

TEST(WsCodecDeflate, HappyPath_SingleCompressedMessage) {
    const std::string plaintext =
        "{\"e\":\"bookTicker\",\"u\":42,\"s\":\"BTCUSDT\","
         "\"b\":\"50000.0\",\"B\":\"1.5\",\"a\":\"50000.5\",\"A\":\"2.0\"}";
    auto compressed = raw_deflate({reinterpret_cast<const uint8_t*>(
                                       plaintext.data()),
                                   plaintext.size()});
    ASSERT_FALSE(compressed.empty());

    auto wire = mk_compressed_frame(kOpText, /*fin=*/true, /*rsv1=*/true,
                                     compressed);
    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[64];
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;
    codec.enable_permessage_deflate(/*server_no_ctx_takeover=*/false);
    auto r = codec.decode(view, out);
    ASSERT_TRUE(r.has_value()) << "decode failed: code="
                                << int(r.error().code)
                                << " detail=" << r.error().detail;
    ASSERT_TRUE(r->has_value());
    auto frame = **r;
    ASSERT_EQ(frame.size(), plaintext.size());
    EXPECT_EQ(0, std::memcmp(frame.data(), plaintext.data(), plaintext.size()));
    EXPECT_EQ(view.length(), 0u);
    // Metrics — deflate_bytes_in is the wire-compressed payload size,
    // deflate_bytes_out is the inflated plaintext.
    EXPECT_EQ(codec.ws_deflate_bytes_in(), compressed.size());
    EXPECT_EQ(codec.ws_deflate_bytes_out(), plaintext.size());
}

// ═══════════════════════════════════════════════════════════════════════
// Poison-pill — compressed frame without negotiated extension
// ═══════════════════════════════════════════════════════════════════════

TEST(WsCodecDeflate, CompressedWithoutNegotiationFails) {
    const std::string plaintext = "{\"x\":1}";
    auto compressed = raw_deflate({reinterpret_cast<const uint8_t*>(
                                       plaintext.data()),
                                   plaintext.size()});
    auto wire = mk_compressed_frame(kOpText, /*fin=*/true, /*rsv1=*/true,
                                     compressed);
    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[16];
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;  // permessage_deflate not enabled
    auto r = codec.decode(view, out);
    ASSERT_FALSE(r.has_value());
    // RSV1 reaches `decode_frame` first with allow_rsv1=false, which
    // surfaces as `WsFrameBad` (kReservedBits). That's the public
    // contract — both `WsFrameBad` and `CodecBad` are valid signals
    // that the connection must be torn down. We assert WsFrameBad
    // explicitly so a future contract change is caught.
    EXPECT_EQ(r.error().code, Error::WsFrameBad);
}

// ═══════════════════════════════════════════════════════════════════════
// Context takeover — two messages compressed against shared dictionary
// ═══════════════════════════════════════════════════════════════════════

TEST(WsCodecDeflate, ContextTakeover_TwoMessages) {
    // First message: a string that contains a verbatim phrase the
    // second message will reuse. With context takeover the second
    // message can back-reference the phrase using LZ77 distance codes;
    // without takeover the inflater state is wiped and the phrase
    // would be present literally in the second compressed payload.
    const std::string m1 =
        "{\"event\":\"bookTicker\",\"symbol\":\"BTCUSDT\",\"bid\":\"50000.0\","
         "\"ask\":\"50000.5\"}";
    const std::string m2 =
        "{\"event\":\"bookTicker\",\"symbol\":\"BTCUSDT\",\"bid\":\"50001.0\","
         "\"ask\":\"50001.5\"}";

    // Single shared deflate stream — the deflater's LZ77 window
    // accumulates across messages so message 2 references back into
    // message 1's substrings.
    z_stream strm{};
    ASSERT_EQ(Z_OK, ::deflateInit2(&strm, Z_DEFAULT_COMPRESSION,
                                    Z_DEFLATED, -MAX_WBITS, 8,
                                    Z_DEFAULT_STRATEGY));

    auto c1 = raw_deflate({reinterpret_cast<const uint8_t*>(m1.data()),
                           m1.size()}, &strm);
    auto c2 = raw_deflate({reinterpret_cast<const uint8_t*>(m2.data()),
                           m2.size()}, &strm);
    ::deflateEnd(&strm);

    ASSERT_FALSE(c1.empty());
    ASSERT_FALSE(c2.empty());

    auto wire1 = mk_compressed_frame(kOpText, true, true, c1);
    auto wire2 = mk_compressed_frame(kOpText, true, true, c2);

    WsCodec codec;
    codec.enable_permessage_deflate(/*server_no_ctx_takeover=*/false);

    {
        SpanPacketView view{wire1.data(), wire1.size()};
        uint8_t out_storage[16];
        OutputBuffer out{out_storage, sizeof(out_storage)};
        auto r = codec.decode(view, out);
        ASSERT_TRUE(r.has_value());
        ASSERT_TRUE(r->has_value());
        auto f = **r;
        ASSERT_EQ(f.size(), m1.size());
        EXPECT_EQ(0, std::memcmp(f.data(), m1.data(), m1.size()));
    }
    {
        SpanPacketView view{wire2.data(), wire2.size()};
        uint8_t out_storage[16];
        OutputBuffer out{out_storage, sizeof(out_storage)};
        auto r = codec.decode(view, out);
        ASSERT_TRUE(r.has_value()) << "msg2 decode failed: "
                                    << r.error().detail;
        ASSERT_TRUE(r->has_value());
        auto f = **r;
        ASSERT_EQ(f.size(), m2.size());
        EXPECT_EQ(0, std::memcmp(f.data(), m2.data(), m2.size()));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// No-context-takeover — inflater wiped between messages
// ═══════════════════════════════════════════════════════════════════════

TEST(WsCodecDeflate, NoContextTakeover_TwoMessages) {
    const std::string m1 = "{\"a\":1,\"b\":2,\"c\":3}";
    const std::string m2 = "{\"x\":\"hello\",\"y\":\"world\"}";

    // Two separate deflate streams — each message compresses against an
    // empty window. Mirrors what a real server would send when it
    // negotiated `server_no_context_takeover`.
    auto c1 = raw_deflate({reinterpret_cast<const uint8_t*>(m1.data()),
                           m1.size()});
    auto c2 = raw_deflate({reinterpret_cast<const uint8_t*>(m2.data()),
                           m2.size()});

    auto wire1 = mk_compressed_frame(kOpText, true, true, c1);
    auto wire2 = mk_compressed_frame(kOpText, true, true, c2);

    WsCodec codec;
    codec.enable_permessage_deflate(/*server_no_ctx_takeover=*/true);

    auto decode_one = [&](std::vector<uint8_t>& wire,
                          const std::string& expected) {
        SpanPacketView view{wire.data(), wire.size()};
        uint8_t out_storage[16];
        OutputBuffer out{out_storage, sizeof(out_storage)};
        auto r = codec.decode(view, out);
        ASSERT_TRUE(r.has_value()) << r.error().detail;
        ASSERT_TRUE(r->has_value());
        auto f = **r;
        ASSERT_EQ(f.size(), expected.size());
        EXPECT_EQ(0, std::memcmp(f.data(), expected.data(),
                                  expected.size()));
    };

    decode_one(wire1, m1);
    decode_one(wire2, m2);
}

// ═══════════════════════════════════════════════════════════════════════
// RSV1 on control frames is a protocol violation
// ═══════════════════════════════════════════════════════════════════════

TEST(WsCodecDeflate, Rsv1OnControlFrameRejected) {
    // Ping frame with RSV1=1 — RFC 7692 §6.1 forbids this.
    auto wire = mk_compressed_frame(kOpPing, /*fin=*/true,
                                     /*rsv1=*/true, {});
    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[16];
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;
    codec.enable_permessage_deflate(false);
    auto r = codec.decode(view, out);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsFrameBad);
}

// ═══════════════════════════════════════════════════════════════════════
// RSV1 on continuation frames is a protocol violation
// ═══════════════════════════════════════════════════════════════════════

TEST(WsCodecDeflate, Rsv1OnContinuationRejected) {
    // First fragment: text+RSV1+FIN=0  → start of compressed message
    const std::string first_part = "AAAAA";
    auto c1 = raw_deflate({reinterpret_cast<const uint8_t*>(first_part.data()),
                           first_part.size()});
    auto f1 = mk_compressed_frame(kOpText, /*fin=*/false,
                                   /*rsv1=*/true, c1);
    // Second fragment: continuation + RSV1=1 + FIN=1 → bad: continuation
    // MUST NOT carry RSV1.
    auto f2 = mk_compressed_frame(kOpContinuation, /*fin=*/true,
                                   /*rsv1=*/true, std::vector<uint8_t>{0x00});

    std::vector<uint8_t> combined;
    combined.insert(combined.end(), f1.begin(), f1.end());
    combined.insert(combined.end(), f2.begin(), f2.end());

    SpanPacketView view{combined.data(), combined.size()};
    uint8_t out_storage[16];
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;
    codec.enable_permessage_deflate(false);

    // First decode buffers the partial fragment.
    auto r1 = codec.decode(view, out);
    ASSERT_TRUE(r1.has_value());
    EXPECT_FALSE(r1->has_value());

    // Second decode (the bad continuation) must reject.
    auto r2 = codec.decode(view, out);
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code, Error::WsFrameBad);
}

// ═══════════════════════════════════════════════════════════════════════
// Decompression bomb — inflated output exceeds max_message_size
// ═══════════════════════════════════════════════════════════════════════

TEST(WsCodecDeflate, DecompressionBombRejected) {
    // 1 MiB of zeros compresses to ~1 KiB. Cap max_message_size at 64 KiB
    // so the inflated output trips the limit halfway through.
    std::vector<uint8_t> plaintext(1u << 20, 0u);
    auto compressed = raw_deflate(plaintext);
    auto wire = mk_compressed_frame(kOpBinary, /*fin=*/true,
                                     /*rsv1=*/true, compressed);

    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[16];
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec{WsCodecConfig{.max_message_size = 64u * 1024u,
                                 .auto_pong = true,
                                 .auto_close_ack = true,
                                 .permessage_deflate = false,
                                 .server_no_context_takeover = false}};
    codec.enable_permessage_deflate(false);
    auto r = codec.decode(view, out);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecOverflow);
}

// ═══════════════════════════════════════════════════════════════════════
// Mixed traffic — uncompressed text frame between two compressed messages
// (server may send unsolicited uncompressed frames; the codec must
// keep its compressed-message accumulator clean)
// ═══════════════════════════════════════════════════════════════════════

TEST(WsCodecDeflate, MixedCompressedAndUncompressed) {
    const std::string compressed_msg = "{\"x\":\"hello compressed\"}";
    const std::string plain_msg      = "{\"x\":\"hello plain\"}";

    auto c1 = raw_deflate({reinterpret_cast<const uint8_t*>(
                              compressed_msg.data()),
                           compressed_msg.size()});

    auto wire1 = mk_compressed_frame(kOpText, true, true, c1);
    std::vector<uint8_t> plain_bytes(plain_msg.begin(), plain_msg.end());
    auto wire2 = mk_plain_frame(kOpText, true, plain_bytes);

    WsCodec codec;
    codec.enable_permessage_deflate(false);

    {
        SpanPacketView view{wire1.data(), wire1.size()};
        uint8_t out_storage[16];
        OutputBuffer out{out_storage, sizeof(out_storage)};
        auto r = codec.decode(view, out);
        ASSERT_TRUE(r.has_value()) << r.error().detail;
        ASSERT_TRUE(r->has_value());
        auto f = **r;
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(f.data()),
                               f.size()),
                  compressed_msg);
    }
    {
        SpanPacketView view{wire2.data(), wire2.size()};
        uint8_t out_storage[16];
        OutputBuffer out{out_storage, sizeof(out_storage)};
        auto r = codec.decode(view, out);
        ASSERT_TRUE(r.has_value()) << r.error().detail;
        ASSERT_TRUE(r->has_value());
        auto f = **r;
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(f.data()),
                               f.size()),
                  plain_msg);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Fragmented compressed message — first frame RSV1, continuations RSV1=0
// (the only RFC-7692-compliant way to fragment)
// ═══════════════════════════════════════════════════════════════════════

TEST(WsCodecDeflate, FragmentedCompressedMessage) {
    // Compress a bigger payload, then split it across two frames at the
    // codec layer (the deflate output is one bytestream split in half;
    // the inflater concatenates the halves at frag_buf_ before
    // inflating, exactly as it would for a real fragmented compressed
    // message on the wire).
    const std::string plaintext =
        "{\"event\":\"depthUpdate\",\"u\":12345,"
         "\"E\":1709120000000,\"s\":\"BTCUSDT\","
         "\"b\":[[\"49999.0\",\"1.5\"],[\"49998.0\",\"3.0\"]],"
         "\"a\":[[\"50001.0\",\"2.0\"],[\"50002.0\",\"4.0\"]]}";

    auto compressed = raw_deflate({reinterpret_cast<const uint8_t*>(
                                       plaintext.data()),
                                   plaintext.size()});
    ASSERT_GE(compressed.size(), 8u) << "compressed payload too small to split";
    const size_t mid = compressed.size() / 2;
    std::span<const uint8_t> first_half{compressed.data(), mid};
    std::span<const uint8_t> second_half{compressed.data() + mid,
                                          compressed.size() - mid};

    auto f1 = mk_compressed_frame(kOpText, /*fin=*/false,
                                   /*rsv1=*/true, first_half);
    auto f2 = mk_compressed_frame(kOpContinuation, /*fin=*/true,
                                   /*rsv1=*/false, second_half);

    std::vector<uint8_t> combined;
    combined.insert(combined.end(), f1.begin(), f1.end());
    combined.insert(combined.end(), f2.begin(), f2.end());

    SpanPacketView view{combined.data(), combined.size()};
    uint8_t out_storage[16];
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;
    codec.enable_permessage_deflate(false);

    auto r1 = codec.decode(view, out);
    ASSERT_TRUE(r1.has_value()) << r1.error().detail;
    EXPECT_FALSE(r1->has_value()) << "first half should buffer, not deliver";

    auto r2 = codec.decode(view, out);
    ASSERT_TRUE(r2.has_value()) << r2.error().detail;
    ASSERT_TRUE(r2->has_value());
    auto f = **r2;
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(f.data()), f.size()),
              plaintext);
}
