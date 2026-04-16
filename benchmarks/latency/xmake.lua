-- ===========================================================================
-- benchmarks/latency — latency benchmark suite
-- ===========================================================================
-- Plan reference: .artifacts/plan-bench-latency-simplify-20260409-065640.md
--
-- Every scenario is a single self-contained `lat_<scenario>.cpp` that forks
-- its own kernel mock and runs the bench client in the parent. For each
-- source file we generate two targets:
--
--   lat_<scenario>         kernel build (POSIX client)
--   lat_<scenario>_dpdk    DPDK build (same source, EPH_USE_DPDK=1)
--
-- The mock is always kernel — only the client transport differs, which
-- is the "echo bench is inherently fair" property recorded in the plan.
--
-- All lat_*.cpp target the eph-net-kernel / eph-net-dpdk + eph-codec API.

local bench_latency_flags = {"-fno-omit-frame-pointer", "-march=native"}

for _, file in ipairs(os.files(path.join(os.scriptdir(), "**/lat_*.cpp"))) do
    local name = path.basename(file)  -- e.g. "lat_tcp", "lat_ex_market"

    -- Kernel build
    target(name)
        set_kind("binary")
        set_group("benchmarks")
        set_default(false)
        add_files(file)
        add_includedirs(os.scriptdir())
        -- core/{signal,socket_bind,socket_io}.hpp are header-only shims
        -- that forward to eph-utils / eph-net helpers.
        add_deps("eph-utils", "eph-net", "eph-net-kernel", "eph-codec")
        add_packages("spdlog")
        add_cxflags(table.unpack(bench_latency_flags))
        set_symbols("debug")
        -- lat_ex_market_2p needs eph-json for full JSON parse + BinanceBookTicker.
        if name == "lat_ex_market_2p" then
            add_deps("eph-json")
        end

    -- DPDK build — same source, EPH_USE_DPDK switches the client transport
    target(name .. "_dpdk")
        set_kind("binary")
        set_group("benchmarks")
        set_default(false)
        add_files(file)
        add_includedirs(os.scriptdir())
        add_deps("eph-utils", "eph-net", "eph-net-dpdk", "eph-codec")
        add_packages("spdlog")
        add_defines("EPH_USE_DPDK=1")
        add_cxflags(table.unpack(bench_latency_flags))
        set_symbols("debug")
        apply_dpdk_pmd_linkgroups()
        if name == "lat_ex_market_2p" then
            add_deps("eph-json")
        end
end
