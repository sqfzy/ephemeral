/// @file framer_showcase.cpp
/// Framer showcase — encode/decode comparison of all three MessageFramer
/// implementations: WsFramer, RawFramer, and LengthPrefixFramer.
///
/// Demonstrates:
///   1. The MessageFramer concept and how framers add wire-format headers
///   2. Encode/decode round-trip for each framer
///   3. How to instantiate Transport with a non-default framer
///
/// No network dependency — this example runs entirely in-process,
/// encoding and decoding buffers to show the wire format differences.
///
/// Usage:
///   ./framer_showcase
///
/// Related examples:
///   - minimal_ws_client.cpp  — uses WsFramer (the default)
///   - ws_echo_client.cpp     — full client with default WsFramer

#include <algorithm>
#include <cstdint>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/net/framer_concept.hpp"
#include "eph/net/length_prefix_framer.hpp"
#include "eph/net/raw_framer.hpp"
#include "eph/net/ws_framer.hpp"

// Also include transport types to show type alias examples
#include "eph/net/socket_transport.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Helper: hex dump of encoded bytes
// ─────────────────────────────────────────────────────────────────────────────

static std::string hex_dump(const uint8_t* data, size_t len) {
    std::string result;
    result.reserve(len * 3);
    for (size_t i = 0; i < len; ++i) {
        if (i > 0) result += ' ';
        result += std::format("{:02x}", data[i]);
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Generic encode/decode round-trip test for any MessageFramer
// ─────────────────────────────────────────────────────────────────────────────

template <eph::net::MessageFramer Framer>
static void demo_framer(std::string_view name, Framer& framer) {
    spdlog::info("=== {} (max overhead: {} bytes) ===", name, Framer::max_overhead());

    const std::string payload = "Hello, ephemeral!";
    auto payload_bytes = reinterpret_cast<const uint8_t*>(payload.data());
    size_t payload_len = payload.size();

    // Encode
    std::vector<uint8_t> wire(payload_len + Framer::max_overhead() + 16);
    size_t encoded_len = framer.encode(
        wire.data(), payload_bytes, payload_len, 0x01 /* msg_type */);

    if (encoded_len == 0) {
        spdlog::error("  Encode failed!");
        return;
    }

    spdlog::info("  Payload:  \"{}\" ({} bytes)", payload, payload_len);
    spdlog::info("  Encoded:  {} bytes", encoded_len);
    spdlog::info("  Overhead: {} bytes", encoded_len - payload_len);
    spdlog::info("  Wire hex: {}", hex_dump(wire.data(), encoded_len));

    // Decode
    auto decoded = Framer::decode(wire.data(), encoded_len);
    if (!decoded) {
        spdlog::error("  Decode failed: {}",
                      eph::net::frame_error_name(decoded.error()));
        return;
    }

    std::string_view decoded_payload(
        reinterpret_cast<const char*>(decoded->payload), decoded->payload_len);
    spdlog::info("  Decoded:  \"{}\" ({} bytes, msg_type=0x{:02x})",
                 decoded_payload, decoded->payload_len, decoded->msg_type);
    spdlog::info("  Control:  {}", decoded->is_control ? "yes" : "no");

    // Verify round-trip integrity
    // Note: WsFramer applies client-side masking (RFC 6455 §5.3), so the
    // decoded payload from our own encoded frame will appear masked.
    // In real use, the server sends unmasked frames back to the client,
    // and decode() returns the original payload correctly.
    bool match = (decoded->payload_len == payload_len) &&
                 std::equal(decoded->payload, decoded->payload + decoded->payload_len,
                            payload_bytes);
    if (match) {
        spdlog::info("  Round-trip: OK");
    } else {
        spdlog::info("  Round-trip: payload differs (expected for WsFramer — "
                     "client frames are masked per RFC 6455)");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Show how to define Transport type aliases with different framers
// ─────────────────────────────────────────────────────────────────────────────

static void demo_transport_aliases() {
    spdlog::info("=== Transport Type Aliases with Different Framers ===");

    // Default: WebSocket framing (used by minimal_ws_client, production_client)
    using WsTransport = eph::net::Transport<eph::net::SocketTransport>;
    // Equivalent to: Transport<SocketTransport, WsFramer, 512, 1024>

    // Raw transport: no framing overhead — for protocols with own boundaries
    using RawTransport = eph::net::Transport<
        eph::net::SocketTransport, eph::net::RawFramer>;

    // Length-prefix transport: 2-byte BE header — for binary protocols (ITCH, etc.)
    using LenPrefixTransport = eph::net::Transport<
        eph::net::SocketTransport, eph::net::LengthPrefixFramer>;

    // Custom payload/queue sizes
    using SmallWsTransport = eph::net::Transport<
        eph::net::SocketTransport, eph::net::WsFramer, 64, 256>;
    using LargeRawTransport = eph::net::Transport<
        eph::net::SocketTransport, eph::net::RawFramer, 4096, 512>;

    spdlog::info("  WsTransport:         max_payload={}, queue_depth={}",
                 WsTransport::max_payload(), WsTransport::queue_depth());
    spdlog::info("  RawTransport:        max_payload={}, queue_depth={}",
                 RawTransport::max_payload(), RawTransport::queue_depth());
    spdlog::info("  LenPrefixTransport:  max_payload={}, queue_depth={}",
                 LenPrefixTransport::max_payload(), LenPrefixTransport::queue_depth());
    spdlog::info("  SmallWsTransport:    max_payload={}, queue_depth={}",
                 SmallWsTransport::max_payload(), SmallWsTransport::queue_depth());
    spdlog::info("  LargeRawTransport:   max_payload={}, queue_depth={}",
                 LargeRawTransport::max_payload(), LargeRawTransport::queue_depth());

    spdlog::info("");
    spdlog::info("  To use a non-default framer, just change the type alias:");
    spdlog::info("    auto result = RawTransport::create(tcp_factory, config);");
    spdlog::info("  The send/recv API is identical — only wire encoding differs.");
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    spdlog::set_level(spdlog::level::info);

    // Note: WsFramer adds masking (random 4-byte key per frame), so the wire
    // hex will differ on each run. RawFramer and LengthPrefixFramer are
    // deterministic.

    eph::net::WsFramer ws_framer;
    demo_framer("WsFramer (RFC 6455 WebSocket)", ws_framer);
    std::cout << "\n";

    eph::net::RawFramer raw_framer;
    demo_framer("RawFramer (pass-through, zero overhead)", raw_framer);
    std::cout << "\n";

    eph::net::LengthPrefixFramer lp_framer;
    demo_framer("LengthPrefixFramer (2-byte BE header)", lp_framer);
    std::cout << "\n";

    demo_transport_aliases();

    return 0;
}
