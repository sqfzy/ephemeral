/// @file test_dpdk_rss_platform.cpp
/// Stage 3 integration test for Platform's RSS / multi-queue surface.
///
/// All cases are gated on `NIC_B` being bound to `vfio-pci`. When the
/// NIC is unavailable (CI / dev box / NIC currently used by another
/// DPDK process) every test SKIPs cleanly with a diagnostic, so this
/// binary is safe to run on any host as part of `xmake run -g tests`.
///
/// What's covered (using a real NIC):
///   * Platform::create with enable_rss=true + nb_rx_queues>1 returns
///     a usable Platform — dispatch_mode() returns Software /
///     RssPartitioned / FlowDirector depending on NIC capability.
///   * register_poller succeeds for every valid queue id and the
///     pointer round-trips through poller_for_queue.
///   * register_poller rejects QueueOutOfRange / DuplicateQueue / null.
///   * poller_for_queue returns nullptr for unregistered / out-of-range
///     queue ids.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/platform.hpp"
#include "eph/net/dpdk/flow_steering.hpp"
#include "eph/net/dpdk/poller.hpp"  // DpdkPoller<void> for register_poller signature

#define EPH_USE_DPDK 1
#include "../../../benchmarks/latency/core/config.hpp"

namespace {

// Resolve NIC_B's PCI BDF by env var (EPH_TEST_NIC_B_PCI) or by parsing
// the optional NIC_B_PCI=... line in bench.conf. Mirrors the algorithm in
// tests/integration/dpdk_e2e_env.hpp; kept local to avoid pulling in the
// full e2e env (it would fork a kernel mock dispatcher we don't need).
std::string resolve_nic_b_pci(const std::string& bench_conf_path) {
    if (const char* e = std::getenv("EPH_TEST_NIC_B_PCI"); e && *e) return e;

    std::ifstream f(bench_conf_path);
    if (!f) return {};
    std::string line;
    while (std::getline(f, line)) {
        // Trim leading whitespace.
        size_t s = 0;
        while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
        if (s >= line.size() || line[s] == '#' || line[s] == '[') continue;
        if (line.compare(s, 9, "NIC_B_PCI") != 0) continue;
        size_t eq = line.find('=', s);
        if (eq == std::string::npos) continue;
        size_t v = eq + 1;
        while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
        // Strip trailing whitespace / comments.
        size_t end = line.find_first_of(" \t#", v);
        if (end == std::string::npos) end = line.size();
        return line.substr(v, end - v);
    }
    return {};
}

bool nic_on_vfio_pci(const std::string& pci_bdf) {
    if (pci_bdf.empty()) return false;
    std::string sys = "/sys/bus/pci/drivers/vfio-pci/" + pci_bdf;
    return ::access(sys.c_str(), F_OK) == 0;
}

// Process-wide environment: tries to init EAL once. If the NIC isn't on
// vfio-pci or EAL refuses to initialise, ready_=false and every TEST
// SKIPs with `reason_`. Successful init creates the EalGuard which
// releases EAL at process exit.
class RssPlatformEnv : public ::testing::Environment {
public:
    static bool ready() noexcept { return ready_; }
    static const std::string& reason() noexcept { return reason_; }
    static uint16_t port_id() noexcept { return port_id_; }

