set_project("eph")
set_version("1.0.0")

add_rules("mode.debug", "mode.release")
set_languages("c++23")

add_rules("plugin.compile_commands.autoupdate", { outputdir = "build" })

if is_mode("release") then
    set_optimize("fastest")
end

add_requires("numactl", "tabulate", "benchmark", "spdlog", "dpdk", { optional = true })
add_requires("aws-lc", { optional = true })
add_requires("gtest", { system = false, configs = { main = true } })

option("use_numa")
    set_default(false)
    set_showmenu(true)
    set_description("Enable NUMA support")
    add_defines("USE_NUMA")

target("eph-base")
    set_kind("headeronly")
    add_includedirs("eph-base/include", { public = true })
    add_headerfiles("eph-base/include/(eph/base/**.hpp)")
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

target("eph-utils")
    set_kind("headeronly")
    add_includedirs("eph-utils/include", { public = true })
    add_headerfiles("eph-utils/include/(eph/utils/**.hpp)")
    add_deps("eph-base", { public = true })
    add_packages("spdlog", { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

target("eph-containers")
    set_kind("headeronly")
    add_includedirs("eph-containers/include", { public = true })
    add_headerfiles("eph-containers/include/(eph/containers/**.hpp)")
    add_deps("eph-base", "eph-utils", { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

local net_log_level = is_mode("debug") and "SPDLOG_LEVEL_TRACE" or "SPDLOG_LEVEL_INFO"

target("eph-net")
    set_kind("headeronly")
    add_includedirs("eph-net/include", { public = true })
    add_headerfiles("eph-net/include/(eph/net/**.hpp)")
    add_deps("eph-base", "eph-utils", "eph-containers", { public = true })
    add_packages("spdlog", "aws-lc", { public = true })
    add_defines("SPDLOG_ACTIVE_LEVEL=" .. net_log_level, { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

target("eph-dpdk")
    set_kind("headeronly")
    add_includedirs("eph-dpdk/include", { public = true })
    add_headerfiles("eph-dpdk/include/(eph/dpdk/**.hpp)")
    add_deps("eph-net", { public = true })
    add_packages("dpdk", { public = true })
    add_cxflags("-march=corei7", { public = true, force = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

-- ===========================================================================
-- benchmarks
-- ===========================================================================

-- WS pipeline benchmark — needs eph-net (aws-lc headers) + eph-dpdk (net_header) + Google Benchmark
target("bench_ws_pipeline")
    set_kind("binary")
    set_group("benchmarks")
    set_default(false)
    add_files("benchmarks/bench_ws_pipeline.cpp")
    add_deps("eph-dpdk")
    add_packages("benchmark")

for _, file in ipairs(os.files("benchmarks/**.cpp")) do
    local name = path.basename(file)
    if name == "bench_ws_pipeline" then goto continue end

    target(name)
        set_kind("binary")
        set_group("benchmarks")
        set_default(false)
        add_files(file)
        add_deps("eph-containers")
        add_packages("tabulate")
        add_packages("benchmark")

    ::continue::
end

-- ===========================================================================
-- tests
-- ===========================================================================

-- Tests that need DPDK (net_header, platform)
local dpdk_tests = {
    test_net_header = true,
    test_dpdk_platform = true,
    test_arp = true,
}

-- Tests that only need eph-net (no DPDK required)
local net_tests = {
    test_websocket = true,
    test_http = true,
    test_tls_record = true,
    test_tcp_concept = true,
}

for _, file in ipairs(os.files("tests/**.cpp")) do
    local name = path.basename(file)

    target(name)
        set_kind("binary")
        set_group("tests")
        set_default(false)
        add_files(file)
        if dpdk_tests[name] then
            add_deps("eph-dpdk")
        elseif net_tests[name] then
            add_deps("eph-net")
        else
            -- Default: add both for safety (containers, utils tests don't need either)
            add_deps("eph-containers")
            add_deps("eph-utils")
            add_deps("eph-base")
        end
        add_packages("gtest")
        add_defines("SPDLOG_NO_EXCEPTIONS")
end

-- ===========================================================================
-- examples
-- ===========================================================================
for _, file in ipairs(os.files("examples/**.cpp")) do
    local name = path.basename(file)

    target(name)
        set_kind("binary")
        set_group("examples")
        set_default(false)
        add_files(file)
        add_deps("eph-containers")
        add_cxflags("-fno-omit-frame-pointer", "-march=native", { force = true })
        set_symbols("debug")
end
