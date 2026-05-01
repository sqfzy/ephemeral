-- ============================================================================
-- eph-net-dpdk — header-only DPDK backend for the eph::net concept layer.
-- ============================================================================
--
-- Provides concrete types satisfying the `eph::net::Stream`, `eph::net::Datagram`
-- and `eph::net::Poller` concepts on top of the DPDK kernel-bypass data plane:
--
--   * eph::net::dpdk::DpdkTcpStream<C, EnableTls>  — Stream impl
--   * eph::net::dpdk::DpdkUdpSocket<C>             — Datagram impl
--   * eph::net::dpdk::DpdkPoller<>                 — Poller impl (lcore burst)
--   * eph::net::dpdk::Eal                          — RAII EAL init
--
-- And absorbs the low-level DPDK primitives that used to live in eph-dpdk
-- (they are header-only and stay under the `eph::dpdk` namespace / `eph/dpdk/`
-- include path to minimise churn):
--
--   * eph::dpdk::TcpSession / UdpSender / Connector / Multicast /
--     ARP / DNS / packet_* / platform / EalGuard
-- And in `eph::net::dpdk`:
--   * FlowSteering / configure_rss / install_flow_rule / FlowRule
--
-- Dependency rule: eph-core + eph-utils + eph-containers + eph-net +
-- aws-lc + DPDK.
--
-- aws-lc is the only OpenSSL flavour in every eph-net-dpdk TU now:
-- ISN generation and WS mask key cache use `getrandom(2)` (no OpenSSL
-- rand dependency). The DPDK backend resolves via the system's libdpdk
-- (isolated /usr/include/dpdk layout).

target("eph-net-dpdk")
    set_kind("headeronly")
    add_includedirs("include", { public = true })
    add_headerfiles("include/(eph/net/dpdk/**.hpp)")
    add_headerfiles("include/(eph/dpdk/**.hpp)")
    add_headerfiles("include/(eph/dpdk.hpp)")
    add_deps("eph-core", "eph-utils", "eph-containers", "eph-net", { public = true })
    add_packages("spdlog", "aws-lc", { public = true })
    add_packages("dpdk", { public = true })
    -- ARM64: DPDK headers need RTE_FORCE_INTRINSICS before rte_config.h.
    add_defines("RTE_FORCE_INTRINSICS", { public = true })
    add_cxflags("-include", "rte_config.h", { public = true, force = true })
    -- DPDK's rte_memcpy uses SSSE3 intrinsics on x86 (requires -mssse3);
    -- on ARM64 the NEON backend is enabled by default.
    if not is_arch("arm64", "arm64-v8a", "aarch64") then
        add_cxflags("-mssse3", { public = true, force = true })
    end
    add_defines("SPDLOG_ACTIVE_LEVEL=" .. net_log_level, { public = true })
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")

-- Module unit tests (v3 API tests). Every test target needs PMD
-- whole-archive linking. Tests use --no-pci mode (see dpdk_test_env.hpp)
-- so they run on any host without a vfio-pci NIC.
for _, file in ipairs(os.files("tests/*.cpp")) do
    target(path.basename(file))
        add_rules("eph-test")
        add_files(file)
        add_includedirs("tests")
        add_deps("eph-net-dpdk", "eph-codec")
        apply_dpdk_pmd_linkgroups()
end

-- Legacy DPDK unit tests — these exercise the low-level `eph::dpdk::*`
-- primitives. They run under --no-pci mode and do NOT touch the
-- high-level Stream / Datagram API.
for _, file in ipairs(os.files("tests/legacy/*.cpp")) do
    target(path.basename(file))
        add_rules("eph-test")
        add_files(file)
        add_includedirs("tests/legacy")
        add_deps("eph-net-dpdk")
        apply_dpdk_pmd_linkgroups()
end

-- DPDK end-to-end integration test binary (real-NIC). Requires NIC_B bound
-- to vfio-pci at run time; tests SKIP otherwise.
target("test_dpdk_e2e")
    add_rules("eph-test")
    add_files("tests/integration/test_dpdk_e2e.cpp")
    add_includedirs("tests/integration")
    -- eph-net for posix_listener / posix_io helpers used by echo_mocks.hpp;
    -- eph-codec for WsCodec used by the DpdkWsAutoResponse e2e case.
    add_deps("eph-net-dpdk", "eph-net", "eph-codec")
    -- The fixture reuses bench's DpdkBenchEnv via #include of
    -- benchmarks/latency/core/dpdk_env.hpp.
    add_includedirs(path.join(os.projectdir(), "benchmarks/latency"))
    add_packages("tomlplusplus")
    add_defines("EPH_USE_DPDK=1")
    add_defines('EPH_BENCH_CONF_ABS_PATH="' ..
        path.join(os.projectdir(), "benchmarks/latency/bench.conf") .. '"')
    apply_dpdk_pmd_linkgroups()

