-- Unit tests for bench-specific headers under benchmarks/latency/core/.
-- General-purpose pieces (cpu_pin, spin_for_ns, PhasedTimer) live in
-- eph-utils and are tested there.

target("test_bench_tsc_protocol")
    add_rules("eph-test")
    add_files("test_tsc_protocol.cpp")
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

target("test_bench_ws_handshake")
    add_rules("eph-test")
    add_files("test_ws_handshake.cpp")
    add_includedirs("$(projectdir)/benchmarks/latency")
    add_deps("eph-utils", "eph-core")
    add_packages("spdlog")

target("test_bench_tsc_protocol_adversarial")
    add_rules("eph-test")
    add_files("test_tsc_protocol_adversarial.cpp")
    add_includedirs("$(projectdir)/benchmarks/latency")
    add_deps("eph-utils")
    add_packages("spdlog")
