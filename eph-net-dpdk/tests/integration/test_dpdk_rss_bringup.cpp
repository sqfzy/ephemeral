/// @file test_dpdk_rss_bringup.cpp
/// Integration tests for `Platform::create`'s RSS bring-up failure-path
/// reshape: probe-based bring-up + hard-fail when multi-queue is asked
/// for without a functional RSS path.
///
/// These tests are independent of `test_dpdk_rss_platform.cpp`'s
/// `RssPlatformEnv` because they need to construct `Platform` instances
/// with several different `PlatformConfig` shapes within a single
/// process, which the env's single-Platform model doesn't support.
/// Instead we do EAL init once in `RssBringupEnv::SetUp` and let each
/// TEST own its own short-lived Platform.
///
/// All cases SKIP cleanly if NIC_B isn't bound to vfio-pci.
///
/// Configuration matrix:
///
///   ┌──────────────────────────────────────┬─────────────────────────┐
///   │ PlatformConfig                       │ Expected outcome        │
///   ├──────────────────────────────────────┼─────────────────────────┤
///   │ nb_rx_queues=4 enable_rss=true       │ Platform::create succeeds
///   │   (probe path on ENA)                │   with rss_using_probed_key()
///   │                                      │   == true, OR fails with
///   │                                      │   "probe also failed" in
///   │                                      │   the error message — both
///   │                                      │   are valid post-reshape
///   │                                      │   outcomes; which one
///   │                                      │   depends on the running
///   │                                      │   ENA driver version.
///   ├──────────────────────────────────────┼─────────────────────────┤
///   │ nb_rx_queues=4 enable_rss=false      │ Platform::create FAILS
///   │                                      │ with a recovery hint
///   │                                      │ pointing at enable_rss /
///   │                                      │ nb_rx_queues=1.
///   ├──────────────────────────────────────┼─────────────────────────┤
///   │ nb_rx_queues=1 enable_rss=true|false │ Platform::create succeeds,
///   │                                      │ rss_using_probed_key() ==
///   │                                      │ false (probe path never
///   │                                      │ runs in single-queue mode).
///   └──────────────────────────────────────┴─────────────────────────┘

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include <rte_ethdev.h>

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

/// Process-wide environment: just brings up EAL pinned to NIC_B. Does
/// NOT create a Platform — each TEST does that with its own config.
class RssBringupEnv : public ::testing::Environment {
public:
    static bool ready() noexcept { return ready_; }
    static const std::string& reason() noexcept { return reason_; }
    /// EAL-resolved DPDK port id for NIC_B (always 0 in our --allow-pinned
    /// EAL bring-up). Provided as a getter so test bodies stay decoupled
    /// from the constant.
    static uint16_t port_id() noexcept { return port_id_; }

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

        std::string allow_arg = "--allow=" + pci;
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

        auto eal = ::eph::dpdk::EalGuard::init(
            static_cast<int>(argv.size()), argv.data());
        if (!eal) {
            reason_ = "EAL init failed: " + eal.error();
            return;
        }
        eal_ = std::make_unique<::eph::dpdk::EalGuard>(std::move(*eal));

        // EAL init can succeed but PCI bus probe still fail (e.g. when
        // /dev/vfio/<group> can't be opened — common on hosts where the
        // device is bound to vfio-pci but the runtime user lacks access
        // to the group device node, or on noiommu hosts without
        // appropriate capabilities). Surface this as a SKIP rather than
        // letting every TEST below trip on "Platform::create returned
        // error" assertions when the failure is environmental.
        const uint16_t avail = rte_eth_dev_count_avail();
        if (avail == 0) {
            reason_ = "EAL inited but rte_eth_dev_count_avail()=0 — VFIO "
                      "group device probably inaccessible (try running "
                      "via sudo, or check /dev/vfio/<group> permissions)";
            // Hold onto eal_ so EAL stays inited for the lifetime of
            // the env (otherwise dtor will hit rte_eal_cleanup which
            // partially works on noiommu hosts).
            return;
        }
        ready_ = true;
        spdlog::info("RssBringupEnv: ready (PCI={}, port_id={}, ports={})",
                     pci, port_id_, avail);
    }

    void TearDown() override {
        eal_.reset();
        ready_ = false;
    }

private:
    static inline bool        ready_ = false;
    static inline std::string reason_;
    static inline uint16_t    port_id_ = 0;
    static inline std::unique_ptr<::eph::dpdk::EalGuard> eal_;
};

#define EPH_RSS_BRINGUP_SKIP_IF_NOT_READY()                              \
    do {                                                                 \
        if (!RssBringupEnv::ready()) {                                   \
            GTEST_SKIP() << "RssBringupEnv not ready: "                  \
                         << RssBringupEnv::reason();                     \
        }                                                                \
    } while (0)

