/// @file test_dpdk_rss_bringup.cpp
/// Integration tests for `Platform::create`'s RSS bring-up failure-path
/// reshape: probe-based bring-up + hard-fail when multi-queue is asked
/// for without a functional RSS path.
///
/// **Architecture — process-per-test isolation**: each TEST forks a
/// child process that does its own `rte_eal_init` + `Platform::create`
/// with the test's specific config + assertions, then exits. The parent
/// waits and checks the child's exit code. This is necessary because
/// `Platform::~Platform` calls `rte_eth_dev_close`, which removes the
/// port from `rte_eth_dev_count_avail` for the rest of the EAL session
/// — without process isolation, the second test in a single process
/// would see "No DPDK ports available" before reaching the assertions
/// it cares about. Forking gives each test a fresh EAL view.
///
/// The env-level setup is intentionally minimal (just verify NIC_B is
/// bound to vfio-pci); EAL init happens per-child.
///
/// All cases SKIP cleanly if NIC_B isn't bound to vfio-pci.
///
/// Configuration matrix (post enable_rss removal):
///
///   ┌──────────────────────────────────────┬─────────────────────────┐
///   │ PlatformConfig                       │ Expected outcome        │
///   ├──────────────────────────────────────┼─────────────────────────┤
///   │ nb_rx_queues=4                       │ Platform::create succeeds
///   │   (RSS auto-engages; probe path on   │   with rss_using_probed_key()
///   │    ENA)                              │   == true, OR fails with
///   │                                      │   "probe also failed" in
///   │                                      │   the error message — both
///   │                                      │   are valid post-reshape
///   │                                      │   outcomes; which one
///   │                                      │   depends on the running
///   │                                      │   ENA driver version.
///   ├──────────────────────────────────────┼─────────────────────────┤
///   │ nb_rx_queues=1                       │ Platform::create succeeds,
///   │                                      │ rss_using_probed_key() ==
///   │                                      │ false (probe path never
///   │                                      │ runs in single-queue mode).
///   └──────────────────────────────────────┴─────────────────────────┘
///
/// Note: the previous "nb_rx_queues=4 enable_rss=false → hard-fail" case
/// is no longer expressible (the field is gone) — the invariant moved
/// from runtime check to type system. See CHANGELOG.

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/platform.hpp"

#define EPH_USE_DPDK 1
#include "../../../benchmarks/latency/core/bench_conf.hpp"

namespace {

bool nic_on_vfio_pci(const std::string& pci_bdf) {
    if (pci_bdf.empty()) return false;
    std::string sys = "/sys/bus/pci/drivers/vfio-pci/" + pci_bdf;
    return ::access(sys.c_str(), F_OK) == 0;
}

/// Check that the running process has write access to `/dev/hugepages/`.
/// Without it, EAL init succeeds far enough to produce log spam but then
/// every per-test child fails with "Couldn't get fd on hugepage file" and
/// the parent reports the failures as test failures rather than
/// environment skips. Most distros ship hugetlbfs root-owned, so on a
/// rootless run this returns false and we skip the suite cleanly.
bool hugepages_writable() noexcept {
    return ::access("/dev/hugepages", W_OK) == 0;
}

/// Env-level shared state: the vfio-pci-bound NIC's PCI BDF. EAL is
/// NOT initialised here — each TEST forks and the child runs its own
/// EAL session, scoped to a single Platform construction.
class RssBringupEnv : public ::testing::Environment {
public:
    static bool ready() noexcept { return ready_; }
    static const std::string& reason() noexcept { return reason_; }
    static const std::string& pci_bdf() noexcept { return pci_bdf_; }

