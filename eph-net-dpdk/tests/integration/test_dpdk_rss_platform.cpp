/// @file test_dpdk_rss_platform.cpp
/// Stage 3 integration test for Platform's RSS / multi-queue surface.
///
/// All cases are gated on `NIC_B` being bound to `vfio-pci`. When the
/// NIC is unavailable (CI / dev box / NIC currently used by another
/// DPDK process) every test SKIPs cleanly with a diagnostic, so this
/// binary is safe to run on any host as part of `xmake run -g tests`.
///
/// What's covered (using a real NIC):
///   * Platform with enable_rss=true + nb_rx_queues>1 returns a usable
///     Platform — dispatch_mode() returns Software (post-PR-2 invariant
///     pin when ENA's hash_update is unsupported) or RssPartitioned /
///     FlowDirector on capable PMDs.
///   * register_poller / poller_for_queue plumbing under DpdkPoller<void>*
///     typed signature (PR-2 m3): happy path + DuplicateQueue +
///     QueueOutOfRange + null + unregistered queue → nullptr.
///   * **End-to-end create_and_attach (Software mode)**: TCP echo round
///     trip via DpdkTcpStream::create_and_attach against a forked kernel
///     mock on NIC_A — proves the turnkey API path actually moves bytes.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/platform.hpp"
#include "eph/dpdk/test/dpdk_env.hpp"
#include "eph/net/dpdk/flow_steering.hpp"
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/dpdk/tcp_stream.hpp"

#define EPH_USE_DPDK 1
#include "../../../benchmarks/latency/core/config.hpp"

#include "echo_mocks.hpp"
#include "mock_dispatcher.hpp"

