/// @file test_dpdk_rss_fanout.cpp
/// Integration test: N concurrent DpdkTcpStream attaches all pinned to
/// the same RSS queue + same exchange endpoint — the canonical HFT
/// "fan-out producer" pattern (e.g. 15-path bn_produce_dpdk_real_bn).
///
/// What this catches that the existing integration suite missed:
///   * Pre-fix `find_src_port_for_queue` was a deterministic linear scan
///     from port_range_start, returning the first match. With identical
///     inputs every call returned the same src_port → all N streams
///     trampled each other's RX or got EADDRINUSE-equivalent collisions.
///   * Existing test_dpdk_rss_platform exercises a SINGLE stream attach.
///     Single-stream tests pass even with the buggy first-match scan.
///
/// What this asserts:
///   1. N create_and_attach calls all succeed (no port allocation
///      failure, no pre-flight 5-tuple collision rejection).
///   2. Each stream's TCP echo round-trip completes — i.e. each stream
///      RXes its OWN unique payload, not a peer's. RX trampling would
///      surface as wrong-payload or no-payload.
///   3. Sufficient fan-out (kFanout) to surface the bug if reintroduced
///      (pre-fix would fail at stream #2 already).
///
/// Run conditions: NIC_B vfio + sudo (same as test_dpdk_rss_platform).
/// SKIPS cleanly otherwise. Mirrors test_dpdk_rss_platform's setup
/// pattern (forks kernel mock dispatcher; brings up DpdkBenchEnv with
/// enable_rss=true / nb_rx_queues=4).

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
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
namespace ed = ::eph::net::dpdk;

bool nic_on_vfio_pci(const std::string& pci_bdf) {
    if (pci_bdf.empty()) return false;
    std::string sys = "/sys/bus/pci/drivers/vfio-pci/" + pci_bdf;
    return ::access(sys.c_str(), F_OK) == 0;
}

// Process-wide environment — same shape as RssPlatformEnv in
// test_dpdk_rss_platform.cpp. Forks the kernel mock dispatcher then
// brings up DpdkBenchEnv with multi-queue + RSS.
class RssFanoutEnv : public ::testing::Environment {
public:
    static bool ready() noexcept { return ready_; }
    static const std::string& reason() noexcept { return reason_; }
    static ::eph::dpdk::test::DpdkBenchEnv& env() { return *env_; }

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
                      "skipping fan-out tests";
            return;
        }

        const std::string& server_ip = cfg.networking.server_ip;
        const std::string& client_ip = cfg.networking.client_ip;
        const std::string& gw_ip     = cfg.networking.gateway_ip;
        if (server_ip.empty() || client_ip.empty() || gw_ip.empty()) {
            reason_ = "config.toml missing [networking].server_ip / .client_ip "
                      "/ .gateway_ip";
            return;
        }

        mock_pid_ = ::fork();
        if (mock_pid_ < 0) {
            reason_ = std::string{"fork() failed: "} + std::strerror(errno);
            return;
        }
        if (mock_pid_ == 0) {
            int rc = ::eph::dpdk::test_e2e::run_mock_dispatcher(server_ip);
            ::_exit(rc);
        }
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

        std::this_thread::sleep_for(1s);

        ::eph::dpdk::PlatformConfig pcfg{};
        pcfg.port_id          = 0;
        pcfg.nb_rx_queues     = 4;
        pcfg.nb_tx_queues     = 4;
        pcfg.link_timeout_ms  = 0;

        ::eph::dpdk::EalConfig eal_cfg{};
        eal_cfg.program_name = "test_dpdk_rss_fanout";
        eal_cfg.lcores       = {"0-3"};
        eal_cfg.allowed_devs = {pci};
        eal_cfg.extra_args   = {"-n", "4", "--in-memory"};

        auto env_r = ::eph::dpdk::test::DpdkBenchEnv::create(
            std::move(pcfg),
            std::move(eal_cfg),
            /*pins=*/{},
            ::eph::utils::CpuPinPolicy{},
            server_ip, client_ip, gw_ip);
        if (!env_r) {
            reason_ = "DpdkBenchEnv::create failed: " + env_r.error();
            return;
        }
        env_ = std::make_unique<::eph::dpdk::test::DpdkBenchEnv>(
            std::move(*env_r));
        reaper.pid = -1;
        ready_ = true;
        spdlog::info("RssFanoutEnv: ready (PCI={}, mock pid={}, "
                     "nb_rx_queues={}, dispatch_mode={})",
                     pci, mock_pid_, env_->platform.nb_rx_queues(),
                     ed::rx_dispatch_mode_name(env_->platform.dispatch_mode()));
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

