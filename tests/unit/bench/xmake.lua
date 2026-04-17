-- Unit tests for bench-specific headers under benchmarks/latency/core/.
-- General-purpose pieces (cpu_pin, spin_for_ns, PhasedTimer) live in
-- eph-utils and are tested there.

target("test_bench_load_bench_conf")
    add_rules("eph-test")
    add_files("test_load_bench_conf.cpp")
    add_includedirs("$(projectdir)/benchmarks/latency")
    add_deps("eph-utils")
    add_packages("spdlog")

target("test_bench_no_dead_headers")
    add_rules("eph-test")
    add_files("test_no_dead_headers.cpp")
    add_packages("spdlog")

target("test_bench_json_scan")
    add_rules("eph-test")
    add_files("test_json_scan.cpp")
    add_includedirs("$(projectdir)/benchmarks/latency")
    add_deps("eph-utils")
    add_packages("spdlog")

target("test_bench_measurement")
    add_rules("eph-test")
    add_files("test_measurement.cpp")
    add_includedirs("$(projectdir)/benchmarks/latency")
    add_deps("eph-utils")
    add_packages("spdlog")

-- Phase 4 mockex real-server switch: parse_ws_url + resolve_endpoint.
target("test_bench_endpoint")
    add_rules("eph-test")
    add_files("test_endpoint.cpp")
    add_includedirs("$(projectdir)/benchmarks/latency")
    add_deps("eph-utils")
    add_packages("spdlog")

-- TX/RX leg decomposition timestamp-block protocol
-- (benchmarks/latency/core/timestamp_proto.hpp). Pure-function unit
-- tests — no eph dependencies beyond gtest + spdlog.
target("test_timestamp_proto")
    add_rules("eph-test")
    add_files("test_timestamp_proto.cpp")
    add_includedirs("$(projectdir)/benchmarks/latency")
    add_packages("spdlog")

-- Pure-function coverage for bench::synthesize_eal_argv.
-- The source file is guarded by `#ifdef EPH_USE_DPDK` and falls back to
-- a single SKIP-style test when DPDK is not available. Link the real
-- DPDK backend so we exercise the live argv synthesis path on hosts
-- that have DPDK; on other hosts the SKIP branch keeps the target
-- building cleanly.
target("test_bench_dpdk_env_argv")
    add_rules("eph-test")
    add_files("test_dpdk_env_argv.cpp")
    add_includedirs("$(projectdir)/benchmarks/latency")
    add_deps("eph-net-dpdk", "eph-utils")
    add_packages("spdlog")
    add_defines("EPH_USE_DPDK=1")
    apply_dpdk_pmd_linkgroups()
