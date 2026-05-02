#pragma once

/// @file dpdk_e2e_env.hpp
/// gtest Environment for DPDK end-to-end tests (daemon-led model).
///
/// Lifecycle (per test binary, exactly once):
///   SetUp:
///     1. Load bench.conf
///     2. Verify NIC_B is bound to vfio-pci (else mark all tests SKIP)
///     3. Probe `/var/run/dpdk/eph_<sanitized_bdf>/` — if the daemon
///        (eph-nicd) is not running for this NIC, mark all tests SKIP
///     4. fork() — child runs MockDispatcher::run, parent stores child PID
///     5. Parent: Platform::create(PlatformConfig{ pci, queues=1, ... })
///        attaches as a DPDK secondary against the running daemon, then
///        ARP-resolves the gateway and packages everything into a
///        E2EBundle that downstream tests consume.
///     6. Brief sleep to let child mocks finish bind/listen before tests run
///   TearDown:
///     1. SIGTERM child, waitpid
///     2. Destroy E2EBundle (~Platform releases secondary attach + EAL)
///
/// Tests access shared state via the static accessors below.
///
/// Migration note (calm-roaming-sedgewick / S3 placeholder):
///   * The pre-reshape code went through `DpdkBenchEnv::create` →
///     `Platform::launch` (single-process EAL primary). That helper
///     still references the deleted `Platform::launch` /
///     `Platform::create_or_join` API and the deleted
///     `EalConfig`-as-public-factory-input shape.
///   * The new model is daemon-led: a separate `eph-nicd` process owns
///     primary NIC bring-up. This fixture is now a pure secondary —
///     `Platform::create({ pci, queues=1, ... })` and inline ARP/MAC
///     resolution. The previous bundle's accessors (`port_id`,
///     `pool`, `src_mac`, `gw_mac`, `make_tcp_config`,
///     `make_udp_sender`) are reproduced on the local `E2EBundle`
///     struct so individual TEST bodies need no migration.
///   * `cfg.queues = 1` is the only value supported by the S3
///     placeholder (queues 0..(queues-1) blindly claimed). S5 lifts
///     that limitation.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>

#include <arpa/inet.h>
#include <rte_ethdev.h>

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

// bench.conf parsing still lives in latency/core/ — that header is
// bench-specific (it knows about scenario sweeps, payload lists, etc.).
#define EPH_USE_DPDK 1
#include "../../../benchmarks/latency/core/bench_conf.hpp"

#include "eph/dpdk/arp.hpp"
#include "eph/dpdk/detail/bdf_sanitize.hpp"
#include "eph/dpdk/platform.hpp"
#include "eph/dpdk/tcp.hpp"
#include "eph/dpdk/udp.hpp"

#include "mock_dispatcher.hpp"

namespace eph::dpdk::test_e2e {

/// @brief Move-only bundle of a `Platform` (secondary attach) plus the
/// addressing context downstream tests need to drive UDP/TCP traffic
/// against the kernel mock dispatcher.
///
/// Field layout intentionally matches the legacy `DpdkBenchEnv` so the
/// migrated TEST bodies access `env.port_id` / `env.pool` / `env.src_mac`
/// / `env.gw_mac` / `env.src_ip` / `env.dst_ip` / `env.gw_ip` /
/// `env.make_tcp_config(...)` / `env.make_udp_sender(...)` exactly as
/// before. The construction path under the hood is the only thing that
/// changed (Platform::launch → Platform::create against an external
/// daemon).
struct E2EBundle {
    ::eph::dpdk::Platform  platform;
    uint32_t               src_ip{};   ///< local IP (host byte order)
    uint32_t               dst_ip{};   ///< server IP (host byte order)
    uint32_t               gw_ip{};    ///< gateway IP (host byte order)
    rte_ether_addr         src_mac{};
    rte_ether_addr         gw_mac{};
    uint16_t               port_id{};
    rte_mempool*           pool{nullptr};

    E2EBundle(::eph::dpdk::Platform p,
              uint32_t si, uint32_t di, uint32_t gi,
              rte_ether_addr sm, rte_ether_addr gm,
              uint16_t pid, rte_mempool* pl)
        : platform(std::move(p)),
          src_ip(si), dst_ip(di), gw_ip(gi),
          src_mac(sm), gw_mac(gm), port_id(pid), pool(pl) {}

