-- Cross-module integration tests.
-- Each target explicitly declares its multi-module dependencies.

target("test_itch_adapter")
    add_rules("eph-test")
    add_files("test_itch_adapter.cpp")
    add_deps("eph-book", "eph-itch")

target("test_binance_adapter")
    add_rules("eph-test")
    add_files("test_binance_adapter.cpp")
    add_deps("eph-book", "eph-json")

target("test_binance_rest")
    add_rules("eph-test")
    add_files("test_binance_rest.cpp")
    add_deps("eph-json", "eph-net")

target("test_metrics_concept")
    add_rules("eph-test")
    add_files("test_metrics_concept.cpp")
    add_deps("eph-core", "eph-utils")

target("test_transport_e2e")
    add_rules("eph-test")
    add_files("test_transport_e2e.cpp")
    add_deps("eph-transport")

target("test_transport_tls_ws_e2e")
    add_rules("eph-test")
    add_files("test_transport_tls_ws_e2e.cpp")
    add_includedirs(path.join(os.projectdir(), "tests", "support"))
    add_deps("eph-net")
    add_packages("aws-lc")

target("bench_e2e_latency")
    add_rules("eph-bench")
    add_files("bench_e2e_latency.cpp")
    add_deps("eph-transport")

-- ============================================================================
-- v3.3 Phase 6: integration tests / benchmark for the new
--               eph::net::kernel + eph::codec API.
-- ============================================================================

target("test_transport_e2e_v3")
    add_rules("eph-test")
    add_files("test_transport_e2e_v3.cpp")
    add_deps("eph-net-kernel", "eph-codec")

target("test_transport_tls_ws_e2e_v3")
    add_rules("eph-test")
    add_files("test_transport_tls_ws_e2e_v3.cpp")
    add_includedirs(path.join(os.projectdir(), "tests", "support"))
    add_deps("eph-net-kernel", "eph-codec")
    add_packages("aws-lc")

target("test_kernel_udp_v3")
    add_rules("eph-test")
    add_files("test_kernel_udp_v3.cpp")
    add_deps("eph-net-kernel", "eph-codec")

target("bench_e2e_latency_v3")
    add_rules("eph-bench")
    add_files("bench_e2e_latency_v3.cpp")
    add_deps("eph-net-kernel", "eph-codec")
