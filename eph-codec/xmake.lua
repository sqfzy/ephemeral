-- ============================================================================
-- eph-codec — header-only library of stateful codec implementations
-- ============================================================================
--
-- Phase 1 of the v3.3 architecture refactor (see
-- .artifacts/design-eph-v3.3-architecture-20260410.md).
--
-- This module provides implementations that satisfy the new
-- `eph::core::StreamCodec` / `eph::core::DatagramCodec` concepts defined in
-- Phase 0:
--
--   * eph::codec::WsCodec            — stateful WebSocket codec with
--                                       internal control-frame state machine
--                                       (auto pong / close-ack via OutputBuffer)
--   * eph::codec::RawStreamCodec     — identity stream codec (no framing)
--   * eph::codec::LengthPrefixCodec  — 4-byte big-endian length-prefix framer
--   * eph::codec::RawDatagramCodec   — identity datagram codec
--   * eph::codec::Mold64Codec        — MoldUDP64 datagram codec wrapping
--                                       eph-itch's parser
--
-- Dependency rule (target, post-Phase-7): eph-codec depends ONLY on eph-core.
-- Dependency rule (Phase 1, additive): Phase 1 is ADDITIVE — the old framers
-- in eph-transport/eph-itch stay untouched. To avoid duplicating ~900 lines
-- of hardened WebSocket wire-format code during Phase 1, we temporarily
-- reuse eph-transport's `detail/websocket.hpp` header (which in turn pulls in
-- aws-lc for CSPRNG-backed mask keys). Mold64Codec similarly reuses
-- eph-itch's parse_moldudp64 helper. Phase 7 will:
--   1. move the ws wire-format helpers into eph-codec/detail/ws_wire.hpp,
--   2. delete eph-transport,
--   3. drop these transitive deps.

target("eph-codec")
    set_kind("headeronly")
    add_includedirs("include", { public = true })
    add_headerfiles("include/(eph/codec/**.hpp)")
    add_deps("eph-core", { public = true })
    -- Phase 1 transitive deps — removed in Phase 7:
    add_deps("eph-itch",       { public = true })  -- for mold64_codec.hpp
    add_deps("eph-transport",  { public = true })  -- for ws_codec.hpp
    add_packages("spdlog", "aws-lc", { public = true })
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

-- Module benchmarks — auto-globbed; empty for now, reserved for Phase 6.
for _, file in ipairs(os.files("benchmarks/**.cpp")) do
    target(path.basename(file))
        add_rules("eph-bench")
        add_files(file)
        add_deps("eph-codec")
end
