set_project("eph")
set_version("1.0.0")

add_rules("mode.debug", "mode.release")
set_languages("c++23")

add_rules("plugin.compile_commands.autoupdate", { outputdir = "build" })

if is_mode("release") then
    set_optimize("fastest")
end

add_requires("numactl", "tabulate", "benchmark", "spdlog", { optional = true })
add_requires("vcpkg::dpdk", { optional = true, alias = "dpdk" })
add_requires("aws-lc", { optional = true })
add_requires("gtest", { system = false, configs = { main = true } })

option("use_numa")
    set_default(false)
    set_showmenu(true)
    set_description("Enable NUMA support")
    add_defines("USE_NUMA")

target("eph-utils")
    set_kind("headeronly")
    add_includedirs("eph-utils/include", { public = true })
    add_headerfiles("eph-utils/include/(eph/utils/**.hpp)")
    add_headerfiles("eph-utils/include/(eph/version.hpp)")
    add_packages("spdlog", { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

target("eph-containers")
    set_kind("headeronly")
    add_includedirs("eph-containers/include", { public = true })
    add_headerfiles("eph-containers/include/(eph/containers/**.hpp)")
    add_deps("eph-utils", { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

local net_log_level = is_mode("debug") and "SPDLOG_LEVEL_TRACE" or "SPDLOG_LEVEL_INFO"

target("eph-net")
    set_kind("headeronly")
    add_includedirs("eph-net/include", { public = true })
    add_headerfiles("eph-net/include/(eph/net/**.hpp)")
    add_deps("eph-utils", "eph-containers", { public = true })
    add_packages("spdlog", "aws-lc", { public = true })
    add_defines("SPDLOG_ACTIVE_LEVEL=" .. net_log_level, { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

target("eph-itch")
    set_kind("headeronly")
    add_includedirs("eph-itch/include", { public = true })
    add_headerfiles("eph-itch/include/(eph/itch/**.hpp)")
    add_headerfiles("eph-itch/include/(eph/itch.hpp)")
    add_deps("eph-net", { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

target("eph-fix")
    set_kind("headeronly")
    add_includedirs("eph-fix/include", { public = true })
    add_headerfiles("eph-fix/include/(eph/fix/**.hpp)")
    add_headerfiles("eph-fix/include/(eph/fix.hpp)")
    add_deps("eph-net", { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

target("eph-dpdk")
    set_kind("headeronly")
    add_includedirs("eph-dpdk/include", { public = true })
    add_headerfiles("eph-dpdk/include/(eph/dpdk/**.hpp)")
    -- DPDK backend only needs the TcpTransport concept and public types from
    -- eph-net (tcp_concept.hpp, transport_types.hpp), not TLS/WS internals.
    -- We add eph-net's include path directly and depend on eph-utils/containers
    -- to avoid inheriting eph-net's aws-lc package dependency.
    add_deps("eph-utils", "eph-containers", { public = true })
    add_includedirs("eph-net/include", { public = true })
    add_packages("spdlog", { public = true })
    -- aws-lc is optional: only needed when using Transport<TcpSession> aliases
    -- from types.hpp. Raw DPDK TCP (tcp.hpp) works without it.
    add_packages("aws-lc", { public = true })
    add_packages("dpdk", { public = true })
    add_cxflags("-march=native", { public = true, force = true })
    add_defines("SPDLOG_ACTIVE_LEVEL=" .. net_log_level, { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

-- ===========================================================================
-- benchmarks (directory structure encodes module dependency)
-- ===========================================================================

local bench_module_deps = {
    containers = "eph-containers",
    utils      = "eph-utils",
    net        = "eph-net",
    dpdk       = "eph-dpdk",
}

for dir, dep in pairs(bench_module_deps) do
    for _, file in ipairs(os.files("benchmarks/" .. dir .. "/**.cpp")) do
        target(path.basename(file))
            set_kind("binary")
            set_group("benchmarks")
            set_default(false)
            add_files(file)
            add_includedirs("benchmarks")
            add_deps(dep)
            add_packages("benchmark")
            if dir ~= "dpdk" then
                add_packages("tabulate")
            end
    end
end

-- ===========================================================================
-- tests (directory structure encodes module dependency)
-- ===========================================================================

local test_module_deps = {
    containers = "eph-containers",
    utils      = "eph-utils",
    net        = "eph-net",
    itch       = "eph-itch",
    fix        = "eph-fix",
    dpdk       = "eph-dpdk",
}

for dir, dep in pairs(test_module_deps) do
    for _, file in ipairs(os.files("tests/" .. dir .. "/**.cpp")) do
        target(path.basename(file))
            set_kind("binary")
            set_group("tests")
            set_default(false)
            add_files(file)
            add_includedirs("tests")
            add_deps(dep)
            add_packages("gtest")
            add_defines("SPDLOG_NO_EXCEPTIONS")
    end
end

-- ===========================================================================
-- examples
-- ===========================================================================
-- Unified ws_echo_client: always links eph-net (socket backend),
-- conditionally adds DPDK support when the dpdk package is available.
target("ws_echo_client")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/ws_echo_client.cpp")
    add_deps("eph-net")
    add_cxflags("-fno-omit-frame-pointer", "-march=native", { force = true })
    set_symbols("debug")
    if has_package("dpdk") then
        add_deps("eph-dpdk")
        add_defines("EPH_HAS_DPDK")
    end