    void SetUp() override {
        std::string conf_path;
        if (const char* e = std::getenv("EPH_BENCH_CONF"); e && *e) {
            conf_path = e;
        } else if (const char* e = std::getenv("BENCH_CONFIG"); e && *e) {
            conf_path = e;
        } else {
            conf_path = EPH_BENCH_CONF_ABS_PATH;
            if (auto dot = conf_path.rfind("bench.conf"); dot != std::string::npos) {
                std::string toml = conf_path.substr(0, dot) + "config.toml";
                if (::access(toml.c_str(), F_OK) == 0) conf_path = toml;
            }
        }

        auto cfg_r = bench::load_bench_conf(conf_path);
        if (!cfg_r) {
            reason_ = "load_bench_conf failed: " +
                      bench::format_error(cfg_r.error());
            return;
        }
        const bench::BenchConfig& cfg = *cfg_r;

        std::string pci = cfg.networking.nic_b_pci;
        if (const char* e = std::getenv("EPH_TEST_NIC_B_PCI"); e && *e) pci = e;
        if (pci.empty()) {
            reason_ = "NIC_B PCI BDF unknown — set EPH_TEST_NIC_B_PCI or "
                      "networking.nic_b_pci in config.toml";
            return;
        }
        if (!nic_on_vfio_pci(pci)) {
            reason_ = "NIC_B (" + pci + ") is not bound to vfio-pci — "
                      "skipping RSS bring-up tests";
            return;
        }

        // Without write access to /dev/hugepages every per-test EAL child
        // fails on `get_seg_fd: open '<...>map_*' failed: Permission denied`
        // and the parent surfaces those as test failures. Skip cleanly so
        // a developer running the suite without sudo/membership in
        // hugepages' group sees the right diagnostic instead of red Xs.
        if (!hugepages_writable()) {
            reason_ = "no write access to /dev/hugepages (typical when "
                      "running rootless without hugetlbfs group membership) "
                      "— skipping RSS bring-up tests";
            return;
        }

        pci_bdf_ = pci;
        ready_ = true;
        spdlog::info("RssBringupEnv: ready (NIC_B PCI={})", pci_bdf_);
    }

    void TearDown() override {
        ready_ = false;
        pci_bdf_.clear();
    }

private:
    static inline bool        ready_ = false;
    static inline std::string reason_;
    static inline std::string pci_bdf_;
};

#define EPH_RSS_BRINGUP_SKIP_IF_NOT_READY()                              \
    do {                                                                 \
        if (!RssBringupEnv::ready()) {                                   \
            GTEST_SKIP() << "RssBringupEnv not ready: "                  \
                         << RssBringupEnv::reason();                     \
        }                                                                \
    } while (0)

/// Build a baseline `PlatformConfig` for NIC_B. Tests override
/// `nb_rx_queues` to exercise the bring-up matrix; nb_rx_queues > 1
/// auto-engages RSS/FlowDirector bring-up (the previous `enable_rss`
/// flag was removed in favour of this derivation).
::eph::dpdk::PlatformConfig make_pcfg() noexcept {
    ::eph::dpdk::PlatformConfig p{};
    p.port_id         = 0;  // EAL allow-listed only NIC_B → port_id 0
    p.nb_rx_queues    = 1;
    p.nb_tx_queues    = 1;
    p.link_timeout_ms = 0;
    return p;
}

/// Spin up an EAL session pinned to NIC_B. Returns the EalGuard for the
/// caller to keep alive over the Platform lifetime.
std::expected<::eph::dpdk::EalGuard, std::string> init_eal_for_nic_b() {
    std::string allow_arg = "--allow=" + RssBringupEnv::pci_bdf();
    std::vector<std::string> args = {
        "test_dpdk_rss_bringup",
        "-l", "0-3",
        "-n", "4",
        "--in-memory",
        allow_arg,
    };
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& a : args) argv.push_back(a.data());
    return ::eph::dpdk::EalGuard::init(
        static_cast<int>(argv.size()), argv.data());
}

