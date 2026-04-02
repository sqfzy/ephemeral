set_project("eph")
set_version("1.0.0")

add_rules("mode.debug", "mode.release")
set_languages("c++23")
set_warnings("all", "extra")


add_rules("plugin.compile_commands.autoupdate", { outputdir = "build" })

if is_mode("release") then
    set_optimize("fastest")
end

-- GCC 14 on Amazon Linux 2023: add libstdc++ path for __cxa_call_terminate etc.
if os.isfile("/usr/lib/gcc/aarch64-amazon-linux/14/libstdc++.so") then
    add_linkdirs("/usr/lib/gcc/aarch64-amazon-linux/14")
    add_rpathdirs("/usr/lib/gcc/aarch64-amazon-linux/14")
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

local net_log_level = is_mode("debug") and "SPDLOG_LEVEL_TRACE" or "SPDLOG_LEVEL_INFO"

target("eph-utils")
    set_kind("headeronly")
    add_includedirs("eph-utils/include", { public = true })
    add_headerfiles("eph-utils/include/(eph/utils/**.hpp)")
    add_headerfiles("eph-utils/include/(eph/version.hpp)")
    add_deps("eph-core", { public = true })
    add_packages("spdlog", { public = true })
    add_defines("SPDLOG_ACTIVE_LEVEL=" .. net_log_level, { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

    -- Verify C++23 support at configure time (std::expected, std::format).
    -- Uses check_cxxsnippets instead of has_features because xmake's
    -- cxx_std_23 feature detection may not recognize all capable compilers.
    on_config(function (target)
        import("lib.detect.check_cxxsnippets")
        local ok = check_cxxsnippets({test = [[
            #include <expected>
            #include <format>
            void test() {
                std::expected<int, std::string> e = 42;
                auto s = std::format("{}", *e);
            }
        ]]}, {configs = {languages = "c++23"}})
        if not ok then
            raise("C++23 not supported by current compiler.\n"
                  .. "  GCC >= 13 or Clang >= 17 required.\n"
                  .. "  Amazon Linux 2023: dnf install gcc14-g++ && EPH_USE_GCC14=1 xmake build")
        end
    end)

target("eph-containers")
    set_kind("headeronly")
    add_includedirs("eph-containers/include", { public = true })
    add_headerfiles("eph-containers/include/(eph/containers/**.hpp)")
    add_deps("eph-utils", { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

target("eph-core")
    set_kind("headeronly")
    add_includedirs("eph-core/include", { public = true })
    add_headerfiles("eph-core/include/(eph/core/**.hpp)")
    add_headerfiles("eph-core/include/(eph/version.hpp)")
    add_packages("spdlog", { public = true })
    add_defines("SPDLOG_ACTIVE_LEVEL=" .. net_log_level, { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

target("eph-transport")
    set_kind("headeronly")
    add_includedirs("eph-transport/include", { public = true })
    add_headerfiles("eph-transport/include/(eph/transport/**.hpp)")
    add_deps("eph-core", "eph-utils", "eph-containers", { public = true })
    add_packages("spdlog", "aws-lc", { public = true })
    add_defines("SPDLOG_ACTIVE_LEVEL=" .. net_log_level, { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

target("eph-net")
    set_kind("headeronly")
    add_includedirs("eph-net/include", { public = true })
    add_headerfiles("eph-net/include/(eph/net/**.hpp)")
    add_deps("eph-transport", { public = true })
    add_packages("spdlog", { public = true })
    add_defines("SPDLOG_ACTIVE_LEVEL=" .. net_log_level, { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

target("eph-itch")
    set_kind("headeronly")
    add_includedirs("eph-itch/include", { public = true })
    add_headerfiles("eph-itch/include/(eph/itch/**.hpp)")
    add_headerfiles("eph-itch/include/(eph/itch.hpp)")
    add_deps("eph-core", { public = true })
    add_packages("spdlog", { public = true })
    add_defines("SPDLOG_ACTIVE_LEVEL=" .. net_log_level, { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

target("eph-fix")
    set_kind("headeronly")
    add_includedirs("eph-fix/include", { public = true })
    add_headerfiles("eph-fix/include/(eph/fix/**.hpp)")
    add_headerfiles("eph-fix/include/(eph/fix.hpp)")
    -- eph-fix only needs framer_concept.hpp from eph-core (pure stdlib, no aws-lc).
    -- Using add_deps("eph-core") instead of add_deps("eph-net") avoids inheriting
    -- aws-lc package dependency, preventing OpenSSL header conflicts when
    -- eph-fix and eph-dpdk (which brings vcpkg OpenSSL via DPDK) coexist.
    add_deps("eph-core", { public = true })
    add_packages("spdlog", { public = true })
    add_defines("SPDLOG_ACTIVE_LEVEL=" .. net_log_level, { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

target("eph-json")
    set_kind("headeronly")
    add_includedirs("eph-json/include", { public = true })
    add_headerfiles("eph-json/include/(eph/json/**.hpp)")
    add_headerfiles("eph-json/include/(eph/json.hpp)")
    add_deps("eph-core", { public = true })
    add_packages("spdlog", { public = true })
    add_defines("SPDLOG_ACTIVE_LEVEL=" .. net_log_level, { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

target("eph-book")
    set_kind("headeronly")
    add_includedirs("eph-book/include", { public = true })
    add_headerfiles("eph-book/include/(eph/book/**.hpp)")
    add_headerfiles("eph-book/include/(eph/book.hpp)")
    add_deps("eph-core", { public = true })
    add_packages("spdlog", { public = true })
    add_defines("SPDLOG_ACTIVE_LEVEL=" .. net_log_level, { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

target("eph-dpdk")
    set_kind("headeronly")
    add_includedirs("eph-dpdk/include", { public = true })
    add_headerfiles("eph-dpdk/include/(eph/dpdk/**.hpp)")
    add_headerfiles("eph-dpdk/include/(eph/dpdk.hpp)")
    -- DPDK backend needs eph-transport headers for Transport template and
    -- protocol stack. We use add_includedirs instead of add_deps to control
    -- include path ordering: aws-lc's <openssl/*.h> MUST appear before
    -- vcpkg DPDK's bundled OpenSSL (incompatible type definitions).
    -- add_deps("eph-transport") resolves transitive package paths in an
    -- order that places vcpkg before aws-lc, causing header conflicts.
    add_deps("eph-core", "eph-utils", "eph-containers", { public = true })
    add_includedirs("eph-transport/include", { public = true })
    add_packages("spdlog", { public = true })
    add_packages("aws-lc", { public = true })
    add_packages("dpdk", { public = true })
    -- vcpkg's DPDK install includes fmt headers (compiled library, not header-only).
    -- These shadow spdlog's bundled fmt and produce undefined references at link time.
    -- Link vcpkg's libfmt to satisfy those symbols.
    add_links("fmt", { public = true })
    -- ARM64: DPDK headers check RTE_FORCE_INTRINSICS before rte_config.h is included.
    -- vcpkg's pkgconfig doesn't provide this define, so we must add it explicitly.
    -- Also force-include rte_config.h so rte_build_config.h defines are available
    -- before any other DPDK header (e.g. rte_byteorder.h) is parsed.
    add_defines("RTE_FORCE_INTRINSICS", { public = true })
    add_cxflags("-include", "rte_config.h", { public = true, force = true })
    -- NOTE: PMD whole-archive linking is applied to binary targets, not here.
    -- Headeronly targets don't participate in linking, so add_linkgroups here
    -- would not propagate. See apply_dpdk_pmd_linkgroups() below.
    -- NOTE: -march=native is applied to individual binary targets (not here)
    -- to preserve binary portability across CPU generations.
    add_defines("SPDLOG_ACTIVE_LEVEL=" .. net_log_level, { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

-- ===========================================================================
-- benchmarks (directory structure encodes module dependency)
-- ===========================================================================

local bench_module_deps = {
    containers = "eph-containers",
    core       = "eph-core",
    utils      = "eph-utils",
    net        = "eph-net",
    itch       = "eph-itch",
    fix        = "eph-fix",
    json       = "eph-json",
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

-- Manual benchmark targets (cross-module dependencies not expressible in the
-- single-dep-per-directory auto-generation above).

target("bench_array_book")
    set_kind("binary")
    set_group("benchmarks")
    set_default(false)
    add_files("benchmarks/book/bench_array_book.cpp")
    add_deps("eph-book", "eph-json")
    add_packages("benchmark")

target("bench_map_book")
    set_kind("binary")
    set_group("benchmarks")
    set_default(false)
    add_files("benchmarks/book/bench_map_book.cpp")
    add_deps("eph-book", "eph-json")
    add_packages("benchmark")

-- ===========================================================================
-- Static DPDK PMD whole-archive helper
-- ===========================================================================
-- PMD drivers use __attribute__((constructor)) to self-register at load time.
-- Static linking strips unreferenced .a files, losing these constructors.
-- Must be applied to each binary target (not headeronly eph-dpdk).
-- Only wrap PMDs we actually use — all librte_*.a would pull in
-- librte_crypto_openssl which conflicts with aws-lc.
-- MAINTENANCE: when adding support for a new NIC type, add its librte_net_xxx
-- here. When adding a new DPDK subsystem that uses constructor self-registration
-- (like rte_mempool_ring), add it too. Run `strings <binary> | grep rte_pmd` to
-- verify the PMD is linked.
function apply_dpdk_pmd_linkgroups()
    add_linkgroups("rte_net_null", "rte_net_ena", "rte_net_af_packet",
                   "rte_bus_pci", "rte_bus_vdev", "rte_mempool_ring",
                   { whole = true })
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
    json       = "eph-json",
    book       = "eph-book",
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
            if dir == "dpdk" then apply_dpdk_pmd_linkgroups() end
    end
end

-- Manual test targets (cross-module dependencies not expressible in the
-- single-dep-per-directory auto-generation above).

target("test_itch_adapter")
    set_kind("binary")
    set_group("tests")
    set_default(false)
    add_files("tests/book/test_itch_adapter.cpp")
    add_deps("eph-book", "eph-itch")
    add_packages("gtest")
    add_defines("SPDLOG_NO_EXCEPTIONS")

target("test_binance_adapter")
    set_kind("binary")
    set_group("tests")
    set_default(false)
    add_files("tests/book/test_binance_adapter.cpp")
    add_deps("eph-book", "eph-json")
    add_packages("gtest")
    add_defines("SPDLOG_NO_EXCEPTIONS")

target("test_binance_rest")
    set_kind("binary")
    set_group("tests")
    set_default(false)
    add_files("tests/json/test_binance_rest.cpp")
    add_deps("eph-json", "eph-net")
    add_packages("gtest")
    add_defines("SPDLOG_NO_EXCEPTIONS")

target("test_parse_number")
    set_kind("binary")
    set_group("tests")
    set_default(false)
    add_files("tests/core/test_parse_number.cpp")
    add_deps("eph-core")
    add_packages("gtest")
    add_defines("SPDLOG_NO_EXCEPTIONS")

target("test_transport_errors")
    set_kind("binary")
    set_group("tests")
    set_default(false)
    add_files("tests/core/test_transport_errors.cpp")
    add_deps("eph-core")
    add_packages("gtest")
    add_defines("SPDLOG_NO_EXCEPTIONS")

target("test_reconnect_policy")
    set_kind("binary")
    set_group("tests")
    set_default(false)
    add_files("tests/transport/test_reconnect_policy.cpp")
    add_deps("eph-net")
    add_packages("gtest")
    add_defines("SPDLOG_NO_EXCEPTIONS")

target("test_metrics_concept")
    set_kind("binary")
    set_group("tests")
    set_default(false)
    add_files("tests/core/test_metrics_concept.cpp")
    add_deps("eph-core", "eph-utils")
    add_packages("gtest")
    add_defines("SPDLOG_NO_EXCEPTIONS")

target("test_audit_log")
    set_kind("binary")
    set_group("tests")
    set_default(false)
    add_files("tests/utils/test_audit_log.cpp")
    add_deps("eph-utils")
    add_packages("gtest")
    add_defines("SPDLOG_NO_EXCEPTIONS")

target("test_gateway")
    set_kind("binary")
    set_group("tests")
    set_default(false)
    add_files("tests/net/test_gateway.cpp")
    add_deps("eph-net")
    add_packages("gtest")
    add_defines("SPDLOG_NO_EXCEPTIONS")

target("test_kill_switch")
    set_kind("binary")
    set_group("tests")
    set_default(false)
    add_files("tests/net/test_kill_switch.cpp")
    add_deps("eph-net")
    add_packages("gtest")
    add_defines("SPDLOG_NO_EXCEPTIONS")

-- ===========================================================================
-- examples
-- ===========================================================================
-- Socket-only WebSocket echo client (no DPDK dependency).
target("ws_echo_client")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/ws_echo_client.cpp")
    add_deps("eph-net")
    add_cxflags("-fno-omit-frame-pointer", "-march=native", { force = true })
    set_symbols("debug")

-- DPDK WebSocket echo client (requires DPDK).
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

target("bench_market")
    set_kind("binary")
    set_group("benchmarks")
    set_default(false)
    add_files("benchmarks/bench_market.cpp")
    add_deps("eph-net")
    add_defines("EPH_ENABLE_TIMESTAMPS=1")
    add_cxflags("-fno-omit-frame-pointer", "-march=native", { force = true })
    set_symbols("debug")

target("bench_order_rtt")
    set_kind("binary")
    set_group("benchmarks")
    set_default(false)
    add_files("benchmarks/bench_order_rtt.cpp")
    add_deps("eph-net")
    add_defines("EPH_ENABLE_TIMESTAMPS=1")
    add_cxflags("-fno-omit-frame-pointer", "-march=native", { force = true })
    set_symbols("debug")

target("bench_market_dpdk")
    set_kind("binary")
    set_group("benchmarks")
    set_default(false)
    add_files("benchmarks/bench_market_dpdk.cpp")
    add_includedirs("benchmarks")
    add_deps("eph-net", "eph-dpdk", "eph-fix")
    add_defines("EPH_ENABLE_TIMESTAMPS=1")
    add_cxflags("-fno-omit-frame-pointer", "-march=native", { force = true })
    set_symbols("debug")
    apply_dpdk_pmd_linkgroups()

target("bench_order_rtt_dpdk")
    set_kind("binary")
    set_group("benchmarks")
    set_default(false)
    add_files("benchmarks/bench_order_rtt_dpdk.cpp")
    add_includedirs("benchmarks")
    add_deps("eph-net", "eph-dpdk", "eph-fix")
    add_defines("EPH_ENABLE_TIMESTAMPS=1")
    add_cxflags("-fno-omit-frame-pointer", "-march=native", { force = true })
    set_symbols("debug")
    apply_dpdk_pmd_linkgroups()

-- Old bench targets removed: bench_market, bench_market_dpdk, bench_market_multi,
-- bench_market_multi_dpdk, bench_market_persymbol_dpdk, bench_market_pingpong,
-- bench_market_pingpong_dpdk, bench_pingpong, bench_pingpong_dpdk
-- Replaced by: bench_market, bench_order_rtt, bench_market_dpdk, bench_order_rtt_dpdk
