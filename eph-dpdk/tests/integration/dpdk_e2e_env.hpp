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

// Reuse the bench infrastructure in place — same precedent as
// tests/unit/bench/ which already includes from benchmarks/latency/.
#define EPH_USE_DPDK 1
#include "../../../benchmarks/latency/core/config.hpp"
#include "../../../benchmarks/latency/core/dpdk_env.hpp"

#include "mock_dispatcher.hpp"

namespace eph::dpdk::test_e2e {

class DpdkE2ETestEnv : public ::testing::Environment {
public:
    /// Returns true iff EAL is initialized and the mock dispatcher is running.
    static bool ready() noexcept { return ready_; }

    /// Reason the env is not ready (for SKIP messages).
    static const std::string& skip_reason() noexcept { return skip_reason_; }

    /// Access the bench environment (EAL guard, Platform, ARP-resolved gateway MAC).
    static bench::DpdkBenchEnv& env() {
        return *env_;
    }

    /// Access the loaded bench.conf.
    static const bench::BenchConfig& cfg() {
        return cfg_;
    }

    void SetUp() override {
        // ── 1. Load bench.conf ────────────────────────────────────────
        auto cfg_r = bench::load_bench_conf();
        if (!cfg_r) {
            skip_reason_ = "load_bench_conf failed: " + cfg_r.error();
            spdlog::error("DpdkE2ETestEnv: {}", skip_reason_);
            return;
        }
        cfg_ = *cfg_r;

        // ── 2. Resolve NIC_B PCI BDF ──────────────────────────────────
        // BenchConfig doesn't carry NIC_B_PCI (only the lat shell wrapper
        // reads it).  Try the env var first, then bench.conf directly,
        // then /sys/class/net/$NIC_B/device for the kernel-bound case.
        nic_b_pci_ = resolve_nic_b_pci_(cfg_.nic_b);
        if (nic_b_pci_.empty()) {
            skip_reason_ =
                "could not determine NIC_B PCI BDF. Set EPH_TEST_NIC_B_PCI=<bdf> "
                "or add NIC_B_PCI=<bdf> to benchmarks/latency/bench.conf";
            spdlog::warn("DpdkE2ETestEnv: {}", skip_reason_);
            return;
        }
        spdlog::info("DpdkE2ETestEnv: NIC_B={} PCI={}", cfg_.nic_b, nic_b_pci_);

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
            ::_exit(run_mock_dispatcher(cfg_.server_ip));
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

        auto env_r = bench::DpdkBenchEnv::create_full(
            argc, argv,
            cfg_.server_ip, cfg_.local_ip, cfg_.gateway_ip,
            /*dpdk_port_id=*/0);
        if (!env_r) {
            skip_reason_ = "DpdkBenchEnv::create_full failed: " + env_r.error();
            spdlog::error("DpdkE2ETestEnv: {}", skip_reason_);
            stop_mock_();
            return;
        }
        env_.emplace(std::move(*env_r));
        ready_ = true;
        spdlog::info("DpdkE2ETestEnv: ready");
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
    static inline std::optional<bench::DpdkBenchEnv> env_;
    static inline bench::BenchConfig cfg_;
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