    E2EBundle(E2EBundle&&) = default;
    E2EBundle& operator=(E2EBundle&&) = default;

    /// Build a TcpConfig pointing at the kernel echo on (`gw_mac` ->
    /// `dst_ip`:`remote_port`). Local 5-tuple uses the bound src_ip
    /// + caller-supplied `local_port`.
    [[nodiscard]] eph::dpdk::TcpConfig
    make_tcp_config(uint16_t local_port, uint16_t remote_port) const {
        eph::dpdk::TcpConfig tcfg{};
        tcfg.tuple = {src_ip, dst_ip, local_port, remote_port};
        tcfg.src_mac = src_mac;
        tcfg.dst_mac = gw_mac;
        tcfg.port_id = port_id;
        tcfg.tx_queue_id = 0;
        tcfg.rx_queue_id = 0;
        return tcfg;
    }

    /// Build a UdpSender configured against the kernel echo on
    /// (`gw_mac` -> `dst_ip`:`remote_port`).
    [[nodiscard]] std::expected<eph::dpdk::UdpSender, std::string>
    make_udp_sender(uint16_t local_port, uint16_t remote_port) const {
        eph::dpdk::wire::UdpConfig ucfg{};
        ucfg.src_ip      = src_ip;
        ucfg.dst_ip      = dst_ip;
        ucfg.src_port    = local_port;
        ucfg.dst_port    = remote_port;
        ucfg.src_mac     = src_mac;
        ucfg.dst_mac     = gw_mac;
        ucfg.port_id     = port_id;
        ucfg.tx_queue_id = 0;
        ucfg.pool        = pool;
        return eph::dpdk::UdpSender::create(ucfg);
    }
};

class DpdkE2ETestEnv : public ::testing::Environment {
public:
    /// Returns true iff Platform is attached and the mock dispatcher is running.
    static bool ready() noexcept { return ready_; }

    /// Reason the env is not ready (for SKIP messages).
    static const std::string& skip_reason() noexcept { return skip_reason_; }

    /// Access the bench environment (Platform secondary attach + ARP-resolved gateway).
    static E2EBundle& env() {
        return *env_;
    }

    /// Access the loaded config.toml (read-only).
    static const bench::BenchConfig& cfg() {
        return *cfg_;
    }

