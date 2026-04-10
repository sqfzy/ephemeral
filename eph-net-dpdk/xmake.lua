-- ============================================================================
-- eph-net-dpdk — header-only DPDK backend for the v3.3 eph::net concept layer.
-- ============================================================================
--
-- Phase 4 of the v3.3 architecture refactor (see
-- .artifacts/design-eph-v3.3-architecture-20260410.md).
--
-- Provides concrete types satisfying the `eph::net::Stream`, `eph::net::Datagram`
-- and `eph::net::Poller` concepts on top of the DPDK kernel-bypass data plane:
--
--   * eph::net::dpdk::DpdkTcpStream<C, EnableTls>  — Stream impl (wraps TcpSession)
--   * eph::net::dpdk::DpdkUdpSocket<C>             — Datagram impl (wraps UdpSender
--                                                     + inline RX path)
--   * eph::net::dpdk::DpdkPoller<>                 — Poller impl (lcore burst poll
--                                                     with P2 function-pointer type
--                                                     erase for heterogeneous
--                                                     Pollables)
--   * eph::net::dpdk::Eal                          — re-export of EalGuard
--
-- Dependency rule:
--   * Final (post-Phase-7): eph-net-dpdk depends on eph-core + eph-net + DPDK.
--   * TEMPORARY (Phase 4-6): depends pragmatically on `eph-dpdk` to reuse the
--     battle-tested TcpSession / UdpSender / Eal / flow_steering / packet_template
--     / multicast / arp / dns primitives. Phase 7 will migrate those sources
--     into this module (or delete them after inlining) and drop the eph-dpdk
--     dep. This mirrors Phase 1's eph-codec -> eph-transport pragmatic dep.
--
-- Phase 4 is ADDITIVE: the legacy `eph-dpdk/include/eph/dpdk/*` headers stay
-- in place untouched and are only removed in Phase 7. The new DPDK backend
-- lives here in its own `eph::net::dpdk` namespace so it can coexist with the
-- legacy `eph::dpdk::TcpSession` / `eph::dpdk::Reactor` / `eph::dpdk::UdpSender`
-- until then.
--
-- DPDK availability: this module is only enabled when the optional `dpdk`
-- package is resolvable (xmake will silently skip building it if not). The
-- package is declared in the root xmake.lua as `add_requires("vcpkg::dpdk",
-- { optional = true, alias = "dpdk" })`. Kernel-only CI hosts therefore
-- tolerate the module being present without needing DPDK installed.

target("eph-net-dpdk")
    set_kind("headeronly")
    add_includedirs("include", { public = true })
    add_headerfiles("include/(eph/net/dpdk/**.hpp)")
    -- TEMPORARY: reuse eph-dpdk internals. Phase 7 drops this dep after
    -- migrating the source files into this module's own detail/ subdir.
    add_deps("eph-dpdk", { public = true })
    add_deps("eph-core", "eph-net", { public = true })
    add_packages("spdlog", { public = true })
    add_defines("SPDLOG_ACTIVE_LEVEL=" .. net_log_level, { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

-- Module tests — one binary per test_*.cpp file (auto-globbed via eph-test
-- rule). Because DpdkTcpStream / DpdkUdpSocket / DpdkPoller reach into the
-- underlying DPDK PMDs, every test target needs PMD whole-archive linking.
-- The tests use --no-pci mode (see dpdk_test_env.hpp) so they run on any
-- host without a vfio-pci-bound NIC.
for _, file in ipairs(os.files("tests/*.cpp")) do
    target(path.basename(file))
        add_rules("eph-test")
        add_files(file)
        add_includedirs("tests")
        add_deps("eph-net-dpdk", "eph-codec")
        apply_dpdk_pmd_linkgroups()
end
