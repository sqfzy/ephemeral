#pragma once

/// @file dpdk_e2e_env.hpp
/// gtest Environment for DPDK end-to-end tests.
///
/// Lifecycle (per test binary, exactly once):
///   SetUp:
///     1. Load bench.conf
///     2. Verify NIC_B is bound to vfio-pci (else mark all tests SKIP)
///     3. fork() — child runs MockDispatcher::run, parent stores child PID
///     4. Parent: DpdkBenchEnv::create_full() — EAL + Platform + ARP
///     5. Brief sleep to let child mocks finish bind/listen before tests run
///   TearDown:
///     1. SIGTERM child, waitpid
///     2. Destroy DpdkBenchEnv (releases EAL)
///
/// Tests access shared state via the static accessors below.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

// bench.conf parsing still lives in latency/core/ — that header is
// bench-specific (it knows about scenario sweeps, payload lists, etc.).
// DpdkBenchEnv has been promoted to eph::dpdk::test, so we include it
// from its canonical home rather than reverse-including via the bench
// shim.
#define EPH_USE_DPDK 1
#include "../../../benchmarks/latency/core/bench_conf.hpp"
#include "eph/dpdk/test/dpdk_env.hpp"

#include "mock_dispatcher.hpp"

namespace eph::dpdk::test_e2e {

class DpdkE2ETestEnv : public ::testing::Environment {
public:
    /// Returns true iff EAL is initialized and the mock dispatcher is running.
    static bool ready() noexcept { return ready_; }

    /// Reason the env is not ready (for SKIP messages).
    static const std::string& skip_reason() noexcept { return skip_reason_; }

    /// Access the bench environment (EAL guard, Platform, ARP-resolved gateway MAC).
    static ::eph::dpdk::test::DpdkBenchEnv& env() {
        return *env_;
    }

    /// Access the loaded config.toml (read-only).
    static const bench::BenchConfig& cfg() {
        return *cfg_;
    }

    void SetUp() override {
        // ── 1. Load config.toml ───────────────────────────────────────
        // Path resolution mirrors the legacy search order: $BENCH_CONFIG
        // env var wins; otherwise fall back to the compiled-in absolute
        // path provided by xmake (EPH_BENCH_CONF_ABS_PATH).
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
            spdlog::error("DpdkE2ETestEnv: {}", skip_reason_);
            return;
        }
        cfg_.emplace(std::move(*cfg_r));

        // ── 2. Resolve NIC_B PCI BDF ──────────────────────────────────
        // Prefer the env var; fall back to config.toml's
        // networking.nic_b_pci; else /sys/class/net/<nic_b>/device.
        nic_b_pci_ = resolve_nic_b_pci_(cfg_->networking.nic_b);
        if (nic_b_pci_.empty()) {
            skip_reason_ =
                "could not determine NIC_B PCI BDF. Set EPH_TEST_NIC_B_PCI=<bdf> "
                "or add nic_b_pci=<bdf> under [networking] in config.toml";
            spdlog::warn("DpdkE2ETestEnv: {}", skip_reason_);
            return;
        }
        spdlog::info("DpdkE2ETestEnv: NIC_B={} PCI={}",
                     cfg_->networking.nic_b, nic_b_pci_);

        // ── 3. Verify NIC_B is on vfio-pci ────────────────────────────
        if (!nic_on_vfio_pci_(nic_b_pci_)) {
            skip_reason_ = std::string(
                "NIC_B (PCI ") + nic_b_pci_ + ") is not bound to vfio-pci. "
                "Run `sudo benchmarks/latency/lat tcp --dpdk` once first to "
                "transition NIC_B, then re-run this test binary.";
            spdlog::warn("DpdkE2ETestEnv: {}", skip_reason_);
            return;
        }

        // ── 4. fork the mock dispatcher ───────────────────────────────
        pid_t pid = ::fork();
        if (pid < 0) {
            skip_reason_ = std::string("fork failed: ") + ::strerror(errno);
            spdlog::error("DpdkE2ETestEnv: {}", skip_reason_);
            return;
        }
        if (pid == 0) {
            // Child: become the mock dispatcher and never return.
            ::_exit(run_mock_dispatcher(cfg_->networking.server_ip));
        }
        mock_pid_ = pid;
        spdlog::info("DpdkE2ETestEnv: forked mock dispatcher pid={}", pid);

