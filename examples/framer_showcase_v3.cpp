/// @file framer_showcase_v3.cpp
/// v3.3 rewrite of framer_showcase.cpp.
///
/// Demonstrates the stateful Codec interface introduced in Phase 1 of the
/// v3.3 refactor. All three codecs (WsCodec, RawStreamCodec,
/// LengthPrefixCodec) share the same
///
///     encode(buf, cap, Frame)  ->  expected<size_t, ErrorInfo>
///     decode(view, OutputBuffer&) -> expected<optional<Frame>, ErrorInfo>
///
/// shape. This example runs entirely in-process — no sockets, no DPDK —
/// and simply encodes a payload, then feeds the bytes back through
/// decode() to recover it.
///
/// Part of Phase 6 of the v3.3 architecture refactor
/// (.artifacts/design-eph-v3.3-architecture-20260410.md).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/codec/detail/span_packet_view.hpp"
#include "eph/codec/length_prefix_codec.hpp"
#include "eph/codec/raw_stream_codec.hpp"
#include "eph/codec/ws_codec.hpp"
#include "eph/core/codec.hpp"
#include "eph/core/error.hpp"

namespace ec = eph::codec;

// ── Helper: hex dump ───────────────────────────────────────────────────────

static std::string hex_dump(const uint8_t* data, std::size_t len) {
    std::string out;
    out.reserve(len * 3);
    for (std::size_t i = 0; i < len; ++i) {
        if (i > 0) out += ' ';
        out += std::format("{:02x}", data[i]);
    }
    return out;
}

// ── Generic round-trip driver ──────────────────────────────────────────────

template <class Codec>
static void demo(std::string_view name, Codec& codec,
                 std::span<const uint8_t> payload) {
    spdlog::info("=== {} ===", name);
    spdlog::info("  plaintext  : {}", hex_dump(payload.data(), payload.size()));

    uint8_t wire[1024];
    auto enc = codec.encode(wire, sizeof(wire), payload);
    if (!enc) {
        spdlog::error("  encode failed: {}", enc.error().detail);
        return;
    }
    spdlog::info("  wire ({:>3}) : {}", *enc, hex_dump(wire, *enc));

    // Decode: feed `wire[0..*enc]` back through the codec.
    ec::SpanPacketView view(wire, *enc);
    uint8_t scratch[128];
    eph::core::OutputBuffer sink(scratch, sizeof(scratch));

    auto dec = codec.decode(view, sink);
    if (!dec) {
        spdlog::error("  decode failed: {}", dec.error().detail);
        return;
    }
    if (!dec->has_value()) {
        spdlog::warn("  decode returned nullopt (need more bytes)");
        return;
    }
    const auto& frame = **dec;
    spdlog::info("  decoded    : {}", hex_dump(frame.data(), frame.size()));
    const bool match = (frame.size() == payload.size()
        && std::memcmp(frame.data(), payload.data(), payload.size()) == 0);
    spdlog::info("  round-trip : {}", match ? "OK" : "MISMATCH");
}

int main() {
    spdlog::set_level(spdlog::level::info);

    const std::string msg = "v3.3 codec round-trip";
    std::span<const uint8_t> payload(
        reinterpret_cast<const uint8_t*>(msg.data()), msg.size());

    // 1) RawStreamCodec — pass-through.
    {
        ec::RawStreamCodec codec{};
        demo("RawStreamCodec", codec, payload);
    }

    // 2) LengthPrefixCodec — 4-byte LE length header.
    {
        ec::LengthPrefixCodec codec{};
        demo("LengthPrefixCodec", codec, payload);
    }

    // 3) WsCodec — full WebSocket binary frame.
    {
        ec::WsCodec codec{};
        demo("WsCodec", codec, payload);
    }

    return 0;
}
