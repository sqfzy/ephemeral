-- ============================================================================
-- eph-net — networking concept layer + shared detail helpers.
-- ============================================================================
--
-- Responsibilities:
--   * eph::net Stream / Datagram / Pollable / Poller concepts
--     (`include/eph/net/concepts.hpp`)
--   * Shared value types: SocketAddr, TcpState, ReconnectPolicy
--   * Test doubles: FakeStream, FakeDatagram, TestPoller under
--     `include/eph/net/test/`
--   * Header-only test helpers for POSIX mock servers: posix_io.hpp,
--     posix_listener.hpp (used by bench mocks + integration tests)
--   * Detail TLS building blocks (tls_constants, tls_record, tls_encryptor,
--     tls_decryptor, tls_session, tls_inplace, websocket wire helpers)
--     under `include/eph/net/detail/`. The kernel + DPDK backends share
--     these primitives.
--
-- Dependency rule: depends only on eph-core (and spdlog / aws-lc via the
-- detail headers).

target("eph-net")
    set_kind("headeronly")
    add_includedirs("include", { public = true })
    add_headerfiles("include/(eph/net/**.hpp)")
    add_deps("eph-core", { public = true })
    add_packages("spdlog", "aws-lc", { public = true })
    add_defines("SPDLOG_ACTIVE_LEVEL=" .. net_log_level, { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

-- Module tests — one binary per test_*.cpp file (auto-globbed).
for _, file in ipairs(os.files("tests/**.cpp")) do
    target(path.basename(file))
        add_rules("eph-test")
        add_files(file)
        add_deps("eph-net")
end

-- Module benchmarks — auto-globbed.
for _, file in ipairs(os.files("benchmarks/**.cpp")) do
    target(path.basename(file))
        add_rules("eph-bench")
        add_files(file)
        add_deps("eph-net")
end