    void SetUp() override {
        // ── 1. Load config.toml ───────────────────────────────────────
        std::string conf_path;
        if (const char* env = std::getenv("BENCH_CONFIG"); env && *env) {
            conf_path = env;
        } else {
#ifdef EPH_BENCH_CONF_ABS_PATH
            conf_path = EPH_BENCH_CONF_ABS_PATH;
#else
            conf_path = "benchmarks/latency/config.toml";
#endif
            // Prefer the TOML file if it exists alongside the legacy bench.conf.
            if (auto dot = conf_path.rfind("bench.conf"); dot != std::string::npos) {
                std::string toml_path = conf_path.substr(0, dot) + "config.toml";
                if (::access(toml_path.c_str(), F_OK) == 0) {
                    conf_path = std::move(toml_path);
                }
            }
        }

        auto cfg_r = bench::load_bench_conf(conf_path);
        if (!cfg_r) {
            skip_reason_ = "load_bench_conf failed: " +
                           bench::format_error(cfg_r.error());
            SPDLOG_ERROR("DpdkE2ETestEnv: {}", skip_reason_);
            return;
        }
        cfg_.emplace(std::move(*cfg_r));

        // ── 2. Resolve NIC_B PCI BDF ──────────────────────────────────
        nic_b_pci_ = resolve_nic_b_pci_(cfg_->networking.nic_b,
                                        cfg_->networking.nic_b_pci);
        if (nic_b_pci_.empty()) {
            skip_reason_ =
                "could not determine NIC_B PCI BDF. Set EPH_TEST_NIC_B_PCI=<bdf> "
                "or add nic_b_pci=<bdf> under [networking] in config.toml";
            SPDLOG_WARN("DpdkE2ETestEnv: {}", skip_reason_);
            return;
        }
        SPDLOG_INFO("DpdkE2ETestEnv: NIC_B={} PCI={}",
                     cfg_->networking.nic_b, nic_b_pci_);

        // ── 3. Verify NIC_B is on vfio-pci ────────────────────────────
        if (!nic_on_vfio_pci_(nic_b_pci_)) {
            skip_reason_ = std::string(
                "NIC_B (PCI ") + nic_b_pci_ + ") is not bound to vfio-pci. "
                "Run `sudo benchmarks/latency/lat tcp --dpdk` once first to "
                "transition NIC_B, then re-run this test binary.";
            SPDLOG_WARN("DpdkE2ETestEnv: {}", skip_reason_);
            return;
        }

        // ── 4. Verify the eph-nicd daemon is running on this NIC ─────
        // The daemon publishes `/var/run/dpdk/<file_prefix>/` when its
        // EAL primary comes up; absence of that directory means our
        // secondary attach (Platform::create) will fail with a
        // misleading error. SKIP cleanly so a developer running this
        // binary without first starting the daemon sees the right
        // diagnostic instead of a red X.
        auto fp_r = ::eph::dpdk::detail::sanitize_bdf_for_file_prefix(nic_b_pci_);
        if (!fp_r) {
            skip_reason_ =
                "could not derive file_prefix from PCI '" + nic_b_pci_ +
                "': " + fp_r.error().detail;
            SPDLOG_WARN("DpdkE2ETestEnv: {}", skip_reason_);
            return;
        }
        const std::string file_prefix = "eph_" + *fp_r;
        const std::string daemon_runtime = "/var/run/dpdk/" + file_prefix;
        if (::access(daemon_runtime.c_str(), F_OK) != 0) {
            skip_reason_ =
                "eph-nicd daemon is not running for NIC " + nic_b_pci_ +
                " (expected DPDK runtime dir " + daemon_runtime + " to exist). "
                "Start the daemon first, e.g. via `sudo systemctl start "
                "eph-nicd@" + nic_b_pci_ + ".service` (or `sudo eph-nicd "
                "--pci " + nic_b_pci_ + "` ad-hoc) before running this test.";
            SPDLOG_WARN("DpdkE2ETestEnv: {}", skip_reason_);
            return;
        }
        SPDLOG_INFO("DpdkE2ETestEnv: daemon runtime dir present at {}",
                    daemon_runtime);

        // ── 5. fork the mock dispatcher ───────────────────────────────
        pid_t pid = ::fork();
        if (pid < 0) {
            skip_reason_ = std::string("fork failed: ") + ::strerror(errno);
            SPDLOG_ERROR("DpdkE2ETestEnv: {}", skip_reason_);
            return;
        }
        if (pid == 0) {
            // Child: become the mock dispatcher and never return.
            ::_exit(run_mock_dispatcher(cfg_->networking.server_ip));
        }
        mock_pid_ = pid;
        SPDLOG_INFO("DpdkE2ETestEnv: forked mock dispatcher pid={}", pid);

        // Give the child a moment to bind() all listening sockets.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // ── 6. Platform::create + ARP in the parent ──────────────────
        // Single-secondary attach against the running daemon. queues=1
        // is the only value supported by the S3 placeholder; S5 lifts
        // this when the QueueAllocator + RETA-tracking IPC arrives.
        auto bundle_r = build_bundle_(nic_b_pci_,
                                      cfg_->networking.server_ip,
                                      cfg_->networking.client_ip,
                                      cfg_->networking.gateway_ip);
        if (!bundle_r) {
            skip_reason_ = "build_bundle failed: " + bundle_r.error();
            SPDLOG_ERROR("DpdkE2ETestEnv: {}", skip_reason_);
            stop_mock_();
            return;
        }
        env_.emplace(std::move(*bundle_r));

        // ── 7. Cold-start warmup ─────────────────────────────────────
        warmup_connect_();

        ready_ = true;
        SPDLOG_INFO("DpdkE2ETestEnv: ready");
    }

    /// Source port reserved exclusively for the cold-start warmup
    /// connect.  Picked outside the next_src_port() range used by tests
    /// (which starts at 40000 and grows) so the two pools cannot collide.
    static constexpr uint16_t kWarmupSrcPort = 65530;

