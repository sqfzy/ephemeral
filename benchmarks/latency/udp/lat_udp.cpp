/// @file udp/lat_udp.cpp
/// lat_udp / lat_udp_dpdk — single-binary UDP latency benchmark.
///
/// Same fork+setns layout as lat_tcp.cpp. UDP is simpler because every
/// datagram already carries its own length, so the mock can echo any
/// payload size without an out-of-band header.

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "eph/utils/cpu.hpp"
#include "eph/utils/cpu_pin.hpp"
#include "eph/utils/time.hpp"

#include "../core/config.hpp"
#include "../core/netns.hpp"
#include "../core/runner.hpp"
#include "../core/sample.hpp"
#include "../core/signal.hpp"
#include "../core/socket_bind.hpp"
#include "../core/tsc_protocol.hpp"

#if defined(EPH_USE_DPDK)
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include "../core/dpdk_env.hpp"
#include "eph/dpdk/packet_parse.hpp"
#include "eph/dpdk/udp.hpp"
#endif

using namespace bench;

namespace {
constexpr std::array<size_t, 6> kDefaultUdpPayloads{
    64, 128, 256, 512, 1024, 1472
};
} // namespace

// ───────────────────────────────────────────────────────────────────────────
// mock_fn — POSIX UDP echo server.
// ───────────────────────────────────────────────────────────────────────────
namespace bench::udp::mock_fn {

int run(const BenchConfig& cfg) {
    eph::utils::CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    if (auto p = eph::utils::pin_thread_strict(cfg.mock_cpu, "mock_lat_udp", policy); !p) {
        spdlog::error("mock_lat_udp: pin failed: {}", p.error());
        return 1;
    }

    auto fd = bench::udp_bind(cfg.server_ip, cfg.server_port);
    if (!fd) { spdlog::error("mock_lat_udp: {}", fd.error()); return 1; }

    spdlog::info("mock_lat_udp: bound {}:{} work_ns={}",
                 cfg.server_ip, cfg.server_port, cfg.server_work_ns);

    constexpr size_t kMaxPayload = 65536;
    std::vector<uint8_t> buf(kMaxPayload);

    while (g_running.load(std::memory_order_acquire)) {
        // Poll for shutdown responsiveness even when idle.
        pollfd p{}; p.fd = *fd; p.events = POLLIN;
        int rv = ::poll(&p, 1, 100);
        if (rv < 0) {
            if (errno == EINTR) continue;
            spdlog::error("mock_lat_udp: poll: {}", std::strerror(errno));
            break;
        }
        if (rv == 0) continue;

        sockaddr_in src{};
        socklen_t slen = sizeof(src);
        ssize_t n = ::recvfrom(*fd, buf.data(), buf.size(), 0,
                               reinterpret_cast<sockaddr*>(&src), &slen);
        if (n < static_cast<ssize_t>(tsc::kBinaryHeaderSize)) continue;

        uint64_t recv_tsc = eph::utils::TSC::now();
        std::memcpy(buf.data() + 8, &recv_tsc, 8);
        eph::utils::spin_for_ns(cfg.server_work_ns);
        uint64_t send_tsc = eph::utils::TSC::now();
        std::memcpy(buf.data() + 16, &send_tsc, 8);

        ::sendto(*fd, buf.data(), n, 0,
                 reinterpret_cast<sockaddr*>(&src), slen);
    }
    ::close(*fd);
    return 0;
}

} // namespace bench::udp::mock_fn

// ───────────────────────────────────────────────────────────────────────────
// client_fn — UDP RTT bench (kernel + DPDK variants).
// ───────────────────────────────────────────────────────────────────────────
namespace bench::udp::client_fn {

#if !defined(EPH_USE_DPDK)
class UdpRttScenario {
public:
    UdpRttScenario(std::string ip, uint16_t port)
        : ip_(std::move(ip)), port_(port) {}

