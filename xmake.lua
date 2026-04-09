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
includes("eph-transport/xmake.lua")
includes("eph-fix/xmake.lua")
includes("eph-itch/xmake.lua")
includes("eph-json/xmake.lua")
includes("eph-book/xmake.lua")
includes("eph-net/xmake.lua")
includes("eph-dpdk/xmake.lua")

-- Cross-module integration tests
includes("tests/integration/xmake.lua")

-- Unit tests for the bench shared layer (header-only library under
-- benchmarks/latency/core/).
includes("tests/unit/bench/xmake.lua")

-- ===========================================================================
-- Latency benchmarks (cross-module, root-managed)
-- ===========================================================================
-- Plan reference: .artifacts/plan-bench-latency-simplify-20260409-065640.md
--
-- Stage 4: each tcp/udp/ws scenario is a single `lat_<scenario>.cpp` that
-- forks the mock + runs the client in one binary, reading all configuration
-- from `bench.conf` via `bench::load_bench_conf()`. Two targets per scenario:
-- the kernel build and the DPDK build (EPH_USE_DPDK selects the transport).

local bench_latency_scenarios = {"tcp", "udp", "ws"}
local bench_latency_flags = {"-fno-omit-frame-pointer", "-march=native"}

for _, s in ipairs(bench_latency_scenarios) do
    -- Kernel build: fork the mock + run kernel client.
    target("lat_" .. s)
        set_kind("binary")
        set_group("benchmarks")
        set_default(false)
        add_files("benchmarks/latency/" .. s .. "/lat_" .. s .. ".cpp")
        add_includedirs("benchmarks/latency")
        add_deps("eph-utils")
        add_packages("spdlog")
        add_cxflags(table.unpack(bench_latency_flags))
        set_symbols("debug")

    -- DPDK build: fork the kernel mock + run DPDK client (NIC-B in vfio-pci).
    target("lat_" .. s .. "_dpdk")
        set_kind("binary")
        set_group("benchmarks")
        set_default(false)
        add_files("benchmarks/latency/" .. s .. "/lat_" .. s .. ".cpp")
        add_includedirs("benchmarks/latency")
        add_deps("eph-utils", "eph-dpdk")
        add_packages("spdlog")
        add_defines("EPH_USE_DPDK=1")
        add_cxflags(table.unpack(bench_latency_flags))
        set_symbols("debug")
        apply_dpdk_pmd_linkgroups()
end

-- Stage 5: exchange scenarios — same single-binary fork pattern as the
-- raw protocol scenarios. Three scenarios (market / order / md_udp), each
-- in a kernel and a DPDK build. Mocks (mock_ws.hpp, mock_md_udp.hpp) are
-- always kernel — DPDK only changes the client transport.
local bench_latency_exchange_scenarios = {"ex_market", "ex_order", "ex_md_udp"}

for _, s in ipairs(bench_latency_exchange_scenarios) do
    target("lat_" .. s)
        set_kind("binary")
        set_group("benchmarks")
        set_default(false)
        add_files("benchmarks/latency/exchange/lat_" .. s .. ".cpp")
        add_includedirs("benchmarks/latency")
        add_deps("eph-utils")
        add_packages("spdlog")
        add_cxflags(table.unpack(bench_latency_flags))
        set_symbols("debug")

    target("lat_" .. s .. "_dpdk")
        set_kind("binary")
        set_group("benchmarks")
        set_default(false)
        add_files("benchmarks/latency/exchange/lat_" .. s .. ".cpp")
        add_includedirs("benchmarks/latency")
        add_deps("eph-utils", "eph-dpdk")
        add_packages("spdlog")
        add_defines("EPH_USE_DPDK=1")
        add_cxflags(table.unpack(bench_latency_flags))
        set_symbols("debug")
        apply_dpdk_pmd_linkgroups()
end

-- ===========================================================================
-- Examples (centralized, user-facing)
-- ===========================================================================

target("ws_echo_client")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/ws_echo_client.cpp")
    add_deps("eph-net")
    add_cxflags("-fno-omit-frame-pointer", "-march=native", { force = true })
    set_symbols("debug")

target("ws_echo_client_dpdk")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/ws_echo_client_dpdk.cpp")
    add_deps("eph-net", "eph-dpdk", "eph-fix")
    add_cxflags("-fno-omit-frame-pointer", "-march=native", { force = true })
    set_symbols("debug")
    apply_dpdk_pmd_linkgroups()

target("minimal_ws_client")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/minimal_ws_client.cpp")
    add_deps("eph-net")

target("production_client")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/production_client.cpp")
    add_deps("eph-net")

target("spsc_queue_demo")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/spsc_queue_demo.cpp")
    add_deps("eph-containers")

target("perf_tuning_basics")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/perf_tuning_basics.cpp")
    add_deps("eph-utils")

target("dpdk_quickstart")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/dpdk_quickstart.cpp")
    add_deps("eph-net", "eph-dpdk", "eph-fix")
    apply_dpdk_pmd_linkgroups()

target("ws_via_proxy")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/ws_via_proxy.cpp")
    add_deps("eph-net")

target("framer_showcase")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/framer_showcase.cpp")
    add_deps("eph-net")

target("fix_trading_demo")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/fix_trading_demo.cpp")
    add_deps("eph-fix", "eph-utils")

target("itch_feed_demo")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/itch_feed_demo.cpp")
    add_deps("eph-itch")

target("binance_book")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/binance_book.cpp")
    add_deps("eph-net", "eph-json", "eph-book")

target("simple_hft")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/simple_hft.cpp")
    add_deps("eph-net")
    add_defines("EPH_ENABLE_TIMESTAMPS=1")
    add_cxflags("-fno-omit-frame-pointer", "-march=native", { force = true })
    set_symbols("debug")

target("simple_hft_dpdk")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/simple_hft_dpdk.cpp")
    add_deps("eph-net", "eph-dpdk", "eph-fix")
    add_defines("EPH_ENABLE_TIMESTAMPS=1")
    add_cxflags("-fno-omit-frame-pointer", "-march=native", { force = true })
    set_symbols("debug")
    apply_dpdk_pmd_linkgroups()
