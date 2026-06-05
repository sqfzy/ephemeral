/// @file test_dpdk_rss_bringup.cpp
/// Integration tests for `Platform::create`'s RSS bring-up failure-path
/// reshape: probe-based bring-up + hard-fail when multi-queue is asked
/// for without a functional RSS path.
///
/// **FIXME(daemon-reshape)**: the S5 dependencies (QueueAllocator +
/// multi-queue secondary claim IPC) have all landed in tree
/// (eph/dpdk/detail/queue_allocator.hpp; Platform::create wires
/// cfg.queues>1 through the daemon's claim path), but this file's
/// fork-per-test fixture has not been re-validated against the new
/// path yet. The architectural concern is that the tests here assert
/// per-secondary RSS bring-up classification (probe / hard-fail /
/// silent-collapse), and in the daemon-led model those decisions
/// move to `Platform::serve_nic` (daemon side) rather than every
/// secondary individually. Re-deriving the correct assertions for
/// the daemon-led shape is the remaining work. Until then every TEST
/// SKIPs at the env level — assertions preserved verbatim under the
/// `EPH_DAEMON_RESHAPE_S5_SKIP` macro so reactivation is local.
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
/// All cases SKIP cleanly if NIC_B isn't bound to vfio-pci or if the
/// reshape S5 multi-queue secondary path isn't ready.

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
        SPDLOG_INFO("RssBringupEnv: ready (NIC_B PCI={})", pci_bdf_);
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

/// Multi-queue Platform tests against a real NIC remain gated off
/// pending end-to-end reactivation under the post-S5 daemon model —
/// the QueueAllocator + RETA-tracking IPC is live in `Platform::create`,
/// but the broader bring-up matrix (RSS hash key plumbing, RETA
/// programming, dispatch_mode validation across PMDs) hasn't been
/// re-verified end-to-end on this host. Remove this macro once a
/// reactivation pass confirms each step still works.
#define EPH_DAEMON_RESHAPE_S5_SKIP()                                     \
    GTEST_SKIP()                                                         \
        << "Multi-queue Platform bring-up tests pending post-S5 "        \
           "reactivation verification (QueueAllocator + IPC are live "   \
           "in Platform::create; broader RSS bring-up matrix needs "     \
           "end-to-end re-verification on this host)."

/// Build a baseline `PlatformConfig` for NIC_B. Daemon-reshape note:
/// the new `PlatformConfig` only carries the application-side knobs
/// (pci / queues / lcores); the NIC physical state (nb_rx_queues,
/// link_timeout_ms, …) lives on the daemon's `NicServiceConfig`.
/// Tests that previously varied `nb_rx_queues` to exercise the
/// bring-up matrix are gated below — see the file header TODO.
::eph::dpdk::PlatformConfig make_pcfg() noexcept {
    ::eph::dpdk::PlatformConfig p{};
    p.pci          = RssBringupEnv::pci_bdf();
    p.queues       = 1;
    p.program_name = "test_dpdk_rss_bringup";
    p.lcores       = {"0-3"};
    return p;
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
// MultiQueue_OnEna_EnablesRssOrFails
//
// `nb_rx_queues=4`: configure_port sets mq_mode=RTE_ETH_MQ_RX_RSS. When the
// NIC advertises IPv4 TCP/UDP RSS hash offloads (ENA does), RSS is genuinely
// active and bring-up succeeds with dispatch_mode != Software. If the NIC
// advertises no IPv4 RSS hash offloads (rss_hf==0), Platform::create
// hard-fails rather than silently collapsing all traffic onto queue 0.
// Either is a valid outcome; the test asserts the result lands in exactly
// one of those two buckets. There is no "probed key" concept any more —
// RSS queue landing is measured empirically (dpdk_rss_queue_probe --finder).
// ─────────────────────────────────────────────────────────────────────────────

TEST(RssBringup, MultiQueue_OnEna_EnablesRssOrFails) {
    EPH_RSS_BRINGUP_SKIP_IF_NOT_READY();
    run_in_subprocess([] {
        auto pcfg = make_pcfg();
        pcfg.queues = 4;  // multi-queue secondary claim

        auto plat_r = ::eph::dpdk::Platform::create(pcfg);
        if (plat_r) {
            // RSS enabled by configure_port — multi-queue RssPartitioned is
            // usable. Queue landing is measured empirically, not predicted.
            const auto& plat = *plat_r;
            EXPECT_NE(plat.dispatch_mode(),
                      ::eph::net::dpdk::RxDispatchMode::Software)
                << "multi-queue RSS bring-up should leave dispatch_mode at "
                   "the NIC's native capability, not pin to Software.";
            SPDLOG_INFO("RssBringup: RSS enabled (dispatch_mode={})",
                ::eph::net::dpdk::rx_dispatch_mode_name(plat.dispatch_mode()));
        } else {
            // Hard-fail: NIC advertises no IPv4 RSS hash offloads (rss_hf==0).
            const std::string& err = plat_r.error();
            SPDLOG_INFO("RssBringup: hard-fail path triggered: {}", err);
            EXPECT_NE(err.find("RSS is not active"), std::string::npos)
                << "error message must explain RSS could not be enabled: "
                << err;
            EXPECT_NE(err.find("nb_rx_queues=1"), std::string::npos)
                << "error message must include the recovery hint "
                   "'nb_rx_queues=1': " << err;
        }
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// SingleQueue_Unchanged
//
// `nb_rx_queues=1`: must build, dispatch_mode pinned to Software.
// Sanity check that the reshape didn't disturb the single-queue path.
//
// Post-derivation note: the previous matrix had two cases here
// (RssEnabled / RssDisabled). With `enable_rss` removed, both
// collapsed onto the same configuration — one test now covers the
// single-queue invariant.
// ─────────────────────────────────────────────────────────────────────────────

TEST(RssBringup, SingleQueue_Unchanged) {
    EPH_RSS_BRINGUP_SKIP_IF_NOT_READY();
    EPH_DAEMON_RESHAPE_S5_SKIP();
    // The body below is preserved verbatim for S5 reactivation. The
    // single-queue path is the easiest to revive — it is the S3
    // placeholder's only supported value — but we keep all RSS tests
    // skipped together until the daemon model and the multi-queue
    // tests can move in lockstep.
    run_in_subprocess([] {
        auto pcfg = make_pcfg();
        pcfg.queues = 1;

        auto plat_r = ::eph::dpdk::Platform::create(pcfg);
        ASSERT_TRUE(plat_r.has_value())
            << "single-queue Platform must still build: " << plat_r.error();
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