-- Stage 3 RSS Platform integration test (real-NIC).  Forks the kernel
-- mock dispatcher (TCP echo etc on NIC_A) and brings up DpdkBenchEnv
-- on NIC_B with enable_rss=true; verifies the Platform RSS registry
-- AND drives an end-to-end create_and_attach Software-mode round trip.
-- SKIPs when NIC_B is not bound to vfio-pci.
target("test_dpdk_rss_platform")
    add_rules("eph-test")
    add_files("tests/integration/test_dpdk_rss_platform.cpp")
    add_includedirs("tests/integration")
    -- eph-net for posix_listener / posix_io helpers used by echo_mocks.hpp;
    -- eph-codec for RawStreamCodec used by the create_and_attach E2E case.
    add_deps("eph-net-dpdk", "eph-net", "eph-codec")
    add_includedirs(path.join(os.projectdir(), "benchmarks/latency"))
    add_packages("tomlplusplus")
    add_defines("EPH_USE_DPDK=1")
    add_defines('EPH_BENCH_CONF_ABS_PATH="' ..
        path.join(os.projectdir(), "benchmarks/latency/bench.conf") .. '"')
    apply_dpdk_pmd_linkgroups()

-- RSS bring-up failure-path reshape integration test (real-NIC). Constructs
-- multiple PlatformConfig shapes (multi-queue + enable_rss=true /
-- multi-queue + enable_rss=false / single-queue) within one EAL session
-- to verify the probe + hard-fail behaviour. SKIPs when NIC_B is not
-- bound to vfio-pci.
target("test_dpdk_rss_bringup")
    add_rules("eph-test")
    add_files("tests/integration/test_dpdk_rss_bringup.cpp")
    add_deps("eph-net-dpdk")
    add_includedirs(path.join(os.projectdir(), "benchmarks/latency"))
    add_packages("tomlplusplus")
    add_defines("EPH_USE_DPDK=1")
    add_defines('EPH_BENCH_CONF_ABS_PATH="' ..
        path.join(os.projectdir(), "benchmarks/latency/bench.conf") .. '"')
    apply_dpdk_pmd_linkgroups()

-- RSS probed-key correctness verification (real-NIC). Sends N UDP probes
-- to a kernel echo on NIC_A and compares NIC-computed mbuf->hash.rss vs
-- software Toeplitz over the probed key, plus observed RX queue vs
-- queue_for_tuple prediction. SKIPs when NIC_B isn't on vfio-pci or when
-- the probe path doesn't activate (configure_rss already worked).
target("test_dpdk_rss_key_correctness")
    add_rules("eph-test")
    add_files("tests/integration/test_dpdk_rss_key_correctness.cpp")
    add_includedirs("tests/integration")
    -- eph-net for posix_listener / posix_io helpers used by echo_mocks.hpp.
    add_deps("eph-net-dpdk", "eph-net")
    add_includedirs(path.join(os.projectdir(), "benchmarks/latency"))
    add_packages("tomlplusplus")
    add_defines("EPH_USE_DPDK=1")
    add_defines('EPH_BENCH_CONF_ABS_PATH="' ..
        path.join(os.projectdir(), "benchmarks/latency/bench.conf") .. '"')
    apply_dpdk_pmd_linkgroups()

-- EalGuard::init_with_pins integration test (rte_eal_init success path
-- with typed LcorePin spec). Runs in --no-pci mode so no vfio binding
-- is needed; SKIPs cleanly when no free hugepages are available.
target("test_eal_init_with_pins")
    add_rules("eph-test")
    add_files("tests/integration/test_eal_init_with_pins.cpp")
    add_deps("eph-net-dpdk")
    apply_dpdk_pmd_linkgroups()

-- RSS multi-queue fan-out regression test (real-NIC). N concurrent
-- DpdkTcpStream attaches all pinned to the same queue + same endpoint
-- — the canonical HFT producer pattern that previously trampled
-- itself due to the deterministic find_src_port_for_queue scan.
-- Forks the kernel mock dispatcher on NIC_A and brings up multi-queue
-- enable_rss=true on NIC_B. SKIPs when NIC_B is not bound to vfio-pci
-- or when the runtime dispatch_mode is not RssPartitioned.
target("test_dpdk_rss_fanout")
    add_rules("eph-test")
    add_files("tests/integration/test_dpdk_rss_fanout.cpp")
    add_includedirs("tests/integration")
    add_deps("eph-net-dpdk", "eph-net", "eph-codec")
    add_includedirs(path.join(os.projectdir(), "benchmarks/latency"))
    add_packages("tomlplusplus")
    add_defines("EPH_USE_DPDK=1")
    add_defines('EPH_BENCH_CONF_ABS_PATH="' ..
        path.join(os.projectdir(), "benchmarks/latency/bench.conf") .. '"')
    apply_dpdk_pmd_linkgroups()

