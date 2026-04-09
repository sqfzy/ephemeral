-- Unit tests for the bench shared layer (benchmarks/latency/core + mock/lib).
-- Header-only library, so each test target just adds its own .cpp + the
-- include path under benchmarks/latency. Depends on eph-utils for TSC,
-- HdrHistogram and CPU helpers.

target("test_bench_tsc_protocol")
    add_rules("eph-test")
    add_files("test_tsc_protocol.cpp")
    add_includedirs("$(projectdir)/benchmarks/latency")
    add_deps("eph-utils")
    add_packages("spdlog")

target("test_bench_cpu_pin")
    add_rules("eph-test")
    add_files("test_cpu_pin.cpp")
    add_includedirs("$(projectdir)/benchmarks/latency")
    add_deps("eph-utils")
    add_packages("spdlog")

target("test_bench_work_spin")
    add_rules("eph-test")
    add_files("test_work_spin.cpp")
    add_includedirs("$(projectdir)/benchmarks/latency")
    add_deps("eph-utils")
    add_packages("spdlog")

target("test_bench_stream_scheduler")
    add_rules("eph-test")
    add_files("test_stream_scheduler.cpp")
    add_includedirs("$(projectdir)/benchmarks/latency")
    add_deps("eph-utils")
    add_packages("spdlog")

target("test_bench_ws_frame")
    add_rules("eph-test")
    add_files("test_ws_frame.cpp")
    add_includedirs("$(projectdir)/benchmarks/latency")
    add_deps("eph-utils")
    add_packages("spdlog")
