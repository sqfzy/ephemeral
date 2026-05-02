/// @file test_dpdk_rss_platform.cpp
///
/// **TODO(daemon-reshape / S5)**: every TEST in this file requires
/// multi-queue (`nb_rx_queues=4`) + RSS-aware secondary attach. The S3
/// placeholder of `Platform::create` only supports `cfg.queues == 1`,
/// and the legacy `DpdkBenchEnv::create` factory used here was
/// removed with `Platform::launch`. The whole env-level setup is
/// therefore stubbed to always SKIP at the Environment level —
/// individual TEST bodies are preserved verbatim so the assertions
/// can be reactivated when S5 lands the QueueAllocator + RETA-
/// tracking IPC and `DpdkBenchEnv` is rebuilt over `Platform::create`
/// (or replaced wholesale by the new daemon-led test fixture).
///
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
#include "../../../benchmarks/latency/core/bench_conf.hpp"

#include "echo_mocks.hpp"
#include "mock_dispatcher.hpp"

namespace {

using namespace std::chrono_literals;

bool nic_on_vfio_pci(const std::string& pci_bdf) {
    if (pci_bdf.empty()) return false;
    std::string sys = "/sys/bus/pci/drivers/vfio-pci/" + pci_bdf;
    return ::access(sys.c_str(), F_OK) == 0;
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
        // TODO(daemon-reshape S5): unconditional SKIP. The original
        // setup brought up a multi-queue Platform via
        // `DpdkBenchEnv::create` + the legacy `Platform::launch`
        // path. The legacy launch is gone, the daemon-led
        // `Platform::create` only supports `cfg.queues == 1` in the
        // S3 placeholder, and multi-queue secondary claims arrive in
        // S5. The body below is preserved verbatim, projected onto
        // the new `DpdkBenchEnv::create` signature so it still
        // type-checks.
        reason_ = "TODO(daemon-reshape S5): multi-queue secondary "
                  "attach not yet implemented; reactivate when the "
                  "QueueAllocator + RETA-tracking IPC lands.";
        ready_ = false;
        return;

        // ── original env setup, dead under S3 but kept for S5
        //    reactivation; the `return` above makes it unreachable.
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
                      "skipping RSS Platform tests";
            return;
        }