        // Give the child a moment to bind() all listening sockets.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // ── 5. EAL + Platform + ARP in the parent ────────────────────
        // Synthesize the minimum EAL argv from the resolved PCI BDF.
        static std::string argv0 = "test_dpdk_e2e";
        static std::string pci_arg;
        pci_arg = std::string("--allow=") + nic_b_pci_;
        static std::string log_arg = "--log-level=lib.eal:warning";
        char* argv[] = {
            const_cast<char*>(argv0.c_str()),
            const_cast<char*>(pci_arg.c_str()),
            const_cast<char*>(log_arg.c_str()),
        };
        int argc = 3;

        auto env_r = ::eph::dpdk::test::DpdkBenchEnv::create_full(
            argc, argv,
            cfg_->networking.server_ip,
            cfg_->networking.client_ip,
            cfg_->networking.gateway_ip,
            /*dpdk_port_id=*/0);
        if (!env_r) {
            skip_reason_ = "DpdkBenchEnv::create_full failed: " + env_r.error();
            spdlog::error("DpdkE2ETestEnv: {}", skip_reason_);
            stop_mock_();
            return;
        }
        env_.emplace(std::move(*env_r));

        // ── 6. Cold-start warmup ─────────────────────────────────────
        // The first TCP connect after EAL bring-up always fails or
        // stalls: AWS VPC route cache, gateway ARP for our DPDK source
        // IP, and the ENA driver's RX queue priming all need 1-2
        // round-trips to settle.  Mirror runner.hpp's "pre_warmup
        // absorbs route cache / ARP / scheduler cold start" pattern by
        // doing a throw-away TCP connect to the kernel echo port.
        // Result is intentionally ignored.
        warmup_connect_();

        ready_ = true;
        spdlog::info("DpdkE2ETestEnv: ready");
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
            // Longer deadline than the test default since this is the
            // cold path; ignore the result either way.
            (void)warmup_sess.connect(std::chrono::seconds{5});
            (void)warmup_sess.close();
        } catch (...) {
            // Even if connect threw, the side-effect of priming the
            // route cache and gateway ARP table is what we care about.
        }
        // Brief idle so the close fully drains before real tests run.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    void TearDown() override {
        env_.reset();  // releases EAL via guard
        stop_mock_();
    }

private:
    /// Check that the given PCI BDF appears in /sys/bus/pci/drivers/vfio-pci/.
    static bool nic_on_vfio_pci_(const std::string& bdf) noexcept {
        if (bdf.empty()) return false;
        std::string p = "/sys/bus/pci/drivers/vfio-pci/" + bdf;
        return ::access(p.c_str(), F_OK) == 0;
    }

    /// Resolve NIC_B's PCI BDF using (in priority order):
    ///   1. EPH_TEST_NIC_B_PCI environment variable
    ///   2. NIC_B_PCI=... line in benchmarks/latency/bench.conf
    ///   3. /sys/class/net/$NIC_B/device symlink (only valid if NIC is
    ///      currently bound to the kernel driver)
    static std::string resolve_nic_b_pci_(const std::string& nic_name) {
        if (const char* e = ::getenv("EPH_TEST_NIC_B_PCI"); e && *e) {
            return e;
        }
        // Try parsing bench.conf directly for NIC_B_PCI=.  We try a
        // compile-time absolute path first (set by xmake.lua) so the
        // resolver works regardless of the test runner's cwd, then a
        // few cwd-relative fallbacks.
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
                // strip leading whitespace and comments
                size_t s = 0;
                while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
                if (s >= line.size() || line[s] == '#') continue;
                if (line.compare(s, 9, "NIC_B_PCI") != 0) continue;
                auto eq = line.find('=', s);
                if (eq == std::string::npos) continue;
                std::string val = line.substr(eq + 1);
                // strip trailing whitespace / inline comment
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
        // Fall back to /sys/class/net/<name>/device symlink
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
            spdlog::info("DpdkE2ETestEnv: mock dispatcher reaped (status={})", wstatus);
            mock_pid_ = -1;
        }
    }

    static inline bool ready_ = false;
    static inline std::string skip_reason_;
    static inline std::string nic_b_pci_;
    static inline pid_t mock_pid_ = -1;
    static inline std::optional<::eph::dpdk::test::DpdkBenchEnv> env_;
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