    /// Best-effort warmup: open a TCP connection to the echo mock and
    /// immediately close it.  Suppresses cold-start failures (route
    /// cache, gateway ARP, ENA RX queue priming) so the real test
    /// cases see a steady-state path.  Mirrors runner.hpp's
    /// "pre_warmup absorbs route cache / ARP / scheduler cold start".
    void warmup_connect_() noexcept {
        try {
            auto tcfg = env_->make_tcp_config(kWarmupSrcPort, kTcpEchoPort);
            eph::dpdk::TcpSession<> warmup_sess(tcfg, env_->pool);
            (void)warmup_sess.connect(std::chrono::seconds{5});
            (void)warmup_sess.close();
        } catch (...) {
            // Even if connect threw, the side-effect of priming the
            // route cache and gateway ARP table is what we care about.
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    void TearDown() override {
        env_.reset();  // releases secondary attach + EAL
        stop_mock_();
    }

private:
    /// Construct the E2E bundle: secondary-attach Platform via
    /// `Platform::create`, fetch local MAC, ARP-resolve the gateway,
    /// parse all three IPs to host byte order. All failure modes
    /// surface as `std::unexpected<std::string>` so the gtest
    /// Environment can convert them into a SKIP message.
    [[nodiscard]] static std::expected<E2EBundle, std::string>
    build_bundle_(const std::string& pci,
                  const std::string& mock_ip,
                  const std::string& client_ip,
                  const std::string& gateway_ip) {
        // 1. Platform::create — daemon-led secondary attach.
        ::eph::dpdk::PlatformConfig pcfg{};
        pcfg.pci          = pci;
        pcfg.queues       = 1;
        pcfg.program_name = "test_dpdk_e2e";
        pcfg.lcores       = {"0-3"};

        auto plat = ::eph::dpdk::Platform::create(pcfg);
        if (!plat) {
            return std::unexpected(
                "Platform::create failed: " + plat.error());
        }

        const uint16_t port_id = plat->port_id();
        rte_mempool* const pool = plat->mempool();

        // 2. Parse IPs to host byte order.
        auto parse_ip = [](const std::string& s)
            -> std::expected<uint32_t, std::string> {
            in_addr addr{};
            if (inet_pton(AF_INET, s.c_str(), &addr) != 1) {
                return std::unexpected("invalid IP: " + s);
            }
            return ntohl(addr.s_addr);
        };
        auto src_ip = parse_ip(client_ip);
        if (!src_ip) return std::unexpected(src_ip.error());
        auto dst_ip = parse_ip(mock_ip);
        if (!dst_ip) return std::unexpected(dst_ip.error());
        auto gw_ip = parse_ip(gateway_ip);
        if (!gw_ip) return std::unexpected(gw_ip.error());

        // 3. Local MAC.
        rte_ether_addr src_mac{};
        if (int rc = rte_eth_macaddr_get(port_id, &src_mac); rc != 0) {
            return std::unexpected(
                "rte_eth_macaddr_get failed: " + std::to_string(rc));
        }
        SPDLOG_INFO(
            "DpdkE2ETestEnv: local MAC "
            "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
            src_mac.addr_bytes[0], src_mac.addr_bytes[1],
            src_mac.addr_bytes[2], src_mac.addr_bytes[3],
            src_mac.addr_bytes[4], src_mac.addr_bytes[5]);

        // 4. ARP resolve gateway MAC.
        auto gw_mac_result = ::eph::dpdk::arp::resolve(
            port_id, pool,
            src_mac, *src_ip, *gw_ip,
            std::chrono::seconds{3});
        if (!gw_mac_result) {
            return std::unexpected(
                "ARP resolve gateway: " + gw_mac_result.error());
        }
        SPDLOG_INFO(
            "DpdkE2ETestEnv: gateway MAC "
            "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
            gw_mac_result->addr_bytes[0], gw_mac_result->addr_bytes[1],
            gw_mac_result->addr_bytes[2], gw_mac_result->addr_bytes[3],
            gw_mac_result->addr_bytes[4], gw_mac_result->addr_bytes[5]);

        return E2EBundle{
            std::move(*plat),
            *src_ip, *dst_ip, *gw_ip,
            src_mac, *gw_mac_result,
            port_id, pool
        };
    }

    /// Check that the given PCI BDF appears in /sys/bus/pci/drivers/vfio-pci/.
    static bool nic_on_vfio_pci_(const std::string& bdf) noexcept {
        if (bdf.empty()) return false;
        std::string p = "/sys/bus/pci/drivers/vfio-pci/" + bdf;
        return ::access(p.c_str(), F_OK) == 0;
    }

    /// Resolve NIC_B's PCI BDF using (in priority order):
    ///   1. EPH_TEST_NIC_B_PCI environment variable (override)
    ///   2. networking.nic_b_pci from the loaded config.toml
    ///   3. NIC_B_PCI=... line in legacy benchmarks/latency/bench.conf
    ///   4. /sys/class/net/$NIC_B/device symlink (only valid if NIC is
    ///      currently bound to the kernel driver)
    static std::string resolve_nic_b_pci_(const std::string& nic_name,
                                          const std::string& nic_b_pci_from_cfg) {
        if (const char* e = ::getenv("EPH_TEST_NIC_B_PCI"); e && *e) {
            return e;
        }
        if (!nic_b_pci_from_cfg.empty()) {
            return nic_b_pci_from_cfg;
        }
        const char* paths[] = {
#ifdef EPH_BENCH_CONF_ABS_PATH
            EPH_BENCH_CONF_ABS_PATH,
#endif
            "benchmarks/latency/bench.conf",
            "../benchmarks/latency/bench.conf",
            "../../benchmarks/latency/bench.conf",
        };
        for (const char* p : paths) {
            std::ifstream f(p);
            if (!f) continue;
            std::string line;
            while (std::getline(f, line)) {
                size_t s = 0;
                while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
                if (s >= line.size() || line[s] == '#') continue;
                if (line.compare(s, 9, "NIC_B_PCI") != 0) continue;
                auto eq = line.find('=', s);
                if (eq == std::string::npos) continue;
                std::string val = line.substr(eq + 1);
                auto hash = val.find('#');
                if (hash != std::string::npos) val.resize(hash);
                while (!val.empty() && (val.back() == ' ' || val.back() == '\t' ||
                                         val.back() == '\r' || val.back() == '\n'))
                    val.pop_back();
                size_t v0 = 0;
                while (v0 < val.size() && (val[v0] == ' ' || val[v0] == '\t')) ++v0;
                return val.substr(v0);
            }
        }
        if (!nic_name.empty()) {
            std::string link = "/sys/class/net/" + nic_name + "/device";
            char buf[64] = {0};
            ssize_t n = ::readlink(link.c_str(), buf, sizeof(buf) - 1);
            if (n > 0) {
                std::string s(buf, static_cast<size_t>(n));
                auto pos = s.rfind('/');
                if (pos != std::string::npos) return s.substr(pos + 1);
                return s;
            }
        }
        return {};
    }

    static void stop_mock_() noexcept {
        if (mock_pid_ > 0) {
            ::kill(mock_pid_, SIGTERM);
            int wstatus = 0;
            ::waitpid(mock_pid_, &wstatus, 0);
            SPDLOG_INFO("DpdkE2ETestEnv: mock dispatcher reaped (status={})", wstatus);
            mock_pid_ = -1;
        }
    }

    static inline bool ready_ = false;
    static inline std::string skip_reason_;
    static inline std::string nic_b_pci_;
    static inline pid_t mock_pid_ = -1;
    static inline std::optional<E2EBundle> env_;
    static inline std::optional<bench::BenchConfig> cfg_;
};

/// Convenience macro: skip a test if the env is not ready (and report why).
#define EPH_DPDK_E2E_SKIP_IF_NOT_READY()                                       \
    do {                                                                       \
        if (!::eph::dpdk::test_e2e::DpdkE2ETestEnv::ready()) {                 \
            GTEST_SKIP() << "DpdkE2ETestEnv not ready: "                       \
                << ::eph::dpdk::test_e2e::DpdkE2ETestEnv::skip_reason();       \
        }                                                                      \
    } while (0)

} // namespace eph::dpdk::test_e2e
