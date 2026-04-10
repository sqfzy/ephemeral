target("eph-dpdk")
    set_kind("headeronly")
    add_includedirs("include", { public = true })
    add_headerfiles("include/(eph/dpdk/**.hpp)")
    add_headerfiles("include/(eph/dpdk.hpp)")
    -- DPDK backend needs eph-transport headers for Transport template.
    -- Uses add_includedirs instead of add_deps("eph-transport") to control
    -- include path ordering: aws-lc's <openssl/*.h> MUST appear before
    -- vcpkg DPDK's bundled OpenSSL (incompatible type definitions).
    add_deps("eph-core", "eph-utils", "eph-containers", { public = true })
    add_includedirs(path.join(os.projectdir(), "eph-transport/include"), { public = true })
    add_packages("spdlog", { public = true })
    add_packages("aws-lc", { public = true })
    add_packages("dpdk", { public = true })
    -- vcpkg's DPDK includes fmt headers that shadow spdlog's bundled fmt.
    add_links("fmt", { public = true })
    -- ARM64: DPDK headers need RTE_FORCE_INTRINSICS before rte_config.h.
    add_defines("RTE_FORCE_INTRINSICS", { public = true })
    add_cxflags("-include", "rte_config.h", { public = true, force = true })
    -- DPDK's rte_memcpy uses SSSE3 intrinsics (_mm_alignr_epi8) on x86,
    -- which GCC requires -mssse3 to enable. On ARM64, DPDK uses NEON
    -- intrinsics instead (enabled by default), so -mssse3 is not needed.
    if not is_arch("arm64", "arm64-v8a", "aarch64") then
        add_cxflags("-mssse3", { public = true, force = true })
    end
    add_defines("SPDLOG_ACTIVE_LEVEL=" .. net_log_level, { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

-- Module unit tests (need PMD whole-archive linking).
-- The integration/ subdirectory is excluded — those tests use real PCI
-- (NIC_B bound to vfio-pci) and cannot share an EAL with --no-pci unit
-- tests, so they are built as a single separate target below.
for _, file in ipairs(os.files("tests/*.cpp")) do
    target(path.basename(file))
        add_rules("eph-test")
        add_files(file)
        add_includedirs("tests")
        add_deps("eph-dpdk")
        apply_dpdk_pmd_linkgroups()
end

-- DPDK end-to-end integration test binary (real-NIC). Single target
-- containing all 7 P0+P1 test cases — see plan-dpdk-integration-tests-
-- 20260410-053355.md for design rationale.  Requires NIC_B bound to
-- vfio-pci at run time; tests SKIP if not.
target("test_dpdk_e2e")
    add_rules("eph-test")
    add_files("tests/integration/test_dpdk_e2e.cpp")
    add_includedirs("tests/integration")
    -- eph-net for posix_listener / posix_io helpers used by echo_mocks.hpp
    add_deps("eph-dpdk", "eph-net")
    -- The fixture reuses bench's DpdkBenchEnv via #include of
    -- benchmarks/latency/core/dpdk_env.hpp, which needs the latency
    -- include root for "core/config.hpp" etc.
    add_includedirs(path.join(os.projectdir(), "benchmarks/latency"))
    add_defines("EPH_USE_DPDK=1")
    -- Compile-time absolute path to bench.conf so the fixture can find it
    -- regardless of the test runner's cwd.
    add_defines('EPH_BENCH_CONF_ABS_PATH="' ..
        path.join(os.projectdir(), "benchmarks/latency/bench.conf") .. '"')
    apply_dpdk_pmd_linkgroups()

-- Module benchmarks (need PMD whole-archive linking)
for _, file in ipairs(os.files("benchmarks/**.cpp")) do
    target(path.basename(file))
        add_rules("eph-bench")
        add_files(file)
        add_deps("eph-dpdk")
        add_includedirs(path.join(os.scriptdir(), "benchmarks"))
        apply_dpdk_pmd_linkgroups()
end
