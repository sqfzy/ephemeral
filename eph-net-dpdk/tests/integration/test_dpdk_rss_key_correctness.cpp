/// @file test_dpdk_rss_key_correctness.cpp
///
/// **FIXME(daemon-reshape)**: the multi-queue secondary attach
/// (`cfg.queues>1` on `Platform::create`) and QueueAllocator IPC
/// dependencies have all landed (eph/dpdk/detail/queue_allocator.hpp;
/// Platform::create wires the daemon claim path). What hasn't been
/// re-validated is this test's expectation that each secondary can
/// directly observe the NIC's probed RSS key — in the daemon-led
/// model the probe-vs-fail decision lives on `Platform::serve_nic`
/// (the daemon side) rather than every secondary individually. The
/// TEST bodies are preserved under `EPH_DAEMON_RESHAPE_S5_SKIP` so
/// reactivation is local once the secondary-side observation API
/// is decided.
///
/// Integration test that empirically verifies whether the RSS hash key
/// returned by `rte_eth_dev_rss_hash_conf_get` on the running NIC actually
/// matches what the hardware uses for RX hashing.
///
/// **Why this exists**: ENA on AWS rejects `rte_eth_dev_rss_hash_update`
/// but exposes the in-use key via `rte_eth_dev_rss_hash_conf_get`. eph's
/// "probe path" assumes the readback key is the real key. That assumption
/// has not been end-to-end validated on real ENA — `test_dpdk_rss_bringup`
/// only checks that bring-up resolves, and `test_dpdk_rss_platform`
/// exercises the RETA-collapse Software path, neither of which touches the
/// predicted-vs-observed RSS hash on the wire.
///
/// **Method**:
///   1. Bring up the NIC with `nb_rx_queues=4 enable_rss=true`.
///   2. Skip cleanly if `rss_using_probed_key()` is false — the test
///      specifically targets the probe path.
///   3. Snapshot RSS state (probed key + RETA) via `query_rss_state`.
///   4. Open a kernel UDP echo on NIC_A (forked outside EAL).
///   5. For each of N distinct src_ports, send a UDP probe from NIC_B to
///      the echo. The reply arrives at NIC_B with a known 5-tuple; the
///      NIC RSS-hashes it and (a) writes the 32-bit hash into
///      `mbuf->hash.rss`, (b) routes the packet via RETA to one of the 4
///      queues. We rx_burst all 4 queues and tag arrivals with their
///      queue id.
///   6. Compare two things per probe:
///      - predicted_hash  = toeplitz_hash_ipv4(probed_key, ...)
///        vs mbuf->hash.rss        → tells us if the KEY matches
///      - predicted_queue = queue_for_tuple(state, ...)
///        vs queue we rx_burst'd from → tells us if KEY+RETA match
///
///   If hash matches but queue disagrees → RETA snapshot stale (unlikely;
///   we just queried it). If hash disagrees → probed key is wrong, ENA
///   truly is using a different key from what `rss_hash_conf_get` returns.
///
/// **Skip behaviour**: SKIP cleanly when NIC_B is not on vfio-pci, when
/// the probe path doesn't activate (e.g. on a PMD where configure_rss
/// already succeeds), or when the kernel echo cannot bind (host with no
/// NIC_A IP). Never fails for environment reasons.

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_udp.h>

#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/platform.hpp"
#include "eph/dpdk/test/dpdk_env.hpp"
#include "eph/dpdk/udp.hpp"
#include "eph/net/dpdk/flow_steering.hpp"

#define EPH_USE_DPDK 1
#include "../../../benchmarks/latency/core/bench_conf.hpp"
#include "echo_mocks.hpp"

namespace {

// ── env-level skip gate ──────────────────────────────────────────────
//
// Same invariant as test_dpdk_rss_bringup: NIC_B must be on vfio-pci.
// Loaded config is also exposed (we need server_ip / client_ip /
// gateway_ip / nic_b_pci to drive the test).

bool nic_on_vfio_pci(const std::string& pci_bdf) {
    if (pci_bdf.empty()) return false;
    return ::access(("/sys/bus/pci/drivers/vfio-pci/" + pci_bdf).c_str(), F_OK) == 0;
}

/// Same rationale as in test_dpdk_rss_bringup.cpp — without write access
/// to /dev/hugepages each EAL init aborts with permission errors and the
/// test surfaces as failed instead of skipped. Skip cleanly so a rootless
/// developer sees a clear diagnostic.
bool hugepages_writable() noexcept {
    return ::access("/dev/hugepages", W_OK) == 0;
}

class RssKeyEnv : public ::testing::Environment {
public:
    static bool ready() noexcept { return ready_; }
    static const std::string& reason() noexcept { return reason_; }
    static const bench::BenchConfig& cfg() noexcept { return *cfg_; }
    static const std::string& pci_bdf() noexcept { return pci_bdf_; }