#define EPH_RSS_FANOUT_SKIP_IF_NOT_READY()                              \
    do {                                                                 \
        if (!RssFanoutEnv::ready()) {                                    \
            GTEST_SKIP() << "RssFanoutEnv not ready: "                   \
                         << RssFanoutEnv::reason();                      \
        }                                                                \
    } while (0)

#define EPH_FANOUT_SKIP_IF_NOT_RSS_PARTITIONED(plat)                     \
    do {                                                                 \
        if ((plat).dispatch_mode() != ed::RxDispatchMode::RssPartitioned) { \
            GTEST_SKIP() << "dispatch_mode is "                          \
                         << ed::rx_dispatch_mode_name((plat).dispatch_mode()) \
                         << "; fan-out test requires RssPartitioned";   \
        }                                                                \
    } while (0)

inline uint16_t initial_src_port(uint16_t i) {
    // Pre-attach src_port — create_and_attach in RssPartitioned + pin
    // mode rebinds it to a hash-correct value. The initial value is
    // arbitrary but must be in the ephemeral range.
    return static_cast<uint16_t>(45000 + i);
}

}  // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// PlatformRssFanout.NStreamsSameQueueSameEndpoint
//
// The actual regression test for the find_src_port_for_queue fan-out bug.
// Attaches kFanout streams all pinned to queue 0, all connecting to the
// same kernel TCP echo mock. Each stream sends a unique 1-byte payload
// (its index) — verifies it RXes back its OWN byte, not a peer's.
// ─────────────────────────────────────────────────────────────────────────────

