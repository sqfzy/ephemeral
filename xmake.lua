set_project("eph")
set_version("1.0.0")

add_rules("mode.debug", "mode.release")
set_languages("c++23")

add_rules("plugin.compile_commands.autoupdate", { outputdir = "build" })

if is_mode("release") then
    set_optimize("fastest")
end

add_requires("numactl", "tabulate", "benchmark", "spdlog", "dpdk", { optional = true })
add_requires("gtest", { system = false, configs = { main = true } })

option("use_numa")
    set_default(false)
    set_showmenu(true)
    set_description("Enable NUMA support")
    add_defines("USE_NUMA")

-- ===========================================================================
-- eph-base: 基础类型、概念约束、缓存行常量
-- ===========================================================================
target("eph-base")
    set_kind("headeronly")
    add_includedirs("eph-base/include", { public = true })
    add_headerfiles("eph-base/include/(eph/base/**.hpp)")
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

-- ===========================================================================
-- eph-utils: CPU 拓扑、TSC 计时、对齐工具、大页内存、性能记录
-- ===========================================================================
target("eph-utils")
    set_kind("headeronly")
    add_includedirs("eph-utils/include", { public = true })
    add_headerfiles("eph-utils/include/(eph/utils/**.hpp)")
    add_deps("eph-base", { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

-- ===========================================================================
-- eph-containers: BoundedQueue, EvictingQueue 及其 Bytes 变体
-- ===========================================================================
target("eph-containers")
    set_kind("headeronly")
    add_includedirs("eph-containers/include", { public = true })
    add_headerfiles("eph-containers/include/(eph/containers/**.hpp)")
    add_deps("eph-base", "eph-utils", { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

-- ===========================================================================
-- eph-dpdk: DPDK Platform + TX Engine (header-only)
-- ===========================================================================
local dpdk_log_level = is_mode("debug") and "SPDLOG_LEVEL_TRACE" or "SPDLOG_LEVEL_INFO"

target("eph-dpdk")
    set_kind("headeronly")
    add_includedirs("eph-dpdk/include", { public = true })
    add_headerfiles("eph-dpdk/include/(eph_dpdk/**.hpp)")
    add_packages("dpdk", "spdlog", { public = true })
    add_defines("SPDLOG_ACTIVE_LEVEL=" .. dpdk_log_level, { public = true })
    -- -march=corei7 required by rte_memcpy.h (SSSE3 intrinsics); propagated
    -- publicly so all dependents compile with the same ISA DPDK was built for.
    add_cxflags("-march=corei7", { public = true, force = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

-- ===========================================================================
-- benchmarks
-- ===========================================================================
for _, file in ipairs(os.files("benchmarks/**.cpp")) do
    local name = path.basename(file)

    target(name)
        set_kind("binary")
        set_group("benchmarks")
        set_default(false)
        add_files(file)
        add_deps("eph-containers")
        add_packages("tabulate")
        add_packages("benchmark")
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
        if name:find("test_platform") or name:find("test_tx_engine") then
            add_deps("eph-dpdk")
            add_packages("gtest")
            add_defines("SPDLOG_NO_EXCEPTIONS")
        elseif name:find("bounded_queue") or name:find("evicting_queue") then
            add_deps("eph-containers")
            add_packages("gtest")
        elseif name:find("cpu") or name:find("time") or name:find("record") or name:find("hugepage") then
            add_deps("eph-utils")
            add_packages("gtest")
        else
            add_deps("eph-base")
            add_packages("gtest")
        end
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