    bool open() {
        int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return false;
        // 100 ms recv timeout — a single dropped packet should not stall.
        timeval tv{}; tv.tv_usec = 100'000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        peer_.sin_family = AF_INET;
        peer_.sin_port = htons(port_);
        if (::inet_pton(AF_INET, ip_.c_str(), &peer_.sin_addr) != 1) {
            ::close(fd); return false;
        }
        fd_ = fd;
        return true;
    }

    bool prepare(size_t payload) {
        if (payload < tsc::kBinaryHeaderSize) return false;
        send_buf_.assign(payload, 0xCD);
        recv_buf_.assign(payload, 0);
        return true;
    }

    bool do_one_rtt(RttSample& out) {
        out.client_send_tsc = eph::utils::TSC::now();
        std::memcpy(send_buf_.data(), &out.client_send_tsc, 8);

        ssize_t s = ::sendto(fd_, send_buf_.data(), send_buf_.size(), 0,
                             reinterpret_cast<const sockaddr*>(&peer_),
                             sizeof(peer_));
        if (s < 0) return false;
        ssize_t n = ::recvfrom(fd_, recv_buf_.data(), recv_buf_.size(), 0,
                               nullptr, nullptr);
        if (n < static_cast<ssize_t>(tsc::kBinaryHeaderSize)) return false;

        out.client_recv_tsc = eph::utils::TSC::now();
        std::memcpy(&out.server_recv_tsc, recv_buf_.data() + 8, 8);
        std::memcpy(&out.server_send_tsc, recv_buf_.data() + 16, 8);
        return true;
    }

    void cleanup() {}