-- Autojoin (Platform::join_dynamic) e2e (reshape mp-mode2-dynamic
-- stage 3). Both peers call join_dynamic with the SAME pci /
-- nb_rx_queues; the first to start is auto-resolved as primary,
-- the second auto-attaches as secondary and CAS-claims slot 1.
-- Coordinated by tests/integration/dpdk_mp_dynamic_e2e.sh; same
-- SKIP-on-missing-env behaviour as the declarative-path MP
-- binaries above.
target("dpdk_mp_dynamic_primary")
    add_rules("eph-test")
    add_files("tests/integration/dpdk_mp_dynamic_primary.cpp")
    add_deps("eph-net-dpdk")
    apply_dpdk_pmd_linkgroups()

target("dpdk_mp_dynamic_secondary")
    add_rules("eph-test")
    add_files("tests/integration/dpdk_mp_dynamic_secondary.cpp")
    add_deps("eph-net-dpdk")
    apply_dpdk_pmd_linkgroups()

-- Acceptance gate for reshape/rss-aware-connect: two autojoin peers
-- both DPDK-TCP-connect to a kernel echo mock spawned inside primary.
-- Pre-fix the secondary's connect hung silently because RSS hashed
-- SYN-ACK to a queue secondary did not own. Coordinated by
-- tests/integration/dpdk_mp_dynamic_tcp_handshake_e2e.sh.
target("dpdk_mp_dynamic_tcp_handshake_primary")
    add_rules("eph-test")
    add_files("tests/integration/dpdk_mp_dynamic_tcp_handshake_primary.cpp")
    add_includedirs("tests/integration")
    -- eph-net for posix_listener / posix_io helpers used by echo_mocks.hpp;
    -- eph-codec for RawStreamCodec used in the connect+echo round-trip.
    add_deps("eph-net-dpdk", "eph-net", "eph-codec")
    add_includedirs(path.join(os.projectdir(), "benchmarks/latency"))
    add_packages("tomlplusplus")
    add_defines("EPH_USE_DPDK=1")
    apply_dpdk_pmd_linkgroups()

target("dpdk_mp_dynamic_tcp_handshake_secondary")
    add_rules("eph-test")
    add_files("tests/integration/dpdk_mp_dynamic_tcp_handshake_secondary.cpp")
    add_includedirs("tests/integration")
    add_deps("eph-net-dpdk", "eph-net", "eph-codec")
    add_includedirs(path.join(os.projectdir(), "benchmarks/latency"))
    add_packages("tomlplusplus")
    add_defines("EPH_USE_DPDK=1")
    apply_dpdk_pmd_linkgroups()

-- Vendor-PMD limitation reproducers. NOT in the "tests" group because
-- the program is *expected* to observe a SIGSEGV in a forked child (it
-- exits 0 when the limitation reproduces, non-zero if the limitation
-- has been lifted) — mixing it into batch test runs is misleading.
-- Build via: `xmake build -g repros`.
--
--   repro_ena_mp_secondary_rxburst — AWS ENA PMD: secondary-process
--     rte_eth_rx_burst() segfaults inside ena_com_get_next_rx_cdesc.
--     See file header for backtrace and DPDK / PMD / kernel / instance
--     metadata. Documented in docs/dpdk-mp-teardown-protocol.md.
target("repro_ena_mp_secondary_rxburst")
    set_kind("binary")
    set_group("repros")
    set_default(false)
    add_files("tests/integration/repro_ena_mp_secondary_rxburst.cpp")
    add_deps("eph-net-dpdk")
    add_defines("SPDLOG_NO_EXCEPTIONS")
    apply_dpdk_pmd_linkgroups()

-- Module benchmarks — low-level DPDK primitive microbenchmarks migrated
-- over from eph-dpdk/benchmarks. Need PMD whole-archive linking.
for _, file in ipairs(os.files("benchmarks/*.cpp")) do
    target(path.basename(file))
        add_rules("eph-bench")
        add_files(file)
        add_deps("eph-net-dpdk")
        add_includedirs(path.join(os.scriptdir(), "benchmarks"))
        apply_dpdk_pmd_linkgroups()
end

-- Module fuzzers
for _, file in ipairs(os.files("fuzzers/*.cpp")) do
    target(path.basename(file))
        set_kind("binary")
        set_group("fuzzers")
        set_default(false)
        add_files(file)
        add_deps("eph-net-dpdk")
        apply_dpdk_pmd_linkgroups()
end
