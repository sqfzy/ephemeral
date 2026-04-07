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

-- Module tests (need PMD whole-archive linking)
for _, file in ipairs(os.files("tests/**.cpp")) do
    target(path.basename(file))
        add_rules("eph-test")
        add_files(file)
        add_includedirs("tests")
        add_deps("eph-dpdk")
        apply_dpdk_pmd_linkgroups()
end

-- Module benchmarks (need PMD whole-archive linking)
for _, file in ipairs(os.files("benchmarks/**.cpp")) do
    target(path.basename(file))
        add_rules("eph-bench")
        add_files(file)
        add_deps("eph-dpdk")
        add_includedirs(path.join(os.projectdir(), "benchmarks"))
        apply_dpdk_pmd_linkgroups()
end