    void SetUp() override {
        std::string conf_path;
        if (const char* e = std::getenv("EPH_BENCH_CONF"); e && *e) conf_path = e;
        else if (const char* e = std::getenv("BENCH_CONFIG"); e && *e) conf_path = e;
        else {
            conf_path = EPH_BENCH_CONF_ABS_PATH;
            if (auto dot = conf_path.rfind("bench.conf"); dot != std::string::npos) {
                std::string toml = conf_path.substr(0, dot) + "config.toml";
                if (::access(toml.c_str(), F_OK) == 0) conf_path = toml;
            }
        }
        auto cfg_r = bench::load_bench_conf(conf_path);
        if (!cfg_r) {
            reason_ = "load_bench_conf failed: " + bench::format_error(cfg_r.error());
            return;
        }
        cfg_.emplace(std::move(*cfg_r));

        std::string pci = cfg_->networking.nic_b_pci;
        if (const char* e = std::getenv("EPH_TEST_NIC_B_PCI"); e && *e) pci = e;
        if (pci.empty()) {
            reason_ = "NIC_B PCI BDF unknown — set EPH_TEST_NIC_B_PCI or "
                      "networking.nic_b_pci in config.toml";
            return;
        }
        if (!nic_on_vfio_pci(pci)) {
            reason_ = "NIC_B (" + pci + ") not bound to vfio-pci — "
                      "skipping RSS key correctness test";
            return;
        }
        if (!hugepages_writable()) {
            reason_ = "no write access to /dev/hugepages (typical when "
                      "running rootless without hugetlbfs group membership) "
                      "— skipping RSS key correctness test";
            return;
        }
        pci_bdf_ = pci;
        ready_ = true;
        SPDLOG_INFO("RssKeyEnv: ready (NIC_B PCI={})", pci_bdf_);
    }

    void TearDown() override {
        ready_ = false;
        pci_bdf_.clear();
        cfg_.reset();
    }

private:
    static inline bool                              ready_   = false;
    static inline std::string                       reason_;
    static inline std::string                       pci_bdf_;
    static inline std::optional<bench::BenchConfig> cfg_;
};

#define EPH_RSS_KEY_SKIP_IF_NOT_READY() \
    do { \
        if (!RssKeyEnv::ready()) GTEST_SKIP() << RssKeyEnv::reason(); \
    } while (0)

/// Multi-queue probe-key correctness tests remain gated off pending
/// end-to-end reactivation under the post-S5 daemon model — the
/// QueueAllocator + RETA-tracking IPC is live in `Platform::create`,
/// but the kNbQueues=4 fixture (mock dispatcher + per-queue UDP echo
/// + Toeplitz-hash queue prediction) hasn't been re-verified end-to-end
/// on this host. Remove this macro once a reactivation pass confirms
/// the full path still works.
#define EPH_DAEMON_RESHAPE_S5_SKIP()                                     \
    GTEST_SKIP()                                                         \
        << "Multi-queue probe-key correctness fixture pending post-S5 " \
           "reactivation verification."

// Fork the gtest env once for the whole binary.
[[maybe_unused]] auto* g_env = ::testing::AddGlobalTestEnvironment(new RssKeyEnv);

// ── helpers ──────────────────────────────────────────────────────────

constexpr uint16_t kUdpEchoPort  = 19101;   // mirrors mock_dispatcher.hpp
constexpr uint16_t kFirstSrcPort = 40000;
constexpr int      kProbeCount   = 8;
constexpr uint16_t kNbQueues     = 4;

/// Run `body` in a forked child, propagate its gtest failure as our own.
/// Mirrors test_dpdk_rss_bringup's helper.
void run_in_subprocess(std::function<void()> body) {
    pid_t pid = ::fork();
    ASSERT_GT(pid, -1) << "fork: " << ::strerror(errno);
    if (pid == 0) {
        body();
        ::_exit(::testing::Test::HasFailure() ? 1 : 0);
    }
    int status = 0;
    pid_t r = ::waitpid(pid, &status, 0);
    ASSERT_EQ(r, pid) << "waitpid: " << ::strerror(errno);
    ASSERT_TRUE(WIFEXITED(status))
        << "child died abnormally (signal="
        << (WIFSIGNALED(status) ? WTERMSIG(status) : -1) << ")";
    EXPECT_EQ(WEXITSTATUS(status), 0)
        << "child reported gtest failure";
}

/// Fork a kernel UDP echo on (server_ip, kUdpEchoPort). The child calls
/// `udp_echo_mock_thread` which busy-loops on recvfrom/sendto; SIGTERM
/// in the parent terminates the child cleanly.
pid_t fork_kernel_udp_echo(const std::string& server_ip) {
    pid_t pid = ::fork();
    if (pid == 0) {
        std::atomic<bool> running{true};
        // Reset SIGTERM handling so the default-action terminate path
        // wins immediately on parent's SIGTERM.
        ::signal(SIGTERM, SIG_DFL);
        eph::dpdk::test_e2e::udp_echo_mock_thread(server_ip, kUdpEchoPort, running);
        ::_exit(0);
    }
    return pid;
}

/// Build the EAL argv pinning EAL to NIC_B's PCI BDF.
std::expected<eph::dpdk::EalGuard, std::string>
init_eal(const std::string& pci_bdf) {
    static std::string allow_arg;
    allow_arg = "--allow=" + pci_bdf;
    static std::vector<std::string> args = {
        "test_dpdk_rss_key_correctness",
        "-l", "0-3",
        "-n", "4",
        "--in-memory",
        allow_arg,
    };
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& a : args) argv.push_back(a.data());
    return eph::dpdk::EalGuard::init_raw(static_cast<int>(argv.size()), argv.data());
}

