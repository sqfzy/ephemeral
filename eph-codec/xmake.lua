-- ============================================================================
-- eph-codec — header-only library of stateful codec implementations.
-- ============================================================================
--
-- Responsibilities:
--   * eph::codec::WsCodec            — stateful WebSocket codec with
--                                       internal control-frame state machine
--                                       (auto pong / close-ack via OutputBuffer).
--                                       Uses the WS wire helpers under
--                                       `eph/net/detail/websocket.hpp` (owned
--                                       by eph-net).
--   * eph::codec::RawStreamCodec     — identity stream codec (no framing).
--   * eph::codec::LengthPrefixCodec  — 4-byte big-endian length-prefix framer.
--   * eph::codec::RawDatagramCodec   — identity datagram codec.
--   * eph::codec::Mold64Codec        — MoldUDP64 datagram codec wrapping
--                                       `eph::itch::parse_moldudp64`.
--
-- Dependency rule:
--   * eph-core         — StreamCodec / DatagramCodec concepts, ErrorInfo, OutputBuffer
--   * eph-net          — TLS detail + WS wire helpers (under eph/net/detail/*)
--   * eph-itch         — MoldUDP64 wire parser (Mold64Codec is a thin wrapper)

target("eph-codec")
    set_kind("headeronly")
    add_includedirs("include", { public = true })
    add_headerfiles("include/(eph/codec/**.hpp)")
    add_deps("eph-core", "eph-net", "eph-itch", { public = true })
    add_packages("spdlog", "aws-lc", { public = true })
    -- zlib provides the raw-deflate inflater used by WsCodec for RFC 7692
    -- permessage-deflate. System zlib is universally available on Linux
    -- (kernel + util-linux already depend on it) — picking it over libdeflate
    -- avoids introducing a new opt-in dep just to inflate a few KB/s of
    -- bookticker JSON. See ws_codec_inflate.hpp for the rationale.
    add_syslinks("z", { public = true })
    add_defines("SPDLOG_ACTIVE_LEVEL=" .. net_log_level, { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

-- Module tests — one binary per test_*.cpp file (auto-globbed via eph-test rule)
for _, file in ipairs(os.files("tests/**.cpp")) do
    target(path.basename(file))
        add_rules("eph-test")
        add_files(file)
        add_deps("eph-codec")
end

-- Module benchmarks — auto-globbed; empty for now.
for _, file in ipairs(os.files("benchmarks/**.cpp")) do
    target(path.basename(file))
        add_rules("eph-bench")
        add_files(file)
        add_deps("eph-codec")
end