/// Run `body` in a forked child. Child runs `body` (which is expected
/// to use gtest EXPECT_*/ASSERT_* macros writing to stderr); on any
/// gtest failure the child exits with status 1, otherwise 0. Parent
/// waits and propagates failure into the calling TEST.
///
/// Rationale: `Platform::~Platform` calls `rte_eth_dev_close`, which
/// removes the port from `rte_eth_dev_count_avail` — running multiple
/// tests in one process means the second sees "No DPDK ports
/// available". Per-test fork gives each test a fresh EAL view.
void run_in_subprocess(std::function<void()> body) {
    pid_t pid = ::fork();
    ASSERT_GT(pid, -1) << "fork() failed: " << ::strerror(errno);
    if (pid == 0) {
        // Child. Run the body and report failure via exit code.
        body();
        // gtest assertion failures write to stderr automatically; here
        // we just translate the in-process failure flag to an exit code.
        const bool failed = ::testing::Test::HasFailure();
        // _exit avoids triggering atexit handlers (notably DPDK's
        // hugepage cleanup on the parent's mmaps).
        ::_exit(failed ? 1 : 0);
    }
    int status = 0;
    pid_t r = ::waitpid(pid, &status, 0);
    ASSERT_EQ(r, pid) << "waitpid failed: " << ::strerror(errno);
    ASSERT_TRUE(WIFEXITED(status))
        << "child died abnormally (signal="
        << (WIFSIGNALED(status) ? WTERMSIG(status) : -1) << ")";
    EXPECT_EQ(WEXITSTATUS(status), 0)
        << "child reported gtest failure — see stderr above for details";
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// MultiQueue_OnEna_ResolvesViaProbeOrFails
//
// `nb_rx_queues=4`: on ENA the legacy `rte_eth_dev_rss_hash_update` is
// rejected, and the post-reshape code either (a) probes the NIC's
// actual key via `rte_eth_dev_rss_hash_conf_get` and brings RSS up via
// the probed key, or (b) hard-fails when the PMD also won't expose its
// key. Both are valid outcomes — the test asserts that the result lands
// in exactly one of those two buckets, never the previous "silently
// degrade to single queue" trap.
// ─────────────────────────────────────────────────────────────────────────────

TEST(RssBringup, MultiQueue_OnEna_ResolvesViaProbeOrFails) {
    EPH_RSS_BRINGUP_SKIP_IF_NOT_READY();
    run_in_subprocess([] {
        auto eal = init_eal_for_nic_b();
        ASSERT_TRUE(eal.has_value()) << "EAL init: " << eal.error();

        auto pcfg = make_pcfg();
        pcfg.nb_rx_queues = 4;
        pcfg.nb_tx_queues = 4;

        auto plat_r = ::eph::dpdk::Platform::create(pcfg);
        if (plat_r) {
            // Path (a): probe-based bring-up succeeded — multi-queue
            // RssPartitioned is genuinely usable on this PMD.
            const auto& plat = *plat_r;
            EXPECT_TRUE(plat.rss_using_probed_key())
                << "configure_rss must have failed AND probe must have "
                   "succeeded for this Platform to exist with nb_rx_queues=4 "
                   "on ENA — but rss_using_probed_key() is false. Either the "
                   "PMD now accepts hash_update (relax to allow false in that "
                   "case) or bring-up logic regressed.";
            EXPECT_NE(plat.dispatch_mode(),
                      ::eph::net::dpdk::RxDispatchMode::Software)
                << "probe-based RSS bring-up should leave dispatch_mode at "
                   "the NIC's native capability, not pin to Software.";
            spdlog::info(
                "RssBringup: probe path succeeded "
                "(dispatch_mode={}, rss_using_probed_key=true)",
                ::eph::net::dpdk::rx_dispatch_mode_name(plat.dispatch_mode()));
        } else {
            // Path (b): both configure_rss and probe failed → hard-fail.
            const std::string& err = plat_r.error();
            spdlog::info("RssBringup: hard-fail path triggered: {}", err);
            EXPECT_NE(err.find("probe also failed"), std::string::npos)
                << "error message must mention 'probe also failed' so "
                   "operators can distinguish this from configure_rss-only "
                   "failure: " << err;
            EXPECT_NE(err.find("nb_rx_queues=1"), std::string::npos)
                << "error message must include the recovery hint "
                   "'nb_rx_queues=1': " << err;
        }
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// SingleQueue_Unchanged
//
// `nb_rx_queues=1`: must build, dispatch_mode pinned to Software,
// rss_using_probed_key=false (probe never runs in single-queue mode).
// Sanity check that the reshape didn't disturb the single-queue path.
//
// Post-derivation note: the previous matrix had two cases here
// (RssEnabled / RssDisabled). With `enable_rss` removed, both
// collapsed onto the same configuration — one test now covers the
// single-queue invariant.
// ─────────────────────────────────────────────────────────────────────────────

TEST(RssBringup, SingleQueue_Unchanged) {
    EPH_RSS_BRINGUP_SKIP_IF_NOT_READY();
    run_in_subprocess([] {
        auto eal = init_eal_for_nic_b();
        ASSERT_TRUE(eal.has_value()) << "EAL init: " << eal.error();

        auto pcfg = make_pcfg();
        pcfg.nb_rx_queues = 1;
        pcfg.nb_tx_queues = 1;

        auto plat_r = ::eph::dpdk::Platform::create(pcfg);
        ASSERT_TRUE(plat_r.has_value())
            << "single-queue Platform must still build: " << plat_r.error();
        EXPECT_FALSE(plat_r->rss_using_probed_key())
            << "probe path must not run in single-queue mode";
        EXPECT_EQ(plat_r->dispatch_mode(),
                  ::eph::net::dpdk::RxDispatchMode::Software)
            << "single-queue dispatch_mode must remain pinned to Software";
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// gtest main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new RssBringupEnv);
    return RUN_ALL_TESTS();
}