/// One probe's recorded result.
struct ProbeResult {
    uint16_t src_port           = 0;
    uint32_t predicted_hash     = 0;
    uint16_t predicted_queue    = 0;
    uint32_t observed_hash      = 0;
    uint16_t observed_queue     = 0xFFFF;  // sentinel: not received
    bool     rss_hash_flag_set  = false;   // RTE_MBUF_F_RX_RSS_HASH on mbuf
    bool     received           = false;
};

/// Parse received mbuf as IPv4/UDP from (echo_ip, kUdpEchoPort) → us.
/// Returns the dst_port (= our original src_port) on match, std::nullopt
/// otherwise (different protocol / wrong tuple).
std::optional<uint16_t> match_echo_reply(rte_mbuf* m,
                                         uint32_t echo_ip_be,
                                         uint32_t our_ip_be) {
    auto* eth = rte_pktmbuf_mtod(m, const rte_ether_hdr*);
    if (rte_be_to_cpu_16(eth->ether_type) != RTE_ETHER_TYPE_IPV4)
        return std::nullopt;
    auto* ip = reinterpret_cast<const rte_ipv4_hdr*>(eth + 1);
    if (ip->next_proto_id != IPPROTO_UDP) return std::nullopt;
    if (ip->src_addr != echo_ip_be) return std::nullopt;
    if (ip->dst_addr != our_ip_be)  return std::nullopt;
    const size_t ip_hdr_len = (ip->version_ihl & 0x0F) * 4u;
    auto* udp = reinterpret_cast<const rte_udp_hdr*>(
        reinterpret_cast<const uint8_t*>(ip) + ip_hdr_len);
    if (rte_be_to_cpu_16(udp->src_port) != kUdpEchoPort) return std::nullopt;
    return rte_be_to_cpu_16(udp->dst_port);
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────
// ProbedKeyMatchesNicHash
// ─────────────────────────────────────────────────────────────────────

TEST(RssKeyCorrectness, ProbedKeyMatchesNicHash) {
    EPH_RSS_KEY_SKIP_IF_NOT_READY();
    // T1.3 reactivation: S5 SKIP removed; env probe handles missing
    // hardware / daemon. Body runs end-to-end when env is ready.

    // Fork the kernel echo BEFORE the verifier subprocess so it lives in
    // the parent's address space (no DPDK / EAL touch).
    const auto& net   = RssKeyEnv::cfg().networking;
    pid_t echo_pid = fork_kernel_udp_echo(net.server_ip);
    ASSERT_GT(echo_pid, -1) << "fork echo: " << ::strerror(errno);
    // Brief settle so the child finishes bind() before we start sending.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    auto echo_cleanup = [&] {
        if (echo_pid > 0) {
            ::kill(echo_pid, SIGTERM);
            int status = 0;
            (void)::waitpid(echo_pid, &status, 0);
        }
    };

    run_in_subprocess([&] {
        // ── 1. Platform (multi-queue + RSS via daemon) ───────────────
        eph::dpdk::PlatformConfig pcfg{};
        pcfg.pci          = RssKeyEnv::pci_bdf();
        pcfg.queues       = kNbQueues;
        pcfg.program_name = "test_dpdk_rss_key_correctness";
        pcfg.lcores       = {"0-3"};

        auto plat_r = eph::dpdk::Platform::create(pcfg);
        if (!plat_r) {
            // ENA refused both update and probe → not the path under test.
            SPDLOG_WARN("Platform::create failed (probe also rejected?): {}",
                         plat_r.error());
            GTEST_SKIP() << "Platform multi-queue + RSS cannot bring up: "
                         << plat_r.error();
            return;
        }
        auto& plat = *plat_r;

        if (!plat.rss_using_probed_key()) {
            // configure_rss succeeded → not the probe path. The eph
            // configure_rss installs kRssDefaultKey and that key is
            // verified by upstream Microsoft test vectors; this test
            // adds nothing for non-probe PMDs.
            GTEST_SKIP() << "rss_using_probed_key()==false on this PMD; "
                            "configure_rss path is already validated by "
                            "test_flow_steering. This test specifically "
                            "targets the probe path (typically ENA).";
            return;
        }
        SPDLOG_INFO("RssKeyCorrectness: probe path active "
                     "(dispatch_mode={}, nb_rx_queues={})",
                     eph::net::dpdk::rx_dispatch_mode_name(plat.dispatch_mode()),
                     kNbQueues);

        // ── 2. Snapshot RssState ────────────────────────────────────
        auto state_r = eph::net::dpdk::query_rss_state(plat.port_id());
        ASSERT_TRUE(state_r.has_value()) << "query_rss_state: " << state_r.error();
        auto& state = *state_r;
        ASSERT_GT(state.key_len, 0u)
            << "probed key_len=0 — Platform::create should have rejected this";
        SPDLOG_INFO("RssKeyCorrectness: probed key_len={} reta_size={}",
                     state.key_len, state.reta_size);

        // ── 3. ARP gateway, parse IPs, fetch local MAC ──────────────
        auto parse_ip_host = [](const std::string& s) -> uint32_t {
            in_addr a{}; ::inet_pton(AF_INET, s.c_str(), &a);
            return ntohl(a.s_addr);
        };
        const uint32_t client_ip_h = parse_ip_host(net.client_ip);
        const uint32_t server_ip_h = parse_ip_host(net.server_ip);
        const uint32_t gateway_h   = parse_ip_host(net.gateway_ip);
        const uint32_t client_ip_be = htonl(client_ip_h);
        const uint32_t server_ip_be = htonl(server_ip_h);

        rte_ether_addr local_mac{};
        ASSERT_EQ(rte_eth_macaddr_get(plat.port_id(), &local_mac), 0);

        // Post commit 5c623044: arp::resolve no longer takes a queue_id
        // (RX is hardcoded to queue 0 for ARP because gratuitous-ARP
        // replies don't follow RSS hashing — the kernel control plane
        // uses queue 0 catch-all).
        auto gw_mac_r = eph::dpdk::arp::resolve(
            plat.port_id(), plat.mempool(),
            local_mac, client_ip_h, gateway_h,
            std::chrono::seconds{3});
        ASSERT_TRUE(gw_mac_r.has_value())
            << "ARP gateway: " << gw_mac_r.error();

        // ── 4. Predict + send N probes ──────────────────────────────
        std::array<ProbeResult, kProbeCount> probes{};
        for (int i = 0; i < kProbeCount; ++i) {
            auto& p = probes[i];
            p.src_port = kFirstSrcPort + i;
            // RSS input "src" is the REMOTE peer as seen by the NIC on
            // RX, i.e. the echo server. The reply arriving at NIC_B
            // carries (src=server_ip:19101, dst=client_ip:p.src_port)
            // and that is what the NIC hashes.
            p.predicted_queue = eph::net::dpdk::queue_for_tuple(
                state, server_ip_h, kUdpEchoPort, client_ip_h, p.src_port);
            p.predicted_hash = eph::net::dpdk::toeplitz_hash_ipv4(
                state.key(), server_ip_h, kUdpEchoPort, client_ip_h, p.src_port);

            // TX a 4-byte cookie ("PROB") via UdpSender bound to this src_port.
            eph::dpdk::wire::UdpConfig ucfg{};
            ucfg.src_ip      = client_ip_h;
            ucfg.dst_ip      = server_ip_h;
            ucfg.src_port    = p.src_port;
            ucfg.dst_port    = kUdpEchoPort;
            ucfg.src_mac     = local_mac;
            ucfg.dst_mac     = *gw_mac_r;
            ucfg.port_id     = plat.port_id();
            ucfg.tx_queue_id = 0;
            ucfg.pool        = plat.mempool();
            auto sender_r = eph::dpdk::UdpSender::create(ucfg);
            ASSERT_TRUE(sender_r.has_value())
                << "UdpSender::create: " << sender_r.error();
            const char payload[4] = {'P','R','O','B'};
            const bool sent = sender_r->send(payload, sizeof(payload));
            EXPECT_TRUE(sent) << "TX failed for src_port=" << p.src_port;
        }

        // ── 5. Spin polling all queues for replies ──────────────────
        constexpr auto kDeadline = std::chrono::seconds{3};
        const auto deadline = std::chrono::steady_clock::now() + kDeadline;
        int replies_received = 0;
        while (replies_received < kProbeCount &&
               std::chrono::steady_clock::now() < deadline) {
            for (uint16_t q = 0; q < kNbQueues; ++q) {
                rte_mbuf* burst[32];
                uint16_t n = rte_eth_rx_burst(plat.port_id(), q, burst, 32);
                for (uint16_t i = 0; i < n; ++i) {
                    rte_mbuf* m = burst[i];
                    auto dport = match_echo_reply(m, server_ip_be, client_ip_be);
                    if (dport && *dport >= kFirstSrcPort &&
                        *dport <  kFirstSrcPort + kProbeCount) {
                        const int idx = *dport - kFirstSrcPort;
                        auto& p = probes[idx];
                        if (!p.received) {
                            p.received          = true;
                            p.observed_queue    = q;
                            p.observed_hash     = m->hash.rss;
                            p.rss_hash_flag_set =
                                (m->ol_flags & RTE_MBUF_F_RX_RSS_HASH) != 0;
                            ++replies_received;
                        }
                    }
                    rte_pktmbuf_free(m);
                }
            }
        }

        // ── 6. Compare and report ────────────────────────────────────
        SPDLOG_INFO("RssKeyCorrectness results "
                     "({} of {} replies received within {}s):",
                     replies_received, kProbeCount, kDeadline.count());
        SPDLOG_INFO("  src_port | predicted_q | observed_q | "
                     "predicted_hash | observed_hash | rss_flag");
        int hash_matches = 0, queue_matches = 0;
        for (const auto& p : probes) {
            SPDLOG_INFO("    {:5} | {:^11} | {:^10} | "
                         "    {:#010x} |     {:#010x} | {}",
                         p.src_port,
                         p.predicted_queue,
                         p.received ? std::to_string(p.observed_queue) : "—",
                         p.predicted_hash,
                         p.observed_hash,
                         p.rss_hash_flag_set ? "set" : "clr");
            if (p.received) {
                if (p.predicted_hash  == p.observed_hash)  ++hash_matches;
                if (p.predicted_queue == p.observed_queue) ++queue_matches;
            }
        }

        // Need at least half the probes to land back to draw a verdict.
        ASSERT_GE(replies_received, kProbeCount / 2)
            << "Too few echo replies came back (" << replies_received
            << "/" << kProbeCount << ") — kernel echo on NIC_A may be wedged "
               "or VPC routing dropped traffic; cannot verdict on RSS key.";

        // Primary assertion: NIC-computed Toeplitz hash equals software
        // hash with probed key. Mismatch => probed key is wrong.
        EXPECT_EQ(hash_matches, replies_received)
            << "NIC mbuf->hash.rss disagrees with software Toeplitz over "
               "the probed key — the probe-based RSS key path is producing "
               "incorrect predictions on this PMD. Probed key is not the "
               "live key the NIC uses for hashing. This is the user's "
               "feared scenario: rss_hash_conf_get returns a placeholder.";

        // Secondary assertion: same key + same RETA snapshot ⇒ predicted
        // queue equals observed queue. If hash matches but this fails,
        // RETA changed under us between query_rss_state and tx — unlikely
        // on a quiescent test NIC.
        EXPECT_EQ(queue_matches, replies_received)
            << "Predicted RX queue disagrees with observed RX queue even "
               "though the hash matched — RETA snapshot may be stale.";

        // Optional: warn (not fail) if RTE_MBUF_F_RX_RSS_HASH wasn't set.
        // ENA usually sets it; if it isn't, observed_hash may be zero and
        // the EXPECT_EQ above would already have failed loudly.
        for (const auto& p : probes) {
            if (p.received && !p.rss_hash_flag_set) {
                SPDLOG_WARN("src_port={} mbuf had RSS hash valid flag clear "
                             "(observed_hash={:#010x}) — driver may not be "
                             "populating mbuf->hash.rss; verdict "
                             "uses possibly-zero hash.",
                             p.src_port, p.observed_hash);
            }
        }
    });

    echo_cleanup();
}

