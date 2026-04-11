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
