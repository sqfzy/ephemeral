set_project("eph")
set_version("1.0.0")

add_rules("mode.debug", "mode.release", "mode.asan", "mode.tsan")
set_languages("c++23")
set_warnings("all", "extra")
set_policy("build.ccache", true)

add_rules("plugin.compile_commands.autoupdate", { outputdir = "build" })

if is_mode("release") then
    set_optimize("fastest")
end

-- Sanitizer modes: xmake f -m asan / xmake f -m tsan
if is_mode("asan") then
    set_optimize("none")
    set_symbols("debug")
    add_cxflags("-fsanitize=address,undefined", "-fno-omit-frame-pointer", { force = true })
    add_ldflags("-fsanitize=address,undefined", { force = true })
    add_defines("EPH_ASAN_ENABLED")
end
if is_mode("tsan") then
    set_optimize("none")
    set_symbols("debug")
    add_cxflags("-fsanitize=thread", "-fno-omit-frame-pointer", { force = true })
    add_ldflags("-fsanitize=thread", { force = true })
    add_defines("EPH_TSAN_ENABLED")
end

-- GCC 14 on Amazon Linux 2023: add libstdc++ path for __cxa_call_terminate etc.
if os.isfile("/usr/lib/gcc/aarch64-amazon-linux/14/libstdc++.so") then
    add_linkdirs("/usr/lib/gcc/aarch64-amazon-linux/14")
    add_rpathdirs("/usr/lib/gcc/aarch64-amazon-linux/14")
end

-- Dependencies
add_requires("numactl", "tabulate", "benchmark", "spdlog", { optional = true })
add_requires("vcpkg::dpdk", { optional = true, alias = "dpdk" })
add_requires("aws-lc", { optional = true })
add_requires("gtest", { system = false, configs = { main = true } })

-- Options
option("use_numa")
    set_default(false)
    set_showmenu(true)
    set_description("Enable NUMA support")
    add_defines("USE_NUMA")

option("native_arch")
    set_default(false)
    set_showmenu(true)
    set_description("Enable -march=native for performance-critical targets")

-- Global constants (inherited by module xmake.lua via includes() scope sharing)
net_log_level = is_mode("debug") and "SPDLOG_LEVEL_TRACE" or "SPDLOG_LEVEL_INFO"

-- Global helper for DPDK PMD whole-archive linking
function apply_dpdk_pmd_linkgroups()
    add_linkgroups("rte_net_null", "rte_net_ena", "rte_net_af_packet",
                   "rte_bus_pci", "rte_bus_vdev", "rte_mempool_ring",
                   { whole = true })
end

-- ===========================================================================
-- Shared rules for test and benchmark targets
-- ===========================================================================

rule("eph-test")
    on_load(function (target)
        target:set("kind", "binary")
        target:set("group", "tests")
        target:set("default", false)
        target:add("packages", "gtest")
        target:add("defines", "SPDLOG_NO_EXCEPTIONS")
        -- PCH available at build/pch_test.hpp; enable with:
        -- target:set("pcxxheader", path.join(os.projectdir(), "build/pch_test.hpp"))
    end)

rule("eph-bench")
    on_load(function (target)
        target:set("kind", "binary")
        target:set("group", "benchmarks")
        target:set("default", false)
        target:add("packages", "benchmark")
        -- PCH available at build/pch_bench.hpp; enable with:
        -- target:set("pcxxheader", path.join(os.projectdir(), "build/pch_bench.hpp"))
        if has_config("native_arch") then
            target:add("cxflags", "-march=native", { force = true })
        end
    end)

-- ===========================================================================
-- Module includes (dependency order)
-- ===========================================================================

includes("eph-core/xmake.lua")
includes("eph-utils/xmake.lua")
includes("eph-containers/xmake.lua")
includes("eph-fix/xmake.lua")
includes("eph-itch/xmake.lua")
includes("eph-json/xmake.lua")
includes("eph-book/xmake.lua")
-- Phase 7: eph-transport and eph-dpdk have been deleted. Their source
-- files were migrated into eph-net/include/eph/net/detail/ (TLS + WS wire
-- helpers) and eph-net-dpdk/include/eph/dpdk/ (DPDK low-level primitives).
includes("eph-net/xmake.lua")
includes("eph-codec/xmake.lua")
includes("eph-net-kernel/xmake.lua")
includes("eph-net-dpdk/xmake.lua")

-- Cross-module integration tests
includes("tests/integration/xmake.lua")

-- Unit tests for the bench shared layer (header-only library under
-- benchmarks/latency/core/).
includes("tests/unit/bench/xmake.lua")

-- Latency benchmark suite (self-managed sub-xmake, one lat_* binary per
-- scenario source file). Plan reference:
-- .artifacts/plan-bench-latency-simplify-20260409-065640.md
includes("benchmarks/latency/xmake.lua")

-- ===========================================================================
-- Examples (centralized, user-facing). Post-Phase-7 all examples target
-- the v3.3 eph-net-kernel / eph-net-dpdk / eph-codec API.
-- ===========================================================================

target("simple_hft")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/simple_hft.cpp")
    add_deps("eph-net-kernel", "eph-codec")

target("simple_hft_dpdk")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/simple_hft_dpdk.cpp")
    add_deps("eph-net-dpdk", "eph-codec")
    apply_dpdk_pmd_linkgroups()

target("simple_hft_dpdk_reactor")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/simple_hft_dpdk_reactor.cpp")
    add_deps("eph-net-dpdk", "eph-codec")
    apply_dpdk_pmd_linkgroups()

target("binance_book")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/binance_book.cpp")
    add_deps("eph-net-kernel", "eph-codec", "eph-json", "eph-book")

target("framer_showcase")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/framer_showcase.cpp")
    add_deps("eph-codec")

target("minimal_ws_client")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/minimal_ws_client.cpp")
    add_deps("eph-net-kernel", "eph-codec")

target("production_client")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/production_client.cpp")
    add_deps("eph-net-kernel", "eph-codec")

target("ws_echo_client")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/ws_echo_client.cpp")
    add_deps("eph-net-kernel", "eph-codec")

target("ws_via_proxy")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/ws_via_proxy.cpp")
    add_deps("eph-net-kernel", "eph-codec")