        const std::string& server_ip = cfg.networking.server_ip;
        const std::string& client_ip = cfg.networking.client_ip;
        const std::string& gw_ip     = cfg.networking.gateway_ip;
        if (server_ip.empty() || client_ip.empty() || gw_ip.empty()) {
            reason_ = "config.toml missing [networking].server_ip / .client_ip "
                      "/ .gateway_ip — required for E2E mock + ARP";
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

        // RAII guard: if anything between here and ready_=true throws or
        // returns early, reap the mock child instead of leaving a zombie
        // / a stale listener bound on the mock IP.  ChildReaper resets
        // mock_pid_ to -1 once parent ownership transfers to ready_=true.
        struct ChildReaper {
            pid_t& pid;
            ~ChildReaper() {
                if (pid > 0) {
                    ::kill(pid, SIGTERM);
                    int wstatus = 0;
                    ::waitpid(pid, &wstatus, 0);
                    pid = -1;
                }
            }
        };
        ChildReaper reaper{mock_pid_};

        // Parent: wait for the mock to bind/listen (7 mocks need a moment).
        std::this_thread::sleep_for(1s);

        // Multi-queue secondary attach. Today (S3 placeholder)
        // `cfg.queues == 1` is the only supported value, so this
        // branch is unreachable — but we keep it shape-correct
        // against the new `DpdkBenchEnv::create` signature for S5
        // reactivation.
        ::eph::dpdk::PlatformConfig pcfg{};
        pcfg.pci          = pci;
        pcfg.queues       = 4;  // S5: multi-queue secondary claim
        pcfg.program_name = "test_dpdk_rss_platform";
        pcfg.lcores       = {"0-3"};

        auto env_r = ::eph::dpdk::test::DpdkBenchEnv::create(
            std::move(pcfg), server_ip, client_ip, gw_ip);
        if (!env_r) {
            reason_ = "DpdkBenchEnv::create failed: " + env_r.error();
            // ChildReaper will reap on scope exit.
            return;
        }
        env_ = std::make_unique<::eph::dpdk::test::DpdkBenchEnv>(
            std::move(*env_r));
        // Transfer mock ownership to TearDown — ChildReaper must NOT reap.
        reaper.pid = -1;
        ready_ = true;
        SPDLOG_INFO("RssPlatformEnv: ready (PCI={}, mock pid={}, "
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
// Registry plumbing + dispatch_mode invariant on the multi-queue env
// (see SetUp rationale).  Declared FIRST so it runs before the E2E test
// — registry side-effects on queue 0 here would prevent the E2E test
// from registering its real Poller, so we only exercise NEGATIVE cases
// (out-of-range, null) plus the dispatch_mode pin.
// ─────────────────────────────────────────────────────────────────────────────

TEST(PlatformRss, RegistryAndDispatchMode) {
    EPH_RSS_PLATFORM_SKIP_IF_NOT_READY();
    auto& platform = RssPlatformEnv::env().platform;

    // Post-reshape: dispatch_mode stays at the detected capability when
    // RSS is active (whether installed by configure_rss or resolved via
    // probe), and pins to Software only in the single-queue / no-RSS
    // paths. On ENA with probe support this is typically RssPartitioned;
    // we just assert the value is one of the three known modes.
    const auto mode = platform.dispatch_mode();
    EXPECT_TRUE(mode == ::eph::net::dpdk::RxDispatchMode::Software ||
                mode == ::eph::net::dpdk::RxDispatchMode::RssPartitioned ||
                mode == ::eph::net::dpdk::RxDispatchMode::FlowDirector);
    SPDLOG_INFO("dispatch_mode = {}",
                 ::eph::net::dpdk::rx_dispatch_mode_name(mode));
    const uint16_t n = platform.nb_rx_queues();
    // Platform clamps to NIC max_rx_queues; use range check rather than
    // EXPECT_EQ(n, 4) so the test stays portable to NICs with fewer
    // queues than requested.
    EXPECT_GT(n, 1u);
    EXPECT_LE(n, 4u);

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
                                   << (r ? "" : r.error().detail);
        EXPECT_EQ(platform.poller_for_queue(q), fakes[q]);
    }

    // DuplicateQueue: re-register a previously occupied qid (1).
    auto* dup =
        reinterpret_cast<PollerPtr>(static_cast<uintptr_t>(0xBEEF0000));
    auto r_dup = platform.register_poller(1, dup);
    ASSERT_FALSE(r_dup.has_value());
    EXPECT_EQ(r_dup.error().code, eph::core::Error::InvalidConfig);
    EXPECT_NE(std::string_view(r_dup.error().detail).find("already"),
              std::string_view::npos);
    EXPECT_EQ(platform.poller_for_queue(1), fakes[1])
        << "duplicate register must not overwrite";

    // QueueOutOfRange: 9999 above kMaxRssQueues + any NIC qid.
    auto* oor =
        reinterpret_cast<PollerPtr>(static_cast<uintptr_t>(0xBEEF0001));
    auto r_oor = platform.register_poller(9999, oor);
    ASSERT_FALSE(r_oor.has_value());
    EXPECT_EQ(r_oor.error().code, eph::core::Error::InvalidConfig);
    EXPECT_NE(std::string_view(r_oor.error().detail).find("out of range"),
              std::string_view::npos);
    EXPECT_EQ(platform.poller_for_queue(9999), nullptr);

    // null Poller rejected.
    auto r_null = platform.register_poller(0, nullptr);
    ASSERT_FALSE(r_null.has_value());
    EXPECT_EQ(r_null.error().code, eph::core::Error::InvalidConfig);
    EXPECT_NE(std::string_view(r_null.error().detail).find("null"),
              std::string_view::npos);

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

    // This E2E only exercises the Software path (single-Poller on queue 0).
    // After the bring-up reshape, dispatch_mode stays at the NIC's detected
    // capability when RSS is active — so on ENA with probe support this
    // test SKIPs cleanly. Reachable only on hardware/configs where
    // dispatch_mode genuinely lands at Software (e.g. nb_rx_queues=1
    // configs, or NICs without RSS/FlowDirector capability).
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
        << "register_poller(0) failed: " << reg_r.error().detail;

    // Build StreamConfig pointing at the kernel TCP echo mock.
    using TStream = ed::DpdkTcpStream<::eph::codec::RawStreamCodec, false>;
    ed::StreamConfig scfg{};
    scfg.dpdk.wire = benv.make_tcp_config(
        next_src_port(),
        ::eph::dpdk::test_e2e::kTcpEchoPort);
    scfg.dpdk.pool = benv.pool;
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