namespace {

using namespace std::chrono_literals;

// Resolve NIC_B's PCI BDF by env var (EPH_TEST_NIC_B_PCI) or by parsing
// the optional NIC_B_PCI=... line in bench.conf.
std::string resolve_nic_b_pci(const std::string& bench_conf_path) {
    if (const char* e = std::getenv("EPH_TEST_NIC_B_PCI"); e && *e) return e;
    std::ifstream f(bench_conf_path);
    if (!f) return {};
    std::string line;
    while (std::getline(f, line)) {
        size_t s = 0;
        while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
        if (s >= line.size() || line[s] == '#' || line[s] == '[') continue;
        if (line.compare(s, 9, "NIC_B_PCI") != 0) continue;
        size_t eq = line.find('=', s);
        if (eq == std::string::npos) continue;
        size_t v = eq + 1;
        while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
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

// Read a top-level (pre-section) lowercase key from bench.conf.
std::string read_global_key(const std::string& path, std::string_view key) {
    std::ifstream f(path);
    if (!f) return {};
    std::string line;
    while (std::getline(f, line)) {
        size_t s = 0;
        while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
        if (s >= line.size() || line[s] == '#') continue;
        if (line[s] == '[') break;  // entered first section, stop scanning
        if (line.compare(s, key.size(), key) != 0) continue;
        size_t eq = line.find('=', s);
        if (eq == std::string::npos) continue;
        size_t v = eq + 1;
        while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
        size_t end = line.find_first_of(" \t#", v);
        if (end == std::string::npos) end = line.size();
        return line.substr(v, end - v);
    }
    return {};
}

// Process-wide environment: forks the kernel mock dispatcher (NIC_A
// side), then brings up DpdkBenchEnv with enable_rss=true on NIC_B.
// Cleans up via SIGTERM + waitpid + DpdkBenchEnv dtor.
class RssPlatformEnv : public ::testing::Environment {
public:
    static bool ready() noexcept { return ready_; }
    static const std::string& reason() noexcept { return reason_; }
    static ::eph::dpdk::test::DpdkBenchEnv& env() { return *env_; }

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

        // Pull the IPs we'll need from bench.conf — server side bind for
        // the kernel mock and source/gateway for the DPDK client.
        std::string server_ip = read_global_key(conf_path, "mock_ip");
        std::string client_ip = read_global_key(conf_path, "client_ip");
        std::string gw_ip     = read_global_key(conf_path, "gateway_ip");
        if (server_ip.empty() || client_ip.empty() || gw_ip.empty()) {
            reason_ = "bench.conf missing mock_ip / client_ip / gateway_ip "
                      "lowercase globals — required for E2E mock + ARP";
            return;
        }

        // Fork the kernel mock dispatcher BEFORE EAL init (parent inits EAL).
        mock_pid_ = ::fork();
        if (mock_pid_ < 0) {
            reason_ = std::string{"fork() failed: "} + std::strerror(errno);
            return;
        }
        if (mock_pid_ == 0) {
            // Child: run all kernel-side mocks (TCP echo on kTcpEchoPort etc).
            int rc = ::eph::dpdk::test_e2e::run_mock_dispatcher(server_ip);
            ::_exit(rc);
        }
        // Parent: wait for the mock to bind/listen (7 mocks need a moment).
        std::this_thread::sleep_for(1s);

        // Synthesize EAL argv pinning to the discovered NIC.
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

        // Multi-queue + enable_rss=true with the post-PR-2 invariant
        // pin AND the RETA-collapse fix: ENA's hash_update unsupported
        // → rss_active=false → dispatch_mode pinned to Software → RETA
        // forced to all-queue-0 → single Poller receives all traffic
        // even though nb_rx_queues=4 is physically allocated.  This
        // exercises the actual fix and proves single-Poller usage is
        // safe under multi-queue config.
        ::eph::dpdk::PlatformConfig pcfg{};
        pcfg.port_id          = 0;
        pcfg.nb_rx_queues     = 4;
        pcfg.nb_tx_queues     = 4;
        pcfg.enable_rss       = true;
        pcfg.link_timeout_ms  = 0;

        auto env_r = ::eph::dpdk::test::DpdkBenchEnv::create_full(
            static_cast<int>(argv.size()), argv.data(),
            server_ip, client_ip, gw_ip, pcfg);
        if (!env_r) {
            reason_ = "DpdkBenchEnv::create_full failed: " + env_r.error();
            // Reap the mock so it doesn't hang as a zombie.
            ::kill(mock_pid_, SIGTERM);
            int wstatus = 0;
            ::waitpid(mock_pid_, &wstatus, 0);
            mock_pid_ = -1;
            return;
        }
        env_ = std::make_unique<::eph::dpdk::test::DpdkBenchEnv>(
            std::move(*env_r));
        ready_ = true;
        spdlog::info("RssPlatformEnv: ready (PCI={}, mock pid={}, "
                     "nb_rx_queues={}, dispatch_mode={})",
                     pci, mock_pid_, env_->platform.nb_rx_queues(),
                     ::eph::net::dpdk::rx_dispatch_mode_name(
                         env_->platform.dispatch_mode()));
    }

    void TearDown() override {
        if (mock_pid_ > 0) {
            ::kill(mock_pid_, SIGTERM);
            int wstatus = 0;
            ::waitpid(mock_pid_, &wstatus, 0);
            mock_pid_ = -1;
        }
        env_.reset();
        ready_ = false;
    }

private:
    static inline bool ready_ = false;
    static inline std::string reason_;
    static inline pid_t mock_pid_ = -1;
    static inline std::unique_ptr<::eph::dpdk::test::DpdkBenchEnv> env_;
};

#define EPH_RSS_PLATFORM_SKIP_IF_NOT_READY()                            \
    do {                                                                 \
        if (!RssPlatformEnv::ready()) {                                  \
            GTEST_SKIP() << "RssPlatformEnv not ready: "                 \
                         << RssPlatformEnv::reason();                    \
        }                                                                \
    } while (0)

// Per-test rotating source-port allocator (mirrors test_dpdk_e2e.cpp).
inline uint16_t next_src_port() {
    static std::atomic<uint16_t> next{40000};
    return next.fetch_add(1, std::memory_order_relaxed);
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// PlatformRss.RegistryAndDispatchMode
//
// Registry plumbing + dispatch_mode invariant on the single-queue env
// (see SetUp rationale).  Declared FIRST so it runs before the E2E test
// — registry side-effects on queue 0 here would prevent the E2E test
// from registering its real Poller, so we only exercise NEGATIVE cases
// (out-of-range, null) plus the dispatch_mode pin.
// ─────────────────────────────────────────────────────────────────────────────

TEST(PlatformRss, RegistryAndDispatchMode) {
    EPH_RSS_PLATFORM_SKIP_IF_NOT_READY();
    auto& platform = RssPlatformEnv::env().platform;

    // PR-2 invariant: ENA's hash_update unsupported → rss_active=false →
    // dispatch_mode pinned to Software.  On Mellanox/Intel where
    // hash_update succeeds, this would stay at the detected mode; here
    // we just assert the value is one of the known three.
    const auto mode = platform.dispatch_mode();
    EXPECT_TRUE(mode == ::eph::net::dpdk::RxDispatchMode::Software ||
                mode == ::eph::net::dpdk::RxDispatchMode::RssPartitioned ||
                mode == ::eph::net::dpdk::RxDispatchMode::FlowDirector);
    spdlog::info("dispatch_mode = {}",
                 ::eph::net::dpdk::rx_dispatch_mode_name(mode));
    const uint16_t n = platform.nb_rx_queues();
    EXPECT_EQ(n, 4u);  // multi-queue env (see SetUp rationale)

    // Register fake DpdkPoller<void>* into queues 1..n-1 only — queue 0
    // is reserved for the E2E test below. Pointers are never dereferenced.
    using PollerPtr = ::eph::net::dpdk::DpdkPoller<void>*;
    PollerPtr fakes[4] = {
        nullptr,
        reinterpret_cast<PollerPtr>(static_cast<uintptr_t>(0xDEAD0001)),
        reinterpret_cast<PollerPtr>(static_cast<uintptr_t>(0xDEAD0002)),
        reinterpret_cast<PollerPtr>(static_cast<uintptr_t>(0xDEAD0003)),
    };
    for (uint16_t q = 1; q < n; ++q) {
        auto r = platform.register_poller(q, fakes[q]);
        EXPECT_TRUE(r.has_value()) << "register_poller(" << q << ") "
                                   << (r ? "" : r.error());
        EXPECT_EQ(platform.poller_for_queue(q), fakes[q]);
    }

    // DuplicateQueue: re-register a previously occupied qid (1).
    auto* dup =
        reinterpret_cast<PollerPtr>(static_cast<uintptr_t>(0xBEEF0000));
    auto r_dup = platform.register_poller(1, dup);
    ASSERT_FALSE(r_dup.has_value());
    EXPECT_NE(r_dup.error().find("DuplicateQueue"), std::string::npos);
    EXPECT_EQ(platform.poller_for_queue(1), fakes[1])
        << "duplicate register must not overwrite";

    // QueueOutOfRange: 9999 above kMaxRssQueues + any NIC qid.
    auto* oor =
        reinterpret_cast<PollerPtr>(static_cast<uintptr_t>(0xBEEF0001));
    auto r_oor = platform.register_poller(9999, oor);
    ASSERT_FALSE(r_oor.has_value());
    EXPECT_NE(r_oor.error().find("QueueOutOfRange"), std::string::npos);
    EXPECT_EQ(platform.poller_for_queue(9999), nullptr);

    // null Poller rejected.
    auto r_null = platform.register_poller(0, nullptr);
    ASSERT_FALSE(r_null.has_value());
    EXPECT_NE(r_null.error().find("null"), std::string::npos);

    // Queue 0 still empty (reserved for E2E).
    EXPECT_EQ(platform.poller_for_queue(0), nullptr);

    // Unregistered queue past nb_rx_queues → nullptr.
    if (n < 64) {
        EXPECT_EQ(platform.poller_for_queue(n), nullptr);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// PlatformRss.CreateAndAttachSoftwareModeE2E
//
// End-to-end: build a real DpdkPoller for queue 0, register it on the
// Platform, call DpdkTcpStream::create_and_attach against the forked
// kernel TCP echo mock on NIC_A:kTcpEchoPort, send "ping", verify echo.
// Proves the turnkey attach path actually drives a TCP connection
// through to data exchange under the post-PR-2 dispatch_mode=Software
// pin (ENA's RSS hash_update is unsupported, so dispatch_mode=Software
// even with enable_rss=true).
// ─────────────────────────────────────────────────────────────────────────────

TEST(PlatformRss, CreateAndAttachSoftwareModeE2E) {
    EPH_RSS_PLATFORM_SKIP_IF_NOT_READY();
    auto& benv = RssPlatformEnv::env();
    auto& platform = benv.platform;

    // The PR-2 invariant pins dispatch_mode=Software when RSS isn't
    // active (true on ENA where hash_update fails).  For the FlowDirector
    // / RssPartitioned variants this would need a hash-update-capable
    // NIC; not exercised here.
    if (platform.dispatch_mode() !=
            ::eph::net::dpdk::RxDispatchMode::Software) {
        GTEST_SKIP() << "dispatch_mode is "
                     << ::eph::net::dpdk::rx_dispatch_mode_name(
                            platform.dispatch_mode())
                     << "; this E2E only exercises the Software path";
    }

    // Build the queue-0 Poller and register it with the Platform.
    namespace ed = ::eph::net::dpdk;
    ed::PollerConfig pcfg{};
    pcfg.port_id = benv.port_id;
    pcfg.rx_queue_id = 0;
    auto poller_r = ed::DpdkPoller<>::create(pcfg);
    ASSERT_TRUE(poller_r.has_value())
        << "DpdkPoller::create failed: " << poller_r.error().detail;
    auto poller = std::move(*poller_r);

    auto reg_r = platform.register_poller(0, poller.get());
    ASSERT_TRUE(reg_r.has_value())
        << "register_poller(0) failed: " << reg_r.error();

    // Build StreamConfig pointing at the kernel TCP echo mock.
    using TStream = ed::DpdkTcpStream<::eph::codec::RawStreamCodec, false>;
    ed::StreamConfig scfg{};
    scfg.legacy = benv.make_tcp_config(
        next_src_port(),
        ::eph::dpdk::test_e2e::kTcpEchoPort);
    scfg.pool = benv.pool;
    scfg.connect_timeout = 10s;  // generous; first-packet warmup over VPC
    // pin_to_queue=0 in Software mode is the only valid value; nullopt
    // is also valid and would land on queue 0.

    auto stream_r = TStream::create_and_attach(std::move(scfg), platform);
    ASSERT_TRUE(stream_r.has_value())
        << "create_and_attach failed: " << stream_r.error().detail;
    auto stream = std::move(*stream_r);

    // Set up RX collector before sending.
    std::atomic<bool> got_echo{false};
    std::vector<uint8_t> received;
    received.reserve(32);
    stream->on_message = [&](std::span<const uint8_t> data) {
        received.insert(received.end(), data.begin(), data.end());
        if (received.size() >= 4 &&
            std::memcmp(received.data(), "ping", 4) == 0) {
            got_echo.store(true, std::memory_order_release);
        }
    };

    // Send the payload.
    const uint8_t payload[] = {'p', 'i', 'n', 'g'};
    auto send_r = stream->send(std::span<const uint8_t>(payload));
    ASSERT_TRUE(send_r.has_value())
        << "stream->send failed: " << send_r.error().detail;

    // Drive the poller until the echo arrives or we time out.
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!got_echo.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        (void)poller->poll();
    }
    EXPECT_TRUE(got_echo.load())
        << "did not receive 'ping' echo within 3s — received "
        << received.size() << " bytes";

    // Detach + close gracefully.
    (void)stream->close_gracefully();
    poller->poll(/*max_iterations*/);  // drain any close packets
    (void)poller->remove(stream.get());
}

// ─────────────────────────────────────────────────────────────────────────────
// gtest main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new RssPlatformEnv);
    return RUN_ALL_TESTS();
}