TEST(PlatformRssFanout, NStreamsSameQueueSameEndpoint) {
    EPH_RSS_FANOUT_SKIP_IF_NOT_READY();
    auto& benv = RssFanoutEnv::env();
    auto& platform = benv.platform;
    EPH_FANOUT_SKIP_IF_NOT_RSS_PARTITIONED(platform);

    // Diagnostic: dump the RSS key our software predictor will use.
    // Compare with `ethtool -x ens35` (run before vfio bind) to confirm
    // whether DPDK PMD really returned the NIC's firmware key (probe
    // succeeded) vs returned a default-filled buffer (probe is fake).
    if (auto state_r = ed::query_rss_state(benv.port_id); state_r) {
        std::string hex;
        hex.reserve(state_r->key_len * 3);
        auto key = state_r->key();
        for (size_t i = 0; i < key.size(); ++i) {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%02x:", key[i]);
            hex += buf;
        }
        if (!hex.empty()) hex.pop_back();
        spdlog::info("RssFanout: probed RSS key (key_len={}, "
                     "rss_using_probed_key={}, reta_size={}): {}",
                     state_r->key_len,
                     platform.rss_using_probed_key(),
                     state_r->reta_size,
                     hex);
    }

    // Production reference is 15 paths; 12 here keeps test runtime under
    // ~5s on dev hosts while still covering the bug surface — pre-fix
    // would fail at stream #2 already.
    constexpr uint16_t kFanout = 12;
    constexpr uint16_t kTargetQueue = 0;

    // Single Poller for queue 0; all kFanout streams share it.
    ed::PollerConfig pcfg{};
    pcfg.port_id = benv.port_id;
    pcfg.rx_queue_id = kTargetQueue;
    auto poller_r = ed::DpdkPoller<>::create(pcfg);
    ASSERT_TRUE(poller_r.has_value())
        << "DpdkPoller::create: " << poller_r.error().detail;
    auto poller = std::move(*poller_r);

    auto reg_r = platform.register_poller(kTargetQueue, poller.get());
    ASSERT_TRUE(reg_r.has_value())
        << "register_poller(" << kTargetQueue << "): "
        << reg_r.error().detail;

    // Per-stream RX state — indexed by stream slot, captures the bytes
    // received on each stream's `on_message`. Used to verify that each
    // stream RX'es its OWN payload (no cross-stream trampling under
    // shared src_port).
    using TStream = ed::DpdkTcpStream<::eph::codec::RawStreamCodec, false>;
    struct PerStream {
        std::unique_ptr<TStream> stream;
        std::vector<uint8_t>     received;
        std::atomic<bool>        got_echo{false};
        uint8_t                  expected_byte{};
    };
    std::vector<std::unique_ptr<PerStream>> slots;
    slots.reserve(kFanout);

    // Attach kFanout streams pinned to the same queue.
    for (uint16_t i = 0; i < kFanout; ++i) {
        ed::StreamConfig scfg{};
        scfg.dpdk.tcp_low_level = benv.make_tcp_config(
            initial_src_port(i),
            ::eph::dpdk::test_e2e::kTcpEchoPort);
        scfg.dpdk.pool = benv.pool;
        scfg.connect_timeout = 10s;
        scfg.dpdk.pin_to_queue = kTargetQueue;

        auto stream_r = TStream::create_and_attach(std::move(scfg), platform);
        ASSERT_TRUE(stream_r.has_value())
            << "stream " << i
            << " create_and_attach failed (pre-fix bug surface here): "
            << stream_r.error().detail;

        auto slot = std::make_unique<PerStream>();
        slot->stream = std::move(*stream_r);
        slot->expected_byte = static_cast<uint8_t>(i);

        // Capture the payload index in the closure for verification.
        auto* slot_ptr = slot.get();
        slot_ptr->stream->on_message =
            [slot_ptr](std::span<const uint8_t> data) {
                slot_ptr->received.insert(slot_ptr->received.end(),
                                           data.begin(), data.end());
                if (slot_ptr->received.size() >= 1) {
                    slot_ptr->got_echo.store(true,
                                              std::memory_order_release);
                }
            };
        slots.push_back(std::move(slot));
    }

    // Send a unique 1-byte payload per stream. We send all then poll
    // everyone — closer to the production "burst-send + poll-loop"
    // pattern than per-stream send-then-poll.
    for (uint16_t i = 0; i < kFanout; ++i) {
        const uint8_t payload[1] = { static_cast<uint8_t>(i) };
        auto send_r = slots[i]->stream->send(
            std::span<const uint8_t>(payload));
        ASSERT_TRUE(send_r.has_value())
            << "stream " << i
            << " send: " << send_r.error().detail;
    }

    // Drive the shared poller until every stream has its own echo back,
    // or we exceed a generous deadline. RSS multi-queue + shared queue
    // means the poller iterates all N streams' TCP sessions per cycle.
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        bool all_done = true;
        for (auto& s : slots) {
            if (!s->got_echo.load(std::memory_order_acquire)) {
                all_done = false;
                break;
            }
        }
        if (all_done) break;
        (void)poller->poll();
    }

    // Verify each stream got its OWN byte back. Pre-fix bug would
    // surface either as (a) some streams have 0 received bytes (5-tuple
    // collision dropped their RX) or (b) some streams received the
    // wrong byte (cross-stream trampling).
    for (uint16_t i = 0; i < kFanout; ++i) {
        const auto& s = *slots[i];
        EXPECT_TRUE(s.got_echo.load(std::memory_order_acquire))
            << "stream " << i << " did not receive any echo "
            << "(pre-fix bug: 5-tuple collision dropped this stream's RX)";
        ASSERT_EQ(s.received.size(), 1u)
            << "stream " << i << " received " << s.received.size()
            << " bytes, expected exactly 1";
        EXPECT_EQ(s.received[0], s.expected_byte)
            << "stream " << i << " received byte " << int(s.received[0])
            << " but expected " << int(s.expected_byte)
            << " (RX trampling: another stream's payload landed here)";
    }

    // Cleanup. Order matters:
    //   1. close_gracefully each stream (FIN-ACK handshake)
    //   2. drain poll() so the close packets land
    //   3. detach each stream from the poller
    //   4. unregister the queue-0 poller from the Platform — without
    //      this, the next test reuses the shared Platform and finds
    //      queue 0 still pointing at our about-to-be-destroyed Poller.
    for (auto& s : slots) {
        (void)s->stream->close_gracefully();
    }
    (void)poller->poll();
    for (auto& s : slots) {
        (void)poller->remove(s->stream.get());
    }
    platform.unregister_poller(kTargetQueue);
}

// ─────────────────────────────────────────────────────────────────────────────
// PlatformRssFanout.NStreamsDistributedAcrossQueues
//
// Sanity check that a stream pinned to queue N≠0 actually has its RX
// SYN-ACK delivered to queue N — exercises the create_and_attach
// queue-alignment fix (rx_queue_id was previously left at whatever the
// caller's TcpConfig defaulted to, while the SYN-ACK landed on the
// hashed-to queue per find_src_port_for_queue's pick — silent 10s
// handshake timeout for every pin_to_queue!=0 attach).
// ─────────────────────────────────────────────────────────────────────────────