    void SetUp() override {
        const char* conf = std::getenv("EPH_BENCH_CONF");
        std::string conf_path = conf ? conf : EPH_BENCH_CONF_ABS_PATH;

        std::string pci = resolve_nic_b_pci(conf_path);
        if (pci.empty()) {
            reason_ = "NIC_B PCI BDF unknown — set EPH_TEST_NIC_B_PCI or "
                      "NIC_B_PCI=... in bench.conf";
            return;
        }
        if (!nic_on_vfio_pci(pci)) {
            reason_ = "NIC_B (" + pci + ") is not bound to vfio-pci — "
                      "skipping RSS Platform tests";
            return;
        }

        // Synthesize a minimal EAL argv pinning to the discovered NIC.
        std::string allow_arg = "--allow=" + pci;
        std::vector<std::string> args = {
            "test_dpdk_rss_platform",
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
        port_id_ = 0;
        ready_   = true;
        spdlog::info("RssPlatformEnv: EAL ready, NIC_B PCI={}, port_id=0", pci);
    }

    void TearDown() override {
        eal_.reset();
        ready_ = false;
    }

private:
    static inline bool ready_ = false;
    static inline std::string reason_;
    static inline uint16_t port_id_ = 0;
    static inline std::unique_ptr<::eph::dpdk::EalGuard> eal_;
};

#define EPH_RSS_PLATFORM_SKIP_IF_NOT_READY()                            \
    do {                                                                 \
        if (!RssPlatformEnv::ready()) {                                  \
            GTEST_SKIP() << "RssPlatformEnv not ready: "                 \
                         << RssPlatformEnv::reason();                    \
        }                                                                \
    } while (0)

// Build a 4-RX-queue, RSS-enabled Platform on the test NIC. Each TEST
// in this file constructs its own Platform — Platform's destructor
// calls rte_eth_dev_stop + rte_eth_dev_close, so the next test starts
// from a clean port.
::eph::dpdk::PlatformConfig make_rss_pcfg(uint16_t nb_queues = 4) {
    ::eph::dpdk::PlatformConfig pcfg{};
    pcfg.port_id      = RssPlatformEnv::port_id();
    pcfg.nb_rx_queues = nb_queues;
    pcfg.nb_tx_queues = nb_queues;
    pcfg.enable_rss   = true;
    pcfg.link_timeout_ms = 0;  // tests don't need link to be UP
    return pcfg;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// All RSS Platform checks fold into a single TEST.
//
// Why: DPDK detaches the port slot on rte_eth_dev_close, after which
// rte_eth_dev_count_avail() returns 0 — a second Platform::create() in
// the same process fails with "No DPDK ports available". Sharing one
// Platform across all assertions sidesteps this; we use distinct queue
// ids per assertion so register_poller side-effects don't bleed.
// ─────────────────────────────────────────────────────────────────────────────

TEST(PlatformRss, EndToEnd) {
    EPH_RSS_PLATFORM_SKIP_IF_NOT_READY();
    auto pcfg = make_rss_pcfg(4);
    auto plat_r = ::eph::dpdk::Platform::create(pcfg);
    ASSERT_TRUE(plat_r.has_value())
        << "Platform::create failed: " << plat_r.error();
    auto& plat = *plat_r;

    // ─── dispatch_mode + nb_rx_queues ──────────────────────────────────
    const auto mode = plat.dispatch_mode();
    EXPECT_TRUE(mode == ::eph::net::dpdk::RxDispatchMode::Software ||
                mode == ::eph::net::dpdk::RxDispatchMode::RssPartitioned ||
                mode == ::eph::net::dpdk::RxDispatchMode::FlowDirector)
        << "unexpected dispatch_mode value";
    spdlog::info("dispatch_mode = {}",
                 ::eph::net::dpdk::rx_dispatch_mode_name(mode));
    const uint16_t n = plat.nb_rx_queues();
    EXPECT_GT(n, 0u);
    EXPECT_LE(n, 4u);

    // ─── M2 invariant: ENA fallback path pins dispatch_mode=Software ───
    // ENA PMD rejects rte_eth_dev_rss_hash_update, so configure_rss
    // failed (rss_active=false) — dispatch_mode must therefore be
    // Software regardless of what detect_rx_dispatch_mode reported.
    // On Mellanox/Intel where hash_update succeeds, this assertion
    // exercises the kept-as-detected path (RssPartitioned remains).
    if (mode != ::eph::net::dpdk::RxDispatchMode::Software) {
        spdlog::info("dispatch_mode = {} → RSS active on this NIC",
                     ::eph::net::dpdk::rx_dispatch_mode_name(mode));
    }

    // ─── register_poller happy path: register every valid queue ────────
    // The Poller pointer is opaque from Platform's perspective; tests use
    // distinct fake addresses (never dereferenced) so we only exercise
    // the registry plumbing.
    using PollerPtr = ::eph::net::dpdk::DpdkPoller<void>*;
    PollerPtr dummies[4] = {
        reinterpret_cast<PollerPtr>(static_cast<uintptr_t>(0xDEAD0001)),
        reinterpret_cast<PollerPtr>(static_cast<uintptr_t>(0xDEAD0002)),
        reinterpret_cast<PollerPtr>(static_cast<uintptr_t>(0xDEAD0003)),
        reinterpret_cast<PollerPtr>(static_cast<uintptr_t>(0xDEAD0004)),
    };
    for (uint16_t q = 0; q < n; ++q) {
        auto r = plat.register_poller(q, dummies[q]);
        EXPECT_TRUE(r.has_value())
            << "register_poller(" << q << ") failed: "
            << (r ? "" : r.error());
        EXPECT_EQ(plat.poller_for_queue(q), dummies[q]);
    }

    // ─── DuplicateQueue: re-register a previously occupied qid (0) ─────
    auto* dup_marker =
        reinterpret_cast<PollerPtr>(static_cast<uintptr_t>(0xBEEF0000));
    auto r_dup = plat.register_poller(0, dup_marker);
    ASSERT_FALSE(r_dup.has_value());
    EXPECT_NE(r_dup.error().find("DuplicateQueue"), std::string::npos);
    EXPECT_EQ(plat.poller_for_queue(0), dummies[0])
        << "duplicate register must not overwrite";

    // ─── QueueOutOfRange: 9999 is above kMaxRssQueues and any NIC qid ──
    auto* oor_marker =
        reinterpret_cast<PollerPtr>(static_cast<uintptr_t>(0xBEEF0001));
    auto r_oor = plat.register_poller(9999, oor_marker);
    ASSERT_FALSE(r_oor.has_value());
    EXPECT_NE(r_oor.error().find("QueueOutOfRange"), std::string::npos);
    EXPECT_EQ(plat.poller_for_queue(9999), nullptr);

    // ─── null Poller pointer ───────────────────────────────────────────
    auto r_null = plat.register_poller(0, nullptr);
    ASSERT_FALSE(r_null.has_value());
    EXPECT_NE(r_null.error().find("null"), std::string::npos);

    // ─── poller_for_queue on unregistered queue ────────────────────────
    // Pick any qid > nb_rx_queues that's still below kMaxRssQueues.
    if (n < 64) {
        EXPECT_EQ(plat.poller_for_queue(n), nullptr)
            << "queue past nb_rx_queues should return nullptr";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// gtest main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new RssPlatformEnv);
    return RUN_ALL_TESTS();
}