    ~UdpRttScenario() { if (fd_ >= 0) ::close(fd_); }

private:
    std::string ip_;
    uint16_t    port_;
    int         fd_ = -1;
    sockaddr_in peer_{};
    std::vector<uint8_t> send_buf_;
    std::vector<uint8_t> recv_buf_;
};

int run(const BenchConfig& cfg, int /*argc*/, char** /*argv*/) {
    if (auto e = bench::enter_netns("bench_ns"); !e) {
        spdlog::error("lat_udp: enter_netns: {}", e.error());
        return 1;
    }
    eph::utils::CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    if (auto p = eph::utils::pin_thread_strict(cfg.client_cpu, "bench_lat_udp", policy); !p) {
        spdlog::error("lat_udp: pin failed: {}", p.error());
        return 1;
    }

    UdpRttScenario scenario{cfg.server_ip, cfg.server_port};
    // Retry: forked mock may not be bound yet.
    bool opened = false;
    for (int i = 0; i < 50; ++i) {
        if (scenario.open()) { opened = true; break; }
        ::usleep(20'000);
    }
    if (!opened) {
        spdlog::error("lat_udp: failed to open client socket");
        return 1;
    }
    spdlog::info("lat_udp (kernel): peer {}:{}", cfg.server_ip, cfg.server_port);

    std::span<const size_t> payloads =
        cfg.udp_payloads.empty()
            ? std::span<const size_t>{kDefaultUdpPayloads}
            : std::span<const size_t>{cfg.udp_payloads};

    BenchRunner runner{cfg, "udp", "kernel"};
    runner.run_rtt_sweep(scenario, payloads);
    return 0;
}

#else // EPH_USE_DPDK

class UdpDpdkRttScenario {
public:
    UdpDpdkRttScenario(eph::dpdk::UdpSender& sender,
                       uint16_t port_id, uint16_t rx_queue,
                       uint16_t expected_dst_port)
        : sender_(sender), port_id_(port_id), rx_queue_(rx_queue),
          expected_dst_port_(expected_dst_port),
          timeout_cycles_(tsc::ns_to_cycles(100'000'000)) {}

    bool prepare(size_t payload) {
        if (payload < tsc::kBinaryHeaderSize) return false;
        send_buf_.assign(payload, 0xCD);
        recv_buf_.assign(payload, 0);
        payload_size_ = payload;
        return true;
    }

    bool do_one_rtt(RttSample& out) {
        out.client_send_tsc = eph::utils::TSC::now();
        std::memcpy(send_buf_.data(), &out.client_send_tsc, 8);

        if (!sender_.send(send_buf_.data(), static_cast<uint16_t>(payload_size_))) {
            return false;
        }

        uint64_t deadline = eph::utils::TSC::now() + timeout_cycles_;
        rte_mbuf* pkts[32];
        while (eph::utils::TSC::now() < deadline) {
            uint16_t nb_rx = rte_eth_rx_burst(port_id_, rx_queue_, pkts, 32);
            for (uint16_t i = 0; i < nb_rx; ++i) {
                auto parsed = eph::dpdk::net::parse_udp_packet(pkts[i]);
                if (parsed &&
                    parsed.dst_port() == expected_dst_port_ &&
                    parsed.payload_len >= tsc::kBinaryHeaderSize) {
                    size_t n = std::min(static_cast<size_t>(parsed.payload_len),
                                        recv_buf_.size());
                    std::memcpy(recv_buf_.data(), parsed.payload, n);
                    for (uint16_t j = 0; j < nb_rx; ++j) rte_pktmbuf_free(pkts[j]);
                    out.client_recv_tsc = eph::utils::TSC::now();
                    std::memcpy(&out.server_recv_tsc, recv_buf_.data() + 8, 8);
                    std::memcpy(&out.server_send_tsc, recv_buf_.data() + 16, 8);
                    return true;
                }
                rte_pktmbuf_free(pkts[i]);
            }
        }
        return false;
    }

    void cleanup() {}

private:
    eph::dpdk::UdpSender& sender_;
    uint16_t port_id_, rx_queue_, expected_dst_port_;
    uint64_t timeout_cycles_;
    size_t payload_size_ = 0;
    std::vector<uint8_t> send_buf_;
    std::vector<uint8_t> recv_buf_;
};

int run(const BenchConfig& cfg, int argc, char** argv) {
    auto env = DpdkBenchEnv::create_full(argc, argv,
        cfg.server_ip, cfg.local_ip, cfg.gateway_ip, /*port_id=*/0);
    if (!env) { spdlog::error("lat_udp(dpdk): {}", env.error()); return 1; }

    eph::utils::CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    if (auto p = eph::utils::pin_thread_strict(cfg.client_cpu, "bench_lat_udp", policy); !p) {
        spdlog::error("lat_udp: pin failed: {}", p.error());
        return 1;
    }

    constexpr uint16_t kLocalPort = 55500;
    auto sender = env->make_udp_sender(kLocalPort, cfg.server_port);
    if (!sender) { spdlog::error("UdpSender: {}", sender.error()); return 1; }
    spdlog::info("lat_udp (dpdk): peer {}:{}", cfg.server_ip, cfg.server_port);

    std::span<const size_t> payloads =
        cfg.udp_payloads.empty()
            ? std::span<const size_t>{kDefaultUdpPayloads}
            : std::span<const size_t>{cfg.udp_payloads};

    UdpDpdkRttScenario scenario{*sender, env->port_id, 0, kLocalPort};
    BenchRunner runner{cfg, "udp", "dpdk"};
    runner.run_rtt_sweep(scenario, payloads);
    return 0;
}

#endif // EPH_USE_DPDK

} // namespace bench::udp::client_fn

// ───────────────────────────────────────────────────────────────────────────
// main
// ───────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    install_signal_handlers();
    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC calibration failed");
        return 1;
    }

    auto cfg_r = load_bench_conf();
    if (!cfg_r) { spdlog::error("{}", cfg_r.error()); return 1; }
    const BenchConfig& cfg = *cfg_r;

    pid_t pid = ::fork();
    if (pid < 0) {
        spdlog::error("fork: {}", std::strerror(errno));
        return 1;
    }
    if (pid == 0) {
        return bench::udp::mock_fn::run(cfg);
    }

    int rc = bench::udp::client_fn::run(cfg, argc, argv);

    ::kill(pid, SIGTERM);
    int wstatus = 0;
    ::waitpid(pid, &wstatus, 0);
    return rc;
}
