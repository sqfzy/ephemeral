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
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

target("eph-containers")
    set_kind("headeronly")
    add_includedirs("eph-containers/include", { public = true })
    add_headerfiles("eph-containers/include/(eph/containers/**.hpp)")
    add_deps("eph-base", "eph-utils", { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

local dpdk_log_level = is_mode("debug") and "SPDLOG_LEVEL_TRACE" or "SPDLOG_LEVEL_INFO"

target("eph-dpdk")
    set_kind("headeronly")
    add_includedirs("eph-dpdk/include", { public = true })
    add_headerfiles("eph-dpdk/include/(eph/dpdk/**.hpp)")
    add_deps("eph-base", "eph-utils", "eph-containers", { public = true })
    add_packages("dpdk", "spdlog", "aws-lc", { public = true })
    add_defines("SPDLOG_ACTIVE_LEVEL=" .. dpdk_log_level, { public = true })
    add_cxflags("-march=corei7", { public = true, force = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

-- ===========================================================================
-- benchmarks
-- ===========================================================================

-- WS pipeline benchmark — needs eph-dpdk (DPDK + OpenSSL headers)
target("bench_ws_pipeline")
    set_kind("binary")
    set_group("benchmarks")
    set_default(false)
    add_files("benchmarks/bench_ws_pipeline.cpp")
    add_deps("eph-dpdk", "eph-containers", "eph-utils", "eph-base")
    add_defines("EPH_PROJECT_ROOT=\"" .. os.projectdir() .. "\"")

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
for _, file in ipairs(os.files("tests/**.cpp")) do
    local name = path.basename(file)

    target(name)
        set_kind("binary")
        set_group("tests")
        set_default(false)
        add_files(file)
        add_deps("eph-dpdk")
        add_deps("eph-containers")
        add_deps("eph-utils")
        add_deps("eph-base")
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