TEST(PlatformRssFanout, NStreamsDistributedAcrossQueues) {
    EPH_RSS_FANOUT_SKIP_IF_NOT_READY();
    auto& benv = RssFanoutEnv::env();
    auto& platform = benv.platform;
    EPH_FANOUT_SKIP_IF_NOT_RSS_PARTITIONED(platform);

    const uint16_t nb_q = platform.nb_rx_queues();
    constexpr uint16_t kFanout = 8;  // multiple per queue when nb_q=4

    // One Poller per queue.
    using TStream = ed::DpdkTcpStream<::eph::codec::RawStreamCodec, false>;
    std::vector<std::unique_ptr<ed::DpdkPoller<>>> pollers(nb_q);
    for (uint16_t q = 0; q < nb_q; ++q) {
        ed::PollerConfig pcfg{};
        pcfg.port_id = benv.port_id;
        pcfg.rx_queue_id = q;
        auto poller_r = ed::DpdkPoller<>::create(pcfg);
        ASSERT_TRUE(poller_r.has_value())
            << "queue " << q << ": " << poller_r.error().detail;
        pollers[q] = std::move(*poller_r);
        auto reg_r = platform.register_poller(q, pollers[q].get());
        ASSERT_TRUE(reg_r.has_value())
            << "register_poller(" << q << "): " << reg_r.error().detail;
    }

    struct PerStream {
        std::unique_ptr<TStream> stream;
        std::vector<uint8_t>     received;
        std::atomic<bool>        got_echo{false};
        uint8_t                  expected_byte{};
        uint16_t                 queue{};
    };
    std::vector<std::unique_ptr<PerStream>> slots;
    slots.reserve(kFanout);

    for (uint16_t i = 0; i < kFanout; ++i) {
        const uint16_t q = i % nb_q;

        ed::StreamConfig scfg{};
        scfg.dpdk.tcp_low_level = benv.make_tcp_config(
            static_cast<uint16_t>(46000 + i),
            ::eph::dpdk::test_e2e::kTcpEchoPort);
        scfg.dpdk.pool = benv.pool;
        scfg.connect_timeout = 10s;
        scfg.dpdk.pin_to_queue = q;

        auto stream_r = TStream::create_and_attach(std::move(scfg), platform);
        ASSERT_TRUE(stream_r.has_value())
            << "stream " << i << " (queue " << q << "): "
            << stream_r.error().detail;

        auto slot = std::make_unique<PerStream>();
        slot->stream = std::move(*stream_r);
        slot->expected_byte = static_cast<uint8_t>(i);
        slot->queue = q;

        auto* slot_ptr = slot.get();
        slot_ptr->stream->on_message =
            [slot_ptr](std::span<const uint8_t> data) {
                slot_ptr->received.insert(slot_ptr->received.end(),
                                           data.begin(), data.end());
                if (slot_ptr->received.size() >= 1) {
                    slot_ptr->got_echo.store(true,
                                              std::memory_order_release);
                }
            };
        slots.push_back(std::move(slot));
    }

    for (uint16_t i = 0; i < kFanout; ++i) {
        const uint8_t payload[1] = { static_cast<uint8_t>(i) };
        ASSERT_TRUE(slots[i]->stream->send(
            std::span<const uint8_t>(payload)).has_value());
    }

    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        bool all_done = true;
        for (auto& s : slots) {
            if (!s->got_echo.load(std::memory_order_acquire)) {
                all_done = false;
                break;
            }
        }
        if (all_done) break;
        // Drive every queue's poller per cycle.
        for (auto& p : pollers) (void)p->poll();
    }

    for (uint16_t i = 0; i < kFanout; ++i) {
        const auto& s = *slots[i];
        EXPECT_TRUE(s.got_echo.load())
            << "stream " << i << " (queue " << s.queue << "): no echo";
        ASSERT_EQ(s.received.size(), 1u);
        EXPECT_EQ(s.received[0], s.expected_byte)
            << "stream " << i << " (queue " << s.queue
            << ") received wrong byte " << int(s.received[0]);
    }

    for (auto& s : slots) {
        (void)s->stream->close_gracefully();
    }
    for (auto& p : pollers) (void)p->poll();
    for (auto& s : slots) {
        for (auto& p : pollers) (void)p->remove(s->stream.get());
    }
    // Unregister all per-queue pollers so the Platform's slot map does
    // not retain dangling pointers after the local Pollers go out of
    // scope at function return.
    for (uint16_t q = 0; q < nb_q; ++q) {
        platform.unregister_poller(q);
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new RssFanoutEnv);
    return RUN_ALL_TESTS();
}