// ─────────────────────────────────────────────────────────────────────
// FindSrcPortForQueueLandsOnTargetQueue
//
// Regression for the find_src_port_for_queue Toeplitz argument
// transposition bug: pre-fix the function put the searched local sp in
// the src_port slot of the inbound RSS hash input, but the NIC hashes
// the SYN-ACK / reply with sp in the dst_port slot. Result: predicted
// queue ≠ actual queue ~75% of the time on a 4-queue NIC.
//
// This test asks find_src_port_for_queue to produce one src_port per
// target queue, sends a UDP probe from each, and verifies the echo
// reply actually lands on the queue we asked for. Pre-fix this fails
// (~1 of 4 succeed by chance); post-fix all four succeed.
// ─────────────────────────────────────────────────────────────────────

TEST(RssKeyCorrectness, FindSrcPortForQueueLandsOnTargetQueue) {
    EPH_RSS_KEY_SKIP_IF_NOT_READY();
    // T1.3 reactivation: S5 SKIP removed; env probe handles missing
    // hardware / daemon. Body runs end-to-end when env is ready.

    const auto& net = RssKeyEnv::cfg().networking;
    pid_t echo_pid = fork_kernel_udp_echo(net.server_ip);
    ASSERT_GT(echo_pid, -1) << "fork echo: " << ::strerror(errno);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto echo_cleanup = [&] {
        if (echo_pid > 0) {
            ::kill(echo_pid, SIGTERM);
            int status = 0; (void)::waitpid(echo_pid, &status, 0);
        }
    };

    run_in_subprocess([&] {
        eph::dpdk::PlatformConfig pcfg{};
        pcfg.pci          = RssKeyEnv::pci_bdf();
        pcfg.queues       = kNbQueues;
        pcfg.program_name = "test_dpdk_rss_key_correctness";
        pcfg.lcores       = {"0-3"};

        auto plat_r = eph::dpdk::Platform::create(pcfg);
        if (!plat_r) {
            GTEST_SKIP() << "Platform multi-queue + RSS cannot bring up: "
                         << plat_r.error();
            return;
        }
        auto& plat = *plat_r;
        if (!plat.rss_using_probed_key()) {
            GTEST_SKIP() << "rss_using_probed_key()==false; this test "
                            "specifically targets the probe path.";
            return;
        }

        auto parse_ip_host = [](const std::string& s) -> uint32_t {
            in_addr a{}; ::inet_pton(AF_INET, s.c_str(), &a);
            return ntohl(a.s_addr);
        };
        const uint32_t client_ip_h  = parse_ip_host(net.client_ip);
        const uint32_t server_ip_h  = parse_ip_host(net.server_ip);
        const uint32_t gateway_h    = parse_ip_host(net.gateway_ip);
        const uint32_t client_ip_be = htonl(client_ip_h);
        const uint32_t server_ip_be = htonl(server_ip_h);

        rte_ether_addr local_mac{};
        ASSERT_EQ(rte_eth_macaddr_get(plat.port_id(), &local_mac), 0);

        auto gw_mac_r = eph::dpdk::arp::resolve(
            plat.port_id(), plat.mempool(),
            local_mac, client_ip_h, gateway_h, std::chrono::seconds{3});
        ASSERT_TRUE(gw_mac_r.has_value())
            << "ARP gateway: " << gw_mac_r.error();

        // For each target queue, ask find_src_port_for_queue for an sp,
        // then send a UDP probe from that sp. Reply queue should == target.
        struct PinResult {
            uint16_t target_queue   = 0;
            uint16_t found_src_port = 0;
            uint16_t observed_queue = 0xFFFF;
            bool     received       = false;
        };
        std::array<PinResult, kNbQueues> pins{};
        for (uint16_t q = 0; q < kNbQueues; ++q) {
            pins[q].target_queue = q;
            // Restrict the search to a narrow disjoint window per queue
            // so different targets never resolve to the same sp.
            const uint16_t range_lo = 50000 + q * 2000;
            const uint16_t range_hi = range_lo + 1999;
            auto sp_r = eph::net::dpdk::find_src_port_for_queue(
                plat.port_id(), q,
                /*remote_ip=*/  server_ip_h,
                /*remote_port=*/kUdpEchoPort,
                /*local_ip=*/   client_ip_h,
                range_lo, range_hi);
            ASSERT_TRUE(sp_r.has_value())
                << "find_src_port_for_queue(target=" << q << "): "
                << sp_r.error();
            pins[q].found_src_port = *sp_r;

            eph::dpdk::wire::UdpConfig ucfg{};
            ucfg.src_ip      = client_ip_h;
            ucfg.dst_ip      = server_ip_h;
            ucfg.src_port    = *sp_r;
            ucfg.dst_port    = kUdpEchoPort;
            ucfg.src_mac     = local_mac;
            ucfg.dst_mac     = *gw_mac_r;
            ucfg.port_id     = plat.port_id();
            ucfg.tx_queue_id = 0;
            ucfg.pool        = plat.mempool();
            auto sender_r = eph::dpdk::UdpSender::create(ucfg);
            ASSERT_TRUE(sender_r.has_value())
                << "UdpSender::create: " << sender_r.error();
            const char payload[4] = {'P','I','N','Q'};
            EXPECT_TRUE(sender_r->send(payload, sizeof(payload)))
                << "TX failed for target_q=" << q
                << " src_port=" << *sp_r;
        }

        // Spin all queues for replies.
        constexpr auto kDeadline = std::chrono::seconds{3};
        const auto deadline = std::chrono::steady_clock::now() + kDeadline;
        int replies_received = 0;
        while (replies_received < kNbQueues &&
               std::chrono::steady_clock::now() < deadline) {
            for (uint16_t q = 0; q < kNbQueues; ++q) {
                rte_mbuf* burst[32];
                uint16_t n = rte_eth_rx_burst(plat.port_id(), q, burst, 32);
                for (uint16_t i = 0; i < n; ++i) {
                    rte_mbuf* m = burst[i];
                    auto dport = match_echo_reply(m, server_ip_be, client_ip_be);
                    if (dport) {
                        for (auto& p : pins) {
                            if (p.found_src_port == *dport && !p.received) {
                                p.received       = true;
                                p.observed_queue = q;
                                ++replies_received;
                                break;
                            }
                        }
                    }
                    rte_pktmbuf_free(m);
                }
            }
        }

        SPDLOG_INFO("FindSrcPortForQueue → echo-reply queue mapping:");
        SPDLOG_INFO("  target_q | found_sp | observed_q");
        int correct = 0;
        for (const auto& p : pins) {
            SPDLOG_INFO("    {:^8} | {:^8} | {:^10}",
                         p.target_queue, p.found_src_port,
                         p.received ? std::to_string(p.observed_queue) : "—");
            if (p.received && p.observed_queue == p.target_queue) ++correct;
        }

        ASSERT_EQ(replies_received, kNbQueues)
            << "Some echo replies did not return — cannot verdict on "
               "find_src_port_for_queue correctness.";
        EXPECT_EQ(correct, kNbQueues)
            << "find_src_port_for_queue picked src_ports that do NOT land "
               "on the requested target queue. This is the Toeplitz "
               "argument-transposition bug: the function is hashing "
               "(remote_ip, sp, local_ip, stale_local_port) but the NIC "
               "hashes (remote_ip, remote_port, local_ip, sp).";
    });

    echo_cleanup();
}
