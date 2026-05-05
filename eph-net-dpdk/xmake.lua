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

-- ============================================================================
-- eph_nicd — the DPDK NIC daemon binary (S4 of the daemon-reshape).
--
-- One daemon per NIC. ops side: `eph-nicd@<bdf>.service` reads
-- `/etc/eph/<bdf>.toml` → `Platform::serve_nic(NicServiceConfig)` → blocks
-- on `Platform::join()` until SIGTERM/SIGINT. Application processes attach
-- as DPDK secondaries against the same `pci` via `Platform::create`.
-- ============================================================================
target("eph_nicd")
    set_kind("binary")
    set_default(true)
    add_files("tools/eph-nicd.cpp")
    add_deps("eph-net-dpdk")
    add_packages("tomlplusplus")
    add_defines("EPH_USE_DPDK=1")
    apply_dpdk_pmd_linkgroups()

-- ============================================================================
-- eph_nicctl — operator tool for inspecting eph-nicd daemon state (S6).
--
-- Attaches as a DPDK secondary to the daemon's EAL session, sends a single
-- `eph_nicctl_query` IPC, prints the reply, exits. See tools/eph-nicctl.cpp
-- header comment for subcommand reference.
-- ============================================================================
target("eph_nicctl")
    set_kind("binary")
    set_default(true)
    add_files("tools/eph-nicctl.cpp")
    add_deps("eph-net-dpdk")
    add_defines("EPH_USE_DPDK=1")
    apply_dpdk_pmd_linkgroups()

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

-- Detail-layer DPDK unit tests — exercise the low-level `eph::dpdk::*`
-- primitives (ARP / DNS / multicast / packet / TCP / UDP / EAL / ICMP
-- registry / net_header). Run under --no-pci mode and do NOT touch
-- the high-level Stream / Datagram API. Renamed from `tests/legacy/`
-- in 2026-05-05 cleanup; see tests/detail/AUDIT.md for context. Not
-- deprecated — these are the source of truth for the detail layer
-- the public types wrap.
for _, file in ipairs(os.files("tests/detail/*.cpp")) do
    target(path.basename(file))
        add_rules("eph-test")
        add_files(file)
        add_includedirs("tests/detail")
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

-- EalGuard::init (typed-pin overload) integration test — exercises the
-- rte_eal_init success path with a typed LcorePin spec. Runs in --no-pci
-- mode so no vfio binding is needed; SKIPs cleanly when no free hugepages
-- are available. Target name retained for git-history continuity (file
-- intentionally not renamed when init_with_pins → init landed).
target("test_eal_init_with_pins")
    add_rules("eph-test")
    add_files("tests/integration/test_eal_init_with_pins.cpp")
    add_deps("eph-net-dpdk")
    apply_dpdk_pmd_linkgroups()

-- T1.4 daemon-kill recovery scenario (skeleton — hardware-gated).
-- Verifies the T1.1+T1.2 wire-up's pre-burst is_alive() check
-- populates DaemonDisconnectedDetail correctly. Two unconditional
-- behavioural-primitive cases run anywhere; the full kill-and-
-- reattach scenario SKIPs on hosts without vfio-pci NIC binding +
-- eph-nicd availability. Reactivation = no code change once env is
-- equipped (see file header).
target("test_dpdk_daemon_recovery")
    add_rules("eph-test")
    add_files("tests/integration/test_dpdk_daemon_recovery.cpp")
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

-- The autojoin e2e binaries (dpdk_mp_dynamic_primary / _secondary,
-- dpdk_mp_dynamic_tcp_handshake_primary / _secondary) and the
-- repro_ena_mp_secondary_rxburst sentinel were removed in the
-- daemon-led Platform reshape (calm-roaming-sedgewick). They depended
-- on Platform::create_or_join (deleted) and the autojoin race
-- semantics it implemented. The new daemon-led model removes the race
-- entirely — peer-to-peer consensus is replaced by a single eph-nicd
-- daemon that owns NIC bring-up; applications only attach via
-- Platform::create. The MP teardown gate sentinel is now covered by
-- the daemon's own startup/shutdown protocol (S2). See
-- eph-net-dpdk/CHANGELOG.md "BREAKING: daemon-led Platform reshape".

-- Module benchmarks — low-level DPDK primitive microbenchmarks migrated
-- over from eph-dpdk/benchmarks. Need PMD whole-archive linking.
-- libnuma is linked when present (T3.6 NUMA-aware pin helper in
-- benchmarks/bench_helpers.hpp gates on __has_include(<numa.h>) at
-- compile time; this matches at link time so benches that include
-- bench_helpers.hpp don't fail with "DSO missing from command line").
for _, file in ipairs(os.files("benchmarks/*.cpp")) do
    target(path.basename(file))
        add_rules("eph-bench")
        add_files(file)
        add_deps("eph-net-dpdk")
        add_includedirs(path.join(os.scriptdir(), "benchmarks"))
        apply_dpdk_pmd_linkgroups()
        if os.exists("/usr/include/numa.h") then
            add_syslinks("numa")
        end
end

-- Module fuzzers — INTENTIONALLY NOT WIRED INTO XMAKE.
--
-- The libFuzzer harnesses under `fuzzers/*.cpp` require Clang ≥ 17 with
-- `-fsanitize=fuzzer`. The project's default toolchain is GCC 14, which
-- has no libFuzzer; building these via the normal target loop produces
-- either a link failure (no `main`, no fuzzer entry) or — for harnesses
-- that forward-declare DPDK types as a TU shim — a struct-redefinition
-- error against the real `<rte_mbuf_core.h>` pulled in by eph-net-dpdk's
-- `add_packages("dpdk")`.
--
-- Build instructions live in `fuzzers/README.md` (`clang++
-- -fsanitize=fuzzer,address,undefined ...`). Do NOT add a glob loop here.
-- See CLAUDE.md ("Tests" section) for the matching policy note.
