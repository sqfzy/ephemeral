-- ============================================================================
-- eph-net-kernel — header-only kernel (epoll) backend for the v3.3 eph::net
--                   concept layer.
-- ============================================================================
--
-- Provides concrete types satisfying the `eph::net::Stream`, `eph::net::Datagram`
-- and `eph::net::Poller` concepts using the Linux epoll + POSIX socket API:
--
--   * eph::net::kernel::KernelTcpStream<C, EnableTls>  — Stream impl
--   * eph::net::kernel::KernelUdpSocket<C>             — Datagram impl
--   * eph::net::kernel::KernelPoller                   — Poller impl
--
-- Dependency rule (post-Phase-7): eph-net-kernel depends ONLY on eph-core and
-- eph-net. The TLS detail headers moved into `eph-net/include/eph/net/detail/`
-- in Phase 7, so the pragmatic dependency on eph-transport has been removed.

target("eph-net-kernel")
    set_kind("headeronly")
    add_includedirs("include", { public = true })
    add_headerfiles("include/(eph/net/kernel/**.hpp)")
    add_deps("eph-core", { public = true })
    add_deps("eph-net",  { public = true })
    add_packages("spdlog", "aws-lc", { public = true })
    add_defines("SPDLOG_ACTIVE_LEVEL=" .. net_log_level, { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

-- Module tests — one binary per test_*.cpp file (auto-globbed via eph-test rule).
-- Tests depend on eph-codec to exercise the concrete KernelTcpStream<C, false> /
-- KernelUdpSocket<C> instantiations end-to-end.
for _, file in ipairs(os.files("tests/**.cpp")) do
    target(path.basename(file))
        add_rules("eph-test")
        add_files(file)
        add_deps("eph-net-kernel", "eph-codec")
end
