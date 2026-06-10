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
-- DPDK: use the distro's system package via pkg-config. This avoids
-- vcpkg's shared-prefix include tree, where every vcpkg port's headers
-- get flattened into one include/ dir — that used to drag openssl's
-- headers into the DPDK TU and collide with aws-lc's (ASN1_NULL /
-- CRYPTO_THREADID typedef conflict). System DPDK has an isolated
-- /usr/include/dpdk layout, so the conflict vanishes entirely and the
-- /tmp/gcc14-wrap/g++ include-order hack is no longer needed.
-- Arch:    sudo pacman -S dpdk
-- Ubuntu:  sudo apt install libdpdk-dev
-- Source:  meson setup -Ddisable_drivers=crypto/openssl && ninja install
add_requires("libdpdk", { system = true, optional = true, alias = "dpdk" })
add_requires("aws-lc", { optional = true })
add_requires("gtest", { system = false, configs = { main = true } })
add_requires("toml++ v3.4.0", { alias = "tomlplusplus" })

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

-- DPDK PMD linking strategy.
--
-- History: with vcpkg's static DPDK, PMDs had to be force-linked with
-- -Wl,--whole-archive (otherwise the linker dropped the registration
-- constructors). That is what this helper used to do.
--
-- System DPDK (pkg-config libdpdk, DPDK >= 20.11) ships as shared libs +
-- a plugin directory at `/usr/lib/dpdk/pmds-<abi>/`. EAL auto-scans that
-- directory at `rte_eal_init()` via dlopen, so no explicit PMD link is
-- required at build time. The helper is kept as a no-op so existing
-- `apply_dpdk_pmd_linkgroups()` call sites in examples/benches don't
-- need to change.
function apply_dpdk_pmd_linkgroups()
    -- Intentionally empty — see comment above.
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
includes("eph-sbe/xmake.lua")
includes("eph-json/xmake.lua")
includes("eph-book/xmake.lua")
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

-- Unified mock-server binary for the latency bench suite. Replaces the
-- per-scenario Python mocks under benchmarks/latency/mocks/. See
-- benchmarks/mockex/xmake.lua and the plan at
-- .claude/plans/elegant-toasting-popcorn.md.
includes("benchmarks/mockex/xmake.lua")

-- ===========================================================================
-- Examples (centralized, user-facing). All examples target the
-- eph-net-kernel / eph-net-dpdk / eph-codec API.
-- ===========================================================================

target("simple_hft")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/simple_hft.cpp")
    add_deps("eph-net-dpdk", "eph-codec")
    apply_dpdk_pmd_linkgroups()

target("dpdk_mp_demo")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/dpdk_mp_demo.cpp")
    add_deps("eph-net-dpdk", "eph-codec")
    apply_dpdk_pmd_linkgroups()

target("dpdk_rss_demo")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/dpdk_rss_demo.cpp")
    add_deps("eph-net-dpdk", "eph-codec")
    apply_dpdk_pmd_linkgroups()

-- Ops tool, not a demo: empirical DPDK src_port->RX-queue finder (sibling of the
-- kernel-side tools/kernel_rss_queue_probe.py). Lives under tools/ and in its own
-- "tools" group so `xmake build -g examples` does not pull it in; build explicitly
-- with `xmake build dpdk_rss_queue_probe` or `xmake build -g tools`.
target("dpdk_rss_queue_probe")
    set_kind("binary")
    set_group("tools")
    set_default(false)
    add_files("tools/dpdk_rss_queue_probe.cpp")
    add_deps("eph-net-dpdk", "eph-codec")
    apply_dpdk_pmd_linkgroups()

target("dpdk_multicast_md")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/dpdk_multicast_md.cpp")
    add_deps("eph-net-dpdk")
    apply_dpdk_pmd_linkgroups()

target("async_dns_multi_resolve")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/async_dns_multi_resolve.cpp")
    add_deps("eph-net-dpdk")
    apply_dpdk_pmd_linkgroups()

target("reconnect_orch_demo")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/reconnect_orch_demo.cpp")
    add_deps("eph-net-kernel", "eph-codec")

target("binance_book")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/binance_book.cpp")
    add_deps("eph-net-kernel", "eph-codec", "eph-json", "eph-book")

target("coinbase_jwt_rest")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/coinbase_jwt_rest.cpp")
    add_deps("eph-net-kernel", "eph-codec")

target("binance_signed_rest")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/binance_signed_rest.cpp")
    add_deps("eph-net-kernel", "eph-codec")

target("ws_deflate_demo")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/ws_deflate_demo.cpp")
    add_deps("eph-net-kernel", "eph-codec", "eph-utils")

target("binance_latency")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/binance_latency.cpp")
    add_deps("eph-net-dpdk", "eph-codec", "eph-json")
    apply_dpdk_pmd_linkgroups()

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

target("session_reconnect")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/session_reconnect.cpp")
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

target("observability_demo")
    set_kind("binary")
    set_group("examples")
    set_default(false)
    add_files("examples/observability_demo.cpp")
    add_deps("eph-net-kernel", "eph-codec", "eph-utils")
    add_packages("spdlog")
