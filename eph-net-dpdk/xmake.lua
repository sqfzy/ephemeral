-- ============================================================================
-- eph-net-dpdk — header-only DPDK backend for the v3.3 eph::net concept layer.
-- ============================================================================
--
-- Provides concrete types satisfying the `eph::net::Stream`, `eph::net::Datagram`
-- and `eph::net::Poller` concepts on top of the DPDK kernel-bypass data plane:
--
--   * eph::net::dpdk::DpdkTcpStream<C, EnableTls>  — Stream impl
--   * eph::net::dpdk::DpdkUdpSocket<C>             — Datagram impl
--   * eph::net::dpdk::DpdkPoller<>                 — Poller impl (lcore burst)
--   * eph::net::dpdk::Eal                          — RAII EAL init
--
-- And absorbs the low-level DPDK primitives that used to live in eph-dpdk
-- (they are header-only and stay under the `eph::dpdk` namespace / `eph/dpdk/`
-- include path to minimise churn):
--
--   * eph::dpdk::TcpSession / UdpSender / RxDispatcher / Connector / Multicast /
--     ARP / DNS / FlowSteering / packet_* / platform / EalGuard
--
-- Dependency rule (post-Phase-7): eph-core + eph-utils + eph-containers +
-- eph-net + aws-lc + DPDK. The Phase 4 pragmatic `eph-dpdk` dependency has
-- been removed — those source files now live in this module.
--
-- aws-lc vs vcpkg-openssl: the DPDK TU used to pick up vcpkg-openssl (via
-- vcpkg::dpdk's bundled include) which conflicted with aws-lc's openssl
-- headers. Phase 7 removed the two call sites that pulled <openssl/rand.h>
-- (TcpSession ISN generation, WS mask key cache) and replaced them with
-- `getrandom(2)`. After that change aws-lc is the only OpenSSL flavour in
-- any eph-net-dpdk TU and DPDK TLS compiles cleanly.

target("eph-net-dpdk")
    set_kind("headeronly")
    add_includedirs("include", { public = true })
    add_headerfiles("include/(eph/net/dpdk/**.hpp)")
    add_headerfiles("include/(eph/dpdk/**.hpp)")
    add_headerfiles("include/(eph/dpdk.hpp)")
    add_deps("eph-core", "eph-utils", "eph-containers", "eph-net", { public = true })
    -- Include-order trick: xmake's default `add_packages` layout drops
    -- vcpkg DPDK's include directory BEFORE aws-lc's, so a plain
    -- `#include <openssl/evp.h>` resolves to vcpkg-openssl's copy (which
    -- is ABI-incompatible with aws-lc's). The Phase 7 fix also removed
    -- every lingering `<openssl/rand.h>` call in the DPDK subtree
    -- (TcpSession ISN, DNS tx_id, WS mask cache) — but the TLS path in
    -- eph-net/detail/tls_session.hpp STILL needs aws-lc's <openssl/ssl.h>
    -- and friends, and those must be found before vcpkg-openssl's copies.
    --
    -- We rely on the `/tmp/gcc14-wrap/g++` wrapper (shipped with this
    -- workspace) to reorder `-isystem` flags so that any path containing
    -- `aws-lc` is emitted before any path that doesn't. This is a
    -- build-environment workaround; a cleaner long-term fix would be to
    -- drop vcpkg DPDK in favour of a system libdpdk that does not bundle
    -- its own openssl.
    add_packages("spdlog", "aws-lc", { public = true })
    add_packages("dpdk", { public = true })
    -- vcpkg's DPDK includes fmt headers that shadow spdlog's bundled fmt.
    add_links("fmt", { public = true })
    -- ARM64: DPDK headers need RTE_FORCE_INTRINSICS before rte_config.h.
    add_defines("RTE_FORCE_INTRINSICS", { public = true })
    add_cxflags("-include", "rte_config.h", { public = true, force = true })
    -- DPDK's rte_memcpy uses SSSE3 intrinsics on x86 (requires -mssse3);
    -- on ARM64 the NEON backend is enabled by default.
    if not is_arch("arm64", "arm64-v8a", "aarch64") then
        add_cxflags("-mssse3", { public = true, force = true })
    end
    add_defines("SPDLOG_ACTIVE_LEVEL=" .. net_log_level, { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

-- Module unit tests (v3 API tests). Every test target needs PMD
-- whole-archive linking. Tests use --no-pci mode (see dpdk_test_env.hpp)
-- so they run on any host without a vfio-pci NIC.
for _, file in ipairs(os.files("tests/*.cpp")) do
    target(path.basename(file))
        add_rules("eph-test")
        add_files(file)
        add_includedirs("tests")
        add_deps("eph-net-dpdk", "eph-codec")
        apply_dpdk_pmd_linkgroups()
end

-- Legacy DPDK unit tests — these exercise the low-level `eph::dpdk::*`
-- primitives that migrated into this module from eph-dpdk in Phase 7.
-- They run under --no-pci mode and do NOT touch the v3 API.
for _, file in ipairs(os.files("tests/legacy/*.cpp")) do
    target(path.basename(file))
        add_rules("eph-test")
        add_files(file)
        add_includedirs("tests/legacy")
        add_deps("eph-net-dpdk")
        apply_dpdk_pmd_linkgroups()
end

-- DPDK end-to-end integration test binary (real-NIC). Requires NIC_B bound
-- to vfio-pci at run time; tests SKIP otherwise.
target("test_dpdk_e2e")
    add_rules("eph-test")
    add_files("tests/integration/test_dpdk_e2e.cpp")
    add_includedirs("tests/integration")
    -- eph-net for posix_listener / posix_io helpers used by echo_mocks.hpp
    add_deps("eph-net-dpdk", "eph-net")
    -- The fixture reuses bench's DpdkBenchEnv via #include of
    -- benchmarks/latency/core/dpdk_env.hpp.
    add_includedirs(path.join(os.projectdir(), "benchmarks/latency"))
    add_defines("EPH_USE_DPDK=1")
    add_defines('EPH_BENCH_CONF_ABS_PATH="' ..
        path.join(os.projectdir(), "benchmarks/latency/bench.conf") .. '"')
    apply_dpdk_pmd_linkgroups()

-- Module benchmarks — low-level DPDK primitive microbenchmarks migrated
-- over from eph-dpdk/benchmarks. Need PMD whole-archive linking.
for _, file in ipairs(os.files("benchmarks/*.cpp")) do
    target(path.basename(file))
        add_rules("eph-bench")
        add_files(file)
        add_deps("eph-net-dpdk")
        add_includedirs(path.join(os.scriptdir(), "benchmarks"))
        apply_dpdk_pmd_linkgroups()
end

-- Module fuzzers
for _, file in ipairs(os.files("fuzzers/*.cpp")) do
    target(path.basename(file))
        set_kind("binary")
        set_group("fuzzers")
        set_default(false)
        add_files(file)
        add_deps("eph-net-dpdk")
        apply_dpdk_pmd_linkgroups()
end