/// Build a baseline `PlatformConfig` for NIC_B. Tests override individual
/// fields (`nb_rx_queues`, `enable_rss`) to exercise the bring-up matrix.
::eph::dpdk::PlatformConfig make_pcfg() noexcept {
    ::eph::dpdk::PlatformConfig p{};
    p.port_id         = RssBringupEnv::port_id();
    p.nb_rx_queues    = 1;
    p.nb_tx_queues    = 1;
    p.enable_rss      = false;
    p.link_timeout_ms = 0;
    return p;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// MultiQueue_OnEna_ResolvesViaProbeOrFails
//
// `enable_rss=true + nb_rx_queues=4`: on ENA the legacy
// `rte_eth_dev_rss_hash_update` is rejected, and the post-reshape code
// either (a) probes the NIC's actual key via `rte_eth_dev_rss_hash_conf_get`
// and brings RSS up via the probed key, or (b) hard-fails when the PMD
// also won't expose its key. Both are valid outcomes — the test asserts
// that the result lands in exactly one of those two buckets, never the
// previous "silently degrade to single queue" trap.
// ─────────────────────────────────────────────────────────────────────────────

TEST(RssBringup, MultiQueue_OnEna_ResolvesViaProbeOrFails) {
    EPH_RSS_BRINGUP_SKIP_IF_NOT_READY();

    auto pcfg = make_pcfg();
    pcfg.nb_rx_queues = 4;
    pcfg.nb_tx_queues = 4;
    pcfg.enable_rss   = true;

    auto plat_r = ::eph::dpdk::Platform::create(pcfg);
    if (plat_r) {
        // Path (a): probe-based bring-up succeeded — multi-queue
        // RssPartitioned is genuinely usable on this PMD.
        const auto& plat = *plat_r;
        EXPECT_TRUE(plat.rss_using_probed_key())
            << "configure_rss must have failed and probe must have "
               "succeeded for this Platform to exist with multi-queue + "
               "enable_rss=true on ENA — but rss_using_probed_key() is "
               "false. Either the PMD now accepts hash_update (in which "
               "case this assertion is too strong; relax to allow false) "
               "or the bring-up logic regressed.";
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
        // The error message must surface BOTH PMD failures + recovery.
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
}

// ─────────────────────────────────────────────────────────────────────────────
// MultiQueue_NoRss_HardFails
//
// `enable_rss=false + nb_rx_queues=4` is a configuration that the old
// code silently collapsed to single-queue (RETA → queue 0). The reshape
// hard-fails so operators see and handle the misconfiguration.
// ─────────────────────────────────────────────────────────────────────────────

TEST(RssBringup, MultiQueue_NoRss_HardFails) {
    EPH_RSS_BRINGUP_SKIP_IF_NOT_READY();

    auto pcfg = make_pcfg();
    pcfg.nb_rx_queues = 4;
    pcfg.nb_tx_queues = 4;
    pcfg.enable_rss   = false;

    auto plat_r = ::eph::dpdk::Platform::create(pcfg);
    ASSERT_FALSE(plat_r.has_value())
        << "Platform::create must hard-fail when nb_rx_queues>1 is paired "
           "with enable_rss=false — the silent-collapse path was removed.";

    const std::string& err = plat_r.error();
    spdlog::info("RssBringup: enable_rss=false multi-queue rejection: {}",
                 err);
    // Recovery hint: must mention BOTH paths the caller can take.
    EXPECT_NE(err.find("enable_rss=true"), std::string::npos)
        << "recovery message must mention 'enable_rss=true': " << err;
    EXPECT_NE(err.find("nb_rx_queues=1"), std::string::npos)
        << "recovery message must mention 'nb_rx_queues=1': " << err;
}

// ─────────────────────────────────────────────────────────────────────────────
// SingleQueue_Unchanged
//
// `nb_rx_queues=1` paths must be byte-for-byte unchanged by the reshape:
// no probe runs, dispatch_mode pins to Software, rss_using_probed_key
// stays false. Exercises both enable_rss=true and enable_rss=false.
// ─────────────────────────────────────────────────────────────────────────────

TEST(RssBringup, SingleQueue_RssEnabled_Unchanged) {
    EPH_RSS_BRINGUP_SKIP_IF_NOT_READY();

    auto pcfg = make_pcfg();
    pcfg.nb_rx_queues = 1;
    pcfg.nb_tx_queues = 1;
    pcfg.enable_rss   = true;  // honored only when nb_rx_queues > 1

    auto plat_r = ::eph::dpdk::Platform::create(pcfg);
    ASSERT_TRUE(plat_r.has_value())
        << "single-queue Platform must still build: " << plat_r.error();
    EXPECT_FALSE(plat_r->rss_using_probed_key())
        << "probe path must not run in single-queue mode";
    EXPECT_EQ(plat_r->dispatch_mode(),
              ::eph::net::dpdk::RxDispatchMode::Software)
        << "single-queue dispatch_mode must remain pinned to Software";
}

TEST(RssBringup, SingleQueue_RssDisabled_Unchanged) {
    EPH_RSS_BRINGUP_SKIP_IF_NOT_READY();

    auto pcfg = make_pcfg();
    pcfg.nb_rx_queues = 1;
    pcfg.nb_tx_queues = 1;
    pcfg.enable_rss   = false;

    auto plat_r = ::eph::dpdk::Platform::create(pcfg);
    ASSERT_TRUE(plat_r.has_value())
        << "single-queue Platform must still build: " << plat_r.error();
    EXPECT_FALSE(plat_r->rss_using_probed_key());
    EXPECT_EQ(plat_r->dispatch_mode(),
              ::eph::net::dpdk::RxDispatchMode::Software);
}

// ─────────────────────────────────────────────────────────────────────────────
// gtest main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new RssBringupEnv);
    return RUN_ALL_TESTS();
}
